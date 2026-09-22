#pragma once

#include <expected>
#include <optional>
#include <string>
#include <thread>

#include "event_transport.hpp"
#include "key_event.hpp"

namespace keeby {

// Scans /dev/input/event0..31 for the first device exposing a full
// physical-keyboard key set (KEY_A, KEY_Z, KEY_SPACE, KEY_ENTER). Returns
// nullopt if none found. Read-only probing, never grabs or modifies
// anything. Single-keyboard heuristic only (see key_event.hpp).
std::optional<std::string> discover_keyboard_device();

// Owns a background capture thread reading one physical keyboard device
// via libevdev. Never calls EVIOCGRAB: this is a passive observer, every
// other reader of the device (the compositor, the focused application)
// keeps receiving every event untouched.
//
// start() synchronously opens and validates the device before returning,
// so failures are surfaced immediately rather than discovered later on
// a background thread. Only once that succeeds does the capture thread
// start running.
class InputCapture {
public:
    ~InputCapture();

    // Not copyable or movable: owns a running thread and an open fd.
    InputCapture() = default;
    InputCapture(const InputCapture&) = delete;
    InputCapture& operator=(const InputCapture&) = delete;

    // start/stop are serialized by the owning thread. Stop before restarting.
    // Opens `device_path`, verifies it, and starts the capture thread,
    // which pushes KeyEvents into `transport` (which must outlive this
    // object) until stop() is called or the device disconnects. Returns
    // an error message (not thrown, not swallowed) on failure to open
    // or initialize the device — including a permission-denied message
    // naming the narrowest fix — instead of the caller finding out from
    // silence.
    std::expected<void, std::string> start(std::string device_path, KeyEventTransport& transport);

    // Requests the capture thread stop and joins it. Safe to call
    // multiple times, and called automatically by the destructor.
    void stop();

private:
    std::jthread thread_;
};

} // namespace keeby
