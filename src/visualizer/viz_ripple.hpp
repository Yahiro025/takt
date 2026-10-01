#pragma once

// Pure "water-drop" ripple effect: each key press starts a ring that
// expands outward (in key units) from that key's center, brightening
// nearby keycaps as the wave front passes over them, then fading. A fixed
// pool of kMaxRipples slots (oldest recycled on overflow) means a key
// press never allocates. GTK-free; unit-tested in
// tests/visualizer_test.cpp.

#include <algorithm>
#include <array>
#include <cmath>

namespace keeby::viz {

inline constexpr int kMaxRipples = 16;
inline constexpr double kRippleDurationMs = 400.0;      // ~350-450ms per the brief
inline constexpr double kRippleSpeedUnitsPerMs = 0.006; // ring radius growth: ~2.4 key units over the duration
inline constexpr double kRippleSigmaUnits = 0.6;        // ring thickness (soft Gaussian falloff)

struct Ripple {
    double origin_x = 0.0, origin_y = 0.0; // key units
    double age_ms = -1.0;                  // negative: slot free
    bool active() const { return age_ms >= 0.0 && age_ms <= kRippleDurationMs; }
};

class RippleEffect {
public:
    // Starts a new ripple at (x, y) in key units. Recycles a free slot if
    // one exists, otherwise the single oldest (largest age_ms) active one.
    // Never allocates: ripples_ is a fixed-size array.
    void trigger(double x, double y) {
        int slot = 0;
        double oldest_age = -1.0;
        bool found_free = false;
        for (int i = 0; i < kMaxRipples && !found_free; ++i) {
            if (!ripples_[i].active()) {
                slot = i;
                found_free = true;
            } else if (ripples_[i].age_ms > oldest_age) {
                oldest_age = ripples_[i].age_ms;
                slot = i;
            }
        }
        ripples_[slot] = Ripple{x, y, 0.0};
    }

    // Advances every active ripple's age; call once per animation tick.
    void tick(double dt_ms) {
        for (auto& r : ripples_)
            if (r.active()) r.age_ms += dt_ms;
    }

    bool any_active() const {
        for (const auto& r : ripples_)
            if (r.active()) return true;
        return false;
    }

    int active_count() const {
        int n = 0;
        for (const auto& r : ripples_) n += r.active() ? 1 : 0;
        return n;
    }

    // Additive brightness (clamped 0..1) the key at (kx, ky) (key units)
    // gets from every active ripple: amplitude (linear 1->0 over the
    // duration) times a Gaussian falloff of the distance between the key
    // and the ring's current radius -- so a key lights up once, as the
    // expanding ring front sweeps past it, then dims again.
    double brightness_at(double kx, double ky) const {
        double sum = 0.0;
        for (const auto& r : ripples_) {
            if (!r.active()) continue;
            const double amplitude = 1.0 - r.age_ms / kRippleDurationMs;
            const double radius = r.age_ms * kRippleSpeedUnitsPerMs;
            const double dist = std::hypot(kx - r.origin_x, ky - r.origin_y);
            const double d = dist - radius;
            const double falloff = std::exp(-(d * d) / (2.0 * kRippleSigmaUnits * kRippleSigmaUnits));
            sum += amplitude * falloff;
        }
        return std::clamp(sum, 0.0, 1.0);
    }

private:
    std::array<Ripple, kMaxRipples> ripples_{};
};

} // namespace keeby::viz
