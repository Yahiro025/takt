#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "key_event.hpp"

namespace keeby {

inline constexpr uint32_t kNativeSampleRate = 48000;
inline constexpr uint32_t kNativeChannels = 2;
inline constexpr std::size_t kVoicePoolSize = 32;
inline constexpr uint32_t kMaxSampleFrames = kNativeSampleRate * 10;
inline constexpr uint32_t kMaxFramesPerCallback = 4096;
inline constexpr float kVoiceGain = 0.5f;
inline constexpr float kMinVoiceGain = 0.35f;
inline constexpr float kMaxVoiceGain = 0.65f;
inline constexpr float kOutputPeak = 0.8f;
// Keyboard codes, KEY_FN, and the three supported pointer buttons. Other
// extended codes remain reserved; SampleMixer::handle_event rejects them.
inline constexpr std::size_t kKeyCodeCount = KEY_FN + 1;

struct Sample {
    std::vector<float> pcm; // interleaved stereo; owned only outside the RT path
};

// A contiguous run of interchangeable variants inside SoundBank::samples.
// count == 0 means silent -- no press/release sound for that key.
struct KeySound {
    uint16_t first = 0;
    uint8_t count = 0;
};

enum class PlaybackPolicy : uint8_t { Legacy, KeebyCatalogWav, KeebyCatalogSprite };

// Generic, immutable sound-pack data model, keyed by supported raw evdev code.
// Pack-agnostic by design: see docs/007-step-2.6-sound-engine-v2.md.
struct SoundBank {
    std::vector<Sample> samples;
    std::array<KeySound, kKeyCodeCount> press{};
    std::array<KeySound, kKeyCodeCount> release{};
    std::array<float, kKeyCodeCount> pan{}; // [-1, 1], 0 = center; see key_layout.hpp
    PlaybackPolicy playback_policy = PlaybackPolicy::Legacy;
    float press_gain = kVoiceGain;
    float release_gain = kVoiceGain;
};

// Startup-only parameters, also used for exact fixed-gain regression tests.
// Allowed gain range is a subset of [0.35, 0.65]. No runtime configuration.
struct MixerVariation {
    uint32_t seed = 0x4b454542;
    float min_gain = kMinVoiceGain;
    float max_gain = kMaxVoiceGain;
};

// --- Tone filter (Keeby's Thock/Clack x Warm/Bright pad) ---
// RBJ-cookbook shelving biquad, normalized so a0 == 1 (a0 is not stored).
// Direct Form II Transposed: y = b0*x + z1; z1' = b1*x - a1*y + z2; z2' = b2*x - a2*y.
struct BiquadCoeffs {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};
struct BiquadState {
    float z1 = 0.0f, z2 = 0.0f;
};
inline constexpr std::size_t kToneStageCount = 3; // low-shelf(x), high-shelf(x), high-shelf(y)
struct ToneCoeffs {
    // true iff tone_x == tone_y == 0: mix() skips the filter entirely so
    // output is bit-identical to having no tone filter at all.
    bool bypass = true;
    std::array<BiquadCoeffs, kToneStageCount> stage{};
};
// Off-RT. x/y assumed already clamped to [-1, 1]. See
// docs/011-tone-and-control-api.md for the DSP design.
ToneCoeffs compute_tone_coeffs(float x, float y) noexcept;

// Startup only. Accepts WAV, 48 kHz, mono/stereo, 1..480000 frames.
// Mono is duplicated into stereo. Rejects non-finite/out-of-range PCM.
std::expected<Sample, std::string> load_wav_sample(const std::string& path);
// Built-in placeholder sound set; see assets/README.md.
std::expected<SoundBank, std::string> load_default_bank(const std::string& directory);
// Off-RT: range bounds, sample format, pan range. Exposed separately so
// tests and a future pack loader can check a bank before committing to it.
std::expected<void, std::string> validate_sound_bank(const SoundBank& bank);

enum class TriggerResult { Ignored, Started, PoolFull };

class SampleMixer {
public:
    // Throws std::invalid_argument (startup only) if the bank fails
    // validate_sound_bank() or variation's gain range is invalid.
    explicit SampleMixer(SoundBank bank = {}, MixerVariation variation = {});
    SampleMixer(const SampleMixer&) = delete;
    SampleMixer& operator=(const SampleMixer&) = delete;

    // Single audio-thread owner while running. No allocations or exceptions.
    TriggerResult handle_event(const KeyEvent& event) noexcept;
    void mix(float* stereo_output, uint32_t frames) noexcept;
    void reset() noexcept; // owner only, after the audio thread has stopped

    // Desktop-shell control state. Callable from any thread; each is a
    // single relaxed atomic op, safe to call while the audio thread is
    // concurrently reading it in handle_event()/mix(). Disabling suppresses
    // only NEW voice triggers (existing playing voices finish naturally);
    // it does not touch PRNG/variant-history state, matching how any other
    // Ignored event already behaves.
    void set_enabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    // 0.0 = silent, 1.0 = full volume. mix() applies a 2x amplitude boost
    // to all profiles before the tone filter and final output clamp.
    void set_master_gain(float gain) noexcept {
        master_gain_.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    float master_gain() const noexcept { return master_gain_.load(std::memory_order_relaxed); }
    // 0.0 = mono, 1.0 = the bank's own pan, 2.0 = doubled (still clamped to
    // +-1 per key). Clamped; read once per trigger, not per mix() sample.
    void set_stereo_width(float width) noexcept {
        stereo_width_.store(std::clamp(width, 0.0f, 2.0f), std::memory_order_relaxed);
    }
    float stereo_width() const noexcept { return stereo_width_.load(std::memory_order_relaxed); }

    // Thock/Clack (x) x Warm/Bright (y) tone pad, each clamped to [-1, 1].
    // Off-RT: computes new biquad coefficients, heap-allocates them (hence
    // not noexcept -- the only allocating control setter here, exactly like
    // EngineController::set_profile's off-RT bank load), and publishes a
    // pointer to them -- the RT side (mix()) loads that pointer once per
    // callback and never recomputes or copies a coefficient itself.
    // tone_x_/tone_y_ are separate plain atomics purely so tone() can report
    // back the last requested (clamped) values without dereferencing a
    // possibly-retired ToneCoeffs. A concurrent writer race (two threads
    // calling set_tone at once) is serialized by tone_write_mutex_, off-RT
    // only -- mix() never touches that mutex.
    void set_tone(float x, float y);
    std::pair<float, float> tone() const noexcept {
        return {tone_x_.load(std::memory_order_relaxed), tone_y_.load(std::memory_order_relaxed)};
    }

    // --- Step 2.7: externally-owned, hot-swappable banks ---
    // Off-RT. Publishes `bank` as the bank the RT thread should switch to
    // on its next callback; caller retains ownership (SampleMixer only
    // ever reads through this pointer, never frees it). Returns
    // immediately -- poll active_bank() to know when the switch has taken
    // effect. See docs/008-step-2.7-sound-packs.md for the full handshake
    // and its RT-safety audit.
    void request_bank(const SoundBank* bank) noexcept {
        requested_bank_.store(bank, std::memory_order_release);
    }
    // Off-RT: the most recently requested bank (may not be active yet).
    const SoundBank* requested_bank() const noexcept {
        return requested_bank_.load(std::memory_order_acquire);
    }
    // Off-RT: the bank the RT thread has actually switched to and is
    // currently mixing from. Becomes == the last request_bank() argument
    // once the switch has taken effect.
    const SoundBank* active_bank() const noexcept {
        return active_bank_.load(std::memory_order_acquire);
    }
    // Applies a pending request_bank() synchronously instead of waiting
    // for mix() to notice it. Owner-only, and only when the audio thread
    // is confirmed not running (before start() or after stop()) -- same
    // contract as reset(). Safe there specifically because nothing else
    // can be concurrently touching voices_/history/bank_.
    void sync_bank() noexcept;

private:
    struct Voice {
        const float* data = nullptr;
        uint32_t frames = 0;
        uint32_t cursor = 0;
        float gain_l = 0.0f; // legacy per-channel gain or catalog pan multiplier
        float gain_r = 0.0f;
        float source_gain = 0.0f; // catalog phase gain before pan/master controls
    };
    static_assert(std::is_trivially_destructible_v<Voice>);

    // Step 2.7: initial_bank_ is the only bank SampleMixer itself ever
    // owns -- the one given to its constructor by value, kept alive for
    // this object's whole lifetime. Every later bank (via request_bank)
    // is owned by the caller (e.g. AudioBoundary's retired list), never
    // by SampleMixer -- see docs/008-step-2.7-sound-packs.md.
    // bank_ is the RT thread's own cache of "the bank I'm using right
    // now", refreshed only by sync_bank(); reading/writing it needs no
    // synchronization because handle_event()/mix() (the only other
    // readers) run on that same single RT thread. requested_bank_/
    // active_bank_ are the cross-thread control<->RT handshake.
    std::unique_ptr<const SoundBank> initial_bank_;
    const SoundBank* bank_;
    std::atomic<const SoundBank*> requested_bank_;
    std::atomic<const SoundBank*> active_bank_;
    static_assert(std::atomic<const SoundBank*>::is_always_lock_free,
                  "SampleMixer's bank pointers must be lock-free: read/written from the RT callback");
    const MixerVariation variation_;
    uint32_t random_state_;
    // Per-key (not per-range): keys sharing a range still get independent
    // no-repeat history. 0xff = no prior variant.
    std::array<uint8_t, kKeyCodeCount> last_press_variant_{};
    std::array<uint8_t, kKeyCodeCount> last_release_variant_{};
    std::array<Voice, kVoicePoolSize> voices_{};
    std::atomic<bool> enabled_{true};
    std::atomic<float> master_gain_{1.0f};
    std::atomic<float> stereo_width_{1.0f};
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "SampleMixer::enabled_ must be lock-free: read from the RT callback");
    static_assert(std::atomic<float>::is_always_lock_free,
                  "SampleMixer::master_gain_ must be lock-free: read from the RT callback");
    static_assert(std::atomic<float>::is_always_lock_free,
                  "SampleMixer::stereo_width_ must be lock-free: read from the RT callback");

    // Tone filter coefficients: same pointer-publish-and-retire handshake as
    // bank_/requested_bank_/active_bank_ above, not a fixed 2-slot double
    // buffer -- an earlier 2-slot version was TSAN-proven racy (mix() holds
    // a reference to one slot for the whole callback, up to 4096 frames;
    // a writer publishing two updates during that window can lap back
    // around to the very slot still being read). Since ToneCoeffs objects
    // are immutable once published (each set_tone() call heap-allocates a
    // fresh one, off-RT), any object that is neither the RT thread's
    // currently active one nor the just-published one is provably
    // unreachable and safe to free -- identical reasoning to
    // docs/008-step-2.7-sound-packs.md's bank-swap handshake, just with no
    // bounded RT-ack wait (nothing here needs the switch to take effect
    // before the setter returns).
    std::unique_ptr<const ToneCoeffs> initial_tone_; // permanent bypass default; kept for object lifetime
    const ToneCoeffs* tone_cache_;                   // RT thread's own cache; touched only by mix()
    std::atomic<const ToneCoeffs*> requested_tone_;
    std::atomic<const ToneCoeffs*> active_tone_;
    static_assert(std::atomic<const ToneCoeffs*>::is_always_lock_free,
                  "SampleMixer's tone pointers must be lock-free: read/written from the RT callback");
    std::atomic<float> tone_x_{0.0f};
    std::atomic<float> tone_y_{0.0f};
    std::mutex tone_write_mutex_;                              // off-RT only: serializes set_tone() writers
    std::vector<std::unique_ptr<const ToneCoeffs>> retired_tone_; // off-RT only, guarded by tone_write_mutex_
    // RT-owned filter state (persists across callbacks, not reset on a mere
    // coefficient change -- see docs/011). Cleared in reset().
    std::array<BiquadState, kToneStageCount> tone_state_l_{};
    std::array<BiquadState, kToneStageCount> tone_state_r_{};

    uint32_t next_random() noexcept;
    TriggerResult trigger(uint16_t code, const KeySound& sound, uint8_t& last,
                          KeyEventKind kind, uint32_t sequence) noexcept;
    uint32_t source_sequence_ = 0;
    uint32_t pointer_sequence_ = 0;
};

} // namespace keeby
