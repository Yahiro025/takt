#pragma once

// Pure state machine deciding whether keeby-visualizer's panel should be
// visible, from key up/down events plus a periodic tick -- no GTK/timer
// dependency, so it's directly unit-testable (tests/visualizer_test.cpp).
// The panel shows on any key activity (even a key outside the layout table,
// or an up with no matching prior down) and stays visible while any key is
// held; once every key is released it fades out `dismiss` after the last
// activity.

#include <chrono>
#include <cstdint>
#include <unordered_set>

namespace keeby::viz {

class DismissTimer {
public:
    explicit DismissTimer(std::chrono::milliseconds dismiss = std::chrono::milliseconds(1000))
        : dismiss_(dismiss) {}

    void set_dismiss(std::chrono::milliseconds ms) { dismiss_ = ms; }
    std::chrono::milliseconds dismiss() const { return dismiss_; }

    // down=true for a key-down, false for a key-up. Resets the dismiss clock
    // and shows the panel regardless of whether `code` is in the layout.
    void on_key(uint16_t code, bool down, std::chrono::steady_clock::time_point now) {
        if (down) {
            pressed_.insert(code);
        } else {
            pressed_.erase(code);
        }
        last_activity_ = now;
        visible_ = true;
    }

    // Call periodically (e.g. a GLib timeout) while `visible()` is true;
    // returns the up-to-date visibility so the caller knows when to stop
    // ticking. No pending state changes while hidden, so callers should not
    // tick at all in that case (keeps idle CPU at zero).
    bool tick(std::chrono::steady_clock::time_point now) {
        if (visible_ && pressed_.empty() && now - last_activity_ >= dismiss_) visible_ = false;
        return visible_;
    }

    // Cursor motion alone never shows the panel (only on_key does), but
    // while it's already visible, continued motion should keep it from
    // fading -- "stays while you keep typing or moving" per the brief.
    void touch(std::chrono::steady_clock::time_point now) {
        if (visible_) last_activity_ = now;
    }

    bool visible() const { return visible_; }
    bool any_pressed() const { return !pressed_.empty(); }

private:
    std::chrono::milliseconds dismiss_;
    std::unordered_set<uint16_t> pressed_;
    std::chrono::steady_clock::time_point last_activity_{};
    bool visible_ = false;
};

} // namespace keeby::viz
