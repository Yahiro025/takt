#include "sample.hpp"

#include <sndfile.h>

#include <cstdio>

namespace keeby {

bool load_wav_sample(const std::string& path, uint32_t native_rate,
                      uint32_t native_channels, Sample& out) {
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    if (!file) {
        std::fprintf(stderr, "keeby: failed to open sample \"%s\": %s\n",
                     path.c_str(), sf_strerror(nullptr));
        return false;
    }

    if (static_cast<uint32_t>(info.samplerate) != native_rate) {
        std::fprintf(stderr,
            "keeby: sample \"%s\" is %d Hz, engine expects %u Hz; this spike "
            "does not resample, re-encode the asset\n",
            path.c_str(), info.samplerate, native_rate);
        sf_close(file);
        return false;
    }

    std::vector<float> raw(static_cast<size_t>(info.frames) * static_cast<size_t>(info.channels));
    sf_count_t read = sf_readf_float(file, raw.data(), info.frames);
    sf_close(file);
    if (read != info.frames) {
        std::fprintf(stderr, "keeby: short read on sample \"%s\" (%lld/%lld frames)\n",
                     path.c_str(), (long long)read, (long long)info.frames);
        return false;
    }

    out.frame_count = static_cast<uint32_t>(info.frames);
    out.channels = native_channels;
    out.frames.assign(static_cast<size_t>(out.frame_count) * native_channels, 0.0f);

    if (static_cast<uint32_t>(info.channels) == native_channels) {
        out.frames = std::move(raw);
    } else if (info.channels == 1 && native_channels == 2) {
        for (uint32_t i = 0; i < out.frame_count; ++i) {
            out.frames[i * 2 + 0] = raw[i];
            out.frames[i * 2 + 1] = raw[i];
        }
    } else {
        std::fprintf(stderr,
            "keeby: sample \"%s\" has %d channels, engine expects %u "
            "(only mono->stereo upmix is supported here)\n",
            path.c_str(), info.channels, native_channels);
        return false;
    }
    return true;
}

} // namespace keeby
