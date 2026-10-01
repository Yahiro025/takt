#pragma once

#include <gtk/gtk.h>

#include <string>
#include <utility>
#include <vector>

#include "kbus_client.hpp"

namespace keeby::ui {

// Everything the running window needs; one instance lives for the
// process's lifetime, owned by main() and threaded through the GTK
// callbacks as a raw pointer (GTK's C API leaves no room for RAII there).
struct AppContext {
    KeebyClient client;

    GtkWidget* window = nullptr;
    GtkWidget* stack = nullptr;        // "general" / "sound" / "visualizer" pages
    GtkWidget* not_running_card = nullptr;

    // General tab.
    GtkWidget* version_label = nullptr;
    GtkWidget* keyboard_dot_label = nullptr;
    GtkWidget* keyboard_status_label = nullptr;
    GtkWidget* launch_switch = nullptr;
    GtkWidget* sound_switch = nullptr;
    bool suppress_sound_switch_signal = false;
    bool suppress_launch_switch_signal = false;

    // Sound tab.
    GtkWidget* volume_pill = nullptr;
    GtkWidget* tone_pad = nullptr;
    GtkWidget* width_scale = nullptr;
    GtkWidget* width_value_label = nullptr;
    bool suppress_width_signal = false;
    GtkWidget* switches_box = nullptr;

    // Visualizer tab.
    GtkWidget* viz_switch = nullptr;
    GtkWidget* viz_dismiss_scale = nullptr;
    GtkWidget* viz_dismiss_value_label = nullptr;
    // 7 tiles: "Follow Cursor" (index 0) plus the 6 fixed positions.
    GtkWidget* viz_position_tiles[7] = {};
    GtkWidget* viz_follow_speed_row = nullptr; // only visible while "follow-cursor" is selected
    GtkWidget* viz_follow_speed_scale = nullptr;
    GtkWidget* viz_follow_speed_value_label = nullptr;
    std::string viz_position = "follow-cursor";
    bool suppress_viz_switch_signal = false;
    bool suppress_viz_dismiss_signal = false;
    bool suppress_viz_follow_speed_signal = false;

    std::vector<std::pair<std::string, std::string>> profiles; // id, display name
    std::string current_profile;
    std::string pending_profile; // SetProfile in flight for this id, if any
    std::string error_profile;   // last SetProfile failure, if any
    std::string error_message;

    AppContext() = default;
};

GtkWidget* build_window(GtkApplication* app, AppContext& ctx);

// Pulls GetState/GetTone/GetInfo/ListProfilesDetailed and refreshes every
// widget that isn't mid-drag. Called on startup, on the name appearing, and
// after every StateChanged signal.
void refresh_from_engine(AppContext& ctx);

void set_engine_available(AppContext& ctx, bool available);

} // namespace keeby::ui
