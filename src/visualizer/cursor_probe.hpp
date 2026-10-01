#pragma once

// Pure decision policy for keeby-visualizer's absolute-cursor-position
// probe (see viz_app.cpp): flipping the overlay's input region open for a
// moment lets niri deliver a real wl_pointer.enter with an exact
// surface-local position (niri re-hit-tests pointer focus every event-loop
// tick after a client commits -- niri src/niri.rs:1022-1098), which
// CursorEstimator then applies via anchor(), replacing the accumulated
// estimate with ground truth. Probing is disruptive (it can briefly steal a
// click) so it must be rare and only while safe:
//   - never on a key press: the user's hide-when-typing setting hides the
//     real pointer on every keystroke, and niri demotes it to Disabled with
//     no focus if content changes while hidden (niri.rs:1070-1081) -- the
//     pointer is only Visible while it's moving.
//   - only once motion has been flowing continuously for a bit, so a click
//     mid-stroke can't be swallowed by a probe that starts right under it.
//   - never two probes at once; a fresh anchor skips probing entirely.
//     Staleness alone (kProbeStaleMs) is what eventually re-triggers one
//     even with no other reason to.
//
// Deliberately does not store the estimator's anchored/anchor-age state
// itself (see should_probe's parameters) -- that lives with the position
// data in CursorEstimator/AppState, so there is exactly one place that
// tracks it. Clock-free: every method takes the caller's own monotonic
// milliseconds, so this is directly unit-testable (tests/visualizer_test.cpp)
// without waiting on a real clock.

#include <cstdint>

namespace keeby::viz {

inline constexpr int64_t kProbeMinContinuousMs = 60;      // motion must flow this long, unbroken, before probing
inline constexpr int64_t kProbeGapResetMs = 50;            // an inter-message gap >= this restarts continuity
inline constexpr int64_t kProbeStaleMs = 10 * 60 * 1000;   // re-probe even if anchored, once it's this old
inline constexpr int64_t kProbeTimeoutMs = 150;            // abandon an in-flight probe after this long

class ProbePolicy {
public:
    // Call once per received Motion message (any device kind), with the
    // caller's monotonic timestamp in milliseconds.
    void on_motion(int64_t now_ms) {
        if (!has_last_motion_ || (now_ms - last_motion_ms_) >= kProbeGapResetMs) continuous_since_ms_ = now_ms;
        last_motion_ms_ = now_ms;
        has_last_motion_ = true;
    }

    // anchored/anchor_age_ms describe CursorEstimator's own anchor state
    // (see cursor_estimator.hpp's anchor()/anchored()) -- this class only
    // ever decides timing, never stores position/anchor state itself.
    bool should_probe(bool anchored, int64_t anchor_age_ms, int64_t now_ms) const {
        if (in_flight_) return false;
        if (anchored && anchor_age_ms < kProbeStaleMs) return false;
        if (!has_last_motion_) return false;
        return (now_ms - continuous_since_ms_) >= kProbeMinContinuousMs;
    }

    void begin_probe(int64_t now_ms) {
        in_flight_ = true;
        probe_started_ms_ = now_ms;
    }
    bool timed_out(int64_t now_ms) const { return in_flight_ && (now_ms - probe_started_ms_) >= kProbeTimeoutMs; }

    // Call once the probe resolves -- success, failure, or timeout, it
    // doesn't matter which: either way it's no longer in flight. On
    // success the caller separately calls CursorEstimator::anchor() and
    // records the anchor time; on failure it does neither, so the next
    // continuous stroke can retry ("stay pending").
    void end_probe() { in_flight_ = false; }

    bool in_flight() const { return in_flight_; }

private:
    bool has_last_motion_ = false;
    int64_t last_motion_ms_ = 0;
    int64_t continuous_since_ms_ = 0;

    bool in_flight_ = false;
    int64_t probe_started_ms_ = 0;
};

} // namespace keeby::viz
