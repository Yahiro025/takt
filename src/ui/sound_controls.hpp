#pragma once

// Two hand-drawn GTK4 widgets used only by the Sound tab: a vertical volume
// pill and a 2D tone pad. Both are GtkDrawingArea + gesture controllers
// (plain GTK4, no libadwaita) and both throttle outgoing D-Bus updates via
// keeby::ui::Throttle while dragging, always flushing the final value.

#include <gtk/gtk.h>

#include "kbus_client.hpp"

namespace keeby::ui {

// Vertical pill, top = 1.0, bottom = 0.0. `client` must outlive the widget.
GtkWidget* create_volume_pill(KeebyClient* client);
void volume_pill_set_value(GtkWidget* pill, double volume);
bool volume_pill_is_dragging(GtkWidget* pill);

// Square pad; see settings_logic.hpp for the axis mapping. `client` must
// outlive the widget.
GtkWidget* create_tone_pad(KeebyClient* client);
void tone_pad_set_value(GtkWidget* pad, double x, double y);
bool tone_pad_is_dragging(GtkWidget* pad);

} // namespace keeby::ui
