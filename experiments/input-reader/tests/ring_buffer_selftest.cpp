#include "ring_buffer.hpp"

#include <cassert>
#include <cstdio>

int main() {
    SpscRingBuffer<int, 4> rb;

    int v = -1;
    assert(!rb.try_pop(v));

    assert(rb.try_push(1));
    assert(rb.try_push(2));
    assert(rb.try_push(3));
    assert(!rb.try_push(4)); // capacity 4, one slot reserved to distinguish full/empty

    assert(rb.try_pop(v) && v == 1);
    assert(rb.try_pop(v) && v == 2);

    assert(rb.try_push(5)); // wrap-around
    assert(rb.try_push(6));

    assert(rb.try_pop(v) && v == 3);
    assert(rb.try_pop(v) && v == 5);
    assert(rb.try_pop(v) && v == 6);
    assert(!rb.try_pop(v));

    std::printf("ring_buffer_selftest: OK\n");
    return 0;
}
