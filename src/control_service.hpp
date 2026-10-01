#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "visualizer_wire.hpp"

namespace keeby {

struct ControlState {
    bool enabled;
    double volume;        // [0, 1]
    double stereo_width;  // [0, 2]
    std::string profile;
};

struct VisualizerState {
    bool enabled;
    viz::Position position;
    uint16_t dismiss_ms;
    double follow_speed; // [0.1, 4.0], default 1.0 -- see visualizer_wire.hpp
};

// Wired to the real engine by a later step (2.8b); tests supply fakes.
struct ControlHandlers {
    std::function<ControlState()> get_state;
    std::function<void(bool)> set_enabled;
    std::function<void(double)> set_volume;        // callee clamps to [0, 1]
    std::function<void(double)> set_stereo_width;  // callee clamps to [0, 2]
    std::function<std::expected<void, std::string>(const std::string&)> set_profile;
    std::function<std::vector<std::string>()> list_profiles;
    // (id, display name) pairs, same order as list_profiles.
    std::function<std::vector<std::pair<std::string, std::string>>()> list_profiles_detailed;
    std::function<std::pair<double, double>()> get_tone;    // (x, y), each [-1, 1]
    std::function<void(double, double)> set_tone;           // callee clamps each to [-1, 1]
    std::function<bool()> device_connected;
    std::function<void()> quit;

    // Floating on-screen keyboard visualizer (docs/013-visualizer.md).
    // set_visualizer_position/set_visualizer_dismiss receive an
    // already-validated/clamped value: ControlService itself rejects an
    // unknown position string with org.keeby.Error.InvalidArgument before
    // ever calling the handler, and the dismiss_ms handler is expected to
    // clamp (mirrors set_volume/set_stereo_width's "callee clamps" contract).
    std::function<VisualizerState()> get_visualizer;
    std::function<void(bool)> set_visualizer;
    std::function<void(viz::Position)> set_visualizer_position;
    std::function<void(uint32_t)> set_visualizer_dismiss;
    std::function<void(double)> set_visualizer_follow_speed; // callee clamps to [0.1, 4.0]
};

// Exposes org.keeby.Control1 at /org/keeby/Keeby on an existing connection.
// Does not own or drive the connection's event loop (mirrors DBusMenu's
// contract in dbus_menu.hpp). Handlers run on the connection's event-loop
// thread; any exception a handler throws is turned into a D-Bus error
// reply, never left to escape the vtable dispatch.
class ControlService {
public:
    // `connection` must outlive this object; its event loop must be
    // running (or about to start) for method calls to be served.
    ControlService(sdbus::IConnection& connection, ControlHandlers handlers);
    ~ControlService();
    ControlService(const ControlService&) = delete;
    ControlService& operator=(const ControlService&) = delete;

    // Emits StateChanged() on org.keeby.Control1. Called by main() after any
    // state-changing action from any source (tray click/scroll/menu, a
    // Control1 setter, a profile switch) so an open settings window can
    // refresh live. Best-effort: never throws.
    void notify_state_changed() noexcept;

private:
    ControlHandlers handlers_;
    std::unique_ptr<sdbus::IObject> object_;
};

// Requests `name` on `connection` with no queueing. Returns false when
// another process already owns the name; any other requestName failure
// propagates as sdbus::Error.
bool acquire_instance_name(sdbus::IConnection& connection, const std::string& name = "org.keeby.Keeby");

// `keeby ctl <command> [args]` client: makes one call against `bus_name`
// and returns the process exit code (0 ok, 1 call failed, 2 KEEBY not
// running, 64 usage error). Prints results to stdout, errors to stderr.
int run_ctl(const std::vector<std::string>& args, const std::string& bus_name = "org.keeby.Keeby");

} // namespace keeby
