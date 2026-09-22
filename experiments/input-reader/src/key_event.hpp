#pragma once

#include <cstdint>

#include "ring_buffer.hpp"

namespace keeby {

enum class KeyEventKind : uint8_t { Up = 0, Down = 1, Repeat = 2 };

// Physical Linux key identity only. No text, no layout, no Unicode.
struct KeyEvent {
    uint16_t code = 0;       // linux/input-event-codes.h KEY_* value
    KeyEventKind kind = KeyEventKind::Up;
    uint64_t ts_ns = 0;      // kernel evdev event timestamp, nanoseconds
};

// Shared SPSC channel: input-worker thread produces, PipeWire RT thread consumes.
using EventQueue = SpscRingBuffer<KeyEvent, 256>;

} // namespace keeby
