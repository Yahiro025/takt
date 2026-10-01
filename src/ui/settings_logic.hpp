#pragma once

// Pure, GTK/GLib-free logic for keeby-settings: switch-profile grouping and
// color, tone-pad coordinate mapping, and slider/pad update throttling.
// Kept header-only and dependency-free so it's trivially unit-testable
// without a display (see tests/settings_logic_test.cpp).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace keeby::ui {

struct ProfileEntry {
    std::string id;
    std::string name;
};

// First word of the display name is the brand ("Cherry MX Blue" -> "Cherry"),
// except the built-in profile's whole name ("Default (built-in)") is its own
// group so it doesn't get bucketed under some unrelated word.
inline std::string brand_of(const std::string& display_name) {
    if (display_name == "Default (built-in)") return display_name;
    const auto space = display_name.find(' ');
    return space == std::string::npos ? display_name : display_name.substr(0, space);
}

struct ProfileGroup {
    std::string brand;
    std::vector<ProfileEntry> profiles;
};

// Groups profiles by brand_of(name); "Default (built-in)" sorts first, the
// rest alphabetically. Order within a group preserves input order.
inline std::vector<ProfileGroup> group_profiles_by_brand(const std::vector<ProfileEntry>& profiles) {
    std::vector<ProfileGroup> groups;
    for (const auto& p : profiles) {
        const std::string brand = brand_of(p.name);
        auto it = std::find_if(groups.begin(), groups.end(),
                                [&brand](const ProfileGroup& g) { return g.brand == brand; });
        if (it == groups.end()) {
            groups.push_back(ProfileGroup{brand, {p}});
        } else {
            it->profiles.push_back(p);
        }
    }
    std::stable_sort(groups.begin(), groups.end(), [](const ProfileGroup& a, const ProfileGroup& b) {
        constexpr const char* kDefault = "Default (built-in)";
        if (a.brand == kDefault) return b.brand != kDefault;
        if (b.brand == kDefault) return false;
        return a.brand < b.brand;
    });
    return groups;
}

// Deterministic id -> our-own-palette color, as an "#rrggbb" string. Same id
// always maps to the same swatch; distinct ids are spread across the
// palette via FNV-1a. Not Keeby's switch colors -- an original small set.
inline uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

inline std::string color_for_id(const std::string& id) {
    static constexpr const char* kPalette[] = {
        "#5B8DEF", "#3ABEA0", "#E0A63A", "#D0668A", "#8A6FD1",
        "#4FB0C6", "#E08A4F", "#6FA85B", "#C05B5B", "#5B7BC0",
    };
    constexpr size_t kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);
    return kPalette[fnv1a(id) % kPaletteSize];
}

// --- Tone pad mapping -------------------------------------------------
// Tone axes: x in [-1, 1] with Thock at -1 (left) / Clack at +1 (right);
// y in [-1, 1] with Warm/Bright axis such that +1 (top of pad) is
// "brighter". Corner labels: top-left Warm, top-right Bright,
// bottom-left Thock, bottom-right Clack -- i.e. the *bottom* edge is the
// Thock<->Clack axis (x) and the *left* edge is the Warm<->Bright axis in
// the vertical sense (y). Pixel space has y growing downward, so pixel
// top (py=0) maps to tone y=+1 and pixel bottom maps to tone y=-1.

struct Tone {
    double x = 0.0; // Thock(-1) .. Clack(+1)
    double y = 0.0; // Bright at top(+1) .. Thock/Warm-side at bottom(-1)
};

inline double clamp_unit(double v) { return std::clamp(v, -1.0, 1.0); }

inline Tone clamp_tone(Tone t) { return Tone{clamp_unit(t.x), clamp_unit(t.y)}; }

// width/height in pixels (> 0); px/py may be outside the pad (e.g. a drag
// that overshoots) -- the result is always clamped to [-1, 1].
inline Tone pixel_to_tone(double px, double py, double width, double height) {
    if (width <= 0.0 || height <= 0.0) return Tone{0.0, 0.0};
    const double x = -1.0 + 2.0 * (px / width);
    const double y = 1.0 - 2.0 * (py / height);
    return clamp_tone(Tone{x, y});
}

// Inverse of pixel_to_tone; input tone is clamped to [-1, 1] first, so the
// result always lands within [0, width] x [0, height].
inline std::pair<double, double> tone_to_pixel(Tone t, double width, double height) {
    t = clamp_tone(t);
    const double px = (t.x + 1.0) * 0.5 * width;
    const double py = (1.0 - t.y) * 0.5 * height;
    return {px, py};
}

// --- Update throttle ---------------------------------------------------
// Rate-limits outgoing slider/pad D-Bus updates to at most one per
// `min_interval`, while always letting a `final` update (e.g. drag-release,
// or the last one before a new gesture) through immediately so the engine
// never ends up out of sync with where the user left the control.
class Throttle {
public:
    explicit Throttle(std::chrono::steady_clock::duration min_interval) : min_interval_(min_interval) {}

    bool should_emit(std::chrono::steady_clock::time_point now, bool final) {
        if (final || !has_emitted_ || now - last_emit_ >= min_interval_) {
            last_emit_ = now;
            has_emitted_ = true;
            return true;
        }
        return false;
    }

private:
    std::chrono::steady_clock::duration min_interval_;
    std::chrono::steady_clock::time_point last_emit_{};
    bool has_emitted_ = false;
};

} // namespace keeby::ui
