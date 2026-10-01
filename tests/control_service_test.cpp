#include "control_service.hpp"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

// Live session-bus round trip: SKIP (exit 77) when there is no bus, so this
// never touches a real running KEEBY. Uses a unique bus name per run.
namespace {
using keeby::ControlHandlers;
using keeby::ControlState;

bool has_session_bus() {
    const char* addr = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    return addr != nullptr && *addr != '\0';
}

struct FakeState {
    std::mutex mutex;
    bool enabled = true;
    double volume = 1.0;
    double stereo_width = 1.0;
    double tone_x = 0.0;
    double tone_y = 0.0;
    std::string profile = "default";
    std::vector<std::string> profiles = {"default", "clicky"};
    std::vector<std::pair<std::string, std::string>> profiles_detailed = {{"default", "Default"},
                                                                           {"clicky", "Clicky"}};
    bool connected = true;
    int quit_calls = 0;
    bool visualizer_enabled = true;
    keeby::viz::Position visualizer_position = keeby::viz::Position::BottomCenter;
    uint16_t visualizer_dismiss_ms = 1000;
    double visualizer_follow_speed = 1.0;
};

ControlHandlers make_handlers(FakeState& state) {
    ControlHandlers h;
    h.get_state = [&state] {
        std::lock_guard<std::mutex> lk(state.mutex);
        return ControlState{state.enabled, state.volume, state.stereo_width, state.profile};
    };
    h.set_enabled = [&state](bool e) {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.enabled = e;
    };
    h.set_volume = [&state](double v) {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.volume = std::clamp(v, 0.0, 1.0);
    };
    h.set_stereo_width = [&state](double w) {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.stereo_width = std::clamp(w, 0.0, 2.0);
    };
    h.set_profile = [&state](const std::string& id) -> std::expected<void, std::string> {
        std::lock_guard<std::mutex> lk(state.mutex);
        if (std::find(state.profiles.begin(), state.profiles.end(), id) == state.profiles.end()) {
            return std::unexpected("unknown profile: " + id);
        }
        state.profile = id;
        return {};
    };
    h.list_profiles = [&state] {
        std::lock_guard<std::mutex> lk(state.mutex);
        return state.profiles;
    };
    h.list_profiles_detailed = [&state] {
        std::lock_guard<std::mutex> lk(state.mutex);
        return state.profiles_detailed;
    };
    h.get_tone = [&state] {
        std::lock_guard<std::mutex> lk(state.mutex);
        return std::pair<double, double>{state.tone_x, state.tone_y};
    };
    h.set_tone = [&state](double x, double y) {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.tone_x = std::clamp(x, -1.0, 1.0);
        state.tone_y = std::clamp(y, -1.0, 1.0);
    };
    h.device_connected = [&state] {
        std::lock_guard<std::mutex> lk(state.mutex);
        return state.connected;
    };
    h.quit = [&state] {
        std::lock_guard<std::mutex> lk(state.mutex);
        ++state.quit_calls;
    };
    h.get_visualizer = [&state] {
        std::lock_guard<std::mutex> lk(state.mutex);
        return keeby::VisualizerState{state.visualizer_enabled, state.visualizer_position,
                                       state.visualizer_dismiss_ms, state.visualizer_follow_speed};
    };
    h.set_visualizer = [&state](bool e) {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.visualizer_enabled = e;
    };
    h.set_visualizer_position = [&state](keeby::viz::Position p) {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.visualizer_position = p;
    };
    h.set_visualizer_dismiss = [&state](uint32_t ms) {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.visualizer_dismiss_ms = static_cast<uint16_t>(std::clamp<uint32_t>(ms, 250, 5000));
    };
    h.set_visualizer_follow_speed = [&state](double speed) {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.visualizer_follow_speed = std::clamp(speed, 0.1, 4.0);
    };
    return h;
}

// Redirects `target_fd` to a scratch file for the duration of `fn` and
// returns whatever was written to it. A temp file (not a pipe) avoids any
// risk of blocking on a full pipe buffer.
std::string capture_fd(int target_fd, const std::function<void()>& fn) {
    std::fflush(nullptr);
    const int saved = ::dup(target_fd);
    assert(saved != -1);

    char tmpl[] = "/tmp/keeby_ctl_test_XXXXXX";
    const int tmp_fd = ::mkstemp(tmpl);
    assert(tmp_fd != -1);
    ::unlink(tmpl); // unlinked but still open via tmp_fd/target_fd

    const int dup_rc = ::dup2(tmp_fd, target_fd);
    assert(dup_rc != -1);
    ::close(tmp_fd);

    fn();
    std::fflush(nullptr);

    std::string result;
    ::lseek(target_fd, 0, SEEK_SET);
    char buf[4096];
    ssize_t n;
    while ((n = ::read(target_fd, buf, sizeof buf)) > 0) result.append(buf, static_cast<std::size_t>(n));

    ::dup2(saved, target_fd);
    ::close(saved);
    return result;
}

void test_status_and_toggle(const std::string& bus, FakeState& state) {
    int rc = -1;
    std::string out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"status"}, bus); });
    assert(rc == 0);
    assert(out == "enabled=true volume=100 width=100 profile=default tone=0,0\nvisualizer=on,bottom-center,1000,1.0\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"toggle"}, bus); });
    assert(rc == 0);
    assert(out == "enabled=false\n");
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        assert(state.enabled == false);
    }

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"on"}, bus); });
    assert(rc == 0 && out == "enabled=true\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"off"}, bus); });
    assert(rc == 0 && out == "enabled=false\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"on"}, bus); }); // leave enabled for later tests
    assert(rc == 0 && out == "enabled=true\n");
}

void test_volume_and_width(const std::string& bus) {
    int rc = -1;
    std::string out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"volume", "80"}, bus); });
    assert(rc == 0 && out == "volume=80\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"volume", "+10"}, bus); });
    assert(rc == 0 && out == "volume=90\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"volume", "-50"}, bus); });
    assert(rc == 0 && out == "volume=40\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"volume", "500"}, bus); }); // callee clamps
    assert(rc == 0 && out == "volume=100\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"width", "150"}, bus); });
    assert(rc == 0 && out == "width=150\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"width", "250"}, bus); }); // callee clamps to 200
    assert(rc == 0 && out == "width=200\n");

    // Restore for determinism in later tests.
    rc = -1;
    capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"volume", "100"}, bus); });
    assert(rc == 0);
    capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"width", "100"}, bus); });
    assert(rc == 0);
}

void test_profile(const std::string& bus) {
    int rc = -1;
    std::string out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"profile", "clicky"}, bus); });
    assert(rc == 0 && out == "profile=clicky\n");

    std::string err = capture_fd(STDERR_FILENO, [&] { rc = keeby::run_ctl({"profile", "nonexistent"}, bus); });
    assert(rc == 1);
    assert(err.find("unknown profile: nonexistent") != std::string::npos);

    capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"profile", "default"}, bus); });
    assert(rc == 0);
}

void test_profiles_list(const std::string& bus) {
    int rc = -1;
    std::string out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"profiles"}, bus); });
    assert(rc == 0);
    assert(out == "default\nclicky\n");
}

void test_tone(const std::string& bus, FakeState& state) {
    int rc = -1;
    std::string out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"tone", "-50", "25"}, bus); });
    assert(rc == 0 && out == "tone=-50,25\n");
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        assert(state.tone_x == -0.5 && state.tone_y == 0.25);
    }

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"tone", "500", "-500"}, bus); }); // callee clamps
    assert(rc == 0 && out == "tone=100,-100\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"status"}, bus); });
    assert(rc == 0 && out == "enabled=true volume=100 width=100 profile=default tone=100,-100\nvisualizer=on,bottom-center,1000,1.0\n");

    // Restore for determinism in later tests (status is asserted again elsewhere).
    capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"tone", "0", "0"}, bus); });
    assert(rc == 0);

    capture_fd(STDERR_FILENO, [&] { rc = keeby::run_ctl({"tone", "abc", "0"}, bus); }); // unparseable
    assert(rc == 64);
    capture_fd(STDERR_FILENO, [&] { rc = keeby::run_ctl({"tone", "0"}, bus); }); // missing arg
    assert(rc == 64);
}

void test_list_profiles_detailed(const std::string& bus) {
    auto proxy = sdbus::createLightWeightProxy(sdbus::ServiceName{bus}, sdbus::ObjectPath{"/org/keeby/Keeby"});
    std::vector<sdbus::Struct<std::string, std::string>> detailed;
    proxy->callMethod("ListProfilesDetailed")
        .onInterface("org.keeby.Control1")
        .withTimeout(std::chrono::milliseconds(2000))
        .storeResultsTo(detailed);
    assert(detailed.size() == 2);
    assert(std::get<0>(detailed[0]) == "default" && std::get<1>(detailed[0]) == "Default");
    assert(std::get<0>(detailed[1]) == "clicky" && std::get<1>(detailed[1]) == "Clicky");
}

void test_get_info(const std::string& bus, FakeState& state) {
    auto proxy = sdbus::createLightWeightProxy(sdbus::ServiceName{bus}, sdbus::ObjectPath{"/org/keeby/Keeby"});
    std::string version;
    bool connected{};
    proxy->callMethod("GetInfo")
        .onInterface("org.keeby.Control1")
        .withTimeout(std::chrono::milliseconds(2000))
        .storeResultsTo(version, connected);
    assert(version == "0.1.0");
    assert(connected == true);

    {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.connected = false;
    }
    proxy->callMethod("GetInfo")
        .onInterface("org.keeby.Control1")
        .withTimeout(std::chrono::milliseconds(2000))
        .storeResultsTo(version, connected);
    assert(connected == false);
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.connected = true;
    }
}

void test_visualizer(const std::string& bus, FakeState& state) {
    int rc = -1;
    std::string out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "off"}, bus); });
    assert(rc == 0 && out == "visualizer=off\n");
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        assert(state.visualizer_enabled == false);
    }

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "on"}, bus); });
    assert(rc == 0 && out == "visualizer=on\n");

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "position", "top-right"}, bus); });
    assert(rc == 0 && out == "visualizer_position=top-right\n");
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        assert(state.visualizer_position == keeby::viz::Position::TopRight);
    }

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "dismiss", "3000"}, bus); });
    assert(rc == 0 && out == "visualizer_dismiss_ms=3000\n");
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        assert(state.visualizer_dismiss_ms == 3000);
    }

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "dismiss", "50"}, bus); }); // callee clamps
    assert(rc == 0 && out == "visualizer_dismiss_ms=50\n"); // run_ctl echoes the raw arg; the daemon clamps
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        assert(state.visualizer_dismiss_ms == 250);
    }

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "speed", "2.5"}, bus); });
    assert(rc == 0 && out == "visualizer_follow_speed=2.5\n");
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        assert(state.visualizer_follow_speed == 2.5);
    }

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "speed", "50"}, bus); }); // callee clamps
    assert(rc == 0 && out == "visualizer_follow_speed=4.0\n");
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        assert(state.visualizer_follow_speed == 4.0);
    }

    // Huge-but-finite: passes require_finite, must still clamp safely (the
    // engine converts via follow_speed_to_step, whose double->long scaling
    // would otherwise be UB on this input).
    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "speed", "1e308"}, bus); });
    assert(rc == 0 && out == "visualizer_follow_speed=4.0\n");
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        assert(state.visualizer_follow_speed == 4.0);
    }

    out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"status"}, bus); });
    assert(rc == 0);
    assert(out.find("visualizer=on,top-right,250,4.0\n") != std::string::npos);

    // Unknown position: rejected by ControlService itself, before ever
    // calling the handler.
    std::string err = capture_fd(STDERR_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "position", "sideways"}, bus); });
    assert(rc == 1);
    assert(err.find("unknown visualizer position") != std::string::npos);

    // Restore for determinism in later tests.
    capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "on"}, bus); });
    capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "position", "bottom-center"}, bus); });
    capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "dismiss", "1000"}, bus); });
    capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"visualizer", "speed", "1.0"}, bus); });
}

void test_visualizer_invalid_argument_raw(const std::string& bus) {
    auto proxy = sdbus::createLightWeightProxy(sdbus::ServiceName{bus}, sdbus::ObjectPath{"/org/keeby/Keeby"});
    bool threw = false;
    try {
        proxy->callMethod("SetVisualizerPosition")
            .onInterface("org.keeby.Control1")
            .withTimeout(std::chrono::milliseconds(2000))
            .withArguments(std::string("nowhere"));
    } catch (const sdbus::Error& e) {
        threw = true;
        assert(e.getName() == "org.keeby.Error.InvalidArgument");
    }
    assert(threw);
}

// ControlService itself never decides WHEN to emit StateChanged -- that's
// main()'s job (its refresh_and_mark_dirty calls notify_state_changed()
// after every tray/Control1 state change; see docs/011). This test proves
// notify_state_changed() actually reaches a live subscriber, standing in
// for that glue the way this file has no main() to exercise directly.
void test_state_changed_signal(const std::string& bus, keeby::ControlService& service) {
    // Needs its own event-loop thread to receive an async signal push --
    // unlike the lightweight proxy every other test here uses for plain
    // synchronous calls, which deliberately runs no event loop at all.
    auto proxy = sdbus::createProxy(sdbus::ServiceName{bus}, sdbus::ObjectPath{"/org/keeby/Keeby"});
    std::mutex mtx;
    std::condition_variable cv;
    bool received = false;
    proxy->uponSignal("StateChanged").onInterface("org.keeby.Control1").call([&] {
        std::lock_guard<std::mutex> lk(mtx);
        received = true;
        cv.notify_one();
    });

    service.notify_state_changed();

    std::unique_lock<std::mutex> lk(mtx);
    assert(cv.wait_for(lk, std::chrono::seconds(2), [&] { return received; }));
}

void test_not_running_exit_code() {
    int rc = -1;
    std::string err = capture_fd(STDERR_FILENO, [&] {
        rc = keeby::run_ctl({"status"}, "org.keeby.KeebyTest.NoSuchInstance");
    });
    assert(rc == 2);
    assert(err.find("not running") != std::string::npos);
}

void test_usage_errors() {
    int rc = -1;
    capture_fd(STDERR_FILENO, [&] { rc = keeby::run_ctl({}, "org.keeby.Whatever"); });
    assert(rc == 64);
    capture_fd(STDERR_FILENO, [&] { rc = keeby::run_ctl({"bogus-command"}, "org.keeby.Whatever"); });
    assert(rc == 64);
    capture_fd(STDERR_FILENO, [&] { rc = keeby::run_ctl({"volume"}, "org.keeby.Whatever"); }); // missing arg
    assert(rc == 64);
    capture_fd(STDERR_FILENO, [&] { rc = keeby::run_ctl({"volume", "abc"}, "org.keeby.Whatever"); }); // unparseable
    assert(rc == 64);

    std::string out = capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"help"}, "org.keeby.Whatever"); });
    assert(rc == 0);
    assert(!out.empty());
}

void test_invalid_argument_and_profile_failed_raw(const std::string& bus) {
    auto proxy = sdbus::createLightWeightProxy(sdbus::ServiceName{bus}, sdbus::ObjectPath{"/org/keeby/Keeby"});

    bool threw = false;
    try {
        proxy->callMethod("SetVolume")
            .onInterface("org.keeby.Control1")
            .withTimeout(std::chrono::milliseconds(2000))
            .withArguments(std::numeric_limits<double>::quiet_NaN());
    } catch (const sdbus::Error& e) {
        threw = true;
        assert(e.getName() == "org.keeby.Error.InvalidArgument");
    }
    assert(threw);

    threw = false;
    try {
        proxy->callMethod("SetTone")
            .onInterface("org.keeby.Control1")
            .withTimeout(std::chrono::milliseconds(2000))
            .withArguments(std::numeric_limits<double>::infinity(), 0.0);
    } catch (const sdbus::Error& e) {
        threw = true;
        assert(e.getName() == "org.keeby.Error.InvalidArgument");
    }
    assert(threw);

    threw = false;
    try {
        proxy->callMethod("SetProfile")
            .onInterface("org.keeby.Control1")
            .withTimeout(std::chrono::milliseconds(2000))
            .withArguments(std::string("totally-bogus-profile"));
    } catch (const sdbus::Error& e) {
        threw = true;
        assert(e.getName() == "org.keeby.Error.ProfileFailed");
        assert(e.getMessage().find("totally-bogus-profile") != std::string::npos);
    }
    assert(threw);
}

void test_quit(const std::string& bus, FakeState& state) {
    int rc = -1;
    capture_fd(STDOUT_FILENO, [&] { rc = keeby::run_ctl({"quit"}, bus); });
    assert(rc == 0);
    std::lock_guard<std::mutex> lk(state.mutex);
    assert(state.quit_calls == 1);
}

} // namespace

int main() {
    if (!has_session_bus()) {
        std::puts("control_service_test: SKIP (no DBUS_SESSION_BUS_ADDRESS)");
        return 77;
    }

    const std::string bus_name = "org.keeby.KeebyTest.p" + std::to_string(::getpid());

    FakeState state;
    auto server_conn = sdbus::createSessionBusConnection();
    assert(keeby::acquire_instance_name(*server_conn, bus_name));

    { // Single-instance guard: a second owner attempt must fail.
        auto second_conn = sdbus::createSessionBusConnection();
        assert(!keeby::acquire_instance_name(*second_conn, bus_name));
    }

    keeby::ControlService service(*server_conn, make_handlers(state));
    server_conn->enterEventLoopAsync();

    test_status_and_toggle(bus_name, state);
    test_volume_and_width(bus_name);
    test_profile(bus_name);
    test_profiles_list(bus_name);
    test_tone(bus_name, state);
    test_list_profiles_detailed(bus_name);
    test_get_info(bus_name, state);
    test_visualizer(bus_name, state);
    test_visualizer_invalid_argument_raw(bus_name);
    test_state_changed_signal(bus_name, service);
    test_not_running_exit_code();
    test_usage_errors();
    test_invalid_argument_and_profile_failed_raw(bus_name);
    test_quit(bus_name, state);

    std::puts("control_service_test: OK (every run_ctl subcommand incl. tone and visualizer, "
              "single-instance guard, ListProfilesDetailed/GetTone/SetTone/GetInfo/GetVisualizer, "
              "StateChanged signal, ProfileFailed/InvalidArgument error paths, not-running exit code)");
    return 0;
}
