#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace keeby {

// A sound fully decoded into RAM as interleaved float32 PCM, already
// converted to the audio engine's native format so the real-time callback
// never has to convert, resample, or touch disk.
struct Sample {
    std::vector<float> frames; // interleaved, frame_count * channels
    uint32_t frame_count = 0;
    uint32_t channels = 0;
};

// Loads `path` via libsndfile. All I/O happens here, at startup, never
// during playback. Fails loudly (returns false, logs to stderr) rather
// than silently mis-playing audio if the file's sample rate doesn't match
// `native_rate` — this spike does not resample.
bool load_wav_sample(const std::string& path, uint32_t native_rate,
                      uint32_t native_channels, Sample& out);

} // namespace keeby
