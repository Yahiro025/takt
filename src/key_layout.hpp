#pragma once

#include <cstdint>
#include <linux/input-event-codes.h>

namespace keeby {

// Evdev code -> physical x -> pan. x = 0..1 spans the main block (laptop
// keyboards have no more); nav cluster/numpad extend past 1.0 in the same
// unit scale when present. See docs/007-step-2.6-sound-engine-v2.md.
constexpr float key_x(uint16_t code) noexcept {
    switch (code) {
        // Function row (unit centers / 15)
        case KEY_ESC: return 0.033333f;
        case KEY_F1: return 0.166667f;
        case KEY_F2: return 0.233333f;
        case KEY_F3: return 0.300000f;
        case KEY_F4: return 0.366667f;
        case KEY_F5: return 0.466667f;
        case KEY_F6: return 0.533333f;
        case KEY_F7: return 0.600000f;
        case KEY_F8: return 0.666667f;
        case KEY_F9: return 0.766667f;
        case KEY_F10: return 0.833333f;
        case KEY_F11: return 0.900000f;
        case KEY_F12: return 0.966667f;

        // Number row
        case KEY_GRAVE: return 0.033333f;
        case KEY_1: return 0.100000f;
        case KEY_2: return 0.166667f;
        case KEY_3: return 0.233333f;
        case KEY_4: return 0.300000f;
        case KEY_5: return 0.366667f;
        case KEY_6: return 0.433333f;
        case KEY_7: return 0.500000f;
        case KEY_8: return 0.566667f;
        case KEY_9: return 0.633333f;
        case KEY_0: return 0.700000f;
        case KEY_MINUS: return 0.766667f;
        case KEY_EQUAL: return 0.833333f;
        case KEY_BACKSPACE: return 0.933333f;

        // Tab / QWERTY row
        case KEY_TAB: return 0.050000f;
        case KEY_Q: return 0.133333f;
        case KEY_W: return 0.200000f;
        case KEY_E: return 0.266667f;
        case KEY_R: return 0.333333f;
        case KEY_T: return 0.400000f;
        case KEY_Y: return 0.466667f;
        case KEY_U: return 0.533333f;
        case KEY_I: return 0.600000f;
        case KEY_O: return 0.666667f;
        case KEY_P: return 0.733333f;
        case KEY_LEFTBRACE: return 0.800000f;
        case KEY_RIGHTBRACE: return 0.866667f;
        case KEY_BACKSLASH: return 0.950000f;

        // Caps / ASDF row
        case KEY_CAPSLOCK: return 0.058333f;
        case KEY_A: return 0.150000f;
        case KEY_S: return 0.216667f;
        case KEY_D: return 0.283333f;
        case KEY_F: return 0.350000f;
        case KEY_G: return 0.416667f;
        case KEY_H: return 0.483333f;
        case KEY_J: return 0.550000f;
        case KEY_K: return 0.616667f;
        case KEY_L: return 0.683333f;
        case KEY_SEMICOLON: return 0.750000f;
        case KEY_APOSTROPHE: return 0.816667f;
        case KEY_ENTER: return 0.925000f;

        // Shift / ZXCV row
        case KEY_LEFTSHIFT: return 0.075000f;
        case KEY_Z: return 0.183333f;
        case KEY_X: return 0.250000f;
        case KEY_C: return 0.316667f;
        case KEY_V: return 0.383333f;
        case KEY_B: return 0.450000f;
        case KEY_N: return 0.516667f;
        case KEY_M: return 0.583333f;
        case KEY_COMMA: return 0.650000f;
        case KEY_DOT: return 0.716667f;
        case KEY_SLASH: return 0.783333f;
        case KEY_RIGHTSHIFT: return 0.908333f;

        // Bottom row
        case KEY_LEFTCTRL: return 0.041667f;
        case KEY_LEFTMETA: return 0.125000f;
        case KEY_LEFTALT: return 0.208333f;
        case KEY_SPACE: return 0.458333f;
        case KEY_RIGHTALT: return 0.708333f;
        case KEY_RIGHTMETA: return 0.791667f;
        case KEY_COMPOSE: return 0.875000f;
        case KEY_RIGHTCTRL: return 0.958333f;

        // Nav cluster: 3 columns spanning 1.05..1.20
        case KEY_SYSRQ: return 1.075f;   // PrintScreen
        case KEY_SCROLLLOCK: return 1.125f;
        case KEY_PAUSE: return 1.175f;
        case KEY_INSERT: return 1.075f;
        case KEY_HOME: return 1.125f;
        case KEY_PAGEUP: return 1.175f;
        case KEY_DELETE: return 1.075f;
        case KEY_END: return 1.125f;
        case KEY_PAGEDOWN: return 1.175f;
        case KEY_LEFT: return 1.075f;
        case KEY_DOWN: return 1.125f;
        case KEY_UP: return 1.125f;
        case KEY_RIGHT: return 1.175f;

        // Numpad: 4 columns spanning 1.25..1.45
        case KEY_NUMLOCK: return 1.275f;
        case KEY_KPSLASH: return 1.325f;
        case KEY_KPASTERISK: return 1.375f;
        case KEY_KPMINUS: return 1.425f;
        case KEY_KP7: return 1.275f;
        case KEY_KP8: return 1.325f;
        case KEY_KP9: return 1.375f;
        case KEY_KPPLUS: return 1.425f;
        case KEY_KP4: return 1.275f;
        case KEY_KP5: return 1.325f;
        case KEY_KP6: return 1.375f;
        case KEY_KP1: return 1.275f;
        case KEY_KP2: return 1.325f;
        case KEY_KP3: return 1.375f;
        case KEY_KPENTER: return 1.425f;
        case KEY_KP0: return 1.300f; // spans columns 0-1
        case KEY_KPDOT: return 1.375f;

        // KEY_FN and pointer buttons have no horizontal key position.
        case KEY_FN:
        case BTN_LEFT:
        case BTN_RIGHT:
        case BTN_MIDDLE: return 0.5f;

        default: return 0.5f; // unknown key: main-block center
    }
}

// Bank pan stays valid ([-1, 1]) for every key above even at max x (1.45).
constexpr float key_pan(uint16_t code) noexcept {
    return (2.0f * key_x(code) - 1.0f) * 0.5f;
}

} // namespace keeby
