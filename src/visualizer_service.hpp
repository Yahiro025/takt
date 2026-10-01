#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include <sys/types.h> // pid_t

#include "event_transport.hpp"
#include "pointer_event.hpp"
#include "visualizer_wire.hpp"

namespace keeby {

// Per-kind pending delta the sender couldn't forward yet (send() hit
// EAGAIN: the socket is full because the child isn't draining it fast
// enough). The next attempt for that same device kind adds its own event
// on top rather than the dropped sample vanishing -- summing only ever
// happens under this backpressure, never as the normal per-event path
// (see sender_loop in visualizer_service.cpp). Pure/socket-free so it's
// directly unit-testable (tests/visualizer_service_test.cpp).
struct MotionCarry {
    int64_t dx = 0, dy = 0;
    void add(int16_t event_dx, int16_t event_dy) {
        dx += event_dx;
        dy += event_dy;
    }
    void clear() {
        dx = 0;
        dy = 0;
    }
};

// Owns the unprivileged parent side of KEEBY's own private boundary to
// keeby-visualizer (the floating on-screen keyboard overlay): spawns it as
// a child process over a private socketpair(AF_UNIX, SOCK_SEQPACKET |
// SOCK_CLOEXEC) dup2'd to fd 3 (fork+execv, argv only, no shell -- same
// shape as InputCapture's boundary to keeby-inputd; see
// docs/006-step-2.5-security-permissions.md and docs/013-visualizer.md).
//
// Security: only key events (kind/code) and config (position/dismiss_ms)
// are ever written to the child -- nothing else, and never over D-Bus or a
// named socket.
//
// Threading: enable/disable/position/dismiss setters serialize on an
// internal mutex and are meant to be called from one control thread (the
// D-Bus event-loop thread, or main() before it starts) -- same "owner
// serializes" contract as InputCapture/EngineController. transport() is
// safe to hand to InputCapture's forwarding thread as a second SPSC
// producer target: the dedicated sender thread started here is transport()'s
// sole consumer.
class VisualizerService {
public:
    VisualizerService();
    ~VisualizerService();
    VisualizerService(const VisualizerService&) = delete;
    VisualizerService& operator=(const VisualizerService&) = delete;

    // Spawns (if not already running) or terminates+reaps the child.
    // Idempotent. A spawn failure (helper missing, fork/socketpair failure)
    // is logged to stderr and non-fatal; enabled() still reports the
    // *requested* state so a later set_position()/set_dismiss_ms() call
    // retries the spawn (see the class comment on respawn timing below).
    void set_enabled(bool enabled);
    bool enabled() const noexcept { return wanted_enabled_.load(std::memory_order_relaxed); }

    // Position/dismiss_ms are retained even while disabled. A live child is
    // re-sent a config message immediately. If the child had died (or never
    // spawned) while enabled() is true, this also triggers a respawn --
    // "respawn only on the next enable or config change", never automatic.
    void set_position(viz::Position position);
    viz::Position position() const noexcept { return position_.load(std::memory_order_relaxed); }
    void set_dismiss_ms(uint32_t ms); // clamps to [250, 5000]
    uint16_t dismiss_ms() const noexcept { return dismiss_ms_.load(std::memory_order_relaxed); }

    // Follow-cursor speed multiplier, [0.1, 4.0], default 1.0 -- stored on
    // the wire as steps of 0.1x in Config's `reserved` byte (see
    // visualizer_wire.hpp's follow_speed_to_step/from_step). Same
    // retained-while-disabled / re-sent-to-a-live-child contract as
    // position/dismiss_ms above.
    void set_follow_speed(double multiplier);
    double follow_speed() const noexcept { return viz::follow_speed_from_step(follow_speed_step_.load(std::memory_order_relaxed)); }

    // False once a spawned child has been observed to exit or error and has
    // been reaped -- distinct from "never enabled" (also false) and from a
    // clean set_enabled(false) (also false). True only while a live child is
    // believed reachable.
    bool child_alive() const noexcept { return child_alive_.load(std::memory_order_relaxed); }

    // The transport InputCapture's forwarding thread should also push
    // KeyEvents into while the visualizer is enabled -- see
    // InputCapture::set_visualizer_transport(). This object is the queue's
    // sole consumer (the sender thread started by set_enabled(true)).
    KeyEventTransport& transport() noexcept { return transport_; }

    // Second tap, alongside transport() above: InputCapture's forwarding
    // thread pushes decoded PointerMotion samples in here while wired (see
    // InputCapture::set_visualizer_motion_transport,
    // EngineController::set_visualizer_enabled/set_visualizer_position).
    // This object is likewise its sole consumer -- the sender thread
    // coalesces (sums) whatever is pending into at most two Motion messages
    // per loop iteration (one summed per device kind) rather than sending
    // one message per sample, since the overlay only needs the latest
    // cursor estimate, not a full replay.
    PointerMotionTransport& motion_transport() noexcept { return motion_transport_; }

private:
    void spawn_locked();        // must hold mutex_
    void terminate_locked();    // must hold mutex_
    void send_config_locked();  // must hold mutex_; no-op if not connected
    void sender_loop(std::stop_token stoken, pid_t pid, int fd);

    std::mutex mutex_; // serializes enable/disable/position/dismiss/spawn/terminate
    std::atomic<bool> wanted_enabled_{false};
    std::atomic<viz::Position> position_{viz::kDefaultPosition};
    std::atomic<uint16_t> dismiss_ms_{viz::kDefaultDismissMs};
    std::atomic<uint8_t> follow_speed_step_{viz::kDefaultFollowSpeedStep};
    std::atomic<bool> child_alive_{false};

    KeyEventTransport transport_;
    PointerMotionTransport motion_transport_;
    std::jthread sender_thread_;
    int ipc_fd_ = -1;
    pid_t helper_pid_ = -1;
};

} // namespace keeby
