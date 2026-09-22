#include "input_capture.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
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

std::string permission_denied_message(const std::string& path) {
    return "permission denied opening " + path +
        ": fix with ONE of: (1) be a member of the 'input' group "
        "(check: groups; fix: sudo usermod -aG input $USER, then log out "
        "and back in), or (2) rely on your session's logind/seatd seat "
        "ACL (uaccess) if your distro's udev rules grant it dynamically "
        "instead of the group. Never chmod 666 the device.";
}

// Runs on the capture thread. Owns `fd`/`dev` for the duration of the
// call and releases both before returning. `stoken` provides cooperative,
// non-blocking cancellation: poll() times out every 200ms so shutdown is
// never more than ~200ms away even under a quiet keyboard.
void capture_loop(std::stop_token stoken, int fd, libevdev* dev, KeyEventTransport& transport) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    bool disconnected = false;
    while (!stoken.stop_requested() && !disconnected) {
        int pret = poll(&pfd, 1, 200 /*ms*/);
        if (pret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pret == 0) continue; // timeout, loop back to check stop_requested()

        input_event ev;
        int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        while (!stoken.stop_requested() &&
               (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC)) {
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                while (!stoken.stop_requested() && rc == LIBEVDEV_READ_STATUS_SYNC) {
                    rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
                }
                continue;
            }

            if (ev.type == EV_KEY) {
                KeyEvent ke;
                ke.code = static_cast<uint16_t>(ev.code);
                ke.kind = classify_key_value(ev.value);
                ke.ts_ns = timeval_to_ns(ev.time);
                transport.try_push(ke); // overflow policy + counter live inside the transport
            }
            rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        }

        if (rc == -ENODEV) {
            disconnected = true; // no console logging from a background
                                  // thread's steady-state path; caller
                                  // observes this only via thread exit.
        }
        // rc == -EAGAIN just means "no more events right now", expected.
    }

    libevdev_free(dev);
    close(fd);
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
}

std::expected<void, std::string> InputCapture::start(std::string device_path, KeyEventTransport& transport) {
    // Constructing a replacement jthread before joining the old one would
    // briefly give the SPSC queue two producers. Lifecycle calls are owner-only.
    if (thread_.joinable()) return std::unexpected("input capture already started; stop it first");

    int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == EACCES) {
            return std::unexpected(permission_denied_message(device_path));
        }
        return std::unexpected("cannot open " + device_path + ": " + std::strerror(errno));
    }

    libevdev* dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
        close(fd);
        return std::unexpected("libevdev_new_from_fd failed for " + device_path);
    }

    // This device's default evdev clock is CLOCK_REALTIME on this kernel
    // (confirmed empirically in Step 1.4), not CLOCK_MONOTONIC.
    // EVIOCSCLOCKID requests monotonic timestamps for this fd alone,
    // without affecting any other reader of the same device.
    int clockid = CLOCK_MONOTONIC;
    if (ioctl(fd, EVIOCSCLOCKID, &clockid) < 0) {
        libevdev_free(dev);
        close(fd);
        return std::unexpected("EVIOCSCLOCKID(CLOCK_MONOTONIC) failed for " + device_path +
                                ": " + std::strerror(errno));
    }

    // Deliberately never call libevdev_grab(): passive observer only, so
    // every other client of this device keeps receiving every event
    // exactly as before.

    thread_ = std::jthread(capture_loop, fd, dev, std::ref(transport));
    return {};
}

} // namespace keeby
