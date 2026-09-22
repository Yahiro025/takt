#include "key_event.hpp"

#include <cassert>
#include <cstdio>

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

    std::printf("key_event_test: OK\n");
    return 0;
}
