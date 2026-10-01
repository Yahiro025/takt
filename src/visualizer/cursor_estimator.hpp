#pragma once

// Pure cursor-position estimator for keeby-visualizer's follow-cursor mode.
// Wayland gives a client no global pointer position (by design), but niri
// moves its own pointer by exactly the same (already-accelerated) delta
// libinput reports for each event, one event at a time, then clamps to the
// output's pixel range (niri src/input/mod.rs:2405 and :2506-2511,
// loc..loc+size-1). This class mirrors that exactly: every Motion message
// applies as a 1:1 scaled delta (follow_speed is a pure calibration
// multiplier -- 1.0 means exact), clamped the same way after every single
// delta, never after a batch (clamping a summed batch can land somewhere
// niri's own per-event clamp never would -- see
// tests/visualizer_test.cpp's per-event-vs-batch case). anchor() corrects
// any remaining error in one shot from a real, absolute position (see
// cursor_probe.hpp / viz_app.cpp's pointer-enter probe). GTK/Wayland-free
// so it's directly unit-testable (tests/visualizer_test.cpp).

#include <algorithm>
#include <cstdint>

namespace keeby::viz {

struct Bounds {
    double min_x = 0.0, min_y = 0.0, max_x = 1920.0, max_y = 1200.0;
};

class CursorEstimator {
public:
    explicit CursorEstimator(Bounds bounds = {}) : bounds_(bounds) { recenter(); }

    // Single-output local bounds: pass the layer surface's own 0..width /
    // 0..height rect (viz_app.cpp's sync_surface_bounds). This class only
    // ever clamps to whatever single Bounds it's given.
    void set_bounds(Bounds b) {
        bounds_ = b;
        clamp();
    }
    void recenter() {
        x_ = (bounds_.min_x + bounds_.max_x) / 2.0;
        y_ = (bounds_.min_y + bounds_.max_y) / 2.0;
    }
    void set_follow_speed(double speed) { follow_speed_ = speed; }

    // dx/dy: one Motion message's already-accelerated delta (the same
    // units niri itself moves its pointer by), applied 1:1 times
    // follow_speed and clamped immediately. Callers must feed one message
    // at a time -- never pre-sum several before calling this (see the
    // class comment above).
    void accumulate(double dx, double dy) {
        x_ += dx * follow_speed_;
        y_ += dy * follow_speed_;
        clamp();
    }

    // Sets the estimate to a known-exact position (from a successful
    // absolute-position probe) and marks it anchored. Still clamped, so a
    // stale/mismatched surface size can't strand it outside the bounds.
    void anchor(double x, double y) {
        x_ = x;
        y_ = y;
        clamp();
        anchored_ = true;
    }
    bool anchored() const { return anchored_; }
    // The estimate is no longer trustworthy even though it was anchored
    // before (e.g. motion stopped being delivered for a while -- see
    // viz_app.cpp's Config handling) -- clears the flag so a probe policy
    // checking anchored() doesn't skip re-probing as "already fresh". Does
    // not touch x()/y(): the frozen value is still the best guess we have
    // until the next probe or accumulate() replaces it.
    void invalidate() { anchored_ = false; }

    double x() const { return x_; }
    double y() const { return y_; }
    const Bounds& bounds() const { return bounds_; }

private:
    void clamp() {
        // niri clamps to loc..loc+size-1 (mod.rs:2506-2511): the highest
        // valid coordinate is one pixel inside the far edge, not the
        // width/height itself. std::max guards the degenerate (sub-1px)
        // bounds case where max - 1 would otherwise invert the range.
        const double x_hi = std::max(bounds_.min_x, bounds_.max_x - 1.0);
        const double y_hi = std::max(bounds_.min_y, bounds_.max_y - 1.0);
        x_ = std::clamp(x_, bounds_.min_x, x_hi);
        y_ = std::clamp(y_, bounds_.min_y, y_hi);
    }

    Bounds bounds_;
    double follow_speed_ = 1.0;
    double x_ = 0.0, y_ = 0.0;
    bool anchored_ = false;
};

} // namespace keeby::viz
