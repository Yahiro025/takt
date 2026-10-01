#include "sample_mixer.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <sndfile.h>
#include <linux/input-event-codes.h>

#include "key_layout.hpp"

namespace keeby {
namespace {

// RBJ Audio EQ Cookbook shelving filter, shelf slope S = 1. `low` selects
// low-shelf vs. high-shelf; `freq_hz`/`gain_db` per docs/011. Coefficients
// are normalized by a0 (a0 folded into b*/a1/a2, matching BiquadCoeffs'
// "a0 == 1" convention). At gain_db == 0 this is an exact identity filter
// (b0==a0, b1==a1, b2==a2 before normalization) -- callers still prefer an
// explicit bypass flag for bit-exactness rather than relying on that.
BiquadCoeffs rbj_shelf(bool low, float freq_hz, float gain_db, float sample_rate) noexcept {
    const float a = std::pow(10.0f, gain_db / 40.0f);
    const float w0 = 2.0f * std::numbers::pi_v<float> * freq_hz / sample_rate;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    constexpr float kShelfSlope = 1.0f; // S = 1: RBJ's "maximally flat" shelf
    const float alpha = sinw0 / 2.0f * std::sqrt((a + 1.0f / a) * (1.0f / kShelfSlope - 1.0f) + 2.0f);
    const float sqrtA = std::sqrt(a);
    const float two_sqrtA_alpha = 2.0f * sqrtA * alpha;

    float b0, b1, b2, a0, a1, a2;
    if (low) {
        b0 = a * ((a + 1.0f) - (a - 1.0f) * cosw0 + two_sqrtA_alpha);
        b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cosw0);
        b2 = a * ((a + 1.0f) - (a - 1.0f) * cosw0 - two_sqrtA_alpha);
        a0 = (a + 1.0f) + (a - 1.0f) * cosw0 + two_sqrtA_alpha;
        a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cosw0);
        a2 = (a + 1.0f) + (a - 1.0f) * cosw0 - two_sqrtA_alpha;
    } else {
        b0 = a * ((a + 1.0f) + (a - 1.0f) * cosw0 + two_sqrtA_alpha);
        b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cosw0);
        b2 = a * ((a + 1.0f) + (a - 1.0f) * cosw0 - two_sqrtA_alpha);
        a0 = (a + 1.0f) - (a - 1.0f) * cosw0 + two_sqrtA_alpha;
        a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cosw0);
        a2 = (a + 1.0f) - (a - 1.0f) * cosw0 - two_sqrtA_alpha;
    }
    return BiquadCoeffs{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// One sample applied through one biquad stage, Direct Form II Transposed.
float apply_biquad(const BiquadCoeffs& c, float x, BiquadState& s) noexcept {
    const float y = c.b0 * x + s.z1;
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}

float catalog_envelope_gain(uint32_t cursor, uint32_t frames, float target) noexcept {
    if (target <= 0.0f) return 0.0f;
    constexpr uint32_t kAttackFrames = kNativeSampleRate * 3 / 1000;
    constexpr uint32_t kReleaseFrames = kNativeSampleRate * 20 / 1000;
    constexpr float kEnvelopeFloor = 0.001f;
    if (cursor < kAttackFrames) {
        const float t = static_cast<float>(cursor) / static_cast<float>(kAttackFrames);
        return kEnvelopeFloor + (target - kEnvelopeFloor) * t;
    }
    if (frames > kNativeSampleRate * 40 / 1000 && cursor >= frames - kReleaseFrames) {
        const float t = static_cast<float>(cursor - (frames - kReleaseFrames)) /
                        static_cast<float>(kReleaseFrames);
        return target + (kEnvelopeFloor - target) * t;
    }
    return target;
}

bool valid_pcm(const std::vector<float>& pcm, float limit = 1.0f) {
    return !pcm.empty() && pcm.size() % kNativeChannels == 0 &&
           pcm.size() / kNativeChannels <= kMaxSampleFrames &&
           std::all_of(pcm.begin(), pcm.end(), [limit](float value) {
               return std::isfinite(value) && value >= -limit && value <= limit;
           });
}
}

ToneCoeffs compute_tone_coeffs(float x, float y) noexcept {
    ToneCoeffs out;
    out.bypass = (x == 0.0f && y == 0.0f);
    if (out.bypass) return out; // stage[] left default-identity but never read by mix()
    // Tilt: x drives a low-shelf ~250 Hz at -6*x dB plus a high-shelf
    // ~2.5 kHz at +6*x dB (Thock = boosted low/cut high at x=-1, Clack =
    // the reverse at x=+1). y drives an independent high-shelf ~7 kHz at
    // +6*y dB (Bright). See docs/011-tone-and-control-api.md.
    out.stage[0] = rbj_shelf(/*low=*/true, 250.0f, -6.0f * x, static_cast<float>(kNativeSampleRate));
    out.stage[1] = rbj_shelf(/*low=*/false, 2500.0f, 6.0f * x, static_cast<float>(kNativeSampleRate));
    out.stage[2] = rbj_shelf(/*low=*/false, 7000.0f, 6.0f * y, static_cast<float>(kNativeSampleRate));
    return out;
}

std::expected<Sample, std::string> load_wav_sample(const std::string& path) {
    SF_INFO info{};
    std::unique_ptr<SNDFILE, decltype(&sf_close)> file(
        sf_open(path.c_str(), SFM_READ, &info), &sf_close);
    if (!file) return std::unexpected(path + ": " + sf_strerror(nullptr));
    if ((info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV ||
        info.samplerate != static_cast<int>(kNativeSampleRate) ||
        (info.channels != 1 && info.channels != 2) ||
        info.frames <= 0 || info.frames > kMaxSampleFrames) {
        return std::unexpected(path + ": expected 48 kHz mono/stereo WAV, 1..480000 frames");
    }
    Sample sample;
    sample.pcm.resize(static_cast<std::size_t>(info.frames) * kNativeChannels);
    const auto read = sf_readf_float(file.get(), sample.pcm.data(), info.frames);
    if (read != info.frames || sf_error(file.get()) != SF_ERR_NO_ERROR)
        return std::unexpected(path + ": WAV decode failed or short read");
    if (info.channels == 1) {
        // Expand backwards inside the pre-sized stereo buffer.
        for (std::size_t i = static_cast<std::size_t>(info.frames); i-- > 0;) {
            const float value = sample.pcm[i];
            sample.pcm[2 * i] = value;
            sample.pcm[2 * i + 1] = value;
        }
    }
    if (!valid_pcm(sample.pcm))
        return std::unexpected(path + ": PCM must be finite and within [-1, 1]");
    return sample;
}

std::expected<void, std::string> validate_sound_bank(const SoundBank& bank) {
    float pcm_limit = 0.0f;
    switch (bank.playback_policy) {
        case PlaybackPolicy::Legacy: pcm_limit = 1.0f; break;
        case PlaybackPolicy::KeebyCatalogWav:
        case PlaybackPolicy::KeebyCatalogSprite: pcm_limit = 16.0f; break;
        default: return std::unexpected("SoundBank: unknown playback policy");
    }
    if (!std::isfinite(bank.press_gain) || bank.press_gain < 0.0f || bank.press_gain > 2.2f ||
        !std::isfinite(bank.release_gain) || bank.release_gain < 0.0f || bank.release_gain > 2.2f)
        return std::unexpected("SoundBank: playback gain out of range");
    for (const auto& sample : bank.samples)
        if (!valid_pcm(sample.pcm, pcm_limit)) return std::unexpected("SoundBank: invalid sample PCM");
    for (const auto& sound : bank.press)
        if (sound.count != 0 && static_cast<std::size_t>(sound.first) + sound.count > bank.samples.size())
            return std::unexpected("SoundBank: press range out of bounds");
    for (const auto& sound : bank.release)
        if (sound.count != 0 && static_cast<std::size_t>(sound.first) + sound.count > bank.samples.size())
            return std::unexpected("SoundBank: release range out of bounds");
    for (float pan : bank.pan)
        if (!std::isfinite(pan) || pan < -1.0f || pan > 1.0f)
            return std::unexpected("SoundBank: pan out of range");
    return {};
}

std::expected<SoundBank, std::string> load_default_bank(const std::string& directory) {
    SoundBank bank;
    // Appends `names` as one new contiguous range of variants and returns
    // the KeySound describing it.
    auto load_into = [&](std::initializer_list<const char*> names) -> std::expected<KeySound, std::string> {
        KeySound sound{static_cast<uint16_t>(bank.samples.size()), static_cast<uint8_t>(names.size())};
        for (const char* name : names) {
            auto sample = load_wav_sample(directory + "/" + name);
            if (!sample) return std::unexpected(sample.error());
            bank.samples.push_back(std::move(*sample));
        }
        return sound;
    };
    auto click = load_into({"click_down.wav", "click_down_low.wav", "click_down_high.wav"});
    if (!click) return std::unexpected(click.error());
    // click_up* is a legacy name (assets/README.md); it's a deeper PRESS
    // sound for the big stabilized keys, never release-triggered.
    auto deep = load_into({"click_up.wav", "click_up_low.wav", "click_up_high.wav"});
    if (!deep) return std::unexpected(deep.error());
    // Synthesized placeholder release samples (see generate_variants.py);
    // every key uses the same range until Step 2.7 brings recorded packs.
    auto release = load_into({"release.wav", "release_low.wav", "release_high.wav"});
    if (!release) return std::unexpected(release.error());

    for (std::size_t code = 1; code < kKeyCodeCount; ++code) {
        if (!is_supported_input_code(static_cast<uint16_t>(code))) continue;
        bank.press[code] = *click;
        bank.release[code] = *release;
        bank.pan[code] = key_pan(static_cast<uint16_t>(code));
    }
    for (uint16_t code : {KEY_SPACE, KEY_ENTER, KEY_KPENTER, KEY_BACKSPACE})
        bank.press[code] = *deep;

    if (auto ok = validate_sound_bank(bank); !ok) return std::unexpected(ok.error());
    return bank;
}

SampleMixer::SampleMixer(SoundBank bank, MixerVariation variation)
    : initial_bank_(std::make_unique<const SoundBank>(std::move(bank))), bank_(initial_bank_.get()),
      requested_bank_(bank_), active_bank_(bank_), variation_(variation), random_state_(variation.seed),
      initial_tone_(std::make_unique<const ToneCoeffs>()), tone_cache_(initial_tone_.get()),
      requested_tone_(tone_cache_), active_tone_(tone_cache_) {
    if (!std::isfinite(variation.min_gain) || !std::isfinite(variation.max_gain) ||
        variation.min_gain < kMinVoiceGain || variation.max_gain > kMaxVoiceGain ||
        variation.min_gain > variation.max_gain)
        throw std::invalid_argument("invalid mixer gain range");
    if (auto ok = validate_sound_bank(*initial_bank_); !ok) throw std::invalid_argument(ok.error());
    reset();
}

uint32_t SampleMixer::next_random() noexcept {
    // xorshift32: fixed 3 shifts + 3 XORs, unsigned arithmetic, no retries.
    random_state_ ^= random_state_ << 13;
    random_state_ ^= random_state_ >> 17;
    random_state_ ^= random_state_ << 5;
    return random_state_;
}

TriggerResult SampleMixer::trigger(uint16_t code, const KeySound& sound, uint8_t& last,
                                   KeyEventKind kind, uint32_t sequence) noexcept {
    if (sound.count == 0) return TriggerResult::Ignored;
    uint8_t variant = 0;
    const bool source_policy = bank_->playback_policy != PlaybackPolicy::Legacy;
    if (source_policy) {
        variant = static_cast<uint8_t>(sequence % sound.count);
    } else {
        if (sound.count > 1) {
            const uint32_t choice = next_random();
            if (last < sound.count) {
                // Select among all OTHER variants directly: no rejection loop.
                variant = static_cast<uint8_t>(choice % (sound.count - 1));
                if (variant >= last) ++variant;
            } else {
                variant = static_cast<uint8_t>(choice % sound.count);
            }
        }
    }
    float gain = 0.0f;
    if (source_policy) {
        gain = kind == KeyEventKind::Down ? bank_->press_gain : bank_->release_gain;
    } else {
        const float unit = static_cast<float>(next_random() >> 8) / 16777215.0f;
        gain = std::clamp(variation_.min_gain +
            unit * (variation_.max_gain - variation_.min_gain),
            variation_.min_gain, variation_.max_gain);
    }
    // Balance law: pan 0 matches the old single-gain behavior; a channel
    // only ever attenuates, never boosts past the trigger's own gain.
    const float width = stereo_width_.load(std::memory_order_relaxed);
    const float pan = std::clamp(bank_->pan[code] * width, -1.0f, 1.0f);
    const float pan_l = std::min(1.0f, 1.0f - pan);
    const float pan_r = std::min(1.0f, 1.0f + pan);
    const float gain_l = source_policy ? pan_l : gain * pan_l;
    const float gain_r = source_policy ? pan_r : gain * pan_r;
    for (auto& voice : voices_) {
        if (!voice.data) {
            const auto& pcm = bank_->samples[sound.first + variant].pcm;
            voice = {pcm.data(), static_cast<uint32_t>(pcm.size() / kNativeChannels), 0,
                     gain_l, gain_r, source_policy ? gain : 0.0f};
            last = variant;
            return TriggerResult::Started;
        }
    }
    // Drop newest sound only; the KeyEvent was still consumed and counted.
    return TriggerResult::PoolFull;
}

TriggerResult SampleMixer::handle_event(const KeyEvent& event) noexcept {
    if (event.kind == KeyEventKind::Repeat) return TriggerResult::Ignored;
    if (event.code == 0 || event.code >= kKeyCodeCount || !is_supported_input_code(event.code))
        return TriggerResult::Ignored;
    if (!enabled_.load(std::memory_order_relaxed)) return TriggerResult::Ignored;
    const uint32_t sequence = source_sequence_;
    if (bank_->playback_policy != PlaybackPolicy::Legacy && event.kind == KeyEventKind::Down) {
        if (is_pointer_button_code(event.code)) ++pointer_sequence_;
        else ++source_sequence_;
    }
    const uint32_t event_sequence = bank_->playback_policy != PlaybackPolicy::Legacy &&
            is_pointer_button_code(event.code)
        ? pointer_sequence_ - (event.kind == KeyEventKind::Down ? 1u : 0u) : sequence;
    if (event.kind == KeyEventKind::Down)
        return trigger(event.code, bank_->press[event.code], last_press_variant_[event.code], event.kind, event_sequence);
    return trigger(event.code, bank_->release[event.code], last_release_variant_[event.code], event.kind, event_sequence);
}

void SampleMixer::set_tone(float x, float y) {
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);
    tone_x_.store(x, std::memory_order_relaxed);
    tone_y_.store(y, std::memory_order_relaxed);
    // Heap-allocate off-RT (never on the RT path -- see set_tone()'s
    // declaration comment) so the published object is immutable for its
    // whole lifetime: the RT thread can safely hold a raw pointer to it for
    // an entire mix() callback with no risk of a concurrent writer mutating
    // the bytes it's reading (the bug an earlier fixed-2-slot double buffer
    // had -- see tone_cache_'s declaration comment).
    auto next = std::make_unique<const ToneCoeffs>(compute_tone_coeffs(x, y));
    const ToneCoeffs* raw = next.get();
    std::lock_guard<std::mutex> lock(tone_write_mutex_); // off-RT only; mix() never touches this mutex
    retired_tone_.push_back(std::move(next));
    requested_tone_.store(raw, std::memory_order_release);
    // Opportunistic, non-blocking GC. retired_tone_ is in publish order, and
    // mix() only ever advances tone_cache_ FORWARD through that same order
    // (never skips backward) -- so at any instant, whatever mix() is
    // currently using (or is about to switch to, mid-callback, just before
    // its own active_tone_ store catches up) is either the last pointer
    // active_tone_ was observed to hold, or something AFTER it in this
    // list. Erasing only entries strictly BEFORE that position is therefore
    // always safe, even if active_tone_ is stale by several publishes (an
    // earlier version of this GC instead protected only {active, raw} and
    // was TSAN-proven racy: an "in-flight" entry -- newer than the stale
    // active_tone_ a concurrent writer observed, older than raw -- is
    // exactly what a fast RT transition can be mid-way onto).
    const ToneCoeffs* active = active_tone_.load(std::memory_order_acquire);
    const auto active_it = std::find_if(retired_tone_.begin(), retired_tone_.end(),
                                         [&](const std::unique_ptr<const ToneCoeffs>& t) { return t.get() == active; });
    if (active_it != retired_tone_.end()) retired_tone_.erase(retired_tone_.begin(), active_it);
}

void SampleMixer::sync_bank() noexcept {
    const SoundBank* requested = requested_bank_.load(std::memory_order_acquire);
    if (requested == bank_) return;
    // Cut every in-flight voice: Voice::data points into the OLD bank's
    // PCM, which the caller may free as soon as active_bank_ below
    // reflects the switch -- see docs/008's swap-handshake RT-safety audit.
    voices_ = {};
    last_press_variant_.fill(0xff);
    last_release_variant_.fill(0xff);
    source_sequence_ = 0;
    pointer_sequence_ = 0;
    bank_ = requested;
    active_bank_.store(bank_, std::memory_order_release);
}

void SampleMixer::mix(float* output, uint32_t frames) noexcept {
    sync_bank(); // Step 2.7: pick up a pending pack swap, once per callback
    // Caller provides frames*2 floats; AudioBoundary caps frames at 4096.
    std::fill_n(output, static_cast<std::size_t>(frames) * kNativeChannels, 0.0f);
    // One load per callback, not per sample: master gain is a desktop-shell
    // volume knob, not per-trigger variation, so it need not track finer
    // than one mix() window. Multiplying it into each voice's own gain
    // leaves the Step 2.3 randomized per-trigger distribution untouched.
    const float master = 2.0f * master_gain_.load(std::memory_order_relaxed);
    const bool apply_catalog_envelope = bank_->playback_policy != PlaybackPolicy::Legacy;
    for (auto& voice : voices_) {
        if (!voice.data) continue;
        const auto count = std::min(frames, voice.frames - voice.cursor);
        if (!apply_catalog_envelope) {
            const float gain_l = voice.gain_l * master;
            const float gain_r = voice.gain_r * master;
            // Indices 0/1 below are left/right (kNativeChannels is fixed at 2).
            for (uint32_t f = 0; f < count; ++f) {
                output[static_cast<std::size_t>(f) * kNativeChannels + 0] +=
                    voice.data[(voice.cursor + f) * kNativeChannels + 0] * gain_l;
                output[static_cast<std::size_t>(f) * kNativeChannels + 1] +=
                    voice.data[(voice.cursor + f) * kNativeChannels + 1] * gain_r;
            }
        } else {
            for (uint32_t f = 0; f < count; ++f) {
                const uint32_t cursor = voice.cursor + f;
                const float envelope = catalog_envelope_gain(cursor, voice.frames, voice.source_gain);
                output[static_cast<std::size_t>(f) * kNativeChannels + 0] +=
                    voice.data[static_cast<std::size_t>(cursor) * kNativeChannels + 0] * envelope * voice.gain_l * master;
                output[static_cast<std::size_t>(f) * kNativeChannels + 1] +=
                    voice.data[static_cast<std::size_t>(cursor) * kNativeChannels + 1] * envelope * voice.gain_r * master;
            }
        }
        voice.cursor += count;
        if (voice.cursor == voice.frames) voice = {};
    }
    // Tone filter: one acquire load of the published pointer per callback
    // (not per sample) -- refreshing tone_cache_ exactly like sync_bank()
    // refreshes bank_ above -- then the RT thread's own persistent biquad
    // state carries across callbacks unchanged -- see docs/011. At (0, 0)
    // this is `bypass == true` and the loop below never runs, so every
    // pre-existing golden/sample-exact test above is untouched.
    if (const ToneCoeffs* requested = requested_tone_.load(std::memory_order_acquire); requested != tone_cache_) {
        tone_cache_ = requested;
        active_tone_.store(tone_cache_, std::memory_order_release);
    }
    const ToneCoeffs& tone = *tone_cache_;
    if (!tone.bypass) {
        for (uint32_t f = 0; f < frames; ++f) {
            float l = output[static_cast<std::size_t>(f) * kNativeChannels + 0];
            float r = output[static_cast<std::size_t>(f) * kNativeChannels + 1];
            for (std::size_t s = 0; s < kToneStageCount; ++s) {
                l = apply_biquad(tone.stage[s], l, tone_state_l_[s]);
                r = apply_biquad(tone.stage[s], r, tone_state_r_[s]);
            }
            output[static_cast<std::size_t>(f) * kNativeChannels + 0] = l;
            output[static_cast<std::size_t>(f) * kNativeChannels + 1] = r;
        }
    }
    // Clamp once after summation (and after the tone filter, which can
    // otherwise push a shelf-boosted signal past +-kOutputPeak); per-voice
    // clamping would distort cancellation.
    for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * kNativeChannels; ++i)
        output[i] = std::clamp(output[i], -kOutputPeak, kOutputPeak);
}

void SampleMixer::reset() noexcept {
    voices_ = {};
    last_press_variant_.fill(0xff);
    last_release_variant_.fill(0xff);
    tone_state_l_.fill(BiquadState{});
    tone_state_r_.fill(BiquadState{});
    source_sequence_ = 0;
    pointer_sequence_ = 0;
    random_state_ = variation_.seed == 0 ? 1 : variation_.seed;
}

} // namespace keeby
