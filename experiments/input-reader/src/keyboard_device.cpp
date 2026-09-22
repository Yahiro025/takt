#include "keyboard_device.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>

namespace keeby {
namespace {

bool looks_like_full_keyboard(libevdev* dev) {
    return libevdev_has_event_code(dev, EV_KEY, KEY_A) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_Z) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_SPACE) &&
           libevdev_has_event_code(dev, EV_KEY, KEY_ENTER);
}

uint64_t timeval_to_ns(const timeval& tv) {
    return static_cast<uint64_t>(tv.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(tv.tv_usec) * 1'000ull;
}

KeyEventKind classify(int value) {
    switch (value) {
        case 0: return KeyEventKind::Up;
        case 1: return KeyEventKind::Down;
        default: return KeyEventKind::Repeat; // value == 2
    }
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

bool run_keyboard_reader(const std::string& path,
                          EventQueue& queue,
                          const std::atomic<bool>& running,
                          KeyboardDeviceStats& stats) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "keeby: cannot open %s: %s\n", path.c_str(), std::strerror(errno));
        if (errno == EACCES) {
            std::fprintf(stderr,
                "keeby: permission denied. Fix with ONE of:\n"
                "  1) be a member of the 'input' group (check: groups; fix:\n"
                "     sudo usermod -aG input $USER, then log out and back in), or\n"
                "  2) rely on your session's logind/seatd seat ACL (uaccess) if your\n"
                "     distro's udev rules grant it dynamically instead of the group.\n"
                "  Never chmod 666 the device.\n");
        }
        return false;
    }

    libevdev* dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
        std::fprintf(stderr, "keeby: libevdev_new_from_fd failed for %s\n", path.c_str());
        close(fd);
        return false;
    }

    // This kernel's default evdev clock for this device is CLOCK_REALTIME,
    // not CLOCK_MONOTONIC (confirmed empirically: raw ev.time values were
    // epoch-scale wall-clock seconds). EVIOCSCLOCKID lets this fd alone
    // request monotonic timestamps without affecting any other reader
    // (the compositor's own libinput fd keeps whatever clock it already
    // uses), which is what latency math below needs.
    int clockid = CLOCK_MONOTONIC;
    if (ioctl(fd, EVIOCSCLOCKID, &clockid) < 0) {
        std::fprintf(stderr,
            "keeby: warning: EVIOCSCLOCKID(CLOCK_MONOTONIC) failed: %s "
            "(latency measurements will be meaningless)\n",
            std::strerror(errno));
    }

    // Deliberately never call libevdev_grab(): this must stay a passive,
    // non-exclusive observer so every other client (the compositor, the
    // focused application) keeps receiving every event untouched.
    std::fprintf(stderr, "keeby: reading \"%s\" (%s) — not grabbed\n",
                 path.c_str(), libevdev_get_name(dev));

    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    bool disconnected = false;
    while (running.load(std::memory_order_relaxed) && !disconnected) {
        int pret = poll(&pfd, 1, 200 /*ms, so we recheck `running` promptly*/);
        if (pret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pret == 0) continue; // timeout, loop back to check `running`

        input_event ev;
        int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        while (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC) {
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                // Kernel-side buffer overrun; drain the forced-sync state
                // before resuming normal reads. Rare in practice.
                while (rc == LIBEVDEV_READ_STATUS_SYNC) {
                    rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
                }
                continue;
            }

            if (ev.type == EV_KEY) {
                KeyEvent ke;
                ke.code = static_cast<uint16_t>(ev.code);
                ke.kind = classify(ev.value);
                ke.ts_ns = timeval_to_ns(ev.time);

                // Physical Linux keycode + press/release/repeat only —
                // never a character, never text. Printed to the terminal
                // for live verification, never written to a file.
                const char* kind_str = ke.kind == KeyEventKind::Down   ? "DOWN"
                                        : ke.kind == KeyEventKind::Up  ? "UP"
                                                                       : "REPEAT";
                std::fprintf(stderr, "key code=%-3u kind=%-6s ts_ns=%llu\n",
                             ke.code, kind_str, (unsigned long long)ke.ts_ns);

                ++stats.events_read;
                if (!queue.try_push(ke)) {
                    ++stats.events_dropped;
                }
            }
            rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        }

        if (rc == -ENODEV) {
            std::fprintf(stderr, "keeby: device \"%s\" disconnected\n", path.c_str());
            disconnected = true;
        }
        // rc == -EAGAIN just means "no more events right now", expected.
    }

    libevdev_free(dev);
    close(fd);
    return true;
}

} // namespace keeby
