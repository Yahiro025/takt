// Pure-logic unit tests for keeby-settings: brand grouping, deterministic
// switch color, tone-pad coordinate mapping/clamping, and update throttling.
// No display, no D-Bus, no GTK -- runs anywhere.

#include "../src/ui/settings_logic.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace keeby::ui;

namespace {

bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

void test_brand_of() {
    assert(brand_of("Cherry MX Blue") == "Cherry");
    assert(brand_of("Gateron") == "Gateron");
    assert(brand_of("Default (built-in)") == "Default (built-in)");
}

void test_grouping() {
    std::vector<ProfileEntry> profiles = {
        {"gateron_yellow", "Gateron Yellow"},
        {"cherry_blue", "Cherry MX Blue"},
        {"default", "Default (built-in)"},
        {"cherry_red", "Cherry MX Red"},
    };
    const auto groups = group_profiles_by_brand(profiles);
    assert(groups.size() == 3);
    assert(groups[0].brand == "Default (built-in)");
    assert(groups[0].profiles.size() == 1);
    assert(groups[1].brand == "Cherry");
    assert(groups[1].profiles.size() == 2);
    assert(groups[1].profiles[0].id == "cherry_blue"); // input order preserved within group
    assert(groups[1].profiles[1].id == "cherry_red");
    assert(groups[2].brand == "Gateron");
}

void test_color_for_id() {
    const std::string c1 = color_for_id("cherry_blue");
    const std::string c2 = color_for_id("cherry_blue");
    assert(c1 == c2); // deterministic
    assert(c1.size() == 7 && c1[0] == '#');
    const std::string c3 = color_for_id("gateron_yellow");
    assert(c1 != c3); // this pair happens to land on different palette slots
}

void test_tone_mapping() {
    const Tone top_left = pixel_to_tone(0, 0, 100, 100);
    assert(approx(top_left.x, -1.0) && approx(top_left.y, 1.0));

    const Tone bottom_right = pixel_to_tone(100, 100, 100, 100);
    assert(approx(bottom_right.x, 1.0) && approx(bottom_right.y, -1.0));

    const Tone center = pixel_to_tone(50, 50, 100, 100);
    assert(approx(center.x, 0.0) && approx(center.y, 0.0));

    // Overshoot clamps rather than extrapolating.
    const Tone overshoot = pixel_to_tone(-1000, -1000, 100, 100);
    assert(approx(overshoot.x, -1.0) && approx(overshoot.y, 1.0));

    // Round trip.
    const auto [px, py] = tone_to_pixel(Tone{0.5, -0.25}, 100, 100);
    const Tone back = pixel_to_tone(px, py, 100, 100);
    assert(approx(back.x, 0.5) && approx(back.y, -0.25));

    // tone_to_pixel clamps out-of-range tone input.
    const auto [cx, cy] = tone_to_pixel(Tone{5.0, -5.0}, 100, 100);
    assert(approx(cx, 100.0) && approx(cy, 100.0));
}

void test_throttle() {
    Throttle t(std::chrono::milliseconds(30));
    const auto t0 = std::chrono::steady_clock::time_point(std::chrono::milliseconds(1000));
    assert(t.should_emit(t0, false));                                        // first call always emits
    assert(!t.should_emit(t0 + std::chrono::milliseconds(5), false));        // too soon
    assert(t.should_emit(t0 + std::chrono::milliseconds(5), true));          // final always emits
    assert(t.should_emit(t0 + std::chrono::milliseconds(40), false));        // interval elapsed
}

} // namespace

int main() {
    test_brand_of();
    test_grouping();
    test_color_for_id();
    test_tone_mapping();
    test_throttle();
    std::printf("settings_logic_test: OK\n");
    return 0;
}
