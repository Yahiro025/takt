// Transparent full-output backdrop for keeby-visualizer: the overlay is
// one layer-shell surface spanning the whole output, so the window and its
// drawing area opt out of every themed background, image and shadow; only
// the painted panel stays visible. Class-scoped (keeby-viz-*) to never
// affect other GTK apps; installed at APPLICATION priority like src/ui/app.cpp.

#pragma once

#include <gtk/gtk.h>

namespace keeby::viz {

inline constexpr const char* kBackdropWindowClass = "keeby-viz-window";
inline constexpr const char* kBackdropCanvasClass = "keeby-viz-canvas";

inline constexpr const char* kBackdropCss =
    "window.keeby-viz-window, window.keeby-viz-window decoration {\n"
    "  background-color: transparent;\n"
    "  background-image: none;\n"
    "  box-shadow: none;\n"
    "}\n"
    ".keeby-viz-window drawingarea, drawingarea.keeby-viz-canvas {\n"
    "  background-color: transparent;\n"
    "  background-image: none;\n"
    "}\n";

inline void apply_backdrop_style(GtkWidget* window, GtkWidget* area) {
    gtk_widget_add_css_class(window, kBackdropWindowClass);
    gtk_widget_add_css_class(area, kBackdropCanvasClass);
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, kBackdropCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

}  // namespace keeby::viz
