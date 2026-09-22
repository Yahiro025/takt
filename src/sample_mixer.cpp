#include "sample_mixer.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>
#include <sndfile.h>
#include <linux/input-event-codes.h>

namespace keeby {
namespace {
bool valid_pcm(const std::vector<float>& pcm) {
    return pcm.size() % kNativeChannels == 0 &&
           pcm.size() / kNativeChannels <= kMaxSampleFrames &&
           std::all_of(pcm.begin(), pcm.end(), [](float value) {
               return std::isfinite(value) && value >= -1.0f && value <= 1.0f;
           });
}
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

SampleMixer::SampleMixer(SampleBank samples) : samples_(std::move(samples)) {
    for (const auto& sample : samples_)
        if (!valid_pcm(sample.pcm)) throw std::invalid_argument("invalid prepared sample PCM");
}

TriggerResult SampleMixer::handle_event(const KeyEvent& event) noexcept {
    if (event.kind != KeyEventKind::Down) return TriggerResult::Ignored;
    std::size_t index;
    switch (event.code) {
        case KEY_A: case KEY_S: case KEY_D: index = 0; break;
        case KEY_SPACE: case KEY_ENTER: index = 1; break;
        default: return TriggerResult::Ignored;
    }
    const auto& pcm = samples_[index].pcm;
    if (pcm.empty()) return TriggerResult::Ignored;
    for (auto& voice : voices_) {
        if (!voice.data) {
            voice = {pcm.data(), static_cast<uint32_t>(pcm.size() / kNativeChannels), 0};
            return TriggerResult::Started;
        }
    }
    // Drop newest sound only; the KeyEvent was still consumed and counted.
    return TriggerResult::PoolFull;
}

void SampleMixer::mix(float* output, uint32_t frames) noexcept {
    // Caller provides frames*2 floats; AudioBoundary caps frames at 4096.
    std::fill_n(output, static_cast<std::size_t>(frames) * kNativeChannels, 0.0f);
    for (auto& voice : voices_) {
        if (!voice.data) continue;
        const auto count = std::min(frames, voice.frames - voice.cursor);
        for (uint32_t f = 0; f < count; ++f) {
            for (uint32_t c = 0; c < kNativeChannels; ++c) {
                output[static_cast<std::size_t>(f) * kNativeChannels + c] +=
                    voice.data[(voice.cursor + f) * kNativeChannels + c] * kVoiceGain;
            }
        }
        voice.cursor += count;
        if (voice.cursor == voice.frames) voice = {};
    }
    // Clamp once after summation; per-voice clamping would distort cancellation.
    for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * kNativeChannels; ++i)
        output[i] = std::clamp(output[i], -kOutputPeak, kOutputPeak);
}

void SampleMixer::reset() noexcept { voices_ = {}; }

} // namespace keeby
