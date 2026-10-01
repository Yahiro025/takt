#include "kbus_client.hpp"

#include <cstdlib>

namespace keeby::ui {
namespace {

constexpr const char* kObjectPath = "/org/keeby/Keeby";
constexpr const char* kInterface = "org.keeby.Control1";

} // namespace

std::string default_bus_name() {
    const char* env = std::getenv("KEEBY_BUS_NAME");
    return (env && *env) ? env : "org.keeby.Keeby";
}

KeebyClient::KeebyClient(std::string bus_name) : bus_name_(std::move(bus_name)) {
    watch_id_ = g_bus_watch_name(G_BUS_TYPE_SESSION, bus_name_.c_str(), G_BUS_NAME_WATCHER_FLAGS_NONE,
                                  &KeebyClient::on_name_appeared, &KeebyClient::on_name_vanished, this, nullptr);
}

KeebyClient::~KeebyClient() {
    if (watch_id_) g_bus_unwatch_name(watch_id_);
    if (proxy_) {
        if (signal_handler_id_) g_signal_handler_disconnect(proxy_, signal_handler_id_);
        g_object_unref(proxy_);
    }
}

void KeebyClient::on_availability_changed(std::function<void(bool)> cb) {
    availability_cb_ = std::move(cb);
    if (availability_cb_) availability_cb_(is_available());
}

void KeebyClient::on_state_changed(std::function<void()> cb) { state_changed_cb_ = std::move(cb); }

void KeebyClient::on_name_appeared(GDBusConnection*, const gchar*, const gchar*, gpointer user_data) {
    auto* self = static_cast<KeebyClient*>(user_data);
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, nullptr, self->bus_name_.c_str(),
                              kObjectPath, kInterface, nullptr, &KeebyClient::on_proxy_ready, self);
}

void KeebyClient::on_proxy_ready(GObject*, GAsyncResult* res, gpointer user_data) {
    auto* self = static_cast<KeebyClient*>(user_data);
    GError* error = nullptr;
    GDBusProxy* proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (!proxy) {
        if (error) g_error_free(error);
        return; // the name may have vanished again already; on_availability_changed(false) already fired
    }
    if (self->proxy_) {
        if (self->signal_handler_id_) g_signal_handler_disconnect(self->proxy_, self->signal_handler_id_);
        g_object_unref(self->proxy_);
    }
    self->proxy_ = proxy;
    self->signal_handler_id_ =
        g_signal_connect(self->proxy_, "g-signal", G_CALLBACK(&KeebyClient::on_g_signal), self);
    if (self->availability_cb_) self->availability_cb_(true);
}

void KeebyClient::on_name_vanished(GDBusConnection*, const gchar*, gpointer user_data) {
    auto* self = static_cast<KeebyClient*>(user_data);
    if (self->proxy_) {
        if (self->signal_handler_id_) g_signal_handler_disconnect(self->proxy_, self->signal_handler_id_);
        g_object_unref(self->proxy_);
        self->proxy_ = nullptr;
        self->signal_handler_id_ = 0;
    }
    if (self->availability_cb_) self->availability_cb_(false);
}

void KeebyClient::on_g_signal(GDBusProxy*, const gchar*, const gchar* signal, GVariant*, gpointer user_data) {
    auto* self = static_cast<KeebyClient*>(user_data);
    if (g_strcmp0(signal, "StateChanged") == 0 && self->state_changed_cb_) self->state_changed_cb_();
}

void KeebyClient::call(const char* method, GVariant* params, RawDone done) {
    if (!proxy_) {
        if (params) g_variant_unref(g_variant_ref_sink(params));
        if (done) done(nullptr, nullptr);
        return;
    }
    auto* ctx = new RawDone(std::move(done));
    g_dbus_proxy_call(
        proxy_, method, params, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer user_data) {
            auto* cb = static_cast<RawDone*>(user_data);
            GError* error = nullptr;
            GVariant* result = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &error);
            if (*cb) (*cb)(result, error);
            if (result) g_variant_unref(result);
            if (error) g_error_free(error);
            delete cb;
        },
        ctx);
}

void KeebyClient::get_state(std::function<void(bool, EngineState)> done) {
    call("GetState", nullptr, [done = std::move(done)](GVariant* result, GError* error) {
        if (!result || error) {
            if (done) done(false, EngineState{});
            return;
        }
        EngineState s;
        gboolean enabled = FALSE;
        gchar* profile = nullptr;
        g_variant_get(result, "(bdds)", &enabled, &s.volume, &s.stereo_width, &profile);
        s.enabled = enabled;
        s.profile = profile ? profile : "";
        g_free(profile);
        if (done) done(true, s);
    });
}

void KeebyClient::get_visualizer(std::function<void(bool, VisualizerState)> done) {
    call("GetVisualizer", nullptr, [done = std::move(done)](GVariant* result, GError* error) {
        if (!result || error) {
            if (done) done(false, VisualizerState{});
            return;
        }
        VisualizerState v;
        gboolean enabled = FALSE;
        gchar* position = nullptr;
        guint32 dismiss_ms = 0;
        gdouble follow_speed = 1.0;
        g_variant_get(result, "(bsud)", &enabled, &position, &dismiss_ms, &follow_speed);
        v.enabled = enabled;
        v.position = position ? position : "";
        v.dismiss_ms = dismiss_ms;
        v.follow_speed = follow_speed;
        g_free(position);
        if (done) done(true, v);
    });
}

void KeebyClient::get_tone(std::function<void(bool, double, double)> done) {
    call("GetTone", nullptr, [done = std::move(done)](GVariant* result, GError* error) {
        if (!result || error) {
            if (done) done(false, 0.0, 0.0);
            return;
        }
        double x = 0.0, y = 0.0;
        g_variant_get(result, "(dd)", &x, &y);
        if (done) done(true, x, y);
    });
}

void KeebyClient::get_info(std::function<void(bool, EngineInfo)> done) {
    call("GetInfo", nullptr, [done = std::move(done)](GVariant* result, GError* error) {
        if (!result || error) {
            if (done) done(false, EngineInfo{});
            return;
        }
        EngineInfo info;
        gchar* version = nullptr;
        gboolean connected = FALSE;
        g_variant_get(result, "(sb)", &version, &connected);
        info.version = version ? version : "";
        info.device_connected = connected;
        g_free(version);
        if (done) done(true, info);
    });
}

void KeebyClient::list_profiles_detailed(
    std::function<void(bool, std::vector<std::pair<std::string, std::string>>)> done) {
    call("ListProfilesDetailed", nullptr, [done = std::move(done)](GVariant* result, GError* error) {
        std::vector<std::pair<std::string, std::string>> profiles;
        if (!result || error) {
            if (done) done(false, profiles);
            return;
        }
        GVariant* array = g_variant_get_child_value(result, 0);
        GVariantIter iter;
        g_variant_iter_init(&iter, array);
        gchar* id = nullptr;
        gchar* name = nullptr;
        while (g_variant_iter_next(&iter, "(ss)", &id, &name)) {
            profiles.emplace_back(id, name);
            g_free(id);
            g_free(name);
        }
        g_variant_unref(array);
        if (done) done(true, profiles);
    });
}

void KeebyClient::toggle(std::function<void(bool, bool)> done) {
    call("Toggle", nullptr, [done = std::move(done)](GVariant* result, GError* error) {
        if (!result || error) {
            if (done) done(false, false);
            return;
        }
        gboolean enabled = FALSE;
        g_variant_get(result, "(b)", &enabled);
        if (done) done(true, enabled);
    });
}

void KeebyClient::set_profile(const std::string& id, std::function<void(bool, std::string)> done) {
    call("SetProfile", g_variant_new("(s)", id.c_str()),
         [done = std::move(done)](GVariant*, GError* error) {
             if (error) {
                 if (done) done(false, error->message ? error->message : "unknown error");
             } else if (done) {
                 done(true, "");
             }
         });
}

void KeebyClient::set_enabled(bool enabled) { call("SetEnabled", g_variant_new("(b)", enabled), nullptr); }

void KeebyClient::set_volume(double volume) { call("SetVolume", g_variant_new("(d)", volume), nullptr); }

void KeebyClient::set_stereo_width(double width) {
    call("SetStereoWidth", g_variant_new("(d)", width), nullptr);
}

void KeebyClient::set_tone(double x, double y) { call("SetTone", g_variant_new("(dd)", x, y), nullptr); }

void KeebyClient::set_visualizer(bool enabled) { call("SetVisualizer", g_variant_new("(b)", enabled), nullptr); }

void KeebyClient::set_visualizer_position(const std::string& position) {
    call("SetVisualizerPosition", g_variant_new("(s)", position.c_str()), nullptr);
}

void KeebyClient::set_visualizer_dismiss(unsigned ms) {
    call("SetVisualizerDismiss", g_variant_new("(u)", static_cast<guint32>(ms)), nullptr);
}

void KeebyClient::set_visualizer_follow_speed(double multiplier) {
    call("SetVisualizerFollowSpeed", g_variant_new("(d)", multiplier), nullptr);
}

void KeebyClient::quit() { call("Quit", nullptr, nullptr); }

} // namespace keeby::ui
