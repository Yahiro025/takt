#include "input_capture.hpp"
#include "input_wire.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>

namespace keeby {
namespace {

bool looks_like_full_keyboard(libevdev* dev) {
    return libevdev_has_event_code(dev, EV_KEY, KEY_A) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_Z) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_SPACE) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_ENTER);
}

// Test-only override (KEEBY_INPUTD_PATH) first, else the real helper next
// to this executable — never a $PATH search, so a malicious PATH entry
// can't substitute a different binary. The override never crosses a
// privilege boundary by itself: whatever it points to still runs as this
// same unprivileged caller unless it separately carries setgid-input,
// which requires root to set up (see docs/006's security audit).
std::string resolve_helper_path() {
    if (const char* override_path = std::getenv(wire::kHelperPathOverrideEnv))
        return override_path;
    // KEEBY_HELPER_PATH_DEFAULT: compiled in from the KEEBY_HELPER_PATH CMake
    // cache variable; empty in dev builds (falls through below), set by the
    // Arch package to /usr/lib/keeby/keeby-inputd.
    if (KEEBY_HELPER_PATH_DEFAULT[0] != '\0') return KEEBY_HELPER_PATH_DEFAULT;
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) return wire::kHelperName;
    self[n] = '\0';
    std::string dir(self);
    auto slash = dir.find_last_of('/');
    if (slash == std::string::npos) return wire::kHelperName;
    return dir.substr(0, slash + 1) + wire::kHelperName;
}

// SIGTERM, brief bounded wait, escalate to SIGKILL, always reap. Only
// called from stop(), never the RT path; a few hundred ms of blocking here
// is the same shape as PipeWire's own thread_loop_stop() shutdown wait.
void terminate_and_reap(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) { // up to ~1s
        pid_t r = waitpid(pid, nullptr, WNOHANG);
        if (r == pid || r < 0) return;
        struct timespec ts{0, 50'000'000};
        nanosleep(&ts, nullptr);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

// Precise EACCES diagnosis for start()'s access(X_OK) check below. The
// account can be listed in /etc/group's 'keeby' line (gr_mem, exactly what
// `usermod -aG keeby` edits) while this already-running process's own
// group list (getgroups(), inherited at process/session start) still
// lacks that gid -- classic when systemd lingering keeps the user manager
// (and everything it starts) alive across a logout, so only a reboot
// restarts it with fresh groups. Same getgrnam/getgroups shape as
// drop_privilege_or_die() in inputd_main.cpp.
bool user_joined_keeby_group_but_not_in_this_process() {
    struct group* gr = getgrnam("keeby");
    if (!gr) return false;

    struct passwd* pw = getpwuid(getuid());
    bool listed = false;
    if (pw) {
        for (char** m = gr->gr_mem; m && *m; ++m) {
            if (std::strcmp(*m, pw->pw_name) == 0) { listed = true; break; }
        }
    }
    if (!listed) return false; // not a group membership issue at all

    if (getgid() == gr->gr_gid || getegid() == gr->gr_gid) return false; // already active
    int n = getgroups(0, nullptr);
    if (n > 0) {
        std::vector<gid_t> groups(static_cast<std::size_t>(n));
        if (getgroups(n, groups.data()) == n) {
            for (gid_t g : groups)
                if (g == gr->gr_gid) return false; // already active
        }
    }
    return true; // listed in /etc/group, but this process never picked it up
}

} // namespace

std::optional<std::string> discover_keyboard_device() {
    for (int i = 0; i < 32; ++i) {
        std::string path = "/dev/input/event" + std::to_string(i);
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        libevdev* dev = nullptr;
        bool ok = false;
        if (libevdev_new_from_fd(fd, &dev) == 0) {
            ok = looks_like_full_keyboard(dev);
            libevdev_free(dev);
        }
        close(fd);
        if (ok) return path;
    }
    return std::nullopt;
}

InputCapture::~InputCapture() { stop(); }

void InputCapture::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    terminate_and_reap(helper_pid_);
    helper_pid_ = -1;
    if (ipc_fd_ >= 0) {
        close(ipc_fd_);
        ipc_fd_ = -1;
    }
}

std::expected<void, std::string> InputCapture::start(std::string device_path, KeyEventTransport& transport) {
    // Constructing a replacement jthread before joining the old one would
    // briefly give the transport two producers. Lifecycle calls are owner-only.
    if (thread_.joinable()) return std::unexpected("input capture already started; stop it first");

    const std::string helper_path = resolve_helper_path();
    if (access(helper_path.c_str(), X_OK) != 0) {
        const int access_errno = errno;  // copy before any other call can clobber it

        if (access_errno == EACCES && user_joined_keeby_group_but_not_in_this_process()) {
            return std::unexpected(
                "keeby-inputd at " + helper_path + " is not executable by this user (" +
                std::strerror(access_errno) +
                "). You are in the 'keeby' group, but this process started before you "
                "joined it — reboot (logging out is not enough when lingering is enabled).");
        }

        return std::unexpected(
            "keeby-inputd at " + helper_path + " is not executable by this user (" +
            std::strerror(access_errno) +
            "). Packaged install: join the 'keeby' group (sudo usermod -aG keeby $USER), "
            "then reboot. Manual/dev install: scripts/install-keeby-inputd.sh "
            "— see docs/010-step-2.9-packaging.md.");
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0)
        return std::unexpected(std::string("socketpair failed: ") + std::strerror(errno));

    std::string device_arg = std::move(device_path); // kept alive across execv; argv points into it

    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        return std::unexpected(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        // Child: dup2's target fd does not carry FD_CLOEXEC even though sv[1]
        // itself was created with SOCK_CLOEXEC, so fd 3 survives the exec
        // below while every other fd this process holds does not. Edge
        // case: if sv[1] already IS fd 3 (possible whenever fd 3 happened
        // to be free right before socketpair()), dup2(3, 3) is specified as
        // a no-op that returns success without touching FD_CLOEXEC, so the
        // flag set by SOCK_CLOEXEC above would otherwise survive into
        // execve() and close the socket out from under the child before it
        // ever runs. fcntl(F_SETFD) is async-signal-safe (fork() already
        // returned; nothing here runs in a signal handler), so clearing the
        // flag directly is the fix, not dup2.
        if (sv[1] == 3) fcntl(3, F_SETFD, 0);
        else dup2(sv[1], 3);
        char* argv[3] = {const_cast<char*>(helper_path.c_str()), device_arg.data(), nullptr};
        execv(helper_path.c_str(), argv);
        _exit(127); // execv only returns on failure
    }

    close(sv[1]); // parent doesn't use the child's end
    ipc_fd_ = sv[0];
    helper_pid_ = pid;

    // Wait synchronously for the helper's one-time Ready message (or
    // failure/timeout) — the same synchronous "start() surfaces failure
    // immediately" contract this class has always had.
    struct pollfd pfd {};
    pfd.fd = ipc_fd_;
    pfd.events = POLLIN;
    int pret = poll(&pfd, 1, 5000 /*ms*/);
    wire::Message ready{};
    ssize_t got = (pret > 0) ? recv(ipc_fd_, &ready, sizeof(ready), 0) : 0;
    if (pret <= 0 || got != static_cast<ssize_t>(sizeof(ready)) ||
        ready.type != static_cast<uint8_t>(wire::MessageType::Ready)) {
        terminate_and_reap(helper_pid_);
        helper_pid_ = -1;
        close(ipc_fd_);
        ipc_fd_ = -1;
        return std::unexpected(
            "keeby-inputd did not become ready (see its stderr output above for the exact "
            "reason — commonly: not installed setgid input, or no suitable keyboard found). "
            "See docs/006-step-2.5-security-permissions.md for the supported install "
            "procedure. Never run keeby itself with sudo.");
    }

    connected_.store(true, std::memory_order_relaxed);
    thread_ = std::jthread([this, &transport](std::stop_token stoken) {
        struct pollfd loop_pfd {};
        loop_pfd.fd = ipc_fd_;
        loop_pfd.events = POLLIN;
        bool lost = false;
        while (!stoken.stop_requested() && !lost) {
            int r = poll(&loop_pfd, 1, 200 /*ms*/);
            if (r < 0) {
                if (errno == EINTR) continue;
                lost = true;
                break;
            }
            if (r == 0) continue; // timeout, loop back to check stop_requested()

            wire::Message m{};
            ssize_t n = recv(ipc_fd_, &m, sizeof(m), 0);
            if (n == 0) { lost = true; break; } // helper closed its end
            if (n < 0) {
                if (errno == EINTR) continue;
                lost = true;
                break;
            }
            // Malformed size/type/kind: drop the message, never crash.
            if (n != static_cast<ssize_t>(sizeof(m))) continue;

            if (m.type == static_cast<uint8_t>(wire::MessageType::PointerMotion)) {
                // Visualizer-only, and only while wired (set by
                // EngineController exactly when the visualizer is enabled
                // AND its position is follow-cursor) -- never the key
                // transport, never the audio path, never D-Bus, never
                // logged (see docs/006's "Pointer motion for the
                // visualizer" section).
                if (PointerMotionTransport* motion = viz_motion_transport_.load(std::memory_order_acquire)) {
                    int16_t dx = 0, dy = 0;
                    wire::unpack_motion(m.reserved, dx, dy);
                    PointerMotionEvent pe;
                    pe.dx = dx;
                    pe.dy = dy;
                    pe.kind = m.kind == static_cast<uint8_t>(wire::PointerKind::Touchpad)
                                  ? PointerDeviceKind::Touchpad
                                  : PointerDeviceKind::Mouse;
                    pe.ts_ns = m.ts_ns;
                    motion->try_push(pe);
                }
                continue;
            }
            if (m.type != static_cast<uint8_t>(wire::MessageType::KeyEvent)) continue;
            if (m.kind > 2) continue;

            KeyEvent ke;
            ke.code = m.code;
            ke.kind = static_cast<KeyEventKind>(m.kind);
            ke.ts_ns = m.ts_ns;
            transport.try_push(ke); // overflow policy + counter live inside the transport

            // Visualizer tap: only while wired (set_visualizer_transport),
            // and never a repeat -- matches the VizMessage wire contract
            // ("repeat never sent") at the single point every KeyEvent
            // already passes through, rather than downstream in the
            // sender. Drop-on-full, same policy as the primary transport.
            if (KeyEventTransport* viz = viz_transport_.load(std::memory_order_acquire);
                viz && ke.kind != KeyEventKind::Repeat && ke.code < kKeyboardCodeLimit) {
                viz->try_push(ke);
            }
        }
        if (lost) connected_.store(false, std::memory_order_relaxed);
    });
    return {};
}

} // namespace keeby
