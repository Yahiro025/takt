#include "input_capture.hpp"

#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <atomic>
#include <cassert>
#include <cstdio>

// /dev/null supplies a real owned fd; wrapped evdev supplies a continuously
// readable event batch. This checks lifecycle without accessing a keyboard.
namespace {
int device_token, opened = 0;
std::atomic<unsigned> reads{0}, freed{0};
}
extern "C" {
int __wrap_libevdev_new_from_fd(int, libevdev** dev) {
    ++opened;
    *dev = reinterpret_cast<libevdev*>(&device_token);
    return 0;
}
int __wrap_ioctl(int, unsigned long, ...) { return 0; }
int __wrap_libevdev_next_event(libevdev*, unsigned, input_event* event) {
    *event = {};
    event->type = EV_SYN;
    reads.fetch_add(1, std::memory_order_relaxed);
    return LIBEVDEV_READ_STATUS_SUCCESS; // batch never becomes empty
}
void __wrap_libevdev_free(libevdev*) { freed.fetch_add(1, std::memory_order_relaxed); }
}

int main() {
    keeby::KeyEventTransport transport;
    keeby::InputCapture capture;
    assert(capture.start("/dev/null", transport));
    while (reads.load(std::memory_order_relaxed) < 1000) {}
    assert(!capture.start("/dev/null", transport));
    assert(opened == 1); // reject before constructing a second producer
    capture.stop(); // must stop even though evdev keeps returning events
    capture.stop();
    assert(freed == 1);
    assert(capture.start("/dev/null", transport));
    capture.stop();
    assert(opened == 2 && freed == 2);
    std::puts("input_capture_test: OK (single producer, busy drain shutdown, restart)");
}
