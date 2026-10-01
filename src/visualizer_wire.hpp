#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace keeby::viz {

// Wire format between keeby (this process) and keeby-visualizer (the
// on-screen keyboard overlay it spawns as its own child), sent one-per-
// datagram over a private AF_UNIX SOCK_SEQPACKET socket -- never D-Bus,
// never a named/pathname socket another process could connect to. See
// docs/013-visualizer.md for the full security design.
//
// Fixed 8-byte layout, explicit and stable regardless of internal types.
struct VizMessage {
    uint8_t type;         // MessageType
    // type==Key: 1 down, 0 up (repeat never sent). type==Motion: kind ==
    // PointerKind seen on the wire (0 mouse, 1 touchpad) -- see
    // input_wire.hpp.
    uint8_t kind;
    // type==Key: raw evdev KEY_* code. type==Motion: dx, as an int16
    // bit-cast into this field (see motion_dx()/motion_dy() below).
    uint16_t code;
    uint8_t position;     // type==Config only: Position
    // type==Config only: follow-speed in steps of 0.1x, 1..40 (0.1x..4.0x),
    // default 10 (1.0x) -- see kDefaultFollowSpeedStep/follow_speed_from_step()
    // below. Always zero for every other type.
    uint8_t reserved = 0;
    // type==Config: dismiss_ms, 250..5000. type==Motion: dy, as an int16
    // bit-cast into this field.
    uint16_t dismiss_ms;
};
static_assert(sizeof(VizMessage) == 8, "VizMessage must stay a fixed 8-byte wire ABI");
static_assert(std::is_trivially_copyable_v<VizMessage>);

enum class MessageType : uint8_t { Key = 1, Config = 2, Motion = 3 };

// Reads/writes a Motion message's dx/dy, which travel bit-cast (not
// value-cast) into the `code`/`dismiss_ms` fields so the full int16 range
// survives the trip through two uint16 wire fields.
inline int16_t motion_dx(const VizMessage& m) noexcept { return static_cast<int16_t>(m.code); }
inline int16_t motion_dy(const VizMessage& m) noexcept { return static_cast<int16_t>(m.dismiss_ms); }
inline void set_motion_dx(VizMessage& m, int16_t dx) noexcept { m.code = static_cast<uint16_t>(dx); }
inline void set_motion_dy(VizMessage& m, int16_t dy) noexcept { m.dismiss_ms = static_cast<uint16_t>(dy); }

enum class Position : uint8_t {
    TopLeft = 0,
    TopCenter = 1,
    TopRight = 2,
    BottomLeft = 3,
    BottomCenter = 4,
    BottomRight = 5,
    // Estimated-cursor-following overlay position (docs/006's "Pointer
    // motion for the visualizer" section): Wayland gives no global cursor
    // position, so this tracks an estimate built from forwarded mouse/
    // touchpad motion deltas, not a real query. The default position.
    FollowCursor = 6,
};

inline constexpr Position kDefaultPosition = Position::FollowCursor;
inline constexpr uint16_t kDefaultDismissMs = 1000;
inline constexpr uint16_t kMinDismissMs = 250;
inline constexpr uint16_t kMaxDismissMs = 5000;

inline constexpr uint8_t kPositionCount = 7;
inline constexpr const char* kPositionStrings[kPositionCount] = {
    "top-left", "top-center", "top-right", "bottom-left", "bottom-center", "bottom-right", "follow-cursor",
};

inline const char* to_string(Position p) noexcept {
    const auto i = static_cast<uint8_t>(p);
    return i < kPositionCount ? kPositionStrings[i] : kPositionStrings[static_cast<uint8_t>(kDefaultPosition)];
}

inline std::optional<Position> position_from_string(std::string_view s) noexcept {
    for (uint8_t i = 0; i < kPositionCount; ++i)
        if (s == kPositionStrings[i]) return static_cast<Position>(i);
    return std::nullopt;
}

// Config's `reserved` byte: follow-speed in steps of 0.1x, clamped to
// [1, 40] (0.1x .. 4.0x). Step 10 == 1.0x == the default. Pure, testable
// conversions shared by every writer/reader of the Config message.
inline constexpr uint8_t kMinFollowSpeedStep = 1;
inline constexpr uint8_t kMaxFollowSpeedStep = 40;
inline constexpr uint8_t kDefaultFollowSpeedStep = 10;
inline constexpr double kDefaultFollowSpeed = 1.0;

inline uint8_t follow_speed_to_step(double speed) noexcept {
    if (!(speed > 0.0)) return kMinFollowSpeedStep; // handles NaN and <=0 alike
    // Clamp in double before scaling: speed * 10.0 on a huge-but-finite
    // value (e.g. 1e308, which passes ControlService's require_finite) would
    // overflow to +inf, and converting that to long below is UB.
    double clamped = speed;
    if (clamped < 0.1) clamped = 0.1;
    if (clamped > 4.0) clamped = 4.0;
    long step = static_cast<long>(clamped * 10.0 + 0.5);
    if (step < kMinFollowSpeedStep) step = kMinFollowSpeedStep;
    if (step > kMaxFollowSpeedStep) step = kMaxFollowSpeedStep;
    return static_cast<uint8_t>(step);
}

inline double follow_speed_from_step(uint8_t step) noexcept {
    if (step < kMinFollowSpeedStep) step = kMinFollowSpeedStep;
    if (step > kMaxFollowSpeedStep) step = kMaxFollowSpeedStep;
    return static_cast<double>(step) / 10.0;
}

// Helper binary name/env override, mirroring wire::kHelperName /
// wire::kHelperPathOverrideEnv in input_wire.hpp for keeby-inputd. Never
// crosses a privilege boundary: keeby-visualizer runs as this same
// unprivileged user, spawned with argv only (no shell) -- see
// visualizer_service.hpp.
inline constexpr const char* kHelperName = "keeby-visualizer";
inline constexpr const char* kHelperPathOverrideEnv = "KEEBY_VISUALIZER_PATH";

} // namespace keeby::viz
