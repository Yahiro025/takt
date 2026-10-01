#pragma once

// Async GDBus client for org.keeby.Control1, used only by keeby-settings.
// All calls are non-blocking (GDBusProxy async calls integrated with the
// GLib main loop GTK already runs) so the UI never freezes waiting on the
// engine. Bus name defaults to "org.keeby.Keeby" and is overridable via the
// KEEBY_BUS_NAME env var (tests point this at a private fake service).

#include <gio/gio.h>

#include <functional>
#include <string>
#include <vector>

namespace keeby::ui {

std::string default_bus_name();

struct EngineInfo {
    std::string version;
    bool device_connected = false;
};

struct EngineState {
    bool enabled = false;
    double volume = 0.0;
    double stereo_width = 0.0;
    std::string profile;
};

struct VisualizerState {
    bool enabled = false;
    // "top-left","top-center","top-right","bottom-left","bottom-center","bottom-right","follow-cursor"
    std::string position;
    unsigned dismiss_ms = 1000;
    double follow_speed = 1.0; // [0.1, 4.0]; follow-cursor speed multiplier
};

// Thin wrapper around one GDBusProxy targeting org.keeby.Control1 at
// /org/keeby/Keeby on the session bus. Watches the bus name so callers
// learn when KEEBY starts/stops without polling.
class KeebyClient {
public:
    explicit KeebyClient(std::string bus_name = default_bus_name());
    ~KeebyClient();
    KeebyClient(const KeebyClient&) = delete;
    KeebyClient& operator=(const KeebyClient&) = delete;

    // Called (possibly more than once) whenever the name gains/loses an
    // owner, including once synchronously-soon after construction with the
    // initial state.
    void on_availability_changed(std::function<void(bool available)> cb);

    // Called after every StateChanged signal from the engine.
    void on_state_changed(std::function<void()> cb);

    bool is_available() const { return proxy_ != nullptr; }

    void get_state(std::function<void(bool ok, EngineState)> done);
    void get_visualizer(std::function<void(bool ok, VisualizerState)> done);
    void get_tone(std::function<void(bool ok, double x, double y)> done);
    void get_info(std::function<void(bool ok, EngineInfo)> done);
    void list_profiles_detailed(std::function<void(bool ok, std::vector<std::pair<std::string, std::string>>)> done);
    void toggle(std::function<void(bool ok, bool enabled)> done);
    // `done` reports success/failure; on failure `error` holds the D-Bus
    // error message (e.g. from org.keeby.Error.ProfileFailed).
    void set_profile(const std::string& id, std::function<void(bool ok, std::string error)> done);

    // Fire-and-forget setters: no result the UI needs to act on beyond the
    // StateChanged signal the engine emits afterward.
    void set_enabled(bool enabled);
    void set_volume(double volume);
    void set_stereo_width(double width);
    void set_tone(double x, double y);
    void set_visualizer(bool enabled);
    void set_visualizer_position(const std::string& position);
    void set_visualizer_dismiss(unsigned ms);
    void set_visualizer_follow_speed(double multiplier);
    void quit();

private:
    using RawDone = std::function<void(GVariant* result, GError* error)>;
    void call(const char* method, GVariant* params, RawDone done);

    static void on_name_appeared(GDBusConnection*, const gchar*, const gchar*, gpointer user_data);
    static void on_name_vanished(GDBusConnection*, const gchar*, gpointer user_data);
    static void on_proxy_ready(GObject*, GAsyncResult*, gpointer user_data);
    static void on_g_signal(GDBusProxy*, const gchar* sender, const gchar* signal, GVariant* params,
                             gpointer user_data);

    std::string bus_name_;
    guint watch_id_ = 0;
    GDBusProxy* proxy_ = nullptr;
    gulong signal_handler_id_ = 0;
    std::function<void(bool)> availability_cb_;
    std::function<void()> state_changed_cb_;
};

} // namespace keeby::ui
