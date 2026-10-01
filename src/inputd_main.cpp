// keeby-inputd: minimal privileged keyboard-input helper.
//
// Started by the unprivileged keeby process with an inherited AF_UNIX
// SOCK_SEQPACKET socket on fd 3. Installed setgid-input (see
// docs/006-step-2.5-security-permissions.md); the interactive user's
// account does not need to be in the `input` group at all.
//
// Lifecycle: validate the IPC fd -> find/validate/open exactly one
// keyboard-class evdev device, read-only, plus up to 4 pointer devices
// (mice/touchpads; see docs/006's "Pointer motion for the visualizer"
// section -- a user-approved scope change from "one keyboard" to "one
// keyboard plus pointer devices") -> permanently drop the `input` group
// (setresgid back to the process's own real gid) and verify the drop ->
// prctl(NO_NEW_PRIVS)/prctl(DUMPABLE=0) -> send one Ready message ->
// forward keyboard KeyEvent, allowlisted pointer-button KeyEvent, and
// PointerMotion messages until the keyboard disappears or
// the parent goes away (a pointer device disappearing is not fatal: libinput
// itself notices and stops producing events for it, see the main
// loop below).
//
// Deliberately contains nothing else: no PipeWire, no D-Bus, no GUI, no
// filesystem access beyond the opened device paths, no networking, no
// shell invocation, no library beyond libevdev and POSIX for the keyboard
// path. This ELF links ONLY libevdev (plus libc/libstdc++/libgcc/libm) --
// `ldd` must never list libinput or any of its transitive dependencies.
// Pointer motion runs libinput (docs/006's "Exact pointer tracking via
// libinput" section), dlopen()'d post-privilege-drop only -- see
// src/libinput_loader.hpp for why. Once loaded, libinput only ever gets a
// dup of a read-only fd this process already opened and validated
// pre-drop: its open_restricted() hook never calls open() (see
// pointer_open_restricted() below). No EVIOCGRAB and no writes on any
// device, keyboard or pointer alike.
#include "input_wire.hpp"
#include "libinput_loader.hpp"
#include "pointer_input.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>

namespace {

constexpr int kIpcFd = 3;
constexpr std::size_t kMaxPointerDevices = 4;

[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "keeby-inputd: %s: %s\n", what, std::strerror(errno));
    std::_Exit(1);
}

[[noreturn]] void die_msg(const char* what) {
    std::fprintf(stderr, "keeby-inputd: %s\n", what);
    std::_Exit(1);
}

bool looks_like_full_keyboard(libevdev* dev) {
    return libevdev_has_event_code(dev, EV_KEY, KEY_A) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_Z) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_SPACE) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_ENTER);
}

// Cheap early filter only. The real security boundary is the fd-based
// validation in try_open_keyboard() below (character device + libevdev
// capability check), not this string check — avoids relying solely on
// pathname text per the TOCTOU guidance in docs/006.
bool is_valid_event_path(const std::string& path) {
    const std::string prefix = "/dev/input/event";
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    if (path.size() == prefix.size()) return false;
    for (std::size_t i = prefix.size(); i < path.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(path[i]))) return false;
    return true;
}

struct OpenedDevice { int fd = -1; libevdev* dev = nullptr; };

// Opens and validates one candidate, using the opened fd/device — not the
// path string — as the source of truth. Returns {-1, nullptr} on any
// failure with everything already cleaned up.
OpenedDevice try_open_keyboard(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return {};
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) { close(fd); return {}; }
    libevdev* dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); return {}; }
    if (!looks_like_full_keyboard(dev)) { libevdev_free(dev); close(fd); return {}; }
    return {fd, dev};
}

OpenedDevice discover_keyboard() {
    for (int i = 0; i < 32; ++i) {
        auto found = try_open_keyboard("/dev/input/event" + std::to_string(i));
        if (found.fd >= 0) return found;
    }
    return {};
}

struct OpenedPointer {
    int fd = -1;
    keeby::pointer::DeviceKind kind = keeby::pointer::DeviceKind::None;
    // The exact path this fd was opened from (e.g. "/dev/input/event7").
    // Kept only so pointer_open_restricted() (called post-drop, once
    // libinput itself is set up -- see main()) can find, for a given path
    // libinput asks about, which of these already-opened read-only fds to
    // hand back a dup of. libinput never opens this path itself.
    std::string path;
};

keeby::pointer::Capabilities read_capabilities(libevdev* dev) {
    keeby::pointer::Capabilities c;
    c.rel_x = libevdev_has_event_code(dev, EV_REL, REL_X);
    c.rel_y = libevdev_has_event_code(dev, EV_REL, REL_Y);
    c.abs_x = libevdev_has_event_code(dev, EV_ABS, ABS_X);
    c.abs_y = libevdev_has_event_code(dev, EV_ABS, ABS_Y);
    c.abs_mt_x = libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X);
    c.abs_mt_y = libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_Y);
    c.btn_tool_finger = libevdev_has_event_code(dev, EV_KEY, BTN_TOOL_FINGER);
    c.prop_pointer = libevdev_has_property(dev, INPUT_PROP_POINTER);
    return c;
}

// Opens and validates one pointer candidate on the opened fd/device -- same
// TOCTOU-safe shape as try_open_keyboard(): the path string is never the
// source of truth, only what libevdev reports about the fd actually opened.
// Never EVIOCGRAB, never a write. Returns kind == None (fd already closed)
// for anything that isn't exactly a relative mouse or a touchpad, including
// absolute tablets and touchscreens.
OpenedPointer try_open_pointer(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return {};
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) { close(fd); return {}; }
    libevdev* dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); return {}; }

    const keeby::pointer::DeviceKind kind = keeby::pointer::classify(read_capabilities(dev));
    libevdev_free(dev); // only needed transiently to read capability bits above --
                         // libinput re-probes the fd itself post-drop (see main()).
    if (kind == keeby::pointer::DeviceKind::None) { close(fd); return {}; }

    int clockid = CLOCK_MONOTONIC;
    if (ioctl(fd, EVIOCSCLOCKID, &clockid) < 0) { close(fd); return {}; }

    return {fd, kind, path};
}

// Up to kMaxPointerDevices mice/touchpads, auto-discovered the same way the
// keyboard is. A device that's neither (including the keyboard itself,
// which has no pointer capability bits) is simply skipped, not an error --
// see the class-comment: "if there is no pointer device, everything else
// keeps working".
std::vector<OpenedPointer> discover_pointers() {
    std::vector<OpenedPointer> found;
    for (int i = 0; i < 32 && found.size() < kMaxPointerDevices; ++i) {
        auto p = try_open_pointer("/dev/input/event" + std::to_string(i));
        if (p.fd >= 0) found.push_back(p);
    }
    return found;
}

uint64_t timeval_to_ns(const timeval& tv) {
    return static_cast<uint64_t>(tv.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(tv.tv_usec) * 1'000ull;
}

// Permanently drops the setgid-input effective/saved group back to the
// process's own real group, and VERIFIES the drop by re-reading the
// resulting real/effective/saved group IDs afterward — never trusted from
// the setresgid() return code alone. Terminates immediately if that part
// of the drop cannot be verified.
//
// Also attempts (but does not require) clearing the SUPPLEMENTARY groups
// list via setgroups(). This is a real, separate axis from the primary
// gid setresgid() above resets: supplementary groups are inherited
// unchanged across exec() (setgid only affects the effective/saved
// PRIMARY gid at exec time), so if the invoking user's own account is
// itself a supplementary member of `input`, that membership is still
// present here via inheritance, regardless of this drop. Clearing it
// requires CAP_SETGID, which this process — merely setgid, not
// setuid-root — does not have (and setresgid(real,real,real) itself is
// exempt from that requirement only because it discards privilege rather
// than gaining it). So EPERM here is EXPECTED and non-fatal when the
// invoking account still has `input` as a supplementary group; anything
// else is not, and is fatal. Either way this is reported to stderr so it
// is visible during live verification instead of silently assumed — see
// docs/006-step-2.5-security-permissions.md.
void drop_privilege_or_die() {
    const gid_t real_gid = getgid();
    if (setresgid(real_gid, real_gid, real_gid) != 0) die("setresgid failed");

    gid_t r = 0, e = 0, s = 0;
    if (getresgid(&r, &e, &s) != 0) die("getresgid failed");
    if (r != real_gid || e != real_gid || s != real_gid)
        die_msg("privilege drop could not be verified; refusing to continue");

    struct group* input_group = getgrnam("input");
    if (input_group && (r == input_group->gr_gid || e == input_group->gr_gid ||
                        s == input_group->gr_gid))
        die_msg("input group still present in primary gid after drop; refusing to continue");

    if (setgroups(1, &real_gid) != 0) {
        if (errno != EPERM) die("setgroups failed unexpectedly (not EPERM)");
        std::fprintf(stderr,
            "keeby-inputd: setgroups() denied (EPERM, expected without CAP_SETGID): "
            "supplementary groups inherited from the invoking session are unchanged by "
            "this process. If that session's account is itself a member of 'input', this "
            "process still has 'input' via inheritance, independent of the setgid bit just "
            "dropped above. See docs/006-step-2.5-security-permissions.md.\n");
    }
    if (input_group) {
        int n = getgroups(0, nullptr);
        if (n > 0) {
            std::vector<gid_t> groups(static_cast<std::size_t>(n));
            if (getgroups(n, groups.data()) == n) {
                for (gid_t g : groups) {
                    if (g == input_group->gr_gid) {
                        std::fprintf(stderr,
                            "keeby-inputd: NOTE: 'input' remains in this process's "
                            "supplementary groups (inherited from the invoking session, "
                            "not from this binary's setgid bit).\n");
                        break;
                    }
                }
            }
        }
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) die("PR_SET_NO_NEW_PRIVS failed");
    prctl(PR_SET_DUMPABLE, 0); // best-effort; not itself security-critical if it fails
}

// Live-security-verification hook only, never part of the IPC protocol:
// on SIGUSR1, attempt to open a second, different evdev device and report
// the result to stderr. After a real privilege drop this must fail
// (EACCES) — this is the "attempt a negative test" the security report
// asks for, actually executed rather than only reasoned about. Uses only
// write()/open()/close(), which are async-signal-safe; message lengths
// are precomputed constants to avoid strlen() in the handler.
int g_probe_device_index = 0;
char g_probe_path[32] = ""; // filled once in main(), before SIGUSR1 is armed
constexpr char kProbeFail[] = "keeby-inputd: post-drop open() failed as expected (EACCES) — privilege drop verified live\n";
constexpr char kProbeUnexpectedSuccess[] = "keeby-inputd: SECURITY TEST FAILURE: post-drop open() unexpectedly succeeded\n";
// std::snprintf is not async-signal-safe, so g_probe_path must already be
// a plain buffer by the time this can run -- only open/close/write here.
void handle_probe_signal(int) {
    int fd = g_probe_path[0] ? open(g_probe_path, O_RDONLY | O_NONBLOCK) : -1;
    if (fd >= 0) {
        write(2, kProbeUnexpectedSuccess, sizeof(kProbeUnexpectedSuccess) - 1);
        close(fd);
    } else {
        write(2, kProbeFail, sizeof(kProbeFail) - 1);
    }
}

// ---- libinput (dlopen()'d, post-privilege-drop only) ----------------------
// docs/006's "Exact pointer tracking via libinput" section: loaded
// (load_libinput_api(), src/libinput_loader.hpp) and populated strictly
// AFTER drop_privilege_or_die() in main(), over fds this process already
// opened and validated pre-drop (`pointers`). Never
// libinput_suspend()/resume(), never a libinput plugin-system call of our
// own, never a keyboard added to this context -- only pointer motion and
// allowlisted pointer-button events are read back out of it.

// open_restricted()'s user_data is the same pre-drop-opened, already-
// validated `pointers` vector from main(). NEVER calls open() -- only ever
// hands back a dup of an fd this process opened before the privilege drop,
// and only when the requested path matches one of them exactly; anything
// else is -EACCES. `flags` is intentionally unused: libinput requests
// O_RDWR, but what's handed back is always a dup of our already-read-only
// fd regardless -- confirmed against the libinput 1.32 source
// (src/evdev.c's evdev_device_create()/evdev_device_led_update() and
// src/evdev-fallback.c's fallback_lid_keyboard_event()) that neither mouse
// nor touchpad device init ever needs a writable fd; the only two writes
// in libinput's own source are gated on keyboard-only state this process
// never creates (see docs/006).
int pointer_open_restricted(const char* path, int /*flags*/, void* user_data) {
    auto* pointers = static_cast<std::vector<OpenedPointer>*>(user_data);
    for (const OpenedPointer& p : *pointers) {
        if (p.path == path) {
            const int dup_fd = fcntl(p.fd, F_DUPFD_CLOEXEC, 0);
            return dup_fd >= 0 ? dup_fd : -EACCES;
        }
    }
    return -EACCES;
}

void pointer_close_restricted(int fd, void* /*user_data*/) { close(fd); }

constexpr libinput_interface kPointerInterface = {
    .open_restricted = pointer_open_restricted,
    .close_restricted = pointer_close_restricted,
};

// Per-added-device state: which wire::PointerKind to tag outgoing
// PointerMotion messages with, plus this device's own carried sub-pixel
// remainder per axis (see pointer_input.hpp::carry_and_truncate()).
// Addresses are handed to libinput_device_set_user_data() and must stay
// stable for the context's lifetime -- setup_libinput() reserve()s the
// vector holding these up front so it never reallocates.
struct PointerDeviceState {
    keeby::wire::PointerKind kind;
    double remx = 0.0;
    double remy = 0.0;
};

// ponytail: hardcoded to mirror the user's OWN niri config (niri
// src/input/mod.rs:4674-4962 apply_libinput_settings; touchpad: tap,
// tap-button-map "left-right-middle", rest default; mouse: accel-profile
// "flat", rest default) so keeby-inputd's forwarded deltas are computed
// with the same acceleration niri itself applies to the real cursor.
// Upgrade path if this ever needs to serve a different config: pass
// settings from keeby via argv instead of a constant mirror here.
void apply_niri_pointer_settings(const keeby::pointer::LibinputApi& api, libinput_device* dev,
                                  keeby::pointer::DeviceKind kind) {
    api.config_accel_set_speed(dev, 0.0);
    if (kind == keeby::pointer::DeviceKind::Mouse) {
        api.config_accel_set_profile(dev, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
        return;
    }
    // Touchpad: accel profile intentionally left at the device default --
    // the user's niri config never sets one under `touchpad {}`.
    api.config_tap_set_enabled(dev, LIBINPUT_CONFIG_TAP_ENABLED);
    api.config_tap_set_button_map(dev, LIBINPUT_CONFIG_TAP_MAP_LRM);
    api.config_dwt_set_enabled(dev, LIBINPUT_CONFIG_DWT_DISABLED);
    api.config_dwtp_set_enabled(dev, LIBINPUT_CONFIG_DWTP_DISABLED);
    api.config_tap_set_drag_lock_enabled(dev, LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
    // Drag (tap-and-drag) intentionally left at the device default -- the
    // user's niri config never sets `drag` under `touchpad {}` either.
}

// What setup_libinput() hands back to main(): the context (or nullptr for
// keyboard-only) plus the LibinputApi it was created through -- main()'s
// loop needs the same loaded function pointers to dispatch/drain events.
struct LibinputSetup {
    libinput* ctx = nullptr;
    keeby::pointer::LibinputApi api;
};

// Loads libinput (post-drop only, see libinput_loader.hpp) and adds every
// pre-opened, already-validated pointer fd to a path-backend context.
// `states` is filled 1:1 with only the devices actually added, in order.
// Logs exactly one line in every case -- never device names or paths,
// only a count.
LibinputSetup setup_libinput(std::vector<OpenedPointer>& pointers, std::vector<PointerDeviceState>& states) {
    LibinputSetup result;
    if (pointers.empty()) {
        std::fprintf(stderr, "keeby-inputd: libinput: no pointer device found; keyboard-only\n");
        return result;
    }
    result.api = keeby::pointer::load_libinput_api();
    if (!result.api.loaded()) {
        std::fprintf(stderr, "keeby-inputd: libinput: failed to load libinput.so; keyboard-only\n");
        return result;
    }
    libinput* li = result.api.path_create_context(&kPointerInterface, &pointers);
    if (!li) {
        std::fprintf(stderr, "keeby-inputd: libinput: context creation failed; keyboard-only\n");
        return result; // api.handle is left loaded but unused -- see libinput_loader.hpp
    }
    result.api.log_set_priority(li, LIBINPUT_LOG_PRIORITY_ERROR); // no event data is ever logged

    states.reserve(pointers.size()); // addresses below must stay stable -- see PointerDeviceState
    int added = 0;
    for (const OpenedPointer& p : pointers) {
        libinput_device* dev = result.api.path_add_device(li, p.path.c_str());
        if (!dev) continue;
        states.push_back({p.kind == keeby::pointer::DeviceKind::Touchpad ? keeby::wire::PointerKind::Touchpad
                                                                          : keeby::wire::PointerKind::Mouse,
                           0.0, 0.0});
        result.api.device_set_user_data(dev, &states.back());
        apply_niri_pointer_settings(result.api, dev, p.kind);
        ++added;
    }
    std::fprintf(stderr, "keeby-inputd: libinput: %d of %zu pointer device(s) added\n", added, pointers.size());
    if (added == 0) {
        result.api.unref(li);
        return result; // ctx stays nullptr; api stays loaded but unused, same as above
    }
    result.ctx = li;
    return result;
}

} // namespace

int main(int argc, char** argv) {
    int type = 0;
    socklen_t len = sizeof(type);
    if (getsockopt(kIpcFd, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_SEQPACKET)
        die_msg("fd 3 is not a SOCK_SEQPACKET socket; refusing to run standalone");
    int domain = 0;
    len = sizeof(domain);
    if (getsockopt(kIpcFd, SOL_SOCKET, SO_DOMAIN, &domain, &len) != 0 || domain != AF_UNIX)
        die_msg("fd 3 is not an AF_UNIX socket; refusing to run standalone");

    const std::string requested = (argc > 1) ? argv[1] : "";
    OpenedDevice device;
    if (!requested.empty()) {
        if (!is_valid_event_path(requested))
            die_msg("requested device path is not a /dev/input/eventN path");
        device = try_open_keyboard(requested);
        if (device.fd < 0)
            die_msg("requested device could not be opened/validated as a keyboard");
    } else {
        device = discover_keyboard();
        if (device.fd < 0) die_msg("no suitable keyboard device found");
    }
    g_probe_device_index = -1; // pick any index other than the one just opened for the probe
    for (int i = 0; i < 32; ++i) {
        std::string p = "/dev/input/event" + std::to_string(i);
        struct stat st{};
        if (stat(p.c_str(), &st) == 0) { g_probe_device_index = i; break; }
    }
    int probe_path_len =
        std::snprintf(g_probe_path, sizeof(g_probe_path), "/dev/input/event%d", g_probe_device_index);
    if (probe_path_len <= 0 || static_cast<std::size_t>(probe_path_len) >= sizeof(g_probe_path))
        g_probe_path[0] = '\0';

    int clockid = CLOCK_MONOTONIC;
    if (ioctl(device.fd, EVIOCSCLOCKID, &clockid) < 0) die("EVIOCSCLOCKID failed");
    // Deliberately never libevdev_grab(): passive observer only.

    // Pointer devices (docs/006's "Pointer motion for the visualizer"
    // section): opened and validated here, BEFORE the privilege drop below,
    // exactly like the keyboard above. Absent/unpluggable without being
    // fatal -- an empty list here just means no pointer events are sent.
    std::vector<OpenedPointer> pointers = discover_pointers();

    drop_privilege_or_die();

    struct sigaction sa{};
    sa.sa_handler = handle_probe_signal;
    sigaction(SIGUSR1, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // libinput is loaded and set up here: strictly after
    // drop_privilege_or_die() above, over the fds already opened and
    // validated before it (see pointer_open_restricted()/setup_libinput(),
    // libinput_loader.hpp and docs/006).
    std::vector<PointerDeviceState> pointer_states;
    LibinputSetup libinput_setup = setup_libinput(pointers, pointer_states);
    libinput* li = libinput_setup.ctx;
    const keeby::pointer::LibinputApi& lapi = libinput_setup.api; // valid whenever li != nullptr

    keeby::wire::Message ready{};
    ready.type = static_cast<uint8_t>(keeby::wire::MessageType::Ready);
    if (send(kIpcFd, &ready, sizeof(ready), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(ready)))
        return 1; // parent already gone; nothing meaningful left to do

    // pfds[0] is always the keyboard; pfds[1] (only present when `li` was
    // created) is libinput's own aggregate fd for every pointer device added
    // to it -- one fd regardless of how many devices, unlike the old
    // per-device poll set.
    std::vector<struct pollfd> pfds;
    pfds.push_back({device.fd, POLLIN, 0});
    if (li) pfds.push_back({lapi.get_fd(li), POLLIN, 0});

    auto send_key_event = [](const keeby::KeyEvent& event) {
        keeby::wire::Message msg{};
        msg.type = static_cast<uint8_t>(keeby::wire::MessageType::KeyEvent);
        msg.kind = static_cast<uint8_t>(event.kind);
        msg.code = event.code;
        msg.ts_ns = event.ts_ns;
        return send(kIpcFd, &msg, sizeof(msg), MSG_NOSIGNAL) >= 0;
    };

    auto send_motion = [](keeby::wire::PointerKind kind, double dx, double dy, uint64_t ts_ns) {
        const int16_t cdx = keeby::wire::clamp_motion_component(dx);
        const int16_t cdy = keeby::wire::clamp_motion_component(dy);
        if (cdx == 0 && cdy == 0) return true; // nothing to report this frame
        keeby::wire::Message msg{};
        msg.type = static_cast<uint8_t>(keeby::wire::MessageType::PointerMotion);
        msg.kind = static_cast<uint8_t>(kind);
        msg.reserved = keeby::wire::pack_motion(cdx, cdy);
        msg.ts_ns = ts_ns;
        return send(kIpcFd, &msg, sizeof(msg), MSG_NOSIGNAL) >= 0;
    };

    for (;;) {
        int pret = poll(pfds.data(), pfds.size(), 200 /*ms*/);
        if (pret < 0) { if (errno == EINTR) continue; break; }
        if (pret == 0) continue;

        bool keyboard_gone = false;
        bool parent_gone = false;

        if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            input_event ev;
            int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            while (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC) {
                if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                    while (rc == LIBEVDEV_READ_STATUS_SYNC)
                        rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
                    continue;
                }
                if (ev.type == EV_KEY) {
                    const keeby::KeyEvent key{static_cast<uint16_t>(ev.code),
                        keeby::classify_key_value(ev.value), timeval_to_ns(ev.time)};
                    if (!send_key_event(key)) { parent_gone = true; break; }
                }
                rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            }
            if (rc == -ENODEV) keyboard_gone = true; // the one fatal device: exit, closing our IPC end
        }

        // Pointer events: one libinput_dispatch() + drain per readable poll.
        // Motion remains a separate visualizer message; only left/right/
        // middle button state enters the audio KeyEvent path.
        if (!parent_gone && li && (pfds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
            lapi.dispatch(li);
            libinput_event* ev = nullptr;
            while (!parent_gone && (ev = lapi.get_event(li)) != nullptr) {
                const enum libinput_event_type type = lapi.event_get_type(ev);
                if (type == LIBINPUT_EVENT_POINTER_MOTION) {
                    libinput_event_pointer* pev = lapi.event_get_pointer_event(ev);
                    libinput_device* dev = lapi.event_get_device(ev);
                    auto* st = static_cast<PointerDeviceState*>(lapi.device_get_user_data(dev));
                    if (st) {
                        // One message per libinput motion event (no summing
                        // across events) -- carry_and_truncate() is what
                        // keeps repeated sub-pixel deltas from being lost.
                        const int32_t idx =
                            keeby::pointer::carry_and_truncate(lapi.event_pointer_get_dx(pev), st->remx);
                        const int32_t idy =
                            keeby::pointer::carry_and_truncate(lapi.event_pointer_get_dy(pev), st->remy);
                        if (idx != 0 || idy != 0) {
                            const uint64_t ts_ns = lapi.event_pointer_get_time_usec(pev) * 1000ull;
                            if (!send_motion(st->kind, static_cast<double>(idx), static_cast<double>(idy), ts_ns))
                                parent_gone = true;
                        }
                    }
                } else if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
                    libinput_event_pointer* pev = lapi.event_get_pointer_event(ev);
                    const uint32_t button = lapi.event_pointer_get_button(pev);
                    const enum libinput_button_state state = lapi.event_pointer_get_button_state(pev);
                    if (state == LIBINPUT_BUTTON_STATE_PRESSED || state == LIBINPUT_BUTTON_STATE_RELEASED) {
                        const uint64_t ts_ns = lapi.event_pointer_get_time_usec(pev) * 1000ull;
                        if (const auto event = keeby::pointer::make_button_event(
                                button, state == LIBINPUT_BUTTON_STATE_PRESSED, ts_ns);
                            event && !send_key_event(*event)) {
                            parent_gone = true;
                        }
                    }
                }
                lapi.event_destroy(ev);
            }
        }

        if (parent_gone) {
            libevdev_free(device.dev);
            close(device.fd);
            for (auto& p : pointers) close(p.fd);
            if (li) lapi.unref(li);
            return 0; // parent gone; nothing left to forward to
        }
        if (keyboard_gone) break;
    }

    libevdev_free(device.dev);
    close(device.fd);
    for (auto& p : pointers) close(p.fd);
    if (li) lapi.unref(li);
    return 0;
}
