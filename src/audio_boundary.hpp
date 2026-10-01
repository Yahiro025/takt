#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
    explicit AudioBoundary(SoundBank samples = {}, MixerVariation variation = {});
    ~AudioBoundary();
    AudioBoundary(const AudioBoundary&) = delete;
    AudioBoundary& operator=(const AudioBoundary&) = delete;

    // start/stop are serialized by the owning thread. Stop before restarting.
    // stop() also releases resources left by a failed start().
    std::expected<void, std::string> start(KeyEventTransport& transport);
    void stop();

    const AudioBoundaryCounters& counters() const { return counters_; }

    // Thin forwards to the mixer's atomic control state (see sample_mixer.hpp).
    // Callable from any thread, including a desktop-shell/tray thread; never
    // touches PipeWire, never blocks, never runs on the RT callback thread.
    void set_enabled(bool enabled) noexcept { mixer_.set_enabled(enabled); }
    bool enabled() const noexcept { return mixer_.enabled(); }
    void set_master_gain(float gain) noexcept { mixer_.set_master_gain(gain); }
    float master_gain() const noexcept { return mixer_.master_gain(); }
    void set_stereo_width(float width) noexcept { mixer_.set_stereo_width(width); }
    float stereo_width() const noexcept { return mixer_.stereo_width(); }
    // Not noexcept: off-RT, and heap-allocates its published coefficients
    // (see SampleMixer::set_tone), matching set_profile()'s allocating
    // precedent elsewhere in this class's control surface.
    void set_tone(float x, float y) { mixer_.set_tone(x, y); }
    std::pair<float, float> tone() const noexcept { return mixer_.tone(); }

    // Step 2.7: off-RT bank swap. AudioBoundary takes ownership of `bank`
    // and retains it (and any bank superseded but not yet acknowledged)
    // in retired_ until the RT thread proves it's done with it. Serialized
    // by the caller (see EngineController's lifecycle_mutex_, which also
    // serializes this against start()/stop()) -- the same "owner
    // serializes" contract start()/stop() already had between themselves.
    void swap_bank(std::unique_ptr<const SoundBank> bank);

    // Test-only introspection: how many banks swap_bank()/stop() are still
    // holding onto because they aren't yet provably safe to free (or, after
    // stop(), because they're the one the mixer now actually points to).
    std::size_t retired_bank_count() const noexcept { return retired_.size(); }

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
    // Step 2.7: banks superseded by swap_bank() but not yet provably unused
    // by the RT thread. A timed-out swap_bank() call frees NOTHING (an
    // unacknowledged request may still be intermediate, not yet even seen
    // by the RT thread -- see swap_bank()'s definition); pruning down to
    // just the acknowledged/active bank happens on a later successful swap,
    // or in stop() once the RT thread is confirmed gone (which also first
    // applies any still-pending request synchronously, so the mixer's own
    // now-active bank is never the one freed).
    std::vector<std::unique_ptr<const SoundBank>> retired_;
};

} // namespace keeby
