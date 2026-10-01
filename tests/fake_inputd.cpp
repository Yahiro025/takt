// Test-only fake keeby-inputd. Pointed to via KEEBY_INPUTD_PATH so
// InputCapture's real fork/exec/socketpair/IPC path is exercised without
// needing a real keyboard device or setgid privilege. Mode selected via
// FAKE_INPUTD_MODE:
//   normal           (default) Ready, then a fixed scripted press/repeat/
//                     release sequence, then idle until killed.
//   no_ready          exit immediately without sending Ready.
//   ready_then_exit   send Ready, then exit immediately (simulates helper
//                     death/device loss right after startup).
//   malformed         Ready, a truncated message, then one valid event,
//                     then idle.
//   bad_type          Ready, a full-size message with an invalid `type`
//                     field, then one valid event, then idle.
//   pointer_motion    Ready, a KeyEvent, a mouse PointerMotion (dx=5,
//                     dy=-3), a touchpad PointerMotion (dx=-100, dy=200),
//                     another KeyEvent, then idle -- exercises PointerMotion
//                     routing (visualizer tap only, never the key transport).
//   pointer_buttons   Ready, keyboard events and all three allowlisted mouse
//                     button press/release pairs interleaved with motion.
//   fn_key           Ready, KEY_FN press and release.
#include "../src/input_wire.hpp"

#include <cstdlib>
#include <initializer_list>
#include <string>

#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr int kIpcFd = 3;
void send_msg(const keeby::wire::Message& m) { send(kIpcFd, &m, sizeof(m), 0); }
} // namespace

int main() {
    const char* mode_env = std::getenv("FAKE_INPUTD_MODE");
    std::string mode = mode_env ? mode_env : "normal";

    if (mode == "no_ready") return 0;

    keeby::wire::Message ready{};
    ready.type = static_cast<uint8_t>(keeby::wire::MessageType::Ready);
    send_msg(ready);

    if (mode == "ready_then_exit") return 0;

    if (mode == "malformed") {
        char partial[5] = {1, 2, 3, 4, 5};
        send(kIpcFd, partial, sizeof(partial), 0); // truncated: not sizeof(Message)
        keeby::wire::Message ok{};
        ok.type = static_cast<uint8_t>(keeby::wire::MessageType::KeyEvent);
        ok.kind = 1;
        ok.code = 30;
        ok.ts_ns = 12345;
        send_msg(ok);
        pause();
        return 0;
    }

    if (mode == "bad_type") {
        keeby::wire::Message bad{};
        bad.type = 99; // invalid discriminant
        send_msg(bad);
        keeby::wire::Message ok{};
        ok.type = static_cast<uint8_t>(keeby::wire::MessageType::KeyEvent);
        ok.kind = 0;
        ok.code = 31;
        ok.ts_ns = 999;
        send_msg(ok);
        pause();
        return 0;
    }

    if (mode == "pointer_motion") {
        keeby::wire::Message key1{};
        key1.type = static_cast<uint8_t>(keeby::wire::MessageType::KeyEvent);
        key1.kind = 1;
        key1.code = 30;
        key1.ts_ns = 10;
        send_msg(key1);

        keeby::wire::Message mouse{};
        mouse.type = static_cast<uint8_t>(keeby::wire::MessageType::PointerMotion);
        mouse.kind = static_cast<uint8_t>(keeby::wire::PointerKind::Mouse);
        mouse.reserved = keeby::wire::pack_motion(5, -3);
        mouse.ts_ns = 20;
        send_msg(mouse);

        keeby::wire::Message touchpad{};
        touchpad.type = static_cast<uint8_t>(keeby::wire::MessageType::PointerMotion);
        touchpad.kind = static_cast<uint8_t>(keeby::wire::PointerKind::Touchpad);
        touchpad.reserved = keeby::wire::pack_motion(-100, 200);
        touchpad.ts_ns = 30;
        send_msg(touchpad);

        keeby::wire::Message key2{};
        key2.type = static_cast<uint8_t>(keeby::wire::MessageType::KeyEvent);
        key2.kind = 0;
        key2.code = 30;
        key2.ts_ns = 40;
        send_msg(key2);

        pause();
        return 0;
    }

    if (mode == "pointer_buttons") {
        auto send_key = [](uint16_t code, uint8_t kind, uint64_t ts_ns) {
            keeby::wire::Message m{};
            m.type = static_cast<uint8_t>(keeby::wire::MessageType::KeyEvent);
            m.kind = kind;
            m.code = code;
            m.ts_ns = ts_ns;
            send_msg(m);
        };
        auto send_motion = [](uint64_t ts_ns) {
            keeby::wire::Message m{};
            m.type = static_cast<uint8_t>(keeby::wire::MessageType::PointerMotion);
            m.kind = static_cast<uint8_t>(keeby::wire::PointerKind::Mouse);
            m.reserved = keeby::wire::pack_motion(1, 0);
            m.ts_ns = ts_ns;
            send_msg(m);
        };
        send_key(KEY_A, 1, 10);
        send_motion(11);
        uint64_t ts = 20;
        for (uint16_t button : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
            send_key(button, 1, ts++);
            send_motion(ts++);
            send_key(button, 0, ts++);
        }
        send_key(KEY_A, 0, ts);
        pause();
        return 0;
    }

    if (mode == "fn_key") {
        auto send_fn = [](uint8_t kind, uint64_t timestamp) {
            keeby::wire::Message fn{};
            fn.type = static_cast<uint8_t>(keeby::wire::MessageType::KeyEvent);
            fn.kind = kind;
            fn.code = KEY_FN;
            fn.ts_ns = timestamp;
            send_msg(fn);
        };
        send_fn(1, 11);
        send_fn(0, 22);
        pause();
        return 0;
    }

    // normal: press, repeat, release on KEY_A(30).
    keeby::wire::Message press{};
    press.type = static_cast<uint8_t>(keeby::wire::MessageType::KeyEvent);
    press.kind = 1;
    press.code = 30;
    press.ts_ns = 100;
    keeby::wire::Message repeat = press;
    repeat.kind = 2;
    repeat.ts_ns = 200;
    keeby::wire::Message release = press;
    release.kind = 0;
    release.ts_ns = 300;
    send_msg(press);
    send_msg(repeat);
    send_msg(release);
    pause(); // idle until killed by the test/parent
    return 0;
}
