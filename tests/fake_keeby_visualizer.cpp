// Test-only fake keeby-visualizer. Pointed to via KEEBY_VISUALIZER_PATH so
// VisualizerService's real fork/execv/socketpair/IPC path is exercised
// without a real overlay window. Records every VizMessage it receives as a
// text line to $FAKE_VISUALIZER_OUTPUT (append mode, line-buffered flush
// per message) so the test process -- a separate address space -- can poll
// for what was sent. Mode via FAKE_VISUALIZER_MODE:
//   normal       (default) read and log until the socket closes (EOF), then
//                idle (pause()) until killed.
//   no_read      never touch fd 3 at all: exercises "the sender must never
//                block and must drop when the child isn't draining the
//                socket".
//   die_after_2  log every message (config included), but exit immediately
//                right after the second *key* message is logged --
//                simulates a mid-session crash, distinct from a clean
//                shutdown.
#include "../src/visualizer_wire.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr int kIpcFd = 3;

void log_message(FILE* out, const keeby::viz::VizMessage& m) {
    if (m.type == static_cast<uint8_t>(keeby::viz::MessageType::Config)) {
        std::fprintf(out, "config %s %u speed=%u\n",
                      keeby::viz::to_string(static_cast<keeby::viz::Position>(m.position)), m.dismiss_ms,
                      m.reserved);
    } else if (m.type == static_cast<uint8_t>(keeby::viz::MessageType::Key)) {
        std::fprintf(out, "key %s %u\n", m.kind == 1 ? "down" : "up", m.code);
    } else if (m.type == static_cast<uint8_t>(keeby::viz::MessageType::Motion)) {
        std::fprintf(out, "motion %s %d %d\n", m.kind == 1 ? "touchpad" : "mouse",
                      keeby::viz::motion_dx(m), keeby::viz::motion_dy(m));
    } else {
        std::fprintf(out, "unknown %u\n", m.type);
    }
    std::fflush(out);
}
} // namespace

int main() {
    const char* mode_env = std::getenv("FAKE_VISUALIZER_MODE");
    const std::string mode = mode_env ? mode_env : "normal";
    const char* out_path = std::getenv("FAKE_VISUALIZER_OUTPUT");
    FILE* out = out_path ? std::fopen(out_path, "a") : stdout;
    if (!out) out = stdout;

    if (mode == "no_read") {
        pause(); // deliberately never recv()s from fd 3
        return 0;
    }

    const int limit = (mode == "die_after_2") ? 2 : -1;
    int key_count = 0;
    keeby::viz::VizMessage m{};
    ssize_t n;
    while ((n = recv(kIpcFd, &m, sizeof m, 0)) == static_cast<ssize_t>(sizeof m)) {
        log_message(out, m);
        if (limit > 0 && m.type == static_cast<uint8_t>(keeby::viz::MessageType::Key) && ++key_count >= limit) break;
    }
    if (out != stdout) std::fclose(out);
    if (limit > 0 && key_count >= limit) return 0; // die_after_2: exit now, simulating a crash

    pause(); // normal: idle (possibly after EOF) until killed by the test/parent
    return 0;
}
