#include "audio_engine.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace keeby {
namespace {

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

constexpr pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = &AudioEngine::on_process,
};

} // namespace

AudioEngine::AudioEngine() { pw_init(nullptr, nullptr); }

AudioEngine::~AudioEngine() {
    stop();
    pw_deinit();
}

int AudioEngine::add_sample(Sample sample) {
    if (samples_.size() >= 64) return -1; // small fixed cap; this is a spike
    samples_.push_back(std::move(sample));
    return static_cast<int>(samples_.size() - 1);
}

void AudioEngine::map_key(uint16_t keycode, int down_sample, int up_sample) {
    mappings_.push_back(KeyMapping{keycode, down_sample, up_sample});
}

bool AudioEngine::start() {
    loop_ = pw_thread_loop_new("keeby-audio", nullptr);
    if (!loop_) return false;

    pw_thread_loop_lock(loop_);

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        pw_thread_loop_unlock(loop_);
        return false;
    }

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Game",
        PW_KEY_NODE_LATENCY, "128/48000",
        nullptr);

    stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "keeby-spike", props,
                                    &kStreamEvents, this);
    if (!stream_) {
        pw_thread_loop_unlock(loop_);
        return false;
    }

    uint8_t pod_buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = kNativeChannels;
    info.rate = kNativeSampleRate;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    int res = pw_stream_connect(
        stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                      PW_STREAM_FLAG_RT_PROCESS),
        params, 1);

    if (res < 0) {
        std::fprintf(stderr, "keeby: pw_stream_connect failed: %s\n", spa_strerror(res));
        pw_thread_loop_unlock(loop_);
        return false;
    }

    pw_thread_loop_unlock(loop_);

    if (pw_thread_loop_start(loop_) < 0) {
        return false;
    }

    started_ = true;
    return true;
}

void AudioEngine::stop() {
    if (!started_) return;
    if (loop_) {
        pw_thread_loop_lock(loop_);
        if (stream_) {
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
    }
    if (context_) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    started_ = false;
}

void AudioEngine::trigger(int sample_index) {
    if (sample_index < 0 || static_cast<size_t>(sample_index) >= samples_.size()) return;
    const Sample& s = samples_[static_cast<size_t>(sample_index)];
    if (s.frame_count == 0) return;

    size_t slot = voices_.size();
    for (size_t i = 0; i < voices_.size(); ++i) {
        if (!voices_[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == voices_.size()) {
        slot = next_steal_;
        next_steal_ = (next_steal_ + 1) % voices_.size();
    }

    Voice& v = voices_[slot];
    v.data = s.frames.data();
    v.frame_count = s.frame_count;
    v.cursor = 0;
    v.active = true;
}

void AudioEngine::on_process(void* userdata) {
    static_cast<AudioEngine*>(userdata)->process();
}

void AudioEngine::process() {
    pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (!b) return;

    spa_buffer* buf = b->buffer;
    float* dst = static_cast<float*>(buf->datas[0].data);
    if (!dst) {
        pw_stream_queue_buffer(stream_, b);
        return;
    }

    uint32_t n_frames = buf->datas[0].maxsize / (sizeof(float) * kNativeChannels);
    if (b->requested != 0 && static_cast<uint32_t>(b->requested) < n_frames) {
        n_frames = static_cast<uint32_t>(b->requested);
    }

    // Drain every pending key event. Bounded by the ring buffer's fixed
    // capacity, so this never blocks and always terminates quickly.
    KeyEvent ev;
    while (queue_.try_pop(ev)) {
        const uint64_t received_ns = now_ns();
        for (const KeyMapping& m : mappings_) {
            if (m.keycode != ev.code) continue;
            int sample = -1;
            if (ev.kind == KeyEventKind::Down || ev.kind == KeyEventKind::Repeat) {
                sample = m.down_sample;
            } else if (ev.kind == KeyEventKind::Up) {
                sample = m.up_sample;
            }
            if (sample >= 0) {
                trigger(sample);
                const uint64_t lat = received_ns > ev.ts_ns ? received_ns - ev.ts_ns : 0;
                latency_.count.fetch_add(1, std::memory_order_relaxed);
                latency_.sum_ns.fetch_add(lat, std::memory_order_relaxed);
                uint64_t prev_max = latency_.max_ns.load(std::memory_order_relaxed);
                while (lat > prev_max &&
                       !latency_.max_ns.compare_exchange_weak(prev_max, lat, std::memory_order_relaxed)) {
                }
            }
            break;
        }
    }

    // Mix all active voices. O(active voices), fixed pool, no allocation,
    // no locks, no I/O.
    std::memset(dst, 0, static_cast<size_t>(n_frames) * kNativeChannels * sizeof(float));
    for (Voice& v : voices_) {
        if (!v.active) continue;
        uint32_t frames_to_mix = std::min(n_frames, v.frame_count - v.cursor);
        for (uint32_t f = 0; f < frames_to_mix; ++f) {
            for (uint32_t c = 0; c < kNativeChannels; ++c) {
                float& out = dst[f * kNativeChannels + c];
                out += v.data[(v.cursor + f) * kNativeChannels + c];
                out = std::clamp(out, -1.0f, 1.0f);
            }
        }
        v.cursor += frames_to_mix;
        if (v.cursor >= v.frame_count) v.active = false;
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * kNativeChannels;
    buf->datas[0].chunk->size = n_frames * kNativeChannels * sizeof(float);

    pw_stream_queue_buffer(stream_, b);
}

} // namespace keeby
