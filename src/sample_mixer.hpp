#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <type_traits>
#include <vector>

#include "key_event.hpp"

namespace keeby {

inline constexpr uint32_t kNativeSampleRate = 48000;
inline constexpr uint32_t kNativeChannels = 2;
inline constexpr std::size_t kVoicePoolSize = 32;
inline constexpr uint32_t kMaxSampleFrames = kNativeSampleRate * 10;
inline constexpr uint32_t kMaxFramesPerCallback = 4096;
inline constexpr float kVoiceGain = 0.5f;
inline constexpr float kOutputPeak = 0.8f;

struct Sample {
    std::vector<float> pcm; // interleaved stereo; owned only outside the RT path
};
using SampleBank = std::array<Sample, 2>;

// Startup only. Accepts WAV, 48 kHz, mono/stereo, 1..480000 frames.
// Mono is duplicated into stereo. Rejects non-finite/out-of-range PCM.
std::expected<Sample, std::string> load_wav_sample(const std::string& path);

enum class TriggerResult { Ignored, Started, PoolFull };

class SampleMixer {
public:
    // Owns and validates the bank before any callback starts. Empty slots
    // are silent. Throws invalid_argument for invalid prepared PCM.
    explicit SampleMixer(SampleBank samples = {});
    SampleMixer(const SampleMixer&) = delete;
    SampleMixer& operator=(const SampleMixer&) = delete;

    // Single audio-thread owner while running. No allocations or exceptions.
    TriggerResult handle_event(const KeyEvent& event) noexcept;
    void mix(float* stereo_output, uint32_t frames) noexcept;
    void reset() noexcept; // owner only, after the audio thread has stopped

private:
    struct Voice {
        const float* data = nullptr;
        uint32_t frames = 0;
        uint32_t cursor = 0;
    };
    static_assert(std::is_trivially_destructible_v<Voice>);
    const SampleBank samples_;
    std::array<Voice, kVoicePoolSize> voices_{};
};

} // namespace keeby
