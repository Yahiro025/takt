#include "visualizer_service.hpp"
#include "input_capture.hpp"
#include "input_wire.hpp"
#include "pointer_event.hpp"
#include "visualizer_wire.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <unistd.h>

using namespace keeby;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

fs::path make_output_path(const char* tag) {
    fs::path p = fs::temp_directory_path() /
                 (std::string("keeby_viz_test_") + tag + "_" + std::to_string(::getpid()) + ".log");
    std::ofstream(p, std::ios::trunc); // create/truncate
    return p;
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t line_count(const std::string& s) {
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n'));
}

// Bounded busy-wait: the fake helper is a real forked/exec'd process
// writing to a file, so content may lag slightly behind a send() call.
std::string wait_for_lines(const fs::path& p, std::size_t min_lines, std::chrono::seconds timeout) {
    const auto deadline = Clock::now() + timeout;
    std::string content;
    while (Clock::now() < deadline) {
        content = read_all(p);
        if (line_count(content) >= min_lines) return content;
    }
    return content;
}

bool wait_until(bool (*predicate)(const void*), const void* ctx, std::chrono::seconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (predicate(ctx)) return true;
    }
    return false;
}

bool child_dead(const void* ctx) { return !static_cast<const VisualizerService*>(ctx)->child_alive(); }
bool child_live(const void* ctx) { return static_cast<const VisualizerService*>(ctx)->child_alive(); }

void use_fake_visualizer(const char* mode, const fs::path& output) {
    setenv(viz::kHelperPathOverrideEnv, FAKE_VISUALIZER_PATH, 1);
    setenv("FAKE_VISUALIZER_MODE", mode, 1);
    setenv("FAKE_VISUALIZER_OUTPUT", output.c_str(), 1);
}

void use_fake_inputd(const char* mode) {
    setenv(wire::kHelperPathOverrideEnv, FAKE_INPUTD_PATH, 1);
    if (mode) setenv("FAKE_INPUTD_MODE", mode, 1);
    else unsetenv("FAKE_INPUTD_MODE");
}

} // namespace

int main() {
    // 1. Spawn sends config first, then key down/up are forwarded and a
    // repeat pushed directly into the transport is never sent.
    {
        auto out = make_output_path("spawn_config_first");
        use_fake_visualizer("normal", out);

        VisualizerService viz;
        viz.set_position(viz::Position::TopRight);
        viz.set_dismiss_ms(2000);
        viz.set_enabled(true);
        assert(viz.enabled());

        std::string content = wait_for_lines(out, 1, std::chrono::seconds(2));
        assert(content == "config top-right 2000 speed=10\n");

        viz.transport().try_push(KeyEvent{30, KeyEventKind::Down, 0});
        viz.transport().try_push(KeyEvent{30, KeyEventKind::Repeat, 0}); // must never be sent
        viz.transport().try_push(KeyEvent{30, KeyEventKind::Up, 0});

        content = wait_for_lines(out, 3, std::chrono::seconds(2));
        assert(content == "config top-right 2000 speed=10\nkey down 30\nkey up 30\n");

        viz.set_enabled(false);
    }

    // 2. End-to-end: InputCapture's forwarding thread taps every non-repeat
    // KeyEvent into the visualizer's transport too. fake_inputd's "normal"
    // mode sends press/repeat/release of KEY_A(30).
    {
        auto out = make_output_path("input_capture_tap");
        use_fake_visualizer("normal", out);
        use_fake_inputd(nullptr);

        VisualizerService viz;
        viz.set_enabled(true);
        assert(wait_for_lines(out, 1, std::chrono::seconds(2)) == "config follow-cursor 1000 speed=10\n");

        KeyEventTransport transport;
        InputCapture capture;
        capture.set_visualizer_transport(&viz.transport()); // wire before start(): no event may race ahead
        assert(capture.start("", transport));

        std::string content = wait_for_lines(out, 3, std::chrono::seconds(2));
        assert(content == "config follow-cursor 1000 speed=10\nkey down 30\nkey up 30\n");

        capture.stop();
        viz.set_enabled(false);
    }

    // 3. Non-blocking: a child that never reads must never make the sender
    // block, and excess events are dropped, not queued forever.
    {
        auto out = make_output_path("no_read");
        use_fake_visualizer("no_read", out);

        VisualizerService viz;
        viz.set_enabled(true);
        // No config line will ever appear (the fake never reads fd 3), but
        // give the fork/exec a moment before hammering the socket.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const auto began = Clock::now();
        for (int i = 0; i < 2000; ++i) viz.transport().try_push(KeyEvent{30, KeyEventKind::Down, 0});
        // Give the sender thread a moment to actually attempt the sends
        // (and hit EAGAIN) before asserting on elapsed wall time.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        assert(Clock::now() - began < std::chrono::seconds(2)); // never blocked
        assert(viz.child_alive()); // not reading isn't death

        const auto term_began = Clock::now();
        viz.set_enabled(false);
        assert(Clock::now() - term_began < std::chrono::seconds(2)); // bounded terminate even though it never read
    }

    // 4. Child death is detected, reaped (no zombie left for the caller to
    // manage), and a respawn happens on the next enable/config change --
    // never automatically.
    {
        auto out = make_output_path("die_after_2");
        use_fake_visualizer("die_after_2", out);

        VisualizerService viz;
        viz.set_enabled(true);
        assert(wait_for_lines(out, 1, std::chrono::seconds(2)).substr(0, 6) == "config");

        viz.transport().try_push(KeyEvent{30, KeyEventKind::Down, 0});
        viz.transport().try_push(KeyEvent{30, KeyEventKind::Up, 0});
        wait_for_lines(out, 3, std::chrono::seconds(2)); // config + the two events the fake reads before dying

        assert(wait_until(child_dead, &viz, std::chrono::seconds(3)));
        assert(!viz.child_alive()); // reaped, not crashed
        assert(viz.enabled()); // still the requested state -- respawn is deferred, not forgotten

        // Nothing should have respawned it yet on its own.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        assert(!viz.child_alive());

        // The next enable (here: re-asserting the same "on" state, as
        // main.cpp's toggle path would) respawns it.
        setenv("FAKE_VISUALIZER_MODE", "normal", 1); // survive this time
        viz.set_enabled(true);
        assert(wait_until(child_live, &viz, std::chrono::seconds(2)));

        std::string content = wait_for_lines(out, 4, std::chrono::seconds(2));
        assert(line_count(content) >= 4); // a second "config ..." line was appended after respawn

        viz.set_enabled(false);
    }

    // 4b. Respawn also happens on a config change while the previous child
    // is dead, not only on set_enabled(true).
    {
        auto out = make_output_path("respawn_on_config_change");
        use_fake_visualizer("die_after_2", out);

        VisualizerService viz;
        viz.set_enabled(true);
        wait_for_lines(out, 1, std::chrono::seconds(2));
        viz.transport().try_push(KeyEvent{30, KeyEventKind::Down, 0});
        viz.transport().try_push(KeyEvent{30, KeyEventKind::Up, 0});
        assert(wait_until(child_dead, &viz, std::chrono::seconds(3)));

        setenv("FAKE_VISUALIZER_MODE", "normal", 1);
        viz.set_dismiss_ms(1500); // a config change, not an enable
        assert(wait_until(child_live, &viz, std::chrono::seconds(2)));
        // 4 lines total: the first config, the two key events logged before
        // the die_after_2 child exited, and the respawned child's config.
        std::string content = wait_for_lines(out, 4, std::chrono::seconds(2));
        assert(content.find("config follow-cursor 1500 speed=10\n") != std::string::npos);

        viz.set_enabled(false);
    }

    // 5. A clean disable terminates and reaps the child within a bounded
    // time, and is idempotent.
    {
        auto out = make_output_path("clean_disable");
        use_fake_visualizer("normal", out);

        VisualizerService viz;
        viz.set_enabled(true);
        assert(wait_until(child_live, &viz, std::chrono::seconds(2)));

        const auto began = Clock::now();
        viz.set_enabled(false);
        assert(Clock::now() - began < std::chrono::seconds(2));
        assert(!viz.child_alive());

        viz.set_enabled(false); // idempotent
        assert(!viz.child_alive());
    }

    // 6. Follow-speed is carried in Config (as the 0.1x-step reserved byte)
    // and re-sent on a live change.
    {
        auto out = make_output_path("follow_speed_config");
        use_fake_visualizer("normal", out);

        VisualizerService viz;
        viz.set_follow_speed(2.5); // step 25
        viz.set_enabled(true);
        assert(wait_for_lines(out, 1, std::chrono::seconds(2)) == "config follow-cursor 1000 speed=25\n");
        assert(viz.follow_speed() == 2.5);

        viz.set_follow_speed(0.05); // clamped up to the 0.1x floor (step 1)
        std::string content = wait_for_lines(out, 2, std::chrono::seconds(2));
        assert(content.find("config follow-cursor 1000 speed=1\n") != std::string::npos);
        assert(viz.follow_speed() == 0.1);

        viz.set_enabled(false);
    }

    // 7. Pointer motion is forwarded per event -- one Motion message per
    // sample, never summed into a per-batch total -- because niri clamps
    // its real cursor position after every single motion event, not once
    // per batch (nonlinear at an edge; see cursor_estimator.hpp). Also
    // never blocks even against a non-reading child (test 8 below).
    {
        auto out = make_output_path("motion_per_event");
        use_fake_visualizer("normal", out);

        VisualizerService viz;
        viz.set_enabled(true);
        assert(wait_for_lines(out, 1, std::chrono::seconds(2)).substr(0, 6) == "config");

        for (int i = 0; i < 50; ++i) {
            viz.motion_transport().try_push(PointerMotionEvent{5, -2, PointerDeviceKind::Mouse, 0});
        }

        // Exactly one "motion mouse 5 -2" line per sample: proof of
        // one-message-per-event forwarding, not a coalesced sum.
        std::string content = wait_for_lines(out, 51, std::chrono::seconds(2));
        int motion_lines = 0;
        std::istringstream lines(content);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.compare(0, 6, "motion") != 0) continue;
            ++motion_lines;
            assert(line == "motion mouse 5 -2");
        }
        assert(motion_lines == 50);

        viz.set_enabled(false);
    }

    // 8. A non-reading child never makes motion forwarding block either.
    {
        auto out = make_output_path("motion_no_read");
        use_fake_visualizer("no_read", out);

        VisualizerService viz;
        viz.set_enabled(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const auto began = Clock::now();
        for (int i = 0; i < 2000; ++i) {
            viz.motion_transport().try_push(PointerMotionEvent{1, 1, PointerDeviceKind::Touchpad, 0});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        assert(Clock::now() - began < std::chrono::seconds(2));
        assert(viz.child_alive());

        viz.set_enabled(false);
    }

    // 9. Mixed mouse+touchpad motion is forwarded in original arrival
    // order, one Motion message per event -- never coalesced per kind or
    // per batch (see test 7's rationale: a summed batch can clamp to a
    // different place than niri's own per-event clamp would).
    {
        auto out = make_output_path("motion_mixed_kind");
        use_fake_visualizer("normal", out);

        VisualizerService viz;
        // Enqueue BEFORE set_enabled(true): all three samples sit queued
        // when the sender thread starts, so its first drain sees them in
        // this exact order.
        viz.motion_transport().try_push(PointerMotionEvent{10, 0, PointerDeviceKind::Mouse, 0});
        viz.motion_transport().try_push(PointerMotionEvent{0, 20, PointerDeviceKind::Touchpad, 0});
        viz.motion_transport().try_push(PointerMotionEvent{10, 0, PointerDeviceKind::Mouse, 0});
        viz.set_enabled(true);
        assert(wait_for_lines(out, 1, std::chrono::seconds(2)).substr(0, 6) == "config");

        std::string content = wait_for_lines(out, 4, std::chrono::seconds(2));
        std::istringstream lines(content);
        std::string line;
        std::getline(lines, line);
        assert(line.substr(0, 6) == "config");
        std::getline(lines, line);
        assert(line == "motion mouse 10 0");
        std::getline(lines, line);
        assert(line == "motion touchpad 0 20");
        std::getline(lines, line);
        assert(line == "motion mouse 10 0");

        viz.set_enabled(false);
    }

    // 10. Pure-logic test for MotionCarry (visualizer_service.hpp): a
    // sample that couldn't be sent (simulated EAGAIN -- a real
    // stalled-then-resumed reader would need a fake_keeby_visualizer stall
    // mode that doesn't exist yet) is never lost -- it stays carried and
    // the next add() lands on top of it, until something clears it (a
    // successful send, in the real sender_loop). Per-kind independence
    // matters: mouse and touchpad must never bleed into each other's carry.
    {
        MotionCarry c;
        c.add(5, -2); // "send" fails: stays carried
        assert(c.dx == 5 && c.dy == -2);
        c.add(3, 1); // another sample arrives before a retry: adds on top, nothing dropped
        assert(c.dx == 8 && c.dy == -1);
        c.clear(); // a send finally succeeds for the full carried total
        assert(c.dx == 0 && c.dy == 0);

        MotionCarry mouse, touch;
        mouse.add(10, 0);
        touch.add(0, 10);
        assert(mouse.dx == 10 && mouse.dy == 0);
        assert(touch.dx == 0 && touch.dy == 10);

        std::printf("test_motion_carry_never_drops: OK\n");
    }

    unsetenv(viz::kHelperPathOverrideEnv);
    unsetenv("FAKE_VISUALIZER_MODE");
    unsetenv("FAKE_VISUALIZER_OUTPUT");
    unsetenv(wire::kHelperPathOverrideEnv);
    unsetenv("FAKE_INPUTD_MODE");

    std::puts("visualizer_service_test: OK (config-first spawn, InputCapture tap incl. repeat filtering, "
              "non-blocking drop against a non-reading child, child-death reap with deferred respawn on "
              "enable/config-change, bounded/idempotent disable, follow-speed config, pointer-motion "
              "per-event ordering, non-blocking forwarding, motion-carry backpressure handling)");
}
