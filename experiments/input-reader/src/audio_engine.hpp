#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "key_event.hpp"
#include "ring_buffer.hpp"
#include "sample.hpp"

struct pw_thread_loop;
struct pw_context;
struct pw_stream;

namespace keeby {

inline constexpr uint32_t kNativeSampleRate = 48000;
inline constexpr uint32_t kNativeChannels = 2;
inline constexpr size_t kVoicePoolSize = 32;

struct LatencyStats {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> sum_ns{0};
    std::atomic<uint64_t> max_ns{0};
};

// Owns the PipeWire stream (which runs its own real-time thread) and a
// fixed-size voice pool. add_sample()/map_key() must be called before
// start(); they allocate and are not RT-safe. Everything that runs on the
// PipeWire thread (process()) is allocation-free, lock-free, and does no
// I/O.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool start();
    void stop();

    // Returns the sample's slot index, or -1 if the (small, fixed) pool
    // of sample slots is full. Setup-time only, not RT-safe.
    int add_sample(Sample sample);

    // Setup-time only. `down_sample` triggers on Down and Repeat,
    // `up_sample` (-1 = silent) triggers on Up.
    void map_key(uint16_t keycode, int down_sample, int up_sample);

    EventQueue& event_queue() { return queue_; }
    const LatencyStats& latency_stats() const { return latency_; }

    // Public only because it must be assignable into the C pw_stream_events
    // callback table from outside the class. Not part of the intended API.
    static void on_process(void* userdata);

private:
    struct Voice {
        const float* data = nullptr;
        uint32_t frame_count = 0;
        uint32_t cursor = 0;
        bool active = false;
    };

    struct KeyMapping {
        uint16_t keycode = 0;
        int down_sample = -1;
        int up_sample = -1;
    };

    void process(); // RT-safe body, runs on the PipeWire thread
    void trigger(int sample_index); // called only from process()

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_stream* stream_ = nullptr;

    std::vector<Sample> samples_;
    std::vector<KeyMapping> mappings_;
    std::array<Voice, kVoicePoolSize> voices_{};
    size_t next_steal_ = 0;

    EventQueue queue_;
    LatencyStats latency_;
    bool started_ = false;
};

} // namespace keeby
