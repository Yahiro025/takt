// Pure-logic tests for src/pointer_input.hpp: device classification from
// capability bits, and the sub-pixel carry/truncate accumulator fed by
// each libinput pointer-motion event. No fd, no libevdev, no libinput, no
// fork -- see the header comment.
#include "pointer_input.hpp"

#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace keeby::pointer;

int main() {
    // classify(): a relative mouse.
    {
        Capabilities c;
        c.rel_x = true;
        c.rel_y = true;
        assert(classify(c) == DeviceKind::Mouse);
    }

    // Only the left/right/middle pointer buttons become press/release audio
    // events; other buttons and out-of-range values remain ignored.
    for (uint32_t button : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        auto down = make_button_event(button, true, 123);
        auto up = make_button_event(button, false, 456);
        assert(down && down->code == button && down->kind == keeby::KeyEventKind::Down && down->ts_ns == 123);
        assert(up && up->code == button && up->kind == keeby::KeyEventKind::Up && up->ts_ns == 456);
    }
    assert(!make_button_event(BTN_SIDE, true, 0));
    assert(!make_button_event(0x10000u + BTN_LEFT, true, 0));
    // A mouse missing one relative axis is not a mouse at all.
    {
        Capabilities c;
        c.rel_x = true;
        assert(classify(c) == DeviceKind::None);
    }
    // classify(): a touchpad via legacy ABS_X/ABS_Y.
    {
        Capabilities c;
        c.abs_x = true;
        c.abs_y = true;
        c.btn_tool_finger = true;
        c.prop_pointer = true;
        assert(classify(c) == DeviceKind::Touchpad);
    }
    // classify(): a touchpad via ABS_MT_POSITION_X/Y only (no legacy axes).
    {
        Capabilities c;
        c.abs_mt_x = true;
        c.abs_mt_y = true;
        c.btn_tool_finger = true;
        c.prop_pointer = true;
        assert(classify(c) == DeviceKind::Touchpad);
    }
    // Missing INPUT_PROP_POINTER: never a touchpad (this is what excludes an
    // absolute tablet with similar axis/button bits).
    {
        Capabilities c;
        c.abs_x = true;
        c.abs_y = true;
        c.btn_tool_finger = true;
        c.prop_pointer = false;
        assert(classify(c) == DeviceKind::None);
    }
    // Missing BTN_TOOL_FINGER: never a touchpad (this is what excludes a
    // touchscreen, which reports ABS_X/Y + INPUT_PROP_DIRECT, not
    // BTN_TOOL_FINGER + INPUT_PROP_POINTER).
    {
        Capabilities c;
        c.abs_x = true;
        c.abs_y = true;
        c.prop_pointer = true;
        assert(classify(c) == DeviceKind::None);
    }
    // A keyboard: no pointer capability bits at all.
    {
        Capabilities c;
        assert(classify(c) == DeviceKind::None);
    }
    // Mouse check takes priority when (implausibly) both are present.
    {
        Capabilities c;
        c.rel_x = true;
        c.rel_y = true;
        c.abs_x = true;
        c.abs_y = true;
        c.btn_tool_finger = true;
        c.prop_pointer = true;
        assert(classify(c) == DeviceKind::Mouse);
    }

    // carry_and_truncate(): a delta under 1 in magnitude alone never
    // produces a step by itself -- it only carries into `remainder`.
    {
        double rem = 0.0;
        assert(carry_and_truncate(0.4, rem) == 0);
        assert(rem > 0.399 && rem < 0.401);
    }
    {
        double rem = 0.0;
        assert(carry_and_truncate(-0.4, rem) == 0);
        assert(rem < -0.399 && rem > -0.401);
    }
    // Once the carried remainder plus a new delta crosses a whole count,
    // the step is emitted (truncated toward zero) and only the leftover
    // fraction continues to carry.
    {
        double rem = 0.9;
        assert(carry_and_truncate(0.4, rem) == 1); // 0.9+0.4=1.3 -> 1, carry 0.3
        assert(rem > 0.299 && rem < 0.301);
    }
    // Many small positive deltas: no single delta ever reaches 1 count, yet
    // nothing is systematically lost -- the running sum of emitted steps
    // tracks the exact total to within one count.
    {
        double rem = 0.0;
        int sum = 0;
        for (int i = 0; i < 1000; ++i) sum += carry_and_truncate(0.3, rem);
        assert(sum >= 299 && sum <= 300); // exact total is 300.0
    }
    // Symmetric for negative deltas -- no sign-dependent bias.
    {
        double rem = 0.0;
        int sum = 0;
        for (int i = 0; i < 1000; ++i) sum += carry_and_truncate(-0.3, rem);
        assert(sum <= -299 && sum >= -300);
    }
    // Mixed signs / zero-crossings: the conservation invariant holds no
    // matter how the running total crosses zero -- nothing emitted or
    // carried is ever created or lost, only ever redistributed between
    // whole-count steps and the carried remainder.
    {
        double rem = 0.0;
        const double initial_rem = rem;
        double delta_sum = 0.0;
        int step_sum = 0;
        const double deltas[] = {0.6, 0.6, -0.5, -0.5, -0.5, 0.9, -1.3, 0.2, -0.2, 0.05};
        for (double d : deltas) {
            step_sum += carry_and_truncate(d, rem);
            delta_sum += d;
        }
        const double total = initial_rem + delta_sum;
        const double reconstructed = static_cast<double>(step_sum) + rem;
        assert(reconstructed > total - 1e-9 && reconstructed < total + 1e-9);
        // The remainder itself never reaches a full count in magnitude --
        // otherwise it should have been carried into a step instead.
        assert(rem > -1.0 && rem < 1.0);
    }

    std::puts("pointer_input_test: OK (device classification, selected button event mapping, "
              "sub-pixel carry/truncate)");
}
