// Test-only stand-in for the real `keeby` engine's org.keeby.Control1
// D-Bus service. Implements the whole contract (existing methods plus the
// ListProfilesDetailed/GetTone/SetTone/GetInfo/StateChanged additions
// keeby-settings consumes) on a bus name given on argv[1] or $KEEBY_BUS_NAME
// (default org.keeby.Keeby) -- tests always pass a unique name so this
// never collides with a real running KEEBY. Prints "READY\n" to stdout
// once the name is acquired, then serves requests until killed.

#include <gio/gio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kIntrospectionXml = R"XML(
<node>
  <interface name="org.keeby.Control1">
    <method name="Toggle"><arg type="b" direction="out"/></method>
    <method name="SetEnabled"><arg type="b" direction="in"/></method>
    <method name="SetVolume"><arg type="d" direction="in"/></method>
    <method name="SetStereoWidth"><arg type="d" direction="in"/></method>
    <method name="SetProfile"><arg type="s" direction="in"/></method>
    <method name="ListProfiles"><arg type="as" direction="out"/></method>
    <method name="ListProfilesDetailed"><arg type="a(ss)" direction="out"/></method>
    <method name="GetState">
      <arg type="b" direction="out"/><arg type="d" direction="out"/>
      <arg type="d" direction="out"/><arg type="s" direction="out"/>
    </method>
    <method name="GetTone"><arg type="d" direction="out"/><arg type="d" direction="out"/></method>
    <method name="SetTone"><arg type="d" direction="in"/><arg type="d" direction="in"/></method>
    <method name="GetInfo"><arg type="s" direction="out"/><arg type="b" direction="out"/></method>
    <method name="GetVisualizer">
      <arg type="b" direction="out"/><arg type="s" direction="out"/><arg type="u" direction="out"/>
      <arg type="d" direction="out"/>
    </method>
    <method name="SetVisualizer"><arg type="b" direction="in"/></method>
    <method name="SetVisualizerPosition"><arg type="s" direction="in"/></method>
    <method name="SetVisualizerDismiss"><arg type="u" direction="in"/></method>
    <method name="SetVisualizerFollowSpeed"><arg type="d" direction="in"/></method>
    <method name="Quit"/>
    <signal name="StateChanged"/>
  </interface>
</node>
)XML";

struct FakeState {
    bool enabled = true;
    double volume = 1.0;
    double stereo_width = 1.0;
    std::string profile = "default";
    double tone_x = 0.0;
    double tone_y = 0.0;
    bool visualizer_enabled = false;
    std::string visualizer_position = "top-center";
    guint32 visualizer_dismiss_ms = 1000;
    gdouble visualizer_follow_speed = 1.0;
    std::vector<std::pair<std::string, std::string>> profiles = {
        {"default", "Default (built-in)"},
        {"cherry_blue", "Cherry MX Blue"},
        {"cherry_red", "Cherry MX Red"},
        {"gateron_yellow", "Gateron Yellow"},
    };
    GDBusConnection* connection = nullptr;
};

void emit_state_changed(FakeState* state) {
    g_dbus_connection_emit_signal(state->connection, nullptr, "/org/keeby/Keeby", "org.keeby.Control1",
                                   "StateChanged", nullptr, nullptr);
}

void handle_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method_name,
                         GVariant* parameters, GDBusMethodInvocation* invocation, gpointer user_data) {
    auto* state = static_cast<FakeState*>(user_data);

    if (g_strcmp0(method_name, "Toggle") == 0) {
        state->enabled = !state->enabled;
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", state->enabled));
        emit_state_changed(state);
    } else if (g_strcmp0(method_name, "SetEnabled") == 0) {
        gboolean enabled = FALSE;
        g_variant_get(parameters, "(b)", &enabled);
        state->enabled = enabled;
        g_dbus_method_invocation_return_value(invocation, nullptr);
        emit_state_changed(state);
    } else if (g_strcmp0(method_name, "SetVolume") == 0) {
        g_variant_get(parameters, "(d)", &state->volume);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        emit_state_changed(state);
    } else if (g_strcmp0(method_name, "SetStereoWidth") == 0) {
        g_variant_get(parameters, "(d)", &state->stereo_width);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        emit_state_changed(state);
    } else if (g_strcmp0(method_name, "SetProfile") == 0) {
        gchar* id = nullptr;
        g_variant_get(parameters, "(s)", &id);
        bool found = false;
        for (const auto& p : state->profiles) found = found || p.first == id;
        if (!found) {
            g_dbus_method_invocation_return_dbus_error(invocation, "org.keeby.Error.ProfileFailed",
                                                         "unknown profile");
        } else {
            state->profile = id;
            g_dbus_method_invocation_return_value(invocation, nullptr);
            emit_state_changed(state);
        }
        g_free(id);
    } else if (g_strcmp0(method_name, "ListProfiles") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (const auto& p : state->profiles) g_variant_builder_add(&b, "s", p.first.c_str());
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &b));
    } else if (g_strcmp0(method_name, "ListProfilesDetailed") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ss)"));
        for (const auto& p : state->profiles) g_variant_builder_add(&b, "(ss)", p.first.c_str(), p.second.c_str());
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ss))", &b));
    } else if (g_strcmp0(method_name, "GetState") == 0) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(bdds)", state->enabled, state->volume, state->stereo_width,
                                       state->profile.c_str()));
    } else if (g_strcmp0(method_name, "GetTone") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(dd)", state->tone_x, state->tone_y));
    } else if (g_strcmp0(method_name, "SetTone") == 0) {
        g_variant_get(parameters, "(dd)", &state->tone_x, &state->tone_y);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        emit_state_changed(state);
    } else if (g_strcmp0(method_name, "GetInfo") == 0) {
        const char* version = std::getenv("FAKE_VERSION");
        const char* connected_env = std::getenv("FAKE_DEVICE_CONNECTED");
        const gboolean connected = !connected_env || std::strcmp(connected_env, "0") != 0;
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(sb)", version ? version : "0.1.0-test", connected));
    } else if (g_strcmp0(method_name, "GetVisualizer") == 0) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(bsud)", state->visualizer_enabled, state->visualizer_position.c_str(),
                                       state->visualizer_dismiss_ms, state->visualizer_follow_speed));
    } else if (g_strcmp0(method_name, "SetVisualizer") == 0) {
        gboolean enabled = FALSE;
        g_variant_get(parameters, "(b)", &enabled);
        state->visualizer_enabled = enabled;
        g_dbus_method_invocation_return_value(invocation, nullptr);
        emit_state_changed(state);
    } else if (g_strcmp0(method_name, "SetVisualizerPosition") == 0) {
        gchar* position = nullptr;
        g_variant_get(parameters, "(s)", &position);
        state->visualizer_position = position ? position : "";
        g_free(position);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        emit_state_changed(state);
    } else if (g_strcmp0(method_name, "SetVisualizerDismiss") == 0) {
        g_variant_get(parameters, "(u)", &state->visualizer_dismiss_ms);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        emit_state_changed(state);
    } else if (g_strcmp0(method_name, "SetVisualizerFollowSpeed") == 0) {
        g_variant_get(parameters, "(d)", &state->visualizer_follow_speed);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        emit_state_changed(state);
    } else if (g_strcmp0(method_name, "Quit") == 0) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                               "unknown method %s", method_name);
    }
}

const GDBusInterfaceVTable kVTable = {handle_method_call, nullptr, nullptr, {nullptr}};

void on_bus_acquired(GDBusConnection* connection, const gchar*, gpointer user_data) {
    auto* state = static_cast<FakeState*>(user_data);
    state->connection = connection;
    GError* error = nullptr;
    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kIntrospectionXml, &error);
    if (!node) {
        std::fprintf(stderr, "fake_keeby_service: bad introspection xml: %s\n", error->message);
        std::exit(1);
    }
    g_dbus_connection_register_object(connection, "/org/keeby/Keeby", node->interfaces[0], &kVTable, state,
                                       nullptr, &error);
    if (error) {
        std::fprintf(stderr, "fake_keeby_service: register_object failed: %s\n", error->message);
        std::exit(1);
    }
    g_dbus_node_info_unref(node);
}

void on_name_acquired(GDBusConnection*, const gchar*, gpointer) {
    std::printf("READY\n");
    std::fflush(stdout);
}

void on_name_lost(GDBusConnection*, const gchar* name, gpointer) {
    std::fprintf(stderr, "fake_keeby_service: could not acquire name %s\n", name);
    std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
    const char* env_name = std::getenv("KEEBY_BUS_NAME");
    const std::string bus_name = argc > 1 ? argv[1] : (env_name && *env_name ? env_name : "org.keeby.Keeby");

    FakeState state;
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    guint owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, bus_name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
                                     on_bus_acquired, on_name_acquired, on_name_lost, &state, nullptr);
    g_main_loop_run(loop);
    g_bus_unown_name(owner_id);
    g_main_loop_unref(loop);
    return 0;
}
