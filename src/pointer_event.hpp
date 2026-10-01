#pragma once

#include <cstdint>
#include <type_traits>

#include "event_transport.hpp"

namespace keeby {

enum class PointerDeviceKind : uint8_t { Mouse = 0, Touchpad = 1 };

// Decoded, already-clamped motion sample -- the non-RT-thread analogue of
// KeyEvent, carried from InputCapture's forwarding thread into
// VisualizerService's second transport (see input_capture.cpp,
// visualizer_service.cpp). Never touches AudioBoundary or the RT path.
struct PointerMotionEvent {
    int16_t dx = 0;
    int16_t dy = 0;
    PointerDeviceKind kind = PointerDeviceKind::Mouse;
    uint64_t ts_ns = 0;
};
static_assert(std::is_trivially_copyable_v<PointerMotionEvent>);
static_assert(std::is_trivially_destructible_v<PointerMotionEvent>);

// Same sizing rationale as KeyEventTransport (event_transport.hpp): no
// evidence yet this needs to be larger or smaller, revisit if it does.
using PointerMotionTransport = EventTransport<PointerMotionEvent, 256>;

} // namespace keeby
