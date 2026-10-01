// Unit tests for keeby-visualizer's pure logic: the layout table, message
// validation, the dismiss-timer state machine, position<->anchor mapping,
// the cursor estimator, the spring/tilt, and the ripple effect. No
// display, no D-Bus, no GTK.

#include "../src/visualizer/cursor_estimator.hpp"
#include "../src/visualizer/cursor_probe.hpp"
#include "../src/visualizer/viz_dismiss.hpp"
#include "../src/visualizer/viz_layout.hpp"
#include "../src/visualizer/viz_position.hpp"
#include "../src/visualizer/viz_ripple.hpp"
#include "../src/visualizer/viz_spring.hpp"
#include "../src/visualizer_wire.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <linux/input-event-codes.h>
#include <set>
#include <vector>

using namespace keeby::viz;
using namespace std::chrono;

namespace {

void test_layout_table() {
    // Every entry has a non-degenerate rect within the panel bounds.
    for (const auto& r : kLayout) {
        assert(r.w > 0.0f && r.h > 0.0f);
        assert(r.x >= 0.0f && r.x + r.w <= kLayoutWidthUnits + 1e-3f);
        assert(r.y >= 0.0f && r.y + r.h <= kLayoutHeightUnits + 1e-3f);
    }
    // Codes are unique.
    std::set<uint16_t> codes;
    for (const auto& r : kLayout) assert(codes.insert(r.code).second);
    // Standard keys are present.
    for (uint16_t code : {KEY_A, KEY_Z, KEY_SPACE, KEY_ENTER, KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_TAB,
                           KEY_CAPSLOCK, KEY_BACKSPACE, KEY_LEFTCTRL, KEY_LEFTMETA, KEY_LEFTALT}) {
        assert(find_key(code) != nullptr);
    }
    assert(find_key(KEY_F1) == nullptr); // function row deliberately not in this compact layout

    // Glyph keys (drawn as cairo paths, never Unicode -- see docs): present
    // and distinct; every other key stays Glyph::None.
    assert(find_key(KEY_BACKSPACE)->glyph == Glyph::Backspace);
    assert(find_key(KEY_LEFT)->glyph == Glyph::ArrowLeft);
    assert(find_key(KEY_RIGHT)->glyph == Glyph::ArrowRight);
    assert(find_key(KEY_A)->glyph == Glyph::None);
    std::printf("test_layout_table: OK\n");
}

void test_message_validation() {
    VizMessage key_down{1, 1, KEY_A, 0, 0, 0};
    VizMessage key_up{1, 0, KEY_A, 0, 0, 0};
    VizMessage bad_kind{1, 2, KEY_A, 0, 0, 0};
    VizMessage cfg{2, 0, 0, 4, 10, 1000};
    VizMessage cfg_follow_cursor{2, 0, 0, static_cast<uint8_t>(Position::FollowCursor), 10, 1000};
    VizMessage cfg_bad_position{2, 0, 0, 7, 10, 1000}; // 7 is out of range now that 6 == FollowCursor
    VizMessage cfg_dismiss_too_low{2, 0, 0, 0, 10, 100};
    VizMessage cfg_dismiss_too_high{2, 0, 0, 0, 10, 6000};
    VizMessage cfg_speed_zero{2, 0, 0, 0, 0, 1000}; // reserved 0 is outside the 1..40 contract
    VizMessage cfg_speed_too_high{2, 0, 0, 0, 41, 1000};
    VizMessage cfg_speed_min{2, 0, 0, 0, 1, 1000};
    VizMessage cfg_speed_max{2, 0, 0, 0, 40, 1000};
    VizMessage unknown_type{9, 0, 0, 0, 0, 0};
    VizMessage motion_mouse{3, 0, 5, 0, 0, static_cast<uint16_t>(-3)};
    VizMessage motion_touchpad{3, 1, 0, 0, 0, 0};
    VizMessage motion_bad_kind{3, 2, 0, 0, 0, 0};

    assert(is_valid_message(key_down));
    assert(is_valid_message(key_up));
    assert(!is_valid_message(bad_kind));
    assert(is_valid_message(cfg));
    assert(is_valid_message(cfg_follow_cursor));
    assert(!is_valid_message(cfg_bad_position));
    assert(!is_valid_message(cfg_dismiss_too_low));
    assert(!is_valid_message(cfg_dismiss_too_high));
    assert(!is_valid_message(cfg_speed_zero));
    assert(!is_valid_message(cfg_speed_too_high));
    assert(is_valid_message(cfg_speed_min));
    assert(is_valid_message(cfg_speed_max));
    assert(!is_valid_message(unknown_type));
    assert(is_valid_message(motion_mouse));
    assert(is_valid_message(motion_touchpad));
    assert(!is_valid_message(motion_bad_kind));

    // Motion dx/dy decode as signed 16-bit, bit-cast from the wire's
    // unsigned fields.
    assert(motion_dx(motion_mouse) == 5);
    assert(motion_dy(motion_mouse) == -3);
    VizMessage motion_roundtrip{};
    set_motion_dx(motion_roundtrip, -300);
    set_motion_dy(motion_roundtrip, 200);
    assert(motion_dx(motion_roundtrip) == -300);
    assert(motion_dy(motion_roundtrip) == 200);

    // Config reserved-byte <-> follow_speed steps of 0.1x (1..40).
    assert(follow_speed_from_step(kDefaultFollowSpeedStep) == kDefaultFollowSpeed);
    assert(follow_speed_from_step(10) == 1.0);
    assert(follow_speed_from_step(1) == 0.1);
    assert(follow_speed_from_step(40) == 4.0);
    assert(follow_speed_to_step(1.0) == 10);
    assert(follow_speed_to_step(2.5) == 25);
    assert(follow_speed_to_step(50.0) == kMaxFollowSpeedStep); // clamped, like the engine's callee-clamps contract
    std::printf("test_message_validation: OK\n");
}

void test_position_anchor_mapping() {
    struct Case {
        uint8_t position;
        VEdge edge;
        HAlign halign;
    };
    const Case cases[] = {
        {0, VEdge::Top, HAlign::Left},      {1, VEdge::Top, HAlign::Center},
        {2, VEdge::Top, HAlign::Right},     {3, VEdge::Bottom, HAlign::Left},
        {4, VEdge::Bottom, HAlign::Center}, {5, VEdge::Bottom, HAlign::Right},
    };
    for (const auto& c : cases) {
        const Anchor a = anchor_for_position(c.position);
        assert(a.edge == c.edge && a.halign == c.halign);
    }
    std::printf("test_position_anchor_mapping: OK\n");
}

void test_dismiss_timer() {
    DismissTimer t(milliseconds(500));
    const auto t0 = steady_clock::now();
    assert(!t.visible()); // never shown yet

    t.on_key(KEY_A, /*down=*/true, t0);
    assert(t.visible());
    assert(t.any_pressed());

    // Still held: ticking well past the dismiss window keeps it visible.
    assert(t.tick(t0 + milliseconds(2000)) == true);

    t.on_key(KEY_A, /*down=*/false, t0 + milliseconds(2000));
    assert(!t.any_pressed());

    // Not yet past dismiss since release.
    assert(t.tick(t0 + milliseconds(2400)) == true);
    // Past dismiss since release -> hides.
    assert(t.tick(t0 + milliseconds(2600)) == false);
    assert(!t.visible());

    // A key not in the layout table still resets the dismiss clock.
    t.on_key(KEY_F13, /*down=*/true, t0 + milliseconds(3000));
    assert(t.visible());
    t.on_key(KEY_F13, /*down=*/false, t0 + milliseconds(3000));
    assert(t.tick(t0 + milliseconds(3000) + milliseconds(499)) == true);
    assert(t.tick(t0 + milliseconds(3000) + milliseconds(501)) == false);

    // Dismiss duration is adjustable at runtime (SetVisualizerDismiss).
    t.set_dismiss(milliseconds(250));
    assert(t.dismiss() == milliseconds(250));

    // touch(): motion never shows a hidden panel...
    DismissTimer t2(milliseconds(500));
    t2.touch(steady_clock::now());
    assert(!t2.visible());
    // ...but while already visible, it extends the dismiss clock exactly
    // like on_key does.
    const auto t3 = steady_clock::now();
    t2.on_key(KEY_A, true, t3);
    t2.on_key(KEY_A, false, t3);
    t2.touch(t3 + milliseconds(400));
    assert(t2.tick(t3 + milliseconds(800)) == true); // still within 500ms of the touch()
    assert(t2.tick(t3 + milliseconds(950)) == false);
    std::printf("test_dismiss_timer: OK\n");
}

void test_follow_speed_to_step_edges() {
    // Huge-but-finite values (which pass ControlService's require_finite)
    // must clamp, never reach the double->long conversion out of range (UB).
    assert(follow_speed_to_step(1e308) == kMaxFollowSpeedStep);
    assert(follow_speed_to_step(std::numeric_limits<double>::max()) == kMaxFollowSpeedStep);
    assert(follow_speed_to_step(std::numeric_limits<double>::infinity()) == kMaxFollowSpeedStep);
    assert(follow_speed_to_step(-std::numeric_limits<double>::infinity()) == kMinFollowSpeedStep);
    assert(follow_speed_to_step(std::numeric_limits<double>::quiet_NaN()) == kMinFollowSpeedStep);
    assert(follow_speed_to_step(50.0) == kMaxFollowSpeedStep);
    assert(follow_speed_to_step(4.0) == kMaxFollowSpeedStep);
    assert(follow_speed_to_step(2.5) == 25);
    assert(follow_speed_to_step(1.0) == kDefaultFollowSpeedStep);
    assert(follow_speed_to_step(0.05) == kMinFollowSpeedStep);
    assert(follow_speed_to_step(0.0) == kMinFollowSpeedStep);
    assert(follow_speed_to_step(-3.0) == kMinFollowSpeedStep);
    assert(follow_speed_from_step(kMaxFollowSpeedStep) == 4.0);
    std::printf("test_follow_speed_to_step_edges: OK\n");
}

void test_cursor_estimator() {
    Bounds b{0.0, 0.0, 1000.0, 800.0};
    CursorEstimator est(b);
    assert(est.x() == 500.0 && est.y() == 400.0); // starts centered
    assert(!est.anchored());

    // 1:1 accumulation: no accel curve, no per-source branch -- exact.
    est.accumulate(10.0, -4.0);
    assert(est.x() == 510.0 && est.y() == 396.0);
    est.accumulate(-5.0, 2.0);
    assert(est.x() == 505.0 && est.y() == 398.0);

    // follow_speed scales displacement exactly linearly (no curve to blur it).
    CursorEstimator slow(b), fast(b);
    slow.set_follow_speed(1.0);
    fast.set_follow_speed(2.0);
    slow.accumulate(5.0, -3.0);
    fast.accumulate(5.0, -3.0);
    assert(fast.x() - 500.0 == 2.0 * (slow.x() - 500.0));
    assert(fast.y() - 400.0 == 2.0 * (slow.y() - 400.0));

    // Exact clamp at [0, w-1]/[0, h-1] -- niri's loc..loc+size-1
    // (mod.rs:2506-2511), not loc..loc+size: the surface is 1000x800, so
    // the highest reachable coordinate is 999/799, never 1000/800.
    CursorEstimator est2(b);
    est2.accumulate(100000.0, 100000.0);
    assert(est2.x() == 999.0 && est2.y() == 799.0);
    est2.accumulate(100000.0, 100000.0); // already pinned: stays put
    assert(est2.x() == 999.0 && est2.y() == 799.0);
    est2.accumulate(-1000000.0, -1000000.0);
    assert(est2.x() == 0.0 && est2.y() == 0.0);

    // anchor() sets the estimate exactly (still clamped) and marks it
    // anchored, overriding whatever accumulate() had drifted to.
    CursorEstimator est3(b);
    est3.accumulate(9999.0, 0.0); // drifts to the clamped edge
    assert(!est3.anchored());
    est3.anchor(321.0, 654.0);
    assert(est3.x() == 321.0 && est3.y() == 654.0);
    assert(est3.anchored());
    est3.anchor(1e9, -1e9); // anchor is still clamped, not a raw teleport
    assert(est3.x() == 999.0 && est3.y() == 0.0);
    est3.invalidate();
    assert(!est3.anchored());
    assert(est3.x() == 999.0 && est3.y() == 0.0); // invalidate() never moves the estimate
    std::printf("test_cursor_estimator: OK\n");
}

void test_per_event_clamp_is_not_batch_clamp() {
    // niri clamps the pointer position after every single motion event
    // (mod.rs:2506-2511), never once per batch -- so summing deltas first
    // and clamping once can land somewhere niri's real cursor never does.
    // This is why the sender (visualizer_service.cpp) forwards one Motion
    // message per libinput event instead of a per-batch sum.
    const Bounds b{0.0, 0.0, 100.0, 100.0}; // center (50, 50); reachable range [0, 99]
    CursorEstimator per_event(b), batched(b);
    per_event.accumulate(80.0, 0.0);  // 50+80=130 -> clamped to 99
    per_event.accumulate(80.0, 0.0);  // already pinned at 99, stays there
    per_event.accumulate(-70.0, 0.0); // 99-70=29
    batched.accumulate(80.0 + 80.0 - 70.0, 0.0); // naive pre-summed delta (+90): 50+90=140 -> clamped to 99
    assert(per_event.x() == 29.0);
    assert(batched.x() == 99.0);
    std::printf("test_per_event_clamp_is_not_batch_clamp: OK\n");
}

void test_probe_policy() {
    // Unanchored + continuous motion for >= the threshold -> probe.
    // should_probe()'s own `now_ms` is independent of when on_motion() was
    // last called (real usage always evaluates it at the same instant as
    // the triggering on_motion() call -- see viz_app.cpp), so one on_motion
    // establishing continuous_since is enough; a second call here would
    // itself be a >= kProbeGapResetMs jump and wrongly reset continuity.
    {
        ProbePolicy p;
        p.on_motion(0);
        assert(!p.should_probe(false, 0, kProbeMinContinuousMs - 1)); // short of the threshold
        assert(p.should_probe(false, 0, kProbeMinContinuousMs));      // now continuous long enough
    }
    // A short stroke that ends before the threshold never probes.
    {
        ProbePolicy p;
        p.on_motion(0);
        p.on_motion(30); // gap 30 < 50: no reset, still continuous since 0
        assert(!p.should_probe(false, 0, 30));
    }
    // A gap >= kProbeGapResetMs resets continuity: a run that starts right
    // at the reset doesn't inherit the time elapsed before it.
    {
        ProbePolicy p;
        p.on_motion(0);
        p.on_motion(kProbeGapResetMs); // gap == 50: resets right here
        assert(!p.should_probe(false, 0, kProbeGapResetMs));
        const int64_t t = kProbeGapResetMs + kProbeMinContinuousMs;
        assert(p.should_probe(false, 0, t)); // continuous for the full threshold since the reset
    }
    // A fresh anchor suppresses probing even with continuous motion.
    {
        ProbePolicy p;
        p.on_motion(0);
        assert(!p.should_probe(/*anchored=*/true, /*anchor_age_ms=*/0, kProbeMinContinuousMs));
        assert(!p.should_probe(true, kProbeStaleMs - 1, kProbeMinContinuousMs));
    }
    // An anchor at or past the stale threshold probes again.
    {
        ProbePolicy p;
        p.on_motion(0);
        assert(p.should_probe(true, kProbeStaleMs, kProbeMinContinuousMs));
        assert(p.should_probe(true, kProbeStaleMs + 1, kProbeMinContinuousMs));
    }
    // A probe already in flight suppresses another, regardless of anything else.
    {
        ProbePolicy p;
        p.on_motion(0);
        assert(p.should_probe(false, 0, kProbeMinContinuousMs));
        p.begin_probe(kProbeMinContinuousMs);
        assert(p.in_flight());
        assert(!p.should_probe(false, 0, kProbeMinContinuousMs + 1));
        assert(!p.should_probe(true, kProbeStaleMs + 1, kProbeMinContinuousMs + 1));
    }
    // Timeout: an in-flight probe past kProbeTimeoutMs is done; end_probe()
    // (called on any resolution -- success, failure, or timeout) clears
    // in_flight so a still-continuous stroke retries right away ("stay
    // pending, retry on a later stroke").
    {
        ProbePolicy p;
        p.on_motion(0);
        p.begin_probe(kProbeMinContinuousMs);
        const int64_t timeout_at = kProbeMinContinuousMs + kProbeTimeoutMs;
        assert(!p.timed_out(timeout_at - 1));
        assert(p.timed_out(timeout_at));
        p.end_probe();
        assert(!p.in_flight());
        assert(p.should_probe(false, 0, timeout_at)); // still continuous since 0: pending, retried now
    }
    std::printf("test_probe_policy: OK\n");
}

void test_spring() {
    Spring1D s(25.0);
    s.reset(0.0);
    for (int i = 0; i < 300; ++i) s.step(100.0, 0.01); // 3s at 100fps
    assert(std::fabs(s.value() - 100.0) < 0.01);        // converges
    assert(std::fabs(s.velocity()) < 0.01);

    // Bounded overshoot: critically damped, so the value should never
    // exceed the target when approaching it from rest below.
    Spring1D s2(25.0);
    s2.reset(0.0);
    double max_value = 0.0;
    for (int i = 0; i < 100; ++i) {
        s2.step(100.0, 0.01);
        max_value = std::max(max_value, s2.value());
    }
    assert(max_value <= 100.0 + 1e-6);

    // Frame-rate independence: one big step should land at (very nearly)
    // the same place as many small steps covering the same total time.
    Spring1D big(25.0), small(25.0);
    big.reset(0.0);
    small.reset(0.0);
    big.step(50.0, 0.1);
    for (int i = 0; i < 10; ++i) small.step(50.0, 0.01);
    assert(std::fabs(big.value() - small.value()) < 1e-6);
    assert(std::fabs(big.velocity() - small.velocity()) < 1e-6);
    std::printf("test_spring: OK\n");
}

void test_tilt() {
    const Tilt zero = tilt_from_velocity(0.0, 0.0);
    assert(zero.rx_deg == 0.0 && zero.ry_deg == 0.0);

    // Bounded.
    const Tilt fast = tilt_from_velocity(100000.0, 100000.0);
    assert(std::fabs(fast.rx_deg) <= 14.0 + 1e-9);
    assert(std::fabs(fast.ry_deg) <= 14.0 + 1e-9);

    // Returns toward 0 as velocity decreases (rx from vertical velocity).
    const Tilt hi = tilt_from_velocity(0.0, 1000.0);
    const Tilt lo = tilt_from_velocity(0.0, 100.0);
    assert(std::fabs(lo.rx_deg) < std::fabs(hi.rx_deg));
    std::printf("test_tilt: OK\n");
}

void test_ripple() {
    // A neighbor sees the ring pass exactly once: brightness rises to a
    // single interior peak as the ring's radius approaches its distance,
    // then falls back -- never a second local maximum.
    RippleEffect r;
    r.trigger(0.0, 0.0);
    const double nx = 1.2; // ~1.2 key-units from the origin
    std::vector<double> samples;
    for (int i = 0; i < 80; ++i) { // 80 * 5ms = 400ms, the full duration
        samples.push_back(r.brightness_at(nx, 0.0));
        r.tick(5.0);
    }
    size_t peak = 0;
    for (size_t i = 1; i < samples.size(); ++i)
        if (samples[i] > samples[peak]) peak = i;
    assert(peak > 0 && peak < samples.size() - 1); // an interior peak: the ring actually swept past
    for (size_t i = 1; i <= peak; ++i) assert(samples[i] + 1e-9 >= samples[i - 1]);
    for (size_t i = peak + 1; i < samples.size(); ++i) assert(samples[i] <= samples[i - 1] + 1e-9);

    // Falloff is monotonic non-increasing with distance from the ring's
    // current radius.
    RippleEffect r2;
    r2.trigger(0.0, 0.0);
    r2.tick(200.0);
    const double radius = 200.0 * kRippleSpeedUnitsPerMs;
    double prev = r2.brightness_at(radius, 0.0); // at the ring itself: local peak
    for (double d = radius + 0.2; d <= radius + 4.0; d += 0.2) {
        const double b = r2.brightness_at(d, 0.0);
        assert(b <= prev + 1e-9);
        prev = b;
    }

    // Fixed pool: far more triggers than kMaxRipples never grows past it
    // (no allocation -- ripples_ is a fixed std::array -- oldest recycled).
    RippleEffect r3;
    for (int i = 0; i < kMaxRipples * 5; ++i) r3.trigger(0.0, 0.0);
    assert(r3.active_count() <= kMaxRipples);
    std::printf("test_ripple: OK\n");
}

void test_two_output_local_confinement() {
    // Two 1920x1080 outputs side by side: the layer surface lives on ONE
    // of them, so every target must stay inside that surface's local
    // 0..1920/0..1080 coords. The old monitor-union bounds (0..3840) put
    // TopRight.x at ~3840 - margin - panel_w, thousands of pixels outside
    // the single-output surface, where it clipped.
    constexpr double kOutW = 1920.0, kOutH = 1080.0;
    constexpr double kPanelW = 15.0 * 18.0 + 2.0 * 10.0; // == viz_app panel_w()
    constexpr double kPanelH = 5.0 * 18.0 + 2.0 * 10.0;  // == viz_app panel_h()
    constexpr double kOffX = 12.0, kOffY = 18.0;         // == viz_app kFollowOffsetX/Y
    const Bounds local = local_bounds_for_surface(kOutW, kOutH);
    assert(local.min_x == 0.0 && local.min_y == 0.0 && local.max_x == kOutW && local.max_y == kOutH);

    // All six fixed anchors stay fully inside the single-output surface.
    for (uint8_t pos = 0; pos <= 5; ++pos) {
        const PanelPt t = fixed_target_in(local, kPanelW, kPanelH, anchor_for_position(pos));
        assert(t.x >= 0.0 && t.x + kPanelW <= kOutW + 1e-9);
        assert(t.y >= 0.0 && t.y + kPanelH <= kOutH + 1e-9);
    }
    // TopRight lands at the right edge of ONE output...
    const PanelPt tr = fixed_target_in(local, kPanelW, kPanelH, anchor_for_position(2));
    assert(std::fabs(tr.x - (kOutW - kMarginPx - kPanelW)) < 1e-9);
    assert(tr.y == kMarginPx);
    // ...while the same anchor computed against the old union bounds would
    // escape the single-output surface (the regressed bug).
    const Bounds onion{0.0, 0.0, 2.0 * kOutW, kOutH};
    const PanelPt tr_union = fixed_target_in(onion, kPanelW, kPanelH, anchor_for_position(2));
    assert(tr_union.x > kOutW);

    // Follow targets stay confined for cursor samples across the whole
    // local surface, including the far edges and corners.
    const double xs[] = {0.0, kOutW / 2.0, kOutW - 1.0, kOutW};
    const double ys[] = {0.0, kOutH / 2.0, kOutH - 1.0, kOutH};
    for (double cx : xs) {
        for (double cy : ys) {
            const PanelPt t = follow_target_in(local, cx, cy, kPanelW, kPanelH, kOffX, kOffY);
            assert(t.x >= 0.0 && t.x <= std::max(0.0, kOutW - kPanelW));
            assert(t.y >= 0.0 && t.y <= std::max(0.0, kOutH - kPanelH));
        }
    }
    // Cursor estimate clamps to the local output edge (w-1, per niri's own
    // loc..loc+size-1), not the union edge.
    CursorEstimator est(local);
    est.accumulate(100000.0, 0.0);
    assert(est.x() == kOutW - 1.0);
    CursorEstimator est_union(onion);
    est_union.accumulate(100000.0, 0.0);
    assert(est_union.x() == 2.0 * kOutW - 1.0); // union lets it drift a full output away
    std::printf("test_two_output_local_confinement: OK\n");
}

} // namespace

int main() {
    test_layout_table();
    test_message_validation();
    test_position_anchor_mapping();
    test_dismiss_timer();
    test_follow_speed_to_step_edges();
    test_cursor_estimator();
    test_per_event_clamp_is_not_batch_clamp();
    test_probe_policy();
    test_two_output_local_confinement();
    test_spring();
    test_tilt();
    test_ripple();
    std::printf("visualizer_test: OK\n");
    return 0;
}
