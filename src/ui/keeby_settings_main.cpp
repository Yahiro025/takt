// keeby-settings: GTK4 settings window for KEEBY. Talks to the running
// `keeby` engine only over D-Bus (org.keeby.Control1); no audio, input or
// engine code linked into this process. Single-instance via GtkApplication's
// default GApplication uniqueness (app id org.keeby.Settings).

#include <gtk/gtk.h>

#include "app.hpp"

namespace {

void activate(GtkApplication* app, gpointer user_data) {
    auto* ctx = static_cast<keeby::ui::AppContext*>(user_data);
    GtkWidget* window = keeby::ui::build_window(app, *ctx);
    gtk_window_present(GTK_WINDOW(window));
}

} // namespace

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("org.keeby.Settings", G_APPLICATION_DEFAULT_FLAGS);
    keeby::ui::AppContext ctx;
    g_signal_connect(app, "activate", G_CALLBACK(activate), &ctx);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
