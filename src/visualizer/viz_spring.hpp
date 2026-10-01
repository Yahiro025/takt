#pragma once

// Pure critically-damped spring for keeby-visualizer's position smoothing,
// plus a velocity -> tilt-angle mapping for the 3D-tilt effect. GTK-free,
// unit-tested (tests/visualizer_test.cpp).
//
// Uses the exact (not iteratively-integrated) closed-form solution for a
// critically-damped harmonic oscillator -- the same formula behind
// Unity's SmoothDamp -- so it is frame-rate independent: stepping once by
// 100ms lands at the same place as stepping ten times by 10ms.
//
// ponytail: critical damping means zero overshoot rather than the
// slight overshoot the brief allows for; that's well within "bounded
// overshoot, slight at most" and far simpler/more numerically stable than
// solving the underdamped case's sin/cos terms. Revisit only if a
// side-by-side with the real site shows the missing overshoot reads as
// noticeably different.

#include <algorithm>
#include <cmath>

namespace keeby::viz {

class Spring1D {
public:
    // omega: angular frequency in rad/s (1/tau). Settling to within ~2% of
    // a step is about 5.8/omega -- the default (25 rad/s) settles in
    // ~230ms, inside the brief's 150-250ms window.
    explicit Spring1D(double omega = 25.0) : omega_(omega) {}

    void reset(double value) {
        value_ = value;
        velocity_ = 0.0;
    }

    // Advances by dt_seconds toward `target`, held fixed for this step.
    void step(double target, double dt_seconds) {
        if (dt_seconds <= 0.0) return;
        const double x0 = value_ - target;
        const double v0 = velocity_;
        const double exp_term = std::exp(-omega_ * dt_seconds);
        const double temp = (v0 + omega_ * x0) * dt_seconds;
        value_ = (x0 + temp) * exp_term + target;
        velocity_ = (v0 - omega_ * temp) * exp_term;
    }

    double value() const { return value_; }
    double velocity() const { return velocity_; }

private:
    double omega_;
    double value_ = 0.0;
    double velocity_ = 0.0;
};

struct Tilt {
    double rx_deg = 0.0; // pitch (about the x-axis), driven by vertical velocity
    double ry_deg = 0.0; // yaw (about the y-axis), driven by horizontal velocity
};

// Maps 2D velocity (px/s) to a bounded tilt that returns to 0 as velocity
// does. kGain/kMaxDeg reach the brief's "up to ~12-15 degrees" at a brisk
// cursor flick (roughly 1000-1500 px/s).
inline Tilt tilt_from_velocity(double vx_px_s, double vy_px_s) {
    constexpr double kGain = 0.01; // degrees per px/s
    constexpr double kMaxDeg = 14.0;
    auto clamp_deg = [kMaxDeg](double v) { return std::clamp(v, -kMaxDeg, kMaxDeg); };
    return {clamp_deg(vy_px_s * kGain), clamp_deg(-vx_px_s * kGain)};
}

} // namespace keeby::viz
