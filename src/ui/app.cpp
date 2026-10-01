#include "app.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "sound_controls.hpp"
#include "systemctl.hpp"
#include "tabs.hpp"

namespace keeby::ui {
namespace {

const char* kCss = R"CSS(
window { background-color: #222222; }
label.kb-heading { font-weight: 600; font-size: 13px; color: #ffffff; }
label.kb-body { font-size: 13px; color: #ffffff; }
label.kb-secondary { font-size: 11px; color: alpha(#ffffff, 0.55); }
label.kb-mono { font-family: monospace; font-size: 12px; color: alpha(#ffffff, 0.7); }
label.kb-error { font-size: 11px; color: #E0665B; }

.kb-card { background-color: #292929; border-radius: 10px; padding: 4px 12px; }
separator.kb-divider { background-color: alpha(#ffffff, 0.08); min-height: 1px; }

.kb-switcher { background-color: #28272A; border-radius: 999px; padding: 2px; }
.kb-switcher-tab { border-radius: 999px; padding: 4px 16px; color: #ffffff;
                    font-size: 13px; font-weight: 600; background: none; border: none;
                    transition: background-color 150ms ease; }
.kb-switcher-tab:checked { background-color: #4B4B4B; }

switch { transition: background-color 120ms ease; }
switch:checked { background-color: #197EF5; }

scale.kb-slider trough { background-color: #3D3D3D; border-radius: 6px; min-height: 6px; }
scale.kb-slider trough highlight { background-color: #197EF5; border-radius: 6px; }
scale.kb-slider slider { background-color: #ffffff; border-radius: 50%; min-width: 16px; min-height: 16px; }

button.kb-row { background: none; border: none; border-radius: 8px; transition: background-color 150ms ease; }
button.kb-row:hover { background-color: alpha(#ffffff, 0.05); }

.kb-not-running { background-color: #292929; border-radius: 10px; padding: 16px; }
button.kb-start { background-color: #197EF5; color: #ffffff; border-radius: 8px; padding: 6px 16px; }

button.kb-tile { background-color: #292929; border: 1px solid transparent; border-radius: 10px;
                  transition: background-color 150ms ease, border-color 150ms ease; }
button.kb-tile:hover { background-color: #333333; }
button.kb-tile.kb-tile-selected { background-image: linear-gradient(135deg, #174079, #28313C);
                                    border-color: #3E8EF7; }
)CSS";

void install_css() {
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, kCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void switcher_toggled(GtkToggleButton* button, gpointer user_data) {
    if (!gtk_toggle_button_get_active(button)) return;
    auto* pair = static_cast<std::pair<AppContext*, const char*>*>(user_data);
    gtk_stack_set_visible_child_name(GTK_STACK(pair->first->stack), pair->second);
}

GtkWidget* build_switcher(AppContext& ctx) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "kb-switcher");

    GtkWidget* general_btn = gtk_toggle_button_new_with_label("General");
    GtkWidget* sound_btn = gtk_toggle_button_new_with_label("Sound");
    GtkWidget* viz_btn = gtk_toggle_button_new_with_label("Visualizer");
    gtk_widget_add_css_class(general_btn, "kb-switcher-tab");
    gtk_widget_add_css_class(sound_btn, "kb-switcher-tab");
    gtk_widget_add_css_class(viz_btn, "kb-switcher-tab");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(sound_btn), GTK_TOGGLE_BUTTON(general_btn));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(viz_btn), GTK_TOGGLE_BUTTON(general_btn));

    static const char* kGeneral = "general";
    static const char* kSound = "sound";
    static const char* kVisualizer = "visualizer";
    auto* general_pair = new std::pair<AppContext*, const char*>{&ctx, kGeneral};
    auto* sound_pair = new std::pair<AppContext*, const char*>{&ctx, kSound};
    auto* viz_pair = new std::pair<AppContext*, const char*>{&ctx, kVisualizer};
    g_object_set_data_full(G_OBJECT(general_btn), "pair", general_pair,
                            [](gpointer p) { delete static_cast<std::pair<AppContext*, const char*>*>(p); });
    g_object_set_data_full(G_OBJECT(sound_btn), "pair", sound_pair,
                            [](gpointer p) { delete static_cast<std::pair<AppContext*, const char*>*>(p); });
    g_object_set_data_full(G_OBJECT(viz_btn), "pair", viz_pair,
                            [](gpointer p) { delete static_cast<std::pair<AppContext*, const char*>*>(p); });
    // Connect the "toggled" handlers *before* setting the initial active
    // button below -- otherwise the initial activation fires no signal (no
    // handler is attached yet) and the stack never receives its starting
    // page, leaving it on GtkStack's own default (its first-added child,
    // which happens to still often look plausible, masking the bug).
    g_signal_connect(general_btn, "toggled", G_CALLBACK(switcher_toggled), general_pair);
    g_signal_connect(sound_btn, "toggled", G_CALLBACK(switcher_toggled), sound_pair);
    g_signal_connect(viz_btn, "toggled", G_CALLBACK(switcher_toggled), viz_pair);

    // ponytail: dev/screenshot-only convenience, not part of the design spec
    // -- lets the verification script land on any tab deterministically
    // without needing Wayland input automation. Defaults to General.
    const char* initial_tab = std::getenv("KEEBY_SETTINGS_TAB");
    const std::string requested = initial_tab ? initial_tab : "";
    GtkWidget* start_btn = general_btn;
    const char* start_name = kGeneral;
    if (requested == "sound") {
        start_btn = sound_btn;
        start_name = kSound;
    } else if (requested == "visualizer") {
        start_btn = viz_btn;
        start_name = kVisualizer;
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(start_btn), TRUE);
    // Belt-and-suspenders: make the starting page correct even if some GTK
    // version doesn't emit "toggled" for a set_active() that's a no-op
    // (e.g. a button that defaults to already-active).
    gtk_stack_set_visible_child_name(GTK_STACK(ctx.stack), start_name);

    gtk_box_append(GTK_BOX(box), general_btn);
    gtk_box_append(GTK_BOX(box), sound_btn);
    gtk_box_append(GTK_BOX(box), viz_btn);
    return box;
}

void start_clicked(GtkButton*, gpointer) { systemctl_user_async({"start", "keeby.service"}); }

GtkWidget* build_not_running_card() {
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "kb-not-running");
    gtk_widget_set_margin_start(card, 16);
    gtk_widget_set_margin_end(card, 16);
    gtk_widget_set_margin_top(card, 16);
    GtkWidget* label = gtk_label_new("KEEBY isn't running");
    gtk_widget_add_css_class(label, "kb-heading");
    GtkWidget* start = gtk_button_new_with_label("Start");
    gtk_widget_add_css_class(start, "kb-start");
    gtk_widget_set_halign(start, GTK_ALIGN_START);
    g_signal_connect(start, "clicked", G_CALLBACK(start_clicked), nullptr);
    gtk_box_append(GTK_BOX(card), label);
    gtk_box_append(GTK_BOX(card), start);
    return card;
}

} // namespace

void set_engine_available(AppContext& ctx, bool available) {
    gtk_widget_set_visible(ctx.not_running_card, !available);
    gtk_widget_set_sensitive(ctx.stack, available);
    if (available) refresh_from_engine(ctx);
}

void refresh_from_engine(AppContext& ctx) {
    ctx.client.get_state([&ctx](bool ok, EngineState state) {
        if (!ok) return;
        ctx.suppress_sound_switch_signal = true;
        gtk_switch_set_active(GTK_SWITCH(ctx.sound_switch), state.enabled);
        ctx.suppress_sound_switch_signal = false;

        if (!volume_pill_is_dragging(ctx.volume_pill)) volume_pill_set_value(ctx.volume_pill, state.volume);

        // ponytail: no drag-in-progress tracking for the width slider (unlike
        // the pad/pill, which have explicit drag-end events) -- a StateChanged
        // round trip from our own SetStereoWidth just re-sets the same value,
        // which is harmless; upgrade to a drag flag if concurrent-writer
        // jumpiness ever matters in practice.
        ctx.suppress_width_signal = true;
        gtk_range_set_value(GTK_RANGE(ctx.width_scale), state.stereo_width * 100.0);
        ctx.suppress_width_signal = false;

        if (ctx.current_profile != state.profile) {
            ctx.current_profile = state.profile;
            rebuild_switches_list(ctx);
        }
    });

    ctx.client.get_tone([&ctx](bool ok, double x, double y) {
        if (ok && !tone_pad_is_dragging(ctx.tone_pad)) tone_pad_set_value(ctx.tone_pad, x, y);
    });

    ctx.client.get_info([&ctx](bool ok, EngineInfo info) {
        if (!ok) return;
        char buf[64];
        std::snprintf(buf, sizeof buf, "Version %s", info.version.c_str());
        gtk_label_set_text(GTK_LABEL(ctx.version_label), buf);
        if (info.device_connected) {
            gtk_label_set_markup(GTK_LABEL(ctx.keyboard_dot_label), "<span foreground='#3ABE6E'>●</span>");
            gtk_label_set_text(GTK_LABEL(ctx.keyboard_status_label), "Granted");
        } else {
            gtk_label_set_markup(GTK_LABEL(ctx.keyboard_dot_label), "<span foreground='#E0A63A'>●</span>");
            gtk_label_set_text(GTK_LABEL(ctx.keyboard_status_label), "No keyboard");
        }
    });

    ctx.client.list_profiles_detailed([&ctx](bool ok, std::vector<std::pair<std::string, std::string>> profiles) {
        if (!ok) return;
        ctx.profiles = std::move(profiles);
        rebuild_switches_list(ctx);
    });

    ctx.client.get_visualizer([&ctx](bool ok, VisualizerState viz) {
        if (!ok) return;
        ctx.suppress_viz_switch_signal = true;
        gtk_switch_set_active(GTK_SWITCH(ctx.viz_switch), viz.enabled);
        ctx.suppress_viz_switch_signal = false;

        // ponytail: no drag-in-progress tracking, same rationale as the
        // Stereo Width slider above -- a StateChanged echo of our own value
        // is harmless.
        ctx.suppress_viz_dismiss_signal = true;
        gtk_range_set_value(GTK_RANGE(ctx.viz_dismiss_scale), viz.dismiss_ms);
        ctx.suppress_viz_dismiss_signal = false;

        ctx.suppress_viz_follow_speed_signal = true;
        gtk_range_set_value(GTK_RANGE(ctx.viz_follow_speed_scale), viz.follow_speed);
        ctx.suppress_viz_follow_speed_signal = false;

        ctx.viz_position = viz.position;
        refresh_position_tiles(ctx);
    });
}

GtkWidget* build_window(GtkApplication* app, AppContext& ctx) {
    install_css();

    ctx.window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ctx.window), "KEEBY Settings");
    gtk_window_set_default_size(GTK_WINDOW(ctx.window), 440, 560);
    gtk_window_set_resizable(GTK_WINDOW(ctx.window), FALSE);

    // Built before the switcher: switcher_toggled() dereferences ctx.stack
    // as soon as the initial active tab is set.
    ctx.stack = gtk_stack_new();
    gtk_stack_set_vhomogeneous(GTK_STACK(ctx.stack), FALSE);
    gtk_stack_add_named(GTK_STACK(ctx.stack), build_general_tab(ctx), "general");
    gtk_stack_add_named(GTK_STACK(ctx.stack), build_sound_tab(ctx), "sound");
    gtk_stack_add_named(GTK_STACK(ctx.stack), build_visualizer_tab(ctx), "visualizer");

    GtkWidget* header = gtk_header_bar_new();
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), build_switcher(ctx));
    gtk_window_set_titlebar(GTK_WINDOW(ctx.window), header);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ctx.not_running_card = build_not_running_card();
    gtk_widget_set_visible(ctx.not_running_card, FALSE);
    gtk_box_append(GTK_BOX(root), ctx.not_running_card);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), ctx.stack);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(root), scroller);

    gtk_window_set_child(GTK_WINDOW(ctx.window), root);

    ctx.client.on_availability_changed([&ctx](bool available) { set_engine_available(ctx, available); });
    ctx.client.on_state_changed([&ctx] { refresh_from_engine(ctx); });

    return ctx.window;
}

} // namespace keeby::ui
