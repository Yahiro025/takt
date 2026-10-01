// Visualizer tab: on/off switch, dismiss-delay slider, the 7-tile position
// picker (Follow Cursor + 6 fixed positions), and the follow-speed slider
// shown only while Follow Cursor is selected. See docs/012-settings-window.md
// for the shared card/row styling this reuses, and docs/013-visualizer.md /
// docs/013-visualizer-app.md for the GetVisualizer/SetVisualizer* D-Bus
// contract.

#include "tabs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "settings_logic.hpp"

namespace keeby::ui {
namespace {

// Index 0 is the new "Follow Cursor" tile; 1..6 are the pre-existing 6
// fixed positions, laid out 4 + 3 (top row: Follow Cursor + 3 top
// positions; bottom row: the 3 bottom positions), matching Keeby's grid.
constexpr const char* kPositionIds[7] = {"follow-cursor", "top-left",     "top-center",   "top-right",
                                          "bottom-left",   "bottom-center", "bottom-right"};
constexpr const char* kPositionLabels[7] = {"Follow Cursor", "Top Left",     "Top Center",   "Top Right",
                                             "Bottom Left",   "Bottom Center", "Bottom Right"};

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

void on_viz_switch_state_set_impl(gboolean state, gpointer user_data) {
    auto* ctx = static_cast<AppContext*>(user_data);
    if (ctx->suppress_viz_switch_signal) return;
    ctx->client.set_visualizer(state);
}

gboolean viz_switch_state_set_cb(GtkSwitch*, gboolean state, gpointer user_data) {
    on_viz_switch_state_set_impl(state, user_data);
    return FALSE;
}

struct Debounce {
    Throttle throttle{std::chrono::milliseconds(33)};
    guint flush_source = 0;
    AppContext* ctx = nullptr;
};

void send_dismiss(AppContext* ctx) {
    const unsigned ms = static_cast<unsigned>(gtk_range_get_value(GTK_RANGE(ctx->viz_dismiss_scale)) + 0.5);
    ctx->client.set_visualizer_dismiss(ms);
}

gboolean viz_dismiss_flush_cb(gpointer user_data) {
    auto* d = static_cast<Debounce*>(user_data);
    d->flush_source = 0;
    send_dismiss(d->ctx);
    return G_SOURCE_REMOVE;
}

void on_viz_dismiss_value_changed(GtkRange* range, gpointer user_data) {
    auto* ctx = static_cast<AppContext*>(user_data);
    const double ms = gtk_range_get_value(range);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1fs", ms / 1000.0);
    gtk_label_set_text(GTK_LABEL(ctx->viz_dismiss_value_label), buf);
    if (ctx->suppress_viz_dismiss_signal) return;

    auto* d = static_cast<Debounce*>(g_object_get_data(G_OBJECT(range), "viz-dismiss-debounce"));
    if (d->throttle.should_emit(std::chrono::steady_clock::now(), false)) send_dismiss(ctx);
    if (d->flush_source) g_source_remove(d->flush_source);
    d->flush_source = g_timeout_add(60, viz_dismiss_flush_cb, d);
}

void send_follow_speed(AppContext* ctx) {
    ctx->client.set_visualizer_follow_speed(gtk_range_get_value(GTK_RANGE(ctx->viz_follow_speed_scale)));
}

gboolean viz_follow_speed_flush_cb(gpointer user_data) {
    auto* d = static_cast<Debounce*>(user_data);
    d->flush_source = 0;
    send_follow_speed(d->ctx);
    return G_SOURCE_REMOVE;
}

void on_viz_follow_speed_value_changed(GtkRange* range, gpointer user_data) {
    auto* ctx = static_cast<AppContext*>(user_data);
    const double speed = gtk_range_get_value(range);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1f\xc3\x97", speed); // U+00D7 MULTIPLICATION SIGN, e.g. "1.0×"
    gtk_label_set_text(GTK_LABEL(ctx->viz_follow_speed_value_label), buf);
    if (ctx->suppress_viz_follow_speed_signal) return;

    auto* d = static_cast<Debounce*>(g_object_get_data(G_OBJECT(range), "viz-follow-speed-debounce"));
    if (d->throttle.should_emit(std::chrono::steady_clock::now(), false)) send_follow_speed(ctx);
    if (d->flush_source) g_source_remove(d->flush_source);
    d->flush_source = g_timeout_add(60, viz_follow_speed_flush_cb, d);
}

// Small screen icon: index 0 (Follow Cursor) draws a cursor-arrow glyph;
// 1..6 draw a rounded outline with a filled bar at the edge/corner that
// tile represents (purely from the tile's index, no GTK theme dependency).
void draw_position_icon(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) {
    const int index = GPOINTER_TO_INT(data);
    if (index == 0) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
        const double cx = w * 0.42, cy = h * 0.5, s = std::min(w, h) * 0.5;
        cairo_move_to(cr, cx - s * 0.28, cy - s * 0.42);
        cairo_line_to(cr, cx - s * 0.28, cy + s * 0.42);
        cairo_line_to(cr, cx - s * 0.02, cy + s * 0.18);
        cairo_line_to(cr, cx + s * 0.16, cy + s * 0.34);
        cairo_line_to(cr, cx + s * 0.26, cy + s * 0.22);
        cairo_line_to(cr, cx + s * 0.08, cy + s * 0.06);
        cairo_line_to(cr, cx + s * 0.32, cy - s * 0.02);
        cairo_close_path(cr);
        cairo_fill(cr);
        return;
    }
    cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, 1.5, 1.5, w - 3.0, h - 3.0);
    cairo_stroke(cr);

    const int pos = index - 1; // 0..5 fixed-position index
    const bool top = pos < 3;
    const int col = pos % 3; // 0 left, 1 center, 2 right
    const double bar_w = w * 0.34;
    const double bar_h = h * 0.20;
    const double margin = 3.0;
    double x = margin;
    if (col == 1) x = (w - bar_w) / 2.0;
    if (col == 2) x = w - margin - bar_w;
    const double y = top ? margin : h - margin - bar_h;

    cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
    cairo_rectangle(cr, x, y, bar_w, bar_h);
    cairo_fill(cr);
}

struct TileData {
    AppContext* ctx;
    int index;
};

void tile_clicked(GtkButton*, gpointer user_data) {
    auto* td = static_cast<TileData*>(user_data);
    td->ctx->viz_position = kPositionIds[td->index];
    td->ctx->client.set_visualizer_position(kPositionIds[td->index]);
    refresh_position_tiles(*td->ctx);
}

GtkWidget* build_position_tile(AppContext& ctx, int index) {
    GtkWidget* btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "kb-tile");
    gtk_widget_set_hexpand(btn, TRUE);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    GtkWidget* icon = gtk_drawing_area_new();
    gtk_widget_set_size_request(icon, 56, 34);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(icon), draw_position_icon, GINT_TO_POINTER(index), nullptr);
    gtk_box_append(GTK_BOX(box), icon);

    GtkWidget* label = make_row_label(kPositionLabels[index], "kb-secondary");
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), label);

    gtk_button_set_child(GTK_BUTTON(btn), box);

    auto* td = new TileData{&ctx, index};
    g_object_set_data_full(G_OBJECT(btn), "tile-data", td, [](gpointer d) { delete static_cast<TileData*>(d); });
    g_signal_connect(btn, "clicked", G_CALLBACK(tile_clicked), td);

    ctx.viz_position_tiles[index] = btn;
    return btn;
}

} // namespace

void refresh_position_tiles(AppContext& ctx) {
    for (int i = 0; i < 7; ++i) {
        if (!ctx.viz_position_tiles[i]) continue;
        const bool selected = ctx.viz_position == kPositionIds[i];
        if (selected) {
            gtk_widget_add_css_class(ctx.viz_position_tiles[i], "kb-tile-selected");
        } else {
            gtk_widget_remove_css_class(ctx.viz_position_tiles[i], "kb-tile-selected");
        }
    }
    if (ctx.viz_follow_speed_row) {
        gtk_widget_set_visible(ctx.viz_follow_speed_row, ctx.viz_position == "follow-cursor");
    }
}

GtkWidget* build_visualizer_tab(AppContext& ctx) {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);
    gtk_widget_set_margin_top(page, 16);
    gtk_widget_set_margin_bottom(page, 16);

    GtkWidget* card = make_card();

    ctx.viz_switch = gtk_switch_new();
    gtk_widget_set_valign(ctx.viz_switch, GTK_ALIGN_CENTER);
    g_signal_connect(ctx.viz_switch, "state-set", G_CALLBACK(viz_switch_state_set_cb), &ctx);
    gtk_box_append(GTK_BOX(card), make_row(make_row_label("Show Visualizer", "kb-body"), ctx.viz_switch));
    gtk_box_append(GTK_BOX(card), make_divider());

    GtkWidget* dismiss_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(dismiss_header, 8);
    gtk_box_append(GTK_BOX(dismiss_header), make_row_label("Dismiss After", "kb-body"));
    GtkWidget* dismiss_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(dismiss_spacer, TRUE);
    gtk_box_append(GTK_BOX(dismiss_header), dismiss_spacer);
    ctx.viz_dismiss_value_label = make_row_label("1.0s", "kb-mono");
    gtk_box_append(GTK_BOX(dismiss_header), ctx.viz_dismiss_value_label);
    gtk_box_append(GTK_BOX(card), dismiss_header);

    ctx.viz_dismiss_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 250, 5000, 50);
    gtk_scale_set_draw_value(GTK_SCALE(ctx.viz_dismiss_scale), FALSE);
    gtk_widget_add_css_class(ctx.viz_dismiss_scale, "kb-slider");
    gtk_widget_set_margin_bottom(ctx.viz_dismiss_scale, 8);
    gtk_range_set_value(GTK_RANGE(ctx.viz_dismiss_scale), 1000);
    auto* dd = new Debounce{};
    dd->ctx = &ctx;
    g_object_set_data_full(G_OBJECT(ctx.viz_dismiss_scale), "viz-dismiss-debounce", dd,
                            [](gpointer p) { delete static_cast<Debounce*>(p); });
    g_signal_connect(ctx.viz_dismiss_scale, "value-changed", G_CALLBACK(on_viz_dismiss_value_changed), &ctx);
    gtk_box_append(GTK_BOX(card), ctx.viz_dismiss_scale);

    // Follow-speed row: only shown while "follow-cursor" is the selected
    // position (toggled by refresh_position_tiles()).
    ctx.viz_follow_speed_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(card), make_divider());
    GtkWidget* speed_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(speed_header, 8);
    gtk_box_append(GTK_BOX(speed_header), make_row_label("Follow Speed", "kb-body"));
    GtkWidget* speed_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(speed_spacer, TRUE);
    gtk_box_append(GTK_BOX(speed_header), speed_spacer);
    ctx.viz_follow_speed_value_label = make_row_label("1.0\xc3\x97", "kb-mono");
    gtk_box_append(GTK_BOX(speed_header), ctx.viz_follow_speed_value_label);
    gtk_box_append(GTK_BOX(ctx.viz_follow_speed_row), speed_header);

    ctx.viz_follow_speed_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.1, 4.0, 0.1);
    gtk_scale_set_draw_value(GTK_SCALE(ctx.viz_follow_speed_scale), FALSE);
    gtk_widget_add_css_class(ctx.viz_follow_speed_scale, "kb-slider");
    gtk_widget_set_margin_bottom(ctx.viz_follow_speed_scale, 8);
    gtk_range_set_value(GTK_RANGE(ctx.viz_follow_speed_scale), 1.0);
    auto* fd = new Debounce{};
    fd->ctx = &ctx;
    g_object_set_data_full(G_OBJECT(ctx.viz_follow_speed_scale), "viz-follow-speed-debounce", fd,
                            [](gpointer p) { delete static_cast<Debounce*>(p); });
    g_signal_connect(ctx.viz_follow_speed_scale, "value-changed", G_CALLBACK(on_viz_follow_speed_value_changed),
                      &ctx);
    gtk_box_append(GTK_BOX(ctx.viz_follow_speed_row), ctx.viz_follow_speed_scale);
    gtk_box_append(GTK_BOX(card), ctx.viz_follow_speed_row);

    gtk_box_append(GTK_BOX(page), card);

    GtkWidget* heading = make_row_label("Position", "kb-heading");
    gtk_widget_set_margin_top(heading, 6);
    gtk_box_append(GTK_BOX(page), heading);

    // 4 + 3 layout, like Keeby: Follow Cursor + the 3 top positions, then
    // the 3 bottom positions.
    GtkWidget* rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (int i = 0; i < 4; ++i) gtk_box_append(GTK_BOX(row1), build_position_tile(ctx, i));
    for (int i = 4; i < 7; ++i) gtk_box_append(GTK_BOX(row2), build_position_tile(ctx, i));
    gtk_box_append(GTK_BOX(rows), row1);
    gtk_box_append(GTK_BOX(rows), row2);
    gtk_box_append(GTK_BOX(page), rows);
    refresh_position_tiles(ctx);

    return page;
}

} // namespace keeby::ui
