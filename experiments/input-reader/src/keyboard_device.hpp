#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include "key_event.hpp"

namespace keeby {

// Scans /dev/input/event0..31 for the first device exposing a full
// physical-keyboard key set (KEY_A, KEY_Z, KEY_SPACE, KEY_ENTER). Returns
// nullopt if none found. Does not grab or modify anything.
std::optional<std::string> discover_keyboard_device();

struct KeyboardDeviceStats {
    uint64_t events_read = 0;
    uint64_t events_dropped = 0; // ring buffer was full when pushed
};

// Opens `path` read-only, never grabs it (EVIOCGRAB is never called), and
// pushes normalized KeyEvents into `queue` until `running` becomes false
// or the device disconnects. Meant to run on its own thread. Returns
// false only if the device could not be opened at all.
bool run_keyboard_reader(const std::string& path,
                          EventQueue& queue,
                          const std::atomic<bool>& running,
                          KeyboardDeviceStats& stats);

} // namespace keeby
