#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "key_event.hpp"

namespace keeby {

// Bounded single-producer/single-consumer lock-free queue. One producer
// thread may call try_push, one consumer thread may call try_pop,
// concurrently, and neither ever blocks or allocates.
//
// Producer alone writes head_; consumer alone writes tail_. Callers must
// keep those roles on separate, single threads; the API cannot enforce it.
//
// Publishing: slot assignment precedes head_.store(release). The consumer's
// head_.load(acquire) makes that assignment (and prior producer writes)
// visible before copying the slot. This uses sequenced-before plus
// synchronizes-with, not a chain of same-thread release sequences.
// Reuse: the consumer finishes copying before tail_.store(release). The
// producer's tail_.load(acquire) orders the next overwrite after that read.
// BOTH directions are required to avoid a data race on the plain slots.
// Relaxed index loads read only the calling thread's own index. Atomic
// coherence prevents older observations of the peer index from granting
// premature access; a stale observation can instead report full/empty.
//
// Drop-newest: a full push changes only the relaxed atomic drop counter.
// That counter publishes no slot data and may be sampled by other threads.
// It is cumulative modulo 2^64; it does not affect queue correctness.
//
// StorageSlots includes one reserved slot. EventTransport<T, 256> holds
// exactly 255 queued events. capacity() reports that usable count.
// Indices are masked into [0, StorageSlots) on every increment. For a
// representable power of two >= 2, index + 1 fits size_t, so machine
// integer rollover cannot change the full/empty state.
template <typename T, std::size_t StorageSlots>
class EventTransport {
    static_assert((StorageSlots & (StorageSlots - 1)) == 0, "StorageSlots must be a power of two");
    static_assert(StorageSlots >= 2, "StorageSlots must allow at least one usable slot");
    static_assert(std::is_trivially_copyable_v<T> &&
                  std::is_trivially_copy_assignable_v<T> &&
                  std::is_trivially_destructible_v<T>,
                  "RT transport requires trivial values and assignment");

public:
    // Producer side only. Returns false, and increments dropped_count(),
    // if the buffer was full.
    bool try_push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side only. Wait-free: never blocks, never allocates.
    bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buffer_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Safe to call from any thread at any time (including concurrently
    // with try_push/try_pop): a relaxed atomic load, not part of the
    // producer/consumer protocol itself.
    uint64_t dropped_count() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    // The ACTUAL maximum number of elements that can be queued at once
    // — StorageSlots - 1, per the one-slot-reserved design above. Prefer
    // this over the raw template parameter wherever "how many can this
    // hold" matters (tests, capacity planning), so the reserved slot
    // can never be silently miscounted.
    static constexpr std::size_t capacity() noexcept { return StorageSlots - 1; }

private:
    static constexpr std::size_t kMask = StorageSlots - 1;
    std::array<T, StorageSlots> buffer_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<uint64_t> dropped_{0};

    // These are the only atomic types this template is ever instantiated
    // with in this codebase (see KeyEventTransport below and its use in
    // audio_boundary.cpp's real-time process() callback). Verified
    // lock-free at compile time for our sole validated production
    // target: Linux, x86-64, GCC/Clang with libstdc++/libc++. If this
    // template is ever instantiated on another architecture, this
    // assertion — not a silent mutex fallback inside an RT callback —
    // is what should fail the build.
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "EventTransport's index atomics must be lock-free on the target platform");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "EventTransport's dropped-event counter must be lock-free on the target platform");
};

// A transport specifically sized and typed for KeyEvent, shared by the
// input-capture and audio-boundary components. 256 backing slots, 255
// usable (see capacity() above) — carried forward unchanged from Step
// 1.4's feasibility spike, which observed zero drops across roughly
// 1,000 real key events under real, sustained typing (including rapid
// typing and multi-second repeat bursts) at this same size — see
// docs/001-core-feasibility-spec.md. Revisit only with evidence this is
// insufficient under production load.
using KeyEventTransport = EventTransport<KeyEvent, 256>;

} // namespace keeby
