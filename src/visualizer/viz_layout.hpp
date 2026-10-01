#pragma once

// Pure, GTK-free key layout table for keeby-visualizer's compact 60%-style
// panel (see docs/013-visualizer-app.md for the reference image and unit
// grid). Coordinates are in "key units" (1u = one standard keycap incl. its
// gap) so the renderer can multiply by whatever pixel-per-unit size it likes
// -- keeps this header display/DPI-agnostic and trivially unit-testable
// (tests/visualizer_test.cpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <linux/input-event-codes.h>

namespace keeby::viz {

// A handful of keys (backspace, arrows) render as a drawn glyph instead of
// a text legend -- Unicode arrows/backspace pictograms rendered as tofu
// under the fonts available on this machine, so viz_app.cpp draws these
// with cairo paths (see draw_glyph()) keyed off this enum.
enum class Glyph : uint8_t { None, Backspace, ArrowLeft, ArrowRight };

struct KeyRect {
    uint16_t code;
    float x, y, w, h; // key units
    const char* legend;
    Glyph glyph = Glyph::None;
};

inline constexpr float kLayoutWidthUnits = 15.0f;
inline constexpr float kLayoutHeightUnits = 5.0f;

// clang-format off
inline constexpr std::array<KeyRect, 62> kLayout = {{
    // Number row (y=0)
    {KEY_GRAVE, 0.0f, 0.0f, 1.0f, 1.0f, "`"}, {KEY_1, 1.0f, 0.0f, 1.0f, 1.0f, "1"},
    {KEY_2, 2.0f, 0.0f, 1.0f, 1.0f, "2"}, {KEY_3, 3.0f, 0.0f, 1.0f, 1.0f, "3"},
    {KEY_4, 4.0f, 0.0f, 1.0f, 1.0f, "4"}, {KEY_5, 5.0f, 0.0f, 1.0f, 1.0f, "5"},
    {KEY_6, 6.0f, 0.0f, 1.0f, 1.0f, "6"}, {KEY_7, 7.0f, 0.0f, 1.0f, 1.0f, "7"},
    {KEY_8, 8.0f, 0.0f, 1.0f, 1.0f, "8"}, {KEY_9, 9.0f, 0.0f, 1.0f, 1.0f, "9"},
    {KEY_0, 10.0f, 0.0f, 1.0f, 1.0f, "0"}, {KEY_MINUS, 11.0f, 0.0f, 1.0f, 1.0f, "-"},
    {KEY_EQUAL, 12.0f, 0.0f, 1.0f, 1.0f, "="},
    {KEY_BACKSPACE, 13.0f, 0.0f, 2.0f, 1.0f, "", Glyph::Backspace},

    // Tab / QWERTY row (y=1)
    {KEY_TAB, 0.0f, 1.0f, 1.5f, 1.0f, "Tab"}, {KEY_Q, 1.5f, 1.0f, 1.0f, 1.0f, "Q"},
    {KEY_W, 2.5f, 1.0f, 1.0f, 1.0f, "W"}, {KEY_E, 3.5f, 1.0f, 1.0f, 1.0f, "E"},
    {KEY_R, 4.5f, 1.0f, 1.0f, 1.0f, "R"}, {KEY_T, 5.5f, 1.0f, 1.0f, 1.0f, "T"},
    {KEY_Y, 6.5f, 1.0f, 1.0f, 1.0f, "Y"}, {KEY_U, 7.5f, 1.0f, 1.0f, 1.0f, "U"},
    {KEY_I, 8.5f, 1.0f, 1.0f, 1.0f, "I"}, {KEY_O, 9.5f, 1.0f, 1.0f, 1.0f, "O"},
    {KEY_P, 10.5f, 1.0f, 1.0f, 1.0f, "P"}, {KEY_LEFTBRACE, 11.5f, 1.0f, 1.0f, 1.0f, "["},
    {KEY_RIGHTBRACE, 12.5f, 1.0f, 1.0f, 1.0f, "]"}, {KEY_BACKSLASH, 13.5f, 1.0f, 1.5f, 1.0f, "\\"},

    // Caps / ASDF row (y=2)
    {KEY_CAPSLOCK, 0.0f, 2.0f, 1.75f, 1.0f, "Caps"}, {KEY_A, 1.75f, 2.0f, 1.0f, 1.0f, "A"},
    {KEY_S, 2.75f, 2.0f, 1.0f, 1.0f, "S"}, {KEY_D, 3.75f, 2.0f, 1.0f, 1.0f, "D"},
    {KEY_F, 4.75f, 2.0f, 1.0f, 1.0f, "F"}, {KEY_G, 5.75f, 2.0f, 1.0f, 1.0f, "G"},
    {KEY_H, 6.75f, 2.0f, 1.0f, 1.0f, "H"}, {KEY_J, 7.75f, 2.0f, 1.0f, 1.0f, "J"},
    {KEY_K, 8.75f, 2.0f, 1.0f, 1.0f, "K"}, {KEY_L, 9.75f, 2.0f, 1.0f, 1.0f, "L"},
    {KEY_SEMICOLON, 10.75f, 2.0f, 1.0f, 1.0f, ";"}, {KEY_APOSTROPHE, 11.75f, 2.0f, 1.0f, 1.0f, "'"},
    {KEY_ENTER, 12.75f, 2.0f, 2.25f, 1.0f, "Return"},

    // Shift / ZXCV row (y=3)
    {KEY_LEFTSHIFT, 0.0f, 3.0f, 2.25f, 1.0f, "Shift"}, {KEY_Z, 2.25f, 3.0f, 1.0f, 1.0f, "Z"},
    {KEY_X, 3.25f, 3.0f, 1.0f, 1.0f, "X"}, {KEY_C, 4.25f, 3.0f, 1.0f, 1.0f, "C"},
    {KEY_V, 5.25f, 3.0f, 1.0f, 1.0f, "V"}, {KEY_B, 6.25f, 3.0f, 1.0f, 1.0f, "B"},
    {KEY_N, 7.25f, 3.0f, 1.0f, 1.0f, "N"}, {KEY_M, 8.25f, 3.0f, 1.0f, 1.0f, "M"},
    {KEY_COMMA, 9.25f, 3.0f, 1.0f, 1.0f, ","}, {KEY_DOT, 10.25f, 3.0f, 1.0f, 1.0f, "."},
    {KEY_SLASH, 11.25f, 3.0f, 1.0f, 1.0f, "/"}, {KEY_RIGHTSHIFT, 12.25f, 3.0f, 2.75f, 1.0f, "Shift"},

    // Bottom row (y=4) -- Linux legends per the brief: Ctrl/Super/Alt.
    {KEY_FN, 0.0f, 4.0f, 1.0f, 1.0f, "fn"}, {KEY_LEFTCTRL, 1.0f, 4.0f, 1.0f, 1.0f, "Ctrl"},
    {KEY_LEFTALT, 2.0f, 4.0f, 1.0f, 1.0f, "Alt"}, {KEY_LEFTMETA, 3.0f, 4.0f, 1.25f, 1.0f, "Super"},
    {KEY_SPACE, 4.25f, 4.0f, 6.5f, 1.0f, ""}, {KEY_RIGHTMETA, 10.75f, 4.0f, 1.25f, 1.0f, "Super"},
    {KEY_RIGHTALT, 12.0f, 4.0f, 1.0f, 1.0f, "Alt"},
    {KEY_LEFT, 13.0f, 4.0f, 1.0f, 1.0f, "", Glyph::ArrowLeft},
    {KEY_RIGHT, 14.0f, 4.0f, 1.0f, 1.0f, "", Glyph::ArrowRight},
}};
// clang-format on

inline constexpr const KeyRect* find_key(uint16_t code) {
    for (const auto& r : kLayout) {
        if (r.code == code) return &r;
    }
    return nullptr;
}

} // namespace keeby::viz
