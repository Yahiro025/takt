#pragma once

#include <cstdint>
#include <type_traits>

namespace keeby {

// press/release, and repeat when the source distinguishes it (evdev does,
// via EV_KEY value 2 — see docs/002-step-2.1-input-audio-boundary.md).
enum class KeyEventKind : uint8_t { Up = 0, Down = 1, Repeat = 2 };

// Compact, trivially-copyable, real-time-path-safe. No strings, no heap
// ownership, no UI state — nothing here may ever require an allocation
// or a destructor call.
//
// Assumptions about `code`:
//   - it is a raw Linux `KEY_*` constant from linux/input-event-codes.h,
//     i.e. a physical key identity, not a layout-mapped character;
//   - it is produced below the XKB/layout-translation layer, so it is
//     stable across keyboard layouts by construction;
//   - it is NOT unique across distinct physical keyboard devices — if
//     multiple keyboards are ever captured concurrently, disambiguating
//     which physical device an event came from needs a separate field,
//     out of scope for this step (single-device capture only).
struct KeyEvent {
    uint16_t code = 0;
    KeyEventKind kind = KeyEventKind::Up;
    // Monotonic nanoseconds. Only meaningful if the source fd had
    // EVIOCSCLOCKID(CLOCK_MONOTONIC) applied — see input_capture.cpp.
    uint64_t ts_ns = 0;
};

// Maps a raw evdev EV_KEY value to a KeyEventKind. This is the entire
// press/release/repeat state machine: 0/1 map directly, anything else
// (in practice only 2, "repeat") maps to Repeat.
constexpr KeyEventKind classify_key_value(int value) noexcept {
    switch (value) {
        case 0: return KeyEventKind::Up;
        case 1: return KeyEventKind::Down;
        default: return KeyEventKind::Repeat;
    }
}

static_assert(sizeof(KeyEvent) <= 16, "KeyEvent must stay small enough to copy cheaply in the RT path");
static_assert(std::is_trivially_copyable_v<KeyEvent>, "KeyEvent must stay trivially copyable for the lock-free transport");
static_assert(std::is_trivially_destructible_v<KeyEvent>, "KeyEvent must never own a resource needing a real destructor");

} // namespace keeby
