#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <string>

#include "event_transport.hpp"
#include "key_event.hpp"
#include "sample_mixer.hpp"

struct pw_thread_loop;
struct pw_context;
struct pw_stream;

namespace keeby {

// Observable proof that press/release/repeat events reached the audio
// real-time thread. All members are updated only from the PipeWire
// process() callback (single writer) and may be read from any other
// thread at any time (plain atomic loads, no synchronization protocol
// beyond that). Mixer counters distinguish voice-pool rejections from
// transport drops; snapshots of different counters are not one transaction.
struct AudioBoundaryCounters {
    std::atomic<uint64_t> voices_started{0};
    std::atomic<uint64_t> voices_dropped{0};
    std::atomic<uint64_t> presses{0};
    std::atomic<uint64_t> releases{0};
    std::atomic<uint64_t> repeats{0};
    std::atomic<uint64_t> consumed_total{0};
    std::atomic<uint16_t> last_code{0};
    // Incremented when a single callback invocation drains
    // kMaxEventsPerCallback events and the queue may still hold more
    // (deferred to the next callback, never dropped for this reason).
    // Expected to stay at 0 under any realistic keyboard input rate —
    // see kMaxEventsPerCallback's rationale below.
    std::atomic<uint64_t> budget_exhausted{0};

    // Verified lock-free on this codebase's sole validated production
    // target (Linux x86-64, GCC/Clang, libstdc++/libc++): these must
    // never fall back to a mutex, since every one is touched from the
    // PipeWire real-time callback.
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "AudioBoundaryCounters' uint64_t counters must be lock-free on the target platform");
    static_assert(std::atomic<uint16_t>::is_always_lock_free,
                  "AudioBoundaryCounters::last_code must be lock-free on the target platform");
};

// Fixed event-work budget, independent of producer activity and PipeWire
// quantum: at most 32 small copies and counter updates per callback.
// A full 255-event queue takes eight callbacks without new arrivals.
// This conservative stage-2.1 limit defers remaining events without dropping
// them. It bounds operations, not wall-clock time on a preemptible OS.
inline constexpr std::size_t kMaxEventsPerCallback = 32;

// Owns the PipeWire stream (which runs its own real-time thread). start()
// connects to PipeWire and begins running the stream's process()
// callback, which drains `transport` (which must outlive this object)
// non-blockingly every cycle. Nothing in process() allocates, locks,
// touches the filesystem, or logs — see audio_boundary.cpp for the
// itemized audit.
class AudioBoundary {
public:
    explicit AudioBoundary(SampleBank samples = {});
    ~AudioBoundary();
    AudioBoundary(const AudioBoundary&) = delete;
    AudioBoundary& operator=(const AudioBoundary&) = delete;

    // start/stop are serialized by the owning thread. Stop before restarting.
    // stop() also releases resources left by a failed start().
    std::expected<void, std::string> start(KeyEventTransport& transport);
    void stop();

    const AudioBoundaryCounters& counters() const { return counters_; }

    // Public only because it must be assignable into the C
    // pw_stream_events callback table from outside the class.
    static void on_process(void* userdata) noexcept;

private:
    void process() noexcept; // RT-safe body, runs on the PipeWire thread

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_stream* stream_ = nullptr;
    KeyEventTransport* transport_ = nullptr; // non-owning
    SampleMixer mixer_; // owns immutable samples; destroyed only after stop()
    AudioBoundaryCounters counters_;
    bool started_ = false;
};

} // namespace keeby
