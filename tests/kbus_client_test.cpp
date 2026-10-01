// Headless-safe test for keeby::ui::KeebyClient against tests/fake_keeby_service.cpp
// on a private, unique bus name -- never touches a real running KEEBY.
// SKIPs (exit 77) when there is no session bus at all.

#include "../src/ui/kbus_client.hpp"

#include <gio/gio.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace keeby::ui;

namespace {

bool has_session_bus() {
    const char* addr = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    return addr != nullptr && *addr != '\0';
}

std::string unique_bus_name() {
    char buf[160];
    std::snprintf(buf, sizeof buf, "org.keeby.Test.KbusClientTest_%d_%lld", static_cast<int>(getpid()),
                  static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return buf;
}

// Waits up to `timeout_ms` for a '\n'-terminated line on `fd`; returns it
// without the newline, or empty on timeout/EOF.
std::string read_line_with_timeout(int fd, int timeout_ms) {
    std::string acc;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - std::chrono::steady_clock::now())
                                    .count();
        struct pollfd pfd { fd, POLLIN, 0 };
        if (poll(&pfd, 1, remaining > 0 ? static_cast<int>(remaining) : 0) <= 0) break;
        char c;
        const ssize_t n = read(fd, &c, 1);
        if (n <= 0) break;
        if (c == '\n') return acc;
        acc.push_back(c);
    }
    return {};
}

} // namespace

int main() {
    if (!has_session_bus()) {
        std::fprintf(stderr, "kbus_client_test: no session bus, SKIP\n");
        return 77;
    }

    const std::string bus_name = unique_bus_name();

    int out_pipe[2];
    assert(pipe(out_pipe) == 0);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[1]);
        execl(FAKE_KEEBY_SERVICE_PATH, FAKE_KEEBY_SERVICE_PATH, bus_name.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(out_pipe[1]);
    const std::string ready = read_line_with_timeout(out_pipe[0], 5000);
    close(out_pipe[0]);
    assert(ready == "READY");

    setenv("KEEBY_BUS_NAME", bus_name.c_str(), 1);
    KeebyClient client;

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    guint watchdog = g_timeout_add(
        5000,
        [](gpointer) -> gboolean {
            std::fprintf(stderr, "kbus_client_test: timed out waiting for D-Bus round trips\n");
            std::exit(1);
            return G_SOURCE_REMOVE;
        },
        nullptr);

    bool state_changed_received = false;
    client.on_state_changed([&] { state_changed_received = true; });

    // Runs the pre-existing ListProfilesDetailed/SetProfile round trips,
    // called once the new GetVisualizer/SetVisualizer* round trip (below)
    // has finished.
    std::function<void()> run_profile_chain = [&] {
        client.list_profiles_detailed([&](bool ok4, std::vector<std::pair<std::string, std::string>> profiles) {
            assert(ok4);
            assert(profiles.size() == 4);
            assert(profiles[0].first == "default");
            assert(profiles[0].second == "Default (built-in)");

            // Successful SetProfile.
            client.set_profile("cherry_blue", [&](bool ok5, std::string error) {
                assert(ok5);
                assert(error.empty());

                client.get_state([&](bool ok6, EngineState state2) {
                    assert(ok6);
                    assert(state2.profile == "cherry_blue");

                    // A failing SetProfile surfaces the engine's error and
                    // leaves the profile unchanged.
                    client.set_profile("no-such-profile", [&](bool ok7, std::string error7) {
                        assert(!ok7);
                        assert(!error7.empty());
                        // By now the StateChanged signal from the earlier
                        // successful SetProfile has had two full round trips
                        // to arrive.
                        assert(state_changed_received);
                        g_main_loop_quit(loop);
                    });
                });
            });
        });
    };

    client.on_availability_changed([&](bool available) {
        if (!available) return;

        client.get_info([&](bool ok, EngineInfo info) {
            assert(ok);
            assert(info.version == "0.1.0-test");
            assert(info.device_connected);

            client.get_state([&](bool ok2, EngineState state) {
                assert(ok2);
                assert(state.enabled);
                assert(state.volume == 1.0);
                assert(state.stereo_width == 1.0);
                assert(state.profile == "default");

                client.get_tone([&](bool ok3, double x, double y) {
                    assert(ok3);
                    assert(x == 0.0 && y == 0.0);

                    client.get_visualizer([&](bool okv, VisualizerState viz) {
                        assert(okv);
                        assert(!viz.enabled);
                        assert(viz.position == "top-center");
                        assert(viz.dismiss_ms == 1000);
                        assert(viz.follow_speed == 1.0);

                        client.set_visualizer(true);
                        client.set_visualizer_position("bottom-right");
                        client.set_visualizer_dismiss(2500);
                        client.set_visualizer_follow_speed(2.5);

                        client.get_visualizer([&](bool okv2, VisualizerState viz2) {
                            assert(okv2);
                            assert(viz2.enabled);
                            assert(viz2.position == "bottom-right");
                            assert(viz2.dismiss_ms == 2500);
                            assert(viz2.follow_speed == 2.5);

                            run_profile_chain();
                        });
                    });
                });
            });
        });
    });

    g_main_loop_run(loop);
    g_source_remove(watchdog);
    g_main_loop_unref(loop);

    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);

    std::printf("kbus_client_test: OK\n");
    return 0;
}
