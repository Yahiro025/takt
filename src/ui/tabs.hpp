#pragma once

#include <gtk/gtk.h>

#include "app.hpp"

namespace keeby::ui {

GtkWidget* build_general_tab(AppContext& ctx);
GtkWidget* build_sound_tab(AppContext& ctx);
GtkWidget* build_visualizer_tab(AppContext& ctx);

// Rebuilds ctx.switches_box's children from ctx.profiles/current_profile/
// pending_profile/error_profile. Cheap enough (a handful of rows) to just
// throw away and rebuild on every relevant change rather than diff.
void rebuild_switches_list(AppContext& ctx);

// Toggles the "kb-tile-selected" CSS class on ctx.viz_position_tiles to
// match ctx.viz_position. Called after building the tab and after every
// GetVisualizer refresh.
void refresh_position_tiles(AppContext& ctx);

} // namespace keeby::ui
