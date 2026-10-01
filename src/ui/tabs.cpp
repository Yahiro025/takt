#include "tabs.hpp"

#include <cmath>
#include <cstdio>

#include "settings_logic.hpp"
#include "sound_controls.hpp"
#include "systemctl.hpp"

namespace keeby::ui {
namespace {

GtkWidget* make_card() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "kb-card");
    return box;
}

GtkWidget* make_divider() {
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(sep, "kb-divider");
    return sep;
}

GtkWidget* make_row_label(const char* text, const char* css_class) {
    GtkWidget* label = gtk_label_new(text);
    gtk_widget_add_css_class(label, css_class);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

GtkWidget* make_row(GtkWidget* left, GtkWidget* right) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(row, 8);
    gtk_widget_set_margin_bottom(row, 8);
    gtk_box_append(GTK_BOX(row), left);
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(row), spacer);
    gtk_box_append(GTK_BOX(row), right);
    return row;
}

// --- General tab ---------------------------------------------------------

void on_launch_switch_state_set(GtkSwitch*, gboolean state, gpointer user_data) {
    auto* ctx = static_cast<AppContext*>(user_data);
    if (ctx->suppress_launch_switch_signal) return;
    systemctl_user_async({state ? "enable" : "disable", "keeby.service"});
}

gboolean launch_switch_state_set_cb(GtkSwitch* sw, gboolean state, gpointer user_data) {
    on_launch_switch_state_set(sw, state, user_data);
    return FALSE; // let GTK still apply the requested state visually
}

void on_sound_switch_state_set(GtkSwitch*, gboolean state, gpointer user_data) {
    auto* ctx = static_cast<AppContext*>(user_data);
    if (ctx->suppress_sound_switch_signal) return;
    ctx->client.set_enabled(state);
}

gboolean sound_switch_state_set_cb(GtkSwitch* sw, gboolean state, gpointer user_data) {
    on_sound_switch_state_set(sw, state, user_data);
    return FALSE;
}

} // namespace

GtkWidget* build_general_tab(AppContext& ctx) {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);
    gtk_widget_set_margin_top(page, 16);
    gtk_widget_set_margin_bottom(page, 16);

    // Header card: our own icon + name + version.
    GtkWidget* header = make_card();
    GtkWidget* header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    // Packaging installs our icon into the hicolor theme (see CMakeLists.txt),
    // where icon-name lookup finds it; in an unpackaged dev/test run it
    // won't be there yet, so fall back to loading the source SVG directly.
    GtkIconTheme* icon_theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    GtkWidget* icon;
    if (gtk_icon_theme_has_icon(icon_theme, "keeby")) {
        icon = gtk_image_new_from_icon_name("keeby");
    } else {
        icon = gtk_image_new_from_file(KEEBY_ICON_DEV_PATH);
    }
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 48);
    GtkWidget* header_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* title = make_row_label("KEEBY", "kb-heading");
    ctx.version_label = make_row_label("Version …", "kb-secondary");
    gtk_box_append(GTK_BOX(header_text), title);
    gtk_box_append(GTK_BOX(header_text), ctx.version_label);
    gtk_box_append(GTK_BOX(header_row), icon);
    gtk_box_append(GTK_BOX(header_row), header_text);
    gtk_box_append(GTK_BOX(header), header_row);
    gtk_box_append(GTK_BOX(page), header);

    // Controls card.
    GtkWidget* card = make_card();

    ctx.launch_switch = gtk_switch_new();
    gtk_widget_set_valign(ctx.launch_switch, GTK_ALIGN_CENTER);
    ctx.suppress_launch_switch_signal = true;
    gtk_switch_set_active(GTK_SWITCH(ctx.launch_switch), systemctl_user_is_enabled("keeby.service"));
    ctx.suppress_launch_switch_signal = false;
    g_signal_connect(ctx.launch_switch, "state-set", G_CALLBACK(launch_switch_state_set_cb), &ctx);
    gtk_box_append(GTK_BOX(card), make_row(make_row_label("Launch at Login", "kb-body"), ctx.launch_switch));
    gtk_box_append(GTK_BOX(card), make_divider());

    GtkWidget* kb_status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ctx.keyboard_dot_label = gtk_label_new(nullptr);
    ctx.keyboard_status_label = make_row_label("Unknown", "kb-secondary");
    gtk_box_append(GTK_BOX(kb_status_box), ctx.keyboard_dot_label);
    gtk_box_append(GTK_BOX(kb_status_box), ctx.keyboard_status_label);
    gtk_box_append(GTK_BOX(card), make_row(make_row_label("Keyboard Access", "kb-body"), kb_status_box));
    gtk_box_append(GTK_BOX(card), make_divider());

    ctx.sound_switch = gtk_switch_new();
    gtk_widget_set_valign(ctx.sound_switch, GTK_ALIGN_CENTER);
    g_signal_connect(ctx.sound_switch, "state-set", G_CALLBACK(sound_switch_state_set_cb), &ctx);
    gtk_box_append(GTK_BOX(card), make_row(make_row_label("Sound", "kb-body"), ctx.sound_switch));

    gtk_box_append(GTK_BOX(page), card);
    return page;
}

// --- Sound tab -------------------------------------------------------------

namespace {

struct WidthDebounce {
    Throttle throttle{std::chrono::milliseconds(33)};
    guint flush_source = 0;
    AppContext* ctx = nullptr;
};

gboolean width_flush_cb(gpointer user_data) {
    auto* wd = static_cast<WidthDebounce*>(user_data);
    wd->flush_source = 0;
    wd->ctx->client.set_stereo_width(gtk_range_get_value(GTK_RANGE(wd->ctx->width_scale)) / 100.0);
    return G_SOURCE_REMOVE;
}

void on_width_value_changed(GtkRange* range, gpointer user_data) {
    auto* ctx = static_cast<AppContext*>(user_data);
    const double percent = gtk_range_get_value(range);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(percent + 0.5));
    gtk_label_set_text(GTK_LABEL(ctx->width_value_label), buf);
    if (ctx->suppress_width_signal) return;

    auto* wd = static_cast<WidthDebounce*>(g_object_get_data(G_OBJECT(range), "width-debounce"));
    if (wd->throttle.should_emit(std::chrono::steady_clock::now(), false)) {
        ctx->client.set_stereo_width(percent / 100.0);
    }
    if (wd->flush_source) g_source_remove(wd->flush_source);
    wd->flush_source = g_timeout_add(60, width_flush_cb, wd);
}

struct RowData {
    AppContext* ctx;
    std::string id;
};

void row_clicked(GtkButton* button, gpointer) {
    auto* rd = static_cast<RowData*>(g_object_get_data(G_OBJECT(button), "row-data"));
    if (rd->id == rd->ctx->current_profile || rd->id == rd->ctx->pending_profile) return;
    rd->ctx->pending_profile = rd->id;
    rd->ctx->error_profile.clear();
    AppContext* ctx = rd->ctx;
    std::string id = rd->id;
    rebuild_switches_list(*ctx);
    ctx->client.set_profile(id, [ctx, id](bool ok, std::string error) {
        if (ctx->pending_profile == id) ctx->pending_profile.clear();
        if (ok) {
            ctx->current_profile = id;
            ctx->error_profile.clear();
        } else {
            ctx->error_profile = id;
            ctx->error_message = std::move(error);
        }
        rebuild_switches_list(*ctx);
    });
}

GtkWidget* build_switch_row(AppContext& ctx, const ProfileEntry& p) {
    GtkWidget* wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* swatch = gtk_drawing_area_new();
    gtk_widget_set_size_request(swatch, 14, 14);
    gtk_widget_set_valign(swatch, GTK_ALIGN_CENTER);
    const std::string color = color_for_id(p.id);
    auto* color_copy = new std::string(color);
    g_object_set_data_full(G_OBJECT(swatch), "color", color_copy,
                            [](gpointer d) { delete static_cast<std::string*>(d); });
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(swatch),
        [](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) {
            auto* c = static_cast<std::string*>(data);
            GdkRGBA rgba{};
            gdk_rgba_parse(&rgba, c->c_str());
            cairo_set_source_rgb(cr, rgba.red, rgba.green, rgba.blue);
            cairo_new_sub_path(cr);
            const double r = 4.0;
            cairo_arc(cr, w - r, r, r, -M_PI_2, 0);
            cairo_arc(cr, w - r, h - r, r, 0, M_PI_2);
            cairo_arc(cr, r, h - r, r, M_PI_2, M_PI);
            cairo_arc(cr, r, r, r, M_PI, 3 * M_PI_2);
            cairo_close_path(cr);
            cairo_fill(cr);
        },
        color_copy, nullptr);
    gtk_box_append(GTK_BOX(row), swatch);
    gtk_box_append(GTK_BOX(row), make_row_label(p.name.c_str(), "kb-body"));

    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(row), spacer);

    const bool pending = ctx.pending_profile == p.id;
    const bool selected = ctx.current_profile == p.id;
    if (pending) {
        GtkWidget* spinner = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_box_append(GTK_BOX(row), spinner);
    } else if (selected) {
        GtkWidget* check = gtk_image_new_from_icon_name("object-select-symbolic");
        gtk_box_append(GTK_BOX(row), check);
    }

    GtkWidget* button = gtk_button_new();
    gtk_widget_add_css_class(button, "kb-row");
    gtk_widget_add_css_class(button, "flat");
    gtk_button_set_child(GTK_BUTTON(button), row);
    auto* rd = new RowData{&ctx, p.id};
    g_object_set_data_full(G_OBJECT(button), "row-data", rd, [](gpointer d) { delete static_cast<RowData*>(d); });
    g_signal_connect(button, "clicked", G_CALLBACK(row_clicked), nullptr);
    gtk_box_append(GTK_BOX(wrap), button);

    if (ctx.error_profile == p.id) {
        GtkWidget* err = gtk_label_new(ctx.error_message.c_str());
        gtk_widget_add_css_class(err, "kb-error");
        gtk_label_set_xalign(GTK_LABEL(err), 0.0);
        gtk_widget_set_margin_start(err, 32);
        gtk_widget_set_margin_bottom(err, 4);
        gtk_box_append(GTK_BOX(wrap), err);
    }
    return wrap;
}

} // namespace

void rebuild_switches_list(AppContext& ctx) {
    if (!ctx.switches_box) return;
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(ctx.switches_box))) gtk_box_remove(GTK_BOX(ctx.switches_box), child);

    std::vector<ProfileEntry> entries;
    entries.reserve(ctx.profiles.size());
    for (const auto& [id, name] : ctx.profiles) entries.push_back(ProfileEntry{id, name});
    const auto groups = group_profiles_by_brand(entries);

    for (const auto& group : groups) {
        GtkWidget* heading = make_row_label(group.brand.c_str(), "kb-heading");
        gtk_widget_set_margin_top(heading, 6);
        gtk_box_append(GTK_BOX(ctx.switches_box), heading);

        GtkWidget* card = make_card();
        for (size_t i = 0; i < group.profiles.size(); ++i) {
            gtk_box_append(GTK_BOX(card), build_switch_row(ctx, group.profiles[i]));
            if (i + 1 < group.profiles.size()) gtk_box_append(GTK_BOX(card), make_divider());
        }
        gtk_box_append(GTK_BOX(ctx.switches_box), card);
    }

    GtkWidget* hint = make_row_label("More sounds: run keeby-fetch-packs", "kb-secondary");
    gtk_widget_set_margin_top(hint, 4);
    gtk_box_append(GTK_BOX(ctx.switches_box), hint);
}

GtkWidget* build_sound_tab(AppContext& ctx) {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);
    gtk_widget_set_margin_top(page, 16);
    gtk_widget_set_margin_bottom(page, 16);

    GtkWidget* top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_set_halign(top_row, GTK_ALIGN_CENTER);
    ctx.volume_pill = create_volume_pill(&ctx.client);
    ctx.tone_pad = create_tone_pad(&ctx.client);
    gtk_box_append(GTK_BOX(top_row), ctx.volume_pill);
    gtk_box_append(GTK_BOX(top_row), ctx.tone_pad);
    gtk_box_append(GTK_BOX(page), top_row);

    GtkWidget* width_card = make_card();
    GtkWidget* width_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(width_header), make_row_label("Stereo Width", "kb-body"));
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(width_header), spacer);
    ctx.width_value_label = make_row_label("100%", "kb-mono");
    gtk_box_append(GTK_BOX(width_header), ctx.width_value_label);
    gtk_box_append(GTK_BOX(width_card), width_header);

    ctx.width_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 200, 1);
    gtk_scale_set_draw_value(GTK_SCALE(ctx.width_scale), FALSE);
    gtk_widget_add_css_class(ctx.width_scale, "kb-slider");
    gtk_range_set_value(GTK_RANGE(ctx.width_scale), 100);
    auto* wd = new WidthDebounce{};
    wd->ctx = &ctx;
    g_object_set_data_full(G_OBJECT(ctx.width_scale), "width-debounce", wd,
                            [](gpointer d) { delete static_cast<WidthDebounce*>(d); });
    g_signal_connect(ctx.width_scale, "value-changed", G_CALLBACK(on_width_value_changed), &ctx);
    gtk_box_append(GTK_BOX(width_card), ctx.width_scale);
    gtk_box_append(GTK_BOX(page), width_card);

    GtkWidget* switches_heading = make_row_label("Switches", "kb-heading");
    gtk_box_append(GTK_BOX(page), switches_heading);
    ctx.switches_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(page), ctx.switches_box);

    return page;
}

} // namespace keeby::ui
