#include "key_event.hpp"

#include <cassert>
#include <cstdio>
#include <initializer_list>

using keeby::classify_key_value;
using keeby::KeyEventKind;

int main() {
    // The real evdev contract: 0 = release, 1 = press, 2 = repeat.
    assert(classify_key_value(0) == KeyEventKind::Up);
    assert(classify_key_value(1) == KeyEventKind::Down);
    assert(classify_key_value(2) == KeyEventKind::Repeat);

    // Any other raw value is treated as Repeat rather than silently
    // mis-classified as a press or release — verifies the fallback arm,
    // not just the documented three values.
    assert(classify_key_value(3) == KeyEventKind::Repeat);
    assert(classify_key_value(-1) == KeyEventKind::Repeat);

    assert(keeby::is_supported_input_code(KEY_A));
    for (uint16_t code : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        assert(keeby::is_supported_input_code(code));
        assert(keeby::is_pointer_button_code(code));
    }
    assert(keeby::is_supported_input_code(KEY_FN));
    assert(!keeby::is_supported_input_code(256));
    assert(!keeby::is_supported_input_code(271));
    assert(!keeby::is_supported_input_code(275));
    assert(!keeby::is_supported_input_code(463));
    assert(!keeby::is_supported_input_code(BTN_SIDE));

    std::printf("key_event_test: OK\n");
    return 0;
}
