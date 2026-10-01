#include "visualizer_service.hpp"

#include "input_wire.hpp" // wire::clamp_motion_component -- a carried, backpressure-summed delta can exceed int16

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef KEEBY_VISUALIZER_BINDIR_DEFAULT
#define KEEBY_VISUALIZER_BINDIR_DEFAULT ""
#endif

namespace keeby {
namespace {

// Test-only override (KEEBY_VISUALIZER_PATH) first, else next to this
// executable, else the compiled-in installed bin dir. Never a $PATH
// search. Mirrors input_capture.cpp's resolve_helper_path().
std::string resolve_helper_path() {
    if (const char* override_path = std::getenv(viz::kHelperPathOverrideEnv)) return override_path;

    char self[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        std::string dir(self);
        const auto slash = dir.find_last_of('/');
        if (slash != std::string::npos) return dir.substr(0, slash + 1) + viz::kHelperName;
    }
    const std::string bindir = KEEBY_VISUALIZER_BINDIR_DEFAULT;
    if (!bindir.empty()) return bindir + "/" + viz::kHelperName;
    return viz::kHelperName;
}

// SIGTERM, brief bounded wait, escalate to SIGKILL, always reap. Identical
// shape to input_capture.cpp's terminate_and_reap(): only ever called from
// the owning thread while holding VisualizerService::mutex_, never the RT
// path.
void terminate_and_reap(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) { // up to ~1s
        const pid_t r = waitpid(pid, nullptr, WNOHANG);
        if (r == pid || r < 0) return;
        struct timespec ts{0, 50'000'000};
        nanosleep(&ts, nullptr);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

} // namespace

VisualizerService::VisualizerService() = default;

VisualizerService::~VisualizerService() {
    std::lock_guard<std::mutex> lock(mutex_);
    terminate_locked();
}

void VisualizerService::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_enabled_.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        if (!child_alive_.load(std::memory_order_relaxed)) spawn_locked();
    } else {
        terminate_locked();
    }
}

void VisualizerService::set_position(viz::Position position) {
    std::lock_guard<std::mutex> lock(mutex_);
    position_.store(position, std::memory_order_relaxed);
    if (!wanted_enabled_.load(std::memory_order_relaxed)) return;
    if (!child_alive_.load(std::memory_order_relaxed)) { spawn_locked(); return; }
    send_config_locked();
}

void VisualizerService::set_dismiss_ms(uint32_t ms) {
    const uint16_t clamped = static_cast<uint16_t>(std::clamp<uint32_t>(ms, viz::kMinDismissMs, viz::kMaxDismissMs));
    std::lock_guard<std::mutex> lock(mutex_);
    dismiss_ms_.store(clamped, std::memory_order_relaxed);
    if (!wanted_enabled_.load(std::memory_order_relaxed)) return;
    if (!child_alive_.load(std::memory_order_relaxed)) { spawn_locked(); return; }
    send_config_locked();
}

void VisualizerService::set_follow_speed(double multiplier) {
    const uint8_t step = viz::follow_speed_to_step(multiplier);
    std::lock_guard<std::mutex> lock(mutex_);
    follow_speed_step_.store(step, std::memory_order_relaxed);
    if (!wanted_enabled_.load(std::memory_order_relaxed)) return;
    if (!child_alive_.load(std::memory_order_relaxed)) { spawn_locked(); return; }
    send_config_locked();
}

void VisualizerService::send_config_locked() {
    if (ipc_fd_ < 0) return;
    viz::VizMessage m{};
    m.type = static_cast<uint8_t>(viz::MessageType::Config);
    m.position = static_cast<uint8_t>(position_.load(std::memory_order_relaxed));
    m.dismiss_ms = dismiss_ms_.load(std::memory_order_relaxed);
    m.reserved = follow_speed_step_.load(std::memory_order_relaxed);
    if (::send(ipc_fd_, &m, sizeof m, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
        std::fprintf(stderr, "keeby: visualizer config send failed: %s\n", std::strerror(errno));
    }
}

void VisualizerService::spawn_locked() {
    // Clean up a previous dead child's leftover thread/fd first -- this is
    // exactly the deferred "respawn only on the next enable or config
    // change" point: nothing does this automatically when death is detected.
    if (sender_thread_.joinable()) {
        sender_thread_.request_stop();
        sender_thread_.join();
    }
    if (ipc_fd_ >= 0) {
        ::close(ipc_fd_);
        ipc_fd_ = -1;
    }
    helper_pid_ = -1;
    child_alive_.store(false, std::memory_order_relaxed);

    const std::string helper_path = resolve_helper_path();
    if (::access(helper_path.c_str(), X_OK) != 0) {
        std::fprintf(stderr,
                      "keeby: keeby-visualizer not found/executable at %s (%s); visualizer will not show "
                      "until this is fixed and re-enabled\n",
                      helper_path.c_str(), std::strerror(errno));
        return;
    }

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        std::fprintf(stderr, "keeby: visualizer socketpair failed: %s\n", std::strerror(errno));
        return;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "keeby: visualizer fork failed: %s\n", std::strerror(errno));
        ::close(sv[0]);
        ::close(sv[1]);
        return;
    }
    if (pid == 0) {
        // Child: dup2's target does not carry FD_CLOEXEC, so fd 3 survives
        // the exec below while every other fd this process holds does not
        // (sv[1] itself was created with SOCK_CLOEXEC). Edge case: if sv[1]
        // already IS fd 3, dup2(3, 3) is a documented no-op that leaves
        // FD_CLOEXEC set, so the socket would otherwise vanish at exec --
        // clear the flag directly instead (fcntl is async-signal-safe;
        // dup2 would not fix this). Same fix as input_capture.cpp's spawn.
        if (sv[1] == 3) fcntl(3, F_SETFD, 0);
        else dup2(sv[1], 3);
        char* argv[2] = {const_cast<char*>(helper_path.c_str()), nullptr};
        execv(helper_path.c_str(), argv);
        _exit(127); // execv only returns on failure
    }

    ::close(sv[1]); // parent doesn't use the child's end
    ipc_fd_ = sv[0];
    helper_pid_ = pid;
    child_alive_.store(true, std::memory_order_relaxed);

    send_config_locked(); // config right after spawning, before any key event can race ahead of it

    sender_thread_ = std::jthread([this, pid, fd = ipc_fd_](std::stop_token stoken) { sender_loop(stoken, pid, fd); });
}

void VisualizerService::terminate_locked() {
    if (sender_thread_.joinable()) {
        sender_thread_.request_stop();
        sender_thread_.join();
    }
    if (helper_pid_ > 0) {
        // If the sender thread already detected death and reaped it, this
        // waitpid(WNOHANG) inside terminate_and_reap's SIGTERM path simply
        // finds nothing (ESRCH-safe: kill() on an already-reaped pid is a
        // no-op error, never fatal here) -- still correct either way.
        if (child_alive_.load(std::memory_order_relaxed)) terminate_and_reap(helper_pid_);
        helper_pid_ = -1;
    }
    if (ipc_fd_ >= 0) {
        ::close(ipc_fd_);
        ipc_fd_ = -1;
    }
    child_alive_.store(false, std::memory_order_relaxed);
}

void VisualizerService::sender_loop(std::stop_token stoken, pid_t pid, int fd) {
    bool dead = false;
    bool reaped = false;
    KeyEvent ev;
    // Persist across outer-loop iterations (not just the inner motion
    // drain below): a delta that hits EAGAIN must still be there next
    // time, even if that's a later call to this same function body.
    MotionCarry mouse_carry, touch_carry;
    // ponytail: 1ms busy-wait poll -- there is no portable "wake on either
    // a queue push or SIGCHLD" primitive here, and a visualizer needs low
    // latency anyway. Revisit with an eventfd/condvar wake if this ever
    // shows up in a CPU profile.
    while (!stoken.stop_requested() && !dead) {
        bool did_work = false;
        if (transport_.try_pop(ev)) {
            did_work = true;
            if (ev.kind != KeyEventKind::Repeat) {
                viz::VizMessage m{};
                m.type = static_cast<uint8_t>(viz::MessageType::Key);
                m.kind = (ev.kind == KeyEventKind::Down) ? 1 : 0;
                m.code = ev.code;
                const ssize_t n = ::send(fd, &m, sizeof m, MSG_DONTWAIT | MSG_NOSIGNAL);
                // EAGAIN/EWOULDBLOCK: socket full because the child isn't
                // draining it -- drop this event and keep going, never
                // block. Anything else (EPIPE, ECONNRESET, ...): the child
                // is gone or the socket errored.
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) dead = true;
            }
        }
        // Forward every pointer-motion sample as its own Motion message --
        // never sum a drained batch into one, since niri clamps its real
        // cursor position after every single motion event, not once per
        // batch (nonlinear at an edge: see cursor_estimator.hpp and its
        // tests). The one exception is backpressure: a send() that hits
        // EAGAIN must never silently lose that delta, so it stays in this
        // kind's MotionCarry and gets added into whatever the next send
        // for that kind turns out to be (which may be this same event
        // retried, or a newer one on top of it) -- summing only ever
        // happens here, under backpressure, never on the normal path.
        // Never blocks: same EAGAIN-retry/anything-else-is-death policy as
        // the key path above.
        PointerMotionEvent mv;
        while (!dead && motion_transport_.try_pop(mv)) {
            did_work = true;
            MotionCarry& carry = (mv.kind == PointerDeviceKind::Touchpad) ? touch_carry : mouse_carry;
            carry.add(mv.dx, mv.dy);
            viz::VizMessage m{};
            m.type = static_cast<uint8_t>(viz::MessageType::Motion);
            m.kind = static_cast<uint8_t>(mv.kind);
            viz::set_motion_dx(m, wire::clamp_motion_component(static_cast<double>(carry.dx)));
            viz::set_motion_dy(m, wire::clamp_motion_component(static_cast<double>(carry.dy)));
            const ssize_t n = ::send(fd, &m, sizeof m, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // keep carrying; don't clear, don't drop
                dead = true;
                break;
            }
            carry.clear(); // sent (with whatever was carried) -- this kind starts fresh
        }
        if (!dead) {
            int status = 0;
            const pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) { dead = true; reaped = true; } // exited on its own; already reaped
        }
        if (!did_work && !dead) {
            struct timespec ts{0, 1'000'000}; // 1ms
            nanosleep(&ts, nullptr);
        }
    }
    if (dead && !reaped) ::waitpid(pid, nullptr, 0); // send()-detected death: reap, never leave a zombie
    if (dead) child_alive_.store(false, std::memory_order_relaxed);
}

} // namespace keeby
