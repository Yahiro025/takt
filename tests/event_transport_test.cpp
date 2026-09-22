#include "event_transport.hpp"

#include <atomic>
#include <barrier>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using keeby::EventTransport;

static void test_empty_pop_fails() {
    EventTransport<int, 4> t;
    int v = -1;
    assert(!t.try_pop(v));
    assert(t.dropped_count() == 0);
}

static void test_ordering_and_wraparound() {
    EventTransport<int, 4> t;
    int v = -1;

    assert(t.try_push(1));
    assert(t.try_push(2));
    assert(t.try_pop(v) && v == 1);
    assert(t.try_push(3)); // wraps past the array end
    assert(t.try_push(4));
    assert(t.try_pop(v) && v == 2);
    assert(t.try_pop(v) && v == 3);
    assert(t.try_pop(v) && v == 4);
    assert(!t.try_pop(v));
}

static void test_overflow_policy_is_drop_newest_and_counted() {
    // Capacity 4 means 3 usable slots (one is always kept empty to
    // distinguish full from empty).
    EventTransport<int, 4> t;
    assert(t.try_push(10));
    assert(t.try_push(20));
    assert(t.try_push(30));
    assert(t.dropped_count() == 0);

    // Buffer is now full: the next push must fail without touching any
    // already-queued slot, and must be counted.
    assert(!t.try_push(40));
    assert(t.dropped_count() == 1);
    assert(!t.try_push(50));
    assert(t.dropped_count() == 2);

    // The three events that made it in must still come out in order,
    // untouched by the dropped pushes.
    int v = -1;
    assert(t.try_pop(v) && v == 10);
    assert(t.try_pop(v) && v == 20);
    assert(t.try_pop(v) && v == 30);
    assert(!t.try_pop(v));

    // dropped_count is cumulative, not reset by draining.
    assert(t.dropped_count() == 2);
}

static void test_recovers_after_drain() {
    EventTransport<int, 4> t;
    assert(t.try_push(1));
    assert(t.try_push(2));
    assert(t.try_push(3));
    assert(!t.try_push(4)); // dropped

    int v = -1;
    assert(t.try_pop(v) && v == 1);

    // Freed a slot: pushing must succeed again, and the drop from
    // earlier must not block future pushes.
    assert(t.try_push(5));
    assert(t.try_pop(v) && v == 2);
    assert(t.try_pop(v) && v == 3);
    assert(t.try_pop(v) && v == 5);
    assert(!t.try_pop(v));
    assert(t.dropped_count() == 1);
}

static void test_exact_usable_capacity() {
    // Advertised backing size 4 -> exactly 3 usable slots (one is always
    // reserved to disambiguate full from empty).
    EventTransport<int, 4> small;
    assert(small.capacity() == 3);

    // The real production transport: 256 backing slots, 255 usable.
    keeby::KeyEventTransport real;
    assert(real.capacity() == 255);
    for (uint64_t i = 0; i < real.capacity(); ++i)
        assert(real.try_push({static_cast<uint16_t>(i), keeby::KeyEventKind::Down, i}));
    assert(!real.try_push({999, keeby::KeyEventKind::Up, 999}));
    assert(real.dropped_count() == 1);
    keeby::KeyEvent event;
    for (uint64_t i = 0; i < real.capacity(); ++i) {
        assert(real.try_pop(event));
        assert(event.code == i && event.ts_ns == i && event.kind == keeby::KeyEventKind::Down);
    }
    assert(!real.try_pop(event));

    // Push exactly capacity() items: all must succeed, the next must not.
    int v = -1;
    for (std::size_t i = 0; i < small.capacity(); ++i) {
        assert(small.try_push(static_cast<int>(i)));
    }
    assert(!small.try_push(999));
    assert(small.dropped_count() == 1);
    for (std::size_t i = 0; i < small.capacity(); ++i) {
        assert(small.try_pop(v) && v == static_cast<int>(i));
    }
    assert(!small.try_pop(v));
}

static void test_fifo_across_many_wraparounds() {
    // Capacity 8 (7 usable) kept small on purpose: push-one/pop-one in a
    // tight loop forces many wraparounds without ever filling the
    // buffer, deterministically, single-threaded, no sleeps.
    EventTransport<int, 8> t;
    constexpr int kIterations = 100000;
    int v = -1;
    for (int i = 0; i < kIterations; ++i) {
        assert(t.try_push(i));
        assert(t.try_pop(v));
        assert(v == i); // strict FIFO order preserved across every wrap
    }
    assert(t.dropped_count() == 0);
    assert(!t.try_pop(v)); // nothing left behind
}

// Concurrent SPSC stress test. Deterministic in what it asserts, not in
// how many drops occur (that depends on OS scheduling, which is exactly
// why the test does not assert a specific drop count): every attempted
// push must be accounted for as exactly one of {received, dropped}, and
// whatever IS received must be in strict order with no duplication or
// corruption. No sleeps are used for correctness — only a plain atomic
// "producer done" flag, checked with proper acquire/release pairing so
// the consumer's final drain-after-done pass cannot race the producer's
// last push.
static void test_concurrent_spsc_stress() {
    constexpr uint64_t kCount = 2'000'000; // several thousand wraparounds at this capacity
    EventTransport<uint64_t, 1024> t; // 1023 usable slots

    std::vector<uint64_t> accepted;
    accepted.reserve(kCount);
    std::vector<uint64_t> received;
    received.reserve(kCount);
    std::atomic<bool> producer_done{false};

    std::thread consumer([&] {
        uint64_t v;
        for (;;) {
            if (t.try_pop(v)) {
                received.push_back(v);
                continue;
            }
            if (producer_done.load(std::memory_order_acquire)) {
                // One more drain pass: a last successful push could have
                // landed between this try_pop and the flag becoming
                // visible. Anything the producer pushed is guaranteed
                // visible here (happens-before via producer_done's
                // release, sequenced after every try_push on that
                // thread), so this is not a race, just a final sweep.
                while (t.try_pop(v)) received.push_back(v);
                break;
            }
        }
    });

    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount; ++i) {
            if (t.try_push(i)) accepted.push_back(i);
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    // Every attempted push is exactly one of {received, dropped}.
    assert(received.size() + t.dropped_count() == kCount);
    assert(received == accepted); // exact successful sequence, not only sortedness

    // Strict FIFO order, no duplication, no corruption.
    for (std::size_t i = 1; i < received.size(); ++i) {
        assert(received[i] > received[i - 1]);
    }
    if (!received.empty()) {
        assert(received.front() < kCount);
        assert(received.back() < kCount);
    }

    std::printf("  concurrent stress: pushed=%llu received=%zu dropped=%llu\n",
                (unsigned long long)kCount, received.size(), (unsigned long long)t.dropped_count());
}

// Retry each item until accepted: guarantees 500,000 full KeyEvent copies
// and thousands of wraps even if scheduling makes the drop test mostly drop.
static void test_concurrent_key_events() {
    constexpr uint64_t count = 500'000;
    keeby::KeyEventTransport t;
    std::barrier start(2);
    uint64_t rejected = 0; // producer-owned, inspected only after join
    std::thread producer([&] {
        start.arrive_and_wait();
        for (uint64_t i = 0; i < count; ++i) {
            const keeby::KeyEvent event{static_cast<uint16_t>(i),
                static_cast<keeby::KeyEventKind>(i % 3), i};
            while (!t.try_push(event)) ++rejected;
        }
    });
    std::thread consumer([&] {
        start.arrive_and_wait();
        uint64_t previous_drops = 0;
        for (uint64_t i = 0; i < count; ++i) {
            keeby::KeyEvent event;
            while (!t.try_pop(event)) {}
            assert(event.ts_ns == i);
            assert(event.code == static_cast<uint16_t>(i));
            assert(event.kind == static_cast<keeby::KeyEventKind>(i % 3));
            const auto drops = t.dropped_count();
            assert(drops >= previous_drops); // concurrent counter observation
            previous_drops = drops;
        }
    });
    producer.join();
    consumer.join();
    keeby::KeyEvent extra;
    assert(!t.try_pop(extra));
    assert(t.dropped_count() == rejected);
    std::printf("  KeyEvent stress: received=500000, exact fields/FIFO, rejected=%llu\n",
                (unsigned long long)rejected);
}

int main() {
    test_empty_pop_fails();
    test_ordering_and_wraparound();
    test_overflow_policy_is_drop_newest_and_counted();
    test_recovers_after_drain();
    test_exact_usable_capacity();
    test_fifo_across_many_wraparounds();
    test_concurrent_spsc_stress();
    test_concurrent_key_events();
    std::printf("event_transport_test: OK\n");
    return 0;
}
