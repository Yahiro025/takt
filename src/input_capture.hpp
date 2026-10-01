#pragma once

#include <atomic>
#include <expected>
#include <optional>
#include <string>
#include <thread>

#include <sys/types.h> // pid_t

#include "event_transport.hpp"
#include "key_event.hpp"
#include "pointer_event.hpp"

namespace keeby {

// Scans /dev/input/event0..31 for the first device exposing a full
// physical-keyboard key set (KEY_A, KEY_Z, KEY_SPACE, KEY_ENTER). Returns
// nullopt if none found. Read-only probing, never grabs or modifies
// anything. Single-keyboard heuristic only (see key_event.hpp).
//
// Kept as a standalone utility (e.g. for a user diagnosing device paths
// while they still have `input`-group access) but no longer used by the
// production start() path below: since Step 2.5B, only the privileged
// keeby-inputd helper opens /dev/input/* at all — see input_wire.hpp and
// docs/006-step-2.5-security-permissions.md. Calling this from an account
// that is not in `input` will simply find nothing (every open() attempt
// fails with EACCES), the same as any other unprivileged enumeration.
std::optional<std::string> discover_keyboard_device();

// Owns the unprivileged parent side of the keeby-inputd privilege
// boundary: spawns the helper (inheriting one end of a private
// socketpair(AF_UNIX, SOCK_SEQPACKET, ...) — never a pathname socket other
// processes could connect to), waits synchronously for its one-time Ready
// message, then runs a background thread that decodes wire::Message
// datagrams into KeyEvents and pushes them into the transport. This
// process itself never opens /dev/input/* and never needs `input`-group
// membership; see docs/006-step-2.5-security-permissions.md.
//
// start() synchronously waits for the helper to either become ready or
// fail, so failures are surfaced immediately rather than discovered later
// on a background thread. Only once that succeeds does the forwarding
// thread start running.
class InputCapture {
public:
    ~InputCapture();

    // Not copyable or movable: owns a running thread, a child process, and
    // an open fd.
    InputCapture() = default;
    InputCapture(const InputCapture&) = delete;
    InputCapture& operator=(const InputCapture&) = delete;

    // start/stop are serialized by the owning thread. Stop before restarting.
    // `device_path` empty means "let the helper auto-discover a keyboard";
    // non-empty is forwarded to the helper, which validates it itself (see
    // inputd_main.cpp) — this process never validates or opens it.
    // Returns an error message (not thrown, not swallowed) on failure to
    // launch the helper or have it report readiness.
    std::expected<void, std::string> start(std::string device_path, KeyEventTransport& transport);

    // Requests the forwarding thread stop, terminates and reaps the
    // helper process, and closes the IPC socket. Safe to call multiple
    // times, and called automatically by the destructor. Never leaves a
    // zombie or orphaned helper process behind.
    void stop();

    // False once the helper has gone away (device disappeared, crashed, or
    // was killed) — as opposed to a normal stop() shutdown, which leaves
    // this true. Distinguishes "device/helper disappeared" from "not
    // running" for a caller that wants to surface it. Safe to poll from
    // any thread; not used by, and has no effect on, the audio callback.
    bool connected() const noexcept { return connected_.load(std::memory_order_relaxed); }

    // Wires (or unwires, with nullptr) a second transport that the
    // forwarding thread also pushes every non-repeat keyboard KeyEvent into --
    // KEEBY's own tap for the on-screen visualizer (see
    // VisualizerService::transport() and docs/013-visualizer.md). Safe to
    // call from any thread at any time, including while the forwarding
    // thread is running: the pointer is read once per event with acquire
    // ordering, so a caller wiring it up races at worst a single event, and
    // `transport` must outlive either this call being undone or this
    // object's destruction. Never touches AudioBoundary's transport or the
    // RT path.
    void set_visualizer_transport(KeyEventTransport* transport) noexcept {
        viz_transport_.store(transport, std::memory_order_release);
    }

    // Wires (or unwires, with nullptr) a second tap the forwarding thread
    // pushes every decoded PointerMotion sample into -- see
    // EngineController::set_visualizer_enabled/set_visualizer_position,
    // which wire this only while the visualizer is enabled AND its position
    // is follow-cursor (otherwise motion is decoded and dropped, never
    // pushed anywhere). Same relaxed-ownership contract as
    // set_visualizer_transport above: safe from any thread, at any time.
    void set_visualizer_motion_transport(PointerMotionTransport* transport) noexcept {
        viz_motion_transport_.store(transport, std::memory_order_release);
    }

private:
    std::jthread thread_;
    std::atomic<bool> connected_{true};
    std::atomic<KeyEventTransport*> viz_transport_{nullptr};
    std::atomic<PointerMotionTransport*> viz_motion_transport_{nullptr};
    int ipc_fd_ = -1;
    pid_t helper_pid_ = -1;
};

} // namespace keeby
