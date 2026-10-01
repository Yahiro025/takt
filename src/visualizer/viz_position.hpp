#pragma once

// Pure position <-> anchor mapping and VizMessage validation for
// keeby-visualizer. GTK/layer-shell-free so it's unit-testable without a
// display (tests/visualizer_test.cpp); viz_app.cpp translates `Anchor` into
// actual gtk_layer_set_anchor()/gtk_layer_set_margin() calls.

#include <algorithm>
#include <cstdint>

#include "../visualizer_wire.hpp"
#include "cursor_estimator.hpp"

namespace keeby::viz {

enum class VEdge : uint8_t { Top, Bottom };
enum class HAlign : uint8_t { Left, Center, Right };

struct Anchor {
    VEdge edge;
    HAlign halign;
};

// Position/dismiss-range bounds live in visualizer_wire.hpp (kMinDismissMs/
// kMaxDismissMs, plus the Position enum itself) -- reused here rather than
// duplicated.
inline constexpr uint8_t kMinPosition = static_cast<uint8_t>(Position::TopLeft);
inline constexpr uint8_t kMaxPosition = static_cast<uint8_t>(Position::FollowCursor);
inline constexpr int kMarginPx = 24;

// position: 0 top-left,1 top-center,2 top-right,3 bottom-left,
// 4 bottom-center,5 bottom-right (per src/visualizer_wire.hpp). Returns
// top-left for any out-of-range value -- callers should validate first via
// is_valid_config() if they need to reject garbage instead of clamping it.
// FollowCursor has no fixed anchor (viz_app.cpp positions it from the
// cursor estimate instead); the case exists only so this switch stays
// exhaustive, its value is never used.
constexpr Anchor anchor_for_position(uint8_t position) {
    switch (static_cast<Position>(position)) {
        case Position::TopLeft: return {VEdge::Top, HAlign::Left};
        case Position::TopCenter: return {VEdge::Top, HAlign::Center};
        case Position::TopRight: return {VEdge::Top, HAlign::Right};
        case Position::BottomLeft: return {VEdge::Bottom, HAlign::Left};
        case Position::BottomCenter: return {VEdge::Bottom, HAlign::Center};
        case Position::BottomRight: return {VEdge::Bottom, HAlign::Right};
        case Position::FollowCursor: return {VEdge::Bottom, HAlign::Center};
    }
    return {VEdge::Top, HAlign::Left}; // out-of-range: caller should validate first
}

constexpr bool is_valid_type(uint8_t type) {
    return type == static_cast<uint8_t>(MessageType::Key) || type == static_cast<uint8_t>(MessageType::Config) ||
           type == static_cast<uint8_t>(MessageType::Motion);
}

constexpr bool is_valid_follow_speed_step(uint8_t step) {
    return step >= kMinFollowSpeedStep && step <= kMaxFollowSpeedStep;
}

// Malformed/unknown messages (per the spec) must be ignored, never acted on.
constexpr bool is_valid_message(const VizMessage& m) {
    if (!is_valid_type(m.type)) return false;
    if (m.type == static_cast<uint8_t>(MessageType::Key)) return m.kind == 0 || m.kind == 1;
    if (m.type == static_cast<uint8_t>(MessageType::Motion)) return m.kind == 0 || m.kind == 1;
    // type == Config: code/dx/dy fields don't apply; dismiss_ms and
    // position and reserved (follow_speed) all get range-checked.
    return m.position >= kMinPosition && m.position <= kMaxPosition && m.dismiss_ms >= kMinDismissMs &&
           m.dismiss_ms <= kMaxDismissMs && is_valid_follow_speed_step(m.reserved);
}

// Surface-local placement targets (GTK-free). The anchored layer-shell
// surface spans exactly one output, so every target lives in that
// surface's local 0..width/0..height coordinates -- never the union of
// all monitors (which placed fixed anchors thousands of pixels outside a
// single-output surface on multi-monitor layouts). viz_app.cpp derives
// the local Bounds from the live surface allocation (falling back to the
// compositor-assigned monitor before first map) and delegates its
// fixed/follow targets to these helpers.
struct PanelPt {
    double x, y;
};

inline Bounds local_bounds_for_surface(double width_px, double height_px) {
    return {0.0, 0.0, width_px, height_px};
}

inline PanelPt fixed_target_in(const Bounds& local, double panel_w, double panel_h, Anchor a) {
    const double cw = local.max_x - local.min_x, ch = local.max_y - local.min_y;
    const double x = a.halign == HAlign::Left     ? kMarginPx
                     : a.halign == HAlign::Right ? cw - kMarginPx - panel_w
                                                : (cw - panel_w) / 2.0;
    const double y = a.edge == VEdge::Top ? kMarginPx : ch - kMarginPx - panel_h;
    return {x, y};
}

inline PanelPt follow_target_in(const Bounds& local, double cursor_x, double cursor_y, double panel_w,
                                double panel_h, double offset_x, double offset_y) {
    const double cw = local.max_x - local.min_x, ch = local.max_y - local.min_y;
    double x = (cursor_x - local.min_x) + offset_x;
    double y = (cursor_y - local.min_y) + offset_y;
    x = std::clamp(x, 0.0, std::max(0.0, cw - panel_w));
    y = std::clamp(y, 0.0, std::max(0.0, ch - panel_h));
    return {x, y};
}

} // namespace keeby::viz
