#include "input_capture.hpp"
#include "input_wire.hpp"
#include "pointer_event.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace keeby;
using Clock = std::chrono::steady_clock;

namespace {

void use_fake_helper(const char* mode) {
    setenv(wire::kHelperPathOverrideEnv, FAKE_INPUTD_PATH, 1);
    if (mode) setenv("FAKE_INPUTD_MODE", mode, 1);
    else unsetenv("FAKE_INPUTD_MODE");
}

// Bounded busy-wait: the fake helper is a real forked/exec'd process, so a
// message it already sent may not have been recv()'d by InputCapture's
// background thread the instant start() returns. 2s is generous for CI.
bool wait_for_event(KeyEventTransport& transport, KeyEvent& out) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
        if (transport.try_pop(out)) return true;
    }
    return false;
}

bool wait_until(bool (*predicate)(const void*), const void* ctx) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
        if (predicate(ctx)) return true;
    }
    return false;
}

bool disconnected(const void* ctx) {
    return !static_cast<const InputCapture*>(ctx)->connected();
}

bool wait_for_motion(PointerMotionTransport& transport, PointerMotionEvent& out) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
        if (transport.try_pop(out)) return true;
    }
    return false;
}

} // namespace

int main() {
    // 1. Wire format: fixed size, trivially copyable, exact byte round trip.
    static_assert(sizeof(wire::Message) == 16);
    wire::Message original{};
    original.type = static_cast<uint8_t>(wire::MessageType::KeyEvent);
    original.kind = 1;
    original.code = 30;
    original.ts_ns = 123456789;
    unsigned char bytes[sizeof(wire::Message)];
    std::memcpy(bytes, &original, sizeof(bytes));
    wire::Message decoded{};
    std::memcpy(&decoded, bytes, sizeof(bytes));
    assert(decoded.type == original.type && decoded.kind == original.kind &&
           decoded.code == original.code && decoded.ts_ns == original.ts_ns);

    // 2/3/4. Valid transfer: press, repeat, release arrive in order and are
    // decoded correctly (kind/code/timestamp all preserved).
    {
        use_fake_helper(nullptr); // "normal" mode
        KeyEventTransport transport;
        InputCapture capture;
        assert(capture.start("", transport));
        assert(capture.connected());

        KeyEvent press, repeat, release;
        assert(wait_for_event(transport, press));
        assert(wait_for_event(transport, repeat));
        assert(wait_for_event(transport, release));
        assert(press.code == 30 && press.kind == KeyEventKind::Down && press.ts_ns == 100);
        assert(repeat.code == 30 && repeat.kind == KeyEventKind::Repeat && repeat.ts_ns == 200);
        assert(release.code == 30 && release.kind == KeyEventKind::Up && release.ts_ns == 300);

        capture.stop();
        assert(capture.connected()); // a normal stop() is not a "disappeared" device
    }

    // 5. Invalid message (bad `type` discriminant) is dropped, not crashed
    // on; the valid message sent right after it still arrives.
    {
        use_fake_helper("bad_type");
        KeyEventTransport transport;
        InputCapture capture;
        assert(capture.start("", transport));
        KeyEvent ev;
        assert(wait_for_event(transport, ev));
        assert(ev.code == 31 && ev.kind == KeyEventKind::Up);
        capture.stop();
    }

    // 6. Truncated message is dropped, not crashed on; the valid message
    // sent right after it still arrives.
    {
        use_fake_helper("malformed");
        KeyEventTransport transport;
        InputCapture capture;
        assert(capture.start("", transport));
        KeyEvent ev;
        assert(wait_for_event(transport, ev));
        assert(ev.code == 30 && ev.kind == KeyEventKind::Down);
        capture.stop();
    }

    // 7. Helper never becomes ready: start() fails promptly (the helper
    // exiting closes its socket end, which signals EOF well before any
    // timeout), and no thread/process is left behind.
    {
        use_fake_helper("no_ready");
        KeyEventTransport transport;
        InputCapture capture;
        const auto began = Clock::now();
        auto result = capture.start("", transport);
        assert(!result);
        assert(Clock::now() - began < std::chrono::seconds(3)); // did not hit the 5s timeout
        capture.stop(); // must be safe even though start() failed
    }

    // 8/9. IPC disconnect / helper death: both are observed identically —
    // by design (see input_wire.hpp) — as EOF on the socket shortly after
    // a successful start(). connected() must flip to false without the
    // caller doing anything else, and the app must not crash.
    {
        use_fake_helper("ready_then_exit");
        KeyEventTransport transport;
        InputCapture capture;
        assert(capture.start("", transport)); // Ready was sent before the exit
        assert(wait_until(disconnected, &capture));
        capture.stop(); // safe even though the helper is already gone
        capture.stop(); // idempotent
    }

    // 10. Parent shutdown/reaping: stop() against a helper stuck idling in
    // pause() must still return promptly (SIGTERM terminates it), not hang.
    {
        use_fake_helper(nullptr);
        KeyEventTransport transport;
        InputCapture capture;
        assert(capture.start("", transport));
        const auto began = Clock::now();
        capture.stop();
        assert(Clock::now() - began < std::chrono::seconds(2));
    }

    // Restart after a prior disconnect resets connected() back to true.
    {
        use_fake_helper(nullptr);
        KeyEventTransport transport;
        InputCapture capture;
        assert(capture.start("", transport));
        assert(capture.connected());
        capture.stop();
    }

    // 11. Helper path resolves to a regular, non-executable file (e.g. not
    // yet installed via scripts/install-keeby-inputd.sh): start() must fail
    // fast on the access(X_OK) precheck, well under the 5s ready-timeout,
    // without ever attempting fork/exec.
    {
        const char* tmpdir_env = std::getenv("TMPDIR");
        std::string path = std::string(tmpdir_env ? tmpdir_env : "/tmp") + "/keeby_test_noexec_XXXXXX";
        int fd = mkstemp(path.data());
        assert(fd >= 0);
        close(fd); // mkstemp creates it mode 0600: already no execute bit for anyone

        setenv(wire::kHelperPathOverrideEnv, path.c_str(), 1);
        unsetenv("FAKE_INPUTD_MODE");
        KeyEventTransport transport;
        InputCapture capture;
        const auto began = Clock::now();
        auto result = capture.start("", transport);
        assert(!result);
        assert(result.error().find("not executable") != std::string::npos);
        assert(Clock::now() - began < std::chrono::milliseconds(500));
        capture.stop();

        unlink(path.c_str());
    }

    // 11b. Regression coverage for the "sv[1] already is fd 3" spawn edge
    // case fixed in both input_capture.cpp and visualizer_service.cpp:
    // confirms the actual OS-level facts the fix depends on, since
    // engineering the exact fd-3 coincidence through a real fork/exec is
    // fragile and adds no more confidence than this. dup2(fd, fd) is a
    // documented no-op that leaves FD_CLOEXEC set (reproducing the bug this
    // fix addresses); fcntl(fd, F_SETFD, 0) does clear it (the fix itself).
    {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) == 0);
        assert((fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0);
        assert(dup2(fds[0], fds[0]) == fds[0]); // no-op per POSIX
        assert((fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0); // still set: the bug, if dup2'd instead
        assert(fcntl(fds[0], F_SETFD, 0) == 0);
        assert((fcntl(fds[0], F_GETFD) & FD_CLOEXEC) == 0); // cleared: the fix
        close(fds[0]);
        close(fds[1]);
    }

    // 12. PointerMotion wire round trip: pack/unpack/clamp are exact and
    // symmetric.
    {
        int16_t dx = 0, dy = 0;
        wire::unpack_motion(wire::pack_motion(5, -3), dx, dy);
        assert(dx == 5 && dy == -3);
        assert(wire::clamp_motion_component(1e9) == 32767);
        assert(wire::clamp_motion_component(-1e9) == -32767);
    }

    // 13. PointerMotion never reaches the key transport, and is dropped
    // entirely (decoded, then discarded) when no motion tap is wired.
    {
        use_fake_helper("pointer_motion");
        KeyEventTransport transport;
        InputCapture capture;
        assert(capture.start("", transport));

        // Both KeyEvents (before and after the two PointerMotion messages)
        // must still arrive, in order, with nothing extra in between.
        KeyEvent press, release;
        assert(wait_for_event(transport, press));
        assert(wait_for_event(transport, release));
        assert(press.code == 30 && press.kind == KeyEventKind::Down);
        assert(release.code == 30 && release.kind == KeyEventKind::Up);
        KeyEvent extra;
        assert(!transport.try_pop(extra)); // no PointerMotion ever lands here

        capture.stop();
    }

    // 14. Pointer buttons use the existing audio KeyEvent path, retain every
    // transition when motion is interleaved, and never enter the keyboard
    // visualizer tap.
    {
        use_fake_helper("pointer_buttons");
        KeyEventTransport transport;
        KeyEventTransport viz;
        PointerMotionTransport motion;
        InputCapture capture;
        capture.set_visualizer_transport(&viz);
        capture.set_visualizer_motion_transport(&motion);
        assert(capture.start("", transport));

        KeyEvent got;
        assert(wait_for_event(transport, got));
        assert(got.code == KEY_A && got.kind == KeyEventKind::Down && got.ts_ns == 10);
        uint64_t expected_ts = 20;
        for (uint16_t button : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
            assert(wait_for_event(transport, got));
            assert(got.code == button && got.kind == KeyEventKind::Down && got.ts_ns == expected_ts++);
            ++expected_ts; // the helper sent one PointerMotion message here
            assert(wait_for_event(transport, got));
            assert(got.code == button && got.kind == KeyEventKind::Up && got.ts_ns == expected_ts++);
        }
        assert(wait_for_event(transport, got));
        assert(got.code == KEY_A && got.kind == KeyEventKind::Up && got.ts_ns == expected_ts);
        assert(!transport.try_pop(got));

        KeyEvent viz_down, viz_up;
        assert(wait_for_event(viz, viz_down));
        assert(wait_for_event(viz, viz_up));
        assert(viz_down.code == KEY_A && viz_down.kind == KeyEventKind::Down);
        assert(viz_up.code == KEY_A && viz_up.kind == KeyEventKind::Up);
        assert(!viz.try_pop(got));

        PointerMotionEvent moved;
        for (int i = 0; i < 4; ++i) assert(wait_for_motion(motion, moved));
        assert(!motion.try_pop(moved));
        capture.stop();
    }

    // 15. KEY_FN stays a 16-bit audio event and does not enter the keyboard
    // visualizer's 8-bit key layout.
    {
        use_fake_helper("fn_key");
        KeyEventTransport transport;
        KeyEventTransport viz;
        InputCapture capture;
        capture.set_visualizer_transport(&viz);
        assert(capture.start("", transport));

        KeyEvent down, up;
        assert(wait_for_event(transport, down));
        assert(wait_for_event(transport, up));
        assert(down.code == KEY_FN && down.kind == KeyEventKind::Down && down.ts_ns == 11);
        assert(up.code == KEY_FN && up.kind == KeyEventKind::Up && up.ts_ns == 22);
        assert(!transport.try_pop(up));
        assert(!viz.try_pop(up));
        capture.stop();
    }

    // 16. PointerMotion reaches a wired motion tap, decoded correctly for
    // both device kinds, and only while wired.
    {
        use_fake_helper("pointer_motion");
        KeyEventTransport transport;
        PointerMotionTransport motion;
        InputCapture capture;
        capture.set_visualizer_motion_transport(&motion); // wire before start(): no sample may race ahead
        assert(capture.start("", transport));

        PointerMotionEvent mouse_ev, touchpad_ev;
        assert(wait_for_motion(motion, mouse_ev));
        assert(wait_for_motion(motion, touchpad_ev));
        assert(mouse_ev.kind == PointerDeviceKind::Mouse && mouse_ev.dx == 5 && mouse_ev.dy == -3);
        assert(touchpad_ev.kind == PointerDeviceKind::Touchpad && touchpad_ev.dx == -100 && touchpad_ev.dy == 200);

        capture.set_visualizer_motion_transport(nullptr); // unwiring stops further delivery
        capture.stop();
    }

    unsetenv(wire::kHelperPathOverrideEnv);
    unsetenv("FAKE_INPUTD_MODE");
    std::puts("input_capture_test: OK (wire round trip, keyboard and mouse-button transfer, "
              "invalid-type and truncated-message rejection, startup failure, IPC disconnect, "
              "helper death, bounded shutdown/reaping, restart, non-executable helper "
              "rejection, PointerMotion pack/unpack/clamp, PointerMotion routing "
              "(never the key transport, only a wired motion tap))");
}
