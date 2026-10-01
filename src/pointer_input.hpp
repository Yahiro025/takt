#pragma once

// Pure, testable logic for keeby-inputd's pointer path (see docs/006).
// Nothing here touches a file descriptor, libevdev, libinput, or any
// device, so the mapping and motion helpers need no hardware or privilege.

#include <cmath>
#include <cstdint>
#include <optional>

#include "key_event.hpp"

namespace keeby::pointer {

// Which class of device try_open_pointer() decided a candidate is, per the
// security contract: a relative mouse (EV_REL REL_X + REL_Y) or a touchpad
// (ABS_X/ABS_Y or ABS_MT_POSITION_X/Y, plus BTN_TOOL_FINGER, plus
// INPUT_PROP_POINTER). Anything else -- including absolute tablets
// (BTN_TOOL_FINGER absent, or INPUT_PROP_POINTER absent) and touchscreens
// (INPUT_PROP_DIRECT instead of INPUT_PROP_POINTER) -- is None and must
// never be opened as a pointer.
enum class DeviceKind : uint8_t { None = 0, Mouse = 1, Touchpad = 2 };

struct Capabilities {
    bool rel_x = false;
    bool rel_y = false;
    bool abs_x = false;
    bool abs_y = false;
    bool abs_mt_x = false;
    bool abs_mt_y = false;
    bool btn_tool_finger = false;
    bool prop_pointer = false;
};

constexpr DeviceKind classify(const Capabilities& c) noexcept {
    if (c.rel_x && c.rel_y) return DeviceKind::Mouse;
    if (c.prop_pointer && c.btn_tool_finger && ((c.abs_x && c.abs_y) || (c.abs_mt_x && c.abs_mt_y)))
        return DeviceKind::Touchpad;
    return DeviceKind::None;
}

// Folds `delta` into the fractional `remainder` carried from the previous
// call, truncates the sum toward zero, and carries whatever is left over
// for next time. Used once per libinput LIBINPUT_EVENT_POINTER_MOTION
// event (one call per axis, each with its own remainder): libinput's
// per-event dx/dy are frequently sub-pixel (e.g. 0.3 at low speed), and
// naively rounding each event independently would silently drop that
// motion forever instead of letting it accumulate into a whole-count step
// -- carrying the fraction here means no motion is ever lost, only ever
// delayed by at most one event. Pure, no I/O: unit-tested directly.
inline int32_t carry_and_truncate(double delta, double& remainder) noexcept {
    const double total = delta + remainder;
    const double truncated = std::trunc(total);
    remainder = total - truncated;
    return static_cast<int32_t>(truncated);
}

inline std::optional<KeyEvent> make_button_event(uint32_t button, bool pressed, uint64_t ts_ns) noexcept {
    if (button > BTN_MIDDLE || !is_pointer_button_code(static_cast<uint16_t>(button))) return std::nullopt;
    return KeyEvent{static_cast<uint16_t>(button), pressed ? KeyEventKind::Down : KeyEventKind::Up, ts_ns};
}

} // namespace keeby::pointer
