#pragma once

#include <cstdint>
#include <type_traits>

namespace keeby::wire {

// Explicit, versioned wire format between keeby-inputd (privileged helper)
// and InputCapture (unprivileged parent), sent one-per-datagram over an
// AF_UNIX SOCK_SEQPACKET socket. Deliberately NOT the internal KeyEvent
// struct: this is a separate, stable, minimal ABI contract that must not
// silently change just because an internal type does.
//
// `type == Ready` is sent exactly once, after the helper has opened and
// validated the device and permanently dropped its input-group privilege,
// before it sends anything else — the parent's start() waits for this (or
// EOF/timeout) to give the same synchronous "start() surfaces failure
// immediately" contract InputCapture has always had. `type == KeyEvent` is
// everything after that. `type == PointerMotion` (added for the visualizer's
// cursor-following estimate -- see docs/006's "Pointer motion for the
// visualizer" section and docs/013-visualizer.md) is interleaved with
// KeyEvent messages on the same socket, distinguished only by `type`;
// InputCapture sends keyboard events to both audio and visualizer, and
// allowlisted pointer-button events to audio only (see input_capture.cpp).
// PointerMotion stays on the visualizer tap only. There is no explicit
// disconnect message: EOF on the socket (helper exited, for any reason) is
// the sole "gone" signal, keeping the protocol one-way (helper -> parent)
// with nothing to reject or misuse on the helper's read side, since the
// helper never reads from this socket at all after startup.
enum class MessageType : uint8_t { Ready = 0, KeyEvent = 1, PointerMotion = 2 };

// PointerMotion's `kind`: which class of device this sample came from.
enum class PointerKind : uint8_t { Mouse = 0, Touchpad = 1 };

struct Message {
    uint8_t type = static_cast<uint8_t>(MessageType::Ready);
    // KeyEvent: 0=Up,1=Down,2=Repeat. PointerMotion: PointerKind (0=mouse,
    // 1=touchpad). Unused (0) for Ready.
    uint8_t kind = 0;
    uint16_t code = 0;     // valid only when type == KeyEvent: raw evdev KEY_* or BTN_* code
    // KeyEvent/Ready: always zero. PointerMotion: one summed (dx, dy) sample
    // per SYN_REPORT, each clamped to [-32767, 32767] and packed as two
    // little-endian-in-value int16 halves -- low 16 bits dx, high 16 bits
    // dy (see pack_motion()/unpack_motion() below). Mouse: raw REL_X/REL_Y
    // counts. Touchpad: converted to mouse-equivalent units (mm *
    // 1000/25.4) from the one-finger position delta -- never raw device
    // units, so both device classes land in the same scale for the
    // overlay's cursor estimate.
    uint32_t reserved = 0;
    uint64_t ts_ns = 0;    // valid only when type == KeyEvent/PointerMotion: CLOCK_MONOTONIC nanoseconds
};
static_assert(sizeof(Message) == 16, "wire::Message size must stay a fixed, explicit ABI");
static_assert(std::is_trivially_copyable_v<Message>);

// Clamps to the packable range [-32767, 32767] (symmetric -- avoids the
// int16 asymmetry at -32768 so pack/unpack round-trips cleanly either way).
// Pure, no I/O: unit-tested directly.
inline int16_t clamp_motion_component(double v) noexcept {
    if (v > 32767.0) return 32767;
    if (v < -32767.0) return -32767;
    // Values in range always fit in a long after truncation-toward-zero
    // rounding below; +/-0.5 is handled by the caller's accumulation, not
    // here -- this just rounds to the nearest integer count.
    const double rounded = v < 0.0 ? v - 0.5 : v + 0.5;
    return static_cast<int16_t>(rounded);
}

// Packs two already-clamped int16 halves into Message::reserved: low 16
// bits dx, high 16 bits dy. Pure bit manipulation, no I/O.
inline uint32_t pack_motion(int16_t dx, int16_t dy) noexcept {
    return (static_cast<uint32_t>(static_cast<uint16_t>(dx))) |
           (static_cast<uint32_t>(static_cast<uint16_t>(dy)) << 16);
}

inline void unpack_motion(uint32_t reserved, int16_t& dx, int16_t& dy) noexcept {
    dx = static_cast<int16_t>(static_cast<uint16_t>(reserved & 0xFFFFu));
    dy = static_cast<int16_t>(static_cast<uint16_t>((reserved >> 16) & 0xFFFFu));
}

inline constexpr const char* kHelperName = "keeby-inputd";
// Test-only override for which helper binary InputCapture spawns. Never
// crosses a privilege boundary by itself: whatever this points to still
// runs as the same unprivileged caller unless IT separately carries
// setgid-input, which requires root to set up — see
// docs/006-step-2.5-security-permissions.md's security audit.
inline constexpr const char* kHelperPathOverrideEnv = "KEEBY_INPUTD_PATH";

} // namespace keeby::wire
