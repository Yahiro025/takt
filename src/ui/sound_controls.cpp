#include "sound_controls.hpp"

#include <algorithm>
#include <cmath>

#include "settings_logic.hpp"

namespace keeby::ui {
namespace {

constexpr double kPadSize = 176.0;
constexpr double kPillWidth = 64.0;
constexpr double kPillHeight = 176.0;
constexpr double kThrottleMs = 33; // ~30/s, per spec

void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI_2);
    cairo_close_path(cr);
}

// --- Volume pill --------------------------------------------------------

struct PillState {
    KeebyClient* client = nullptr;
    double volume = 1.0; // [0,1]
    bool dragging = false;
    Throttle throttle{std::chrono::milliseconds(static_cast<int>(kThrottleMs))};
};

double pill_point_to_volume(double py, double height) {
    return std::clamp(1.0 - py / height, 0.0, 1.0);
}

void pill_draw(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
    auto* st = static_cast<PillState*>(data);

    // White pill base -- the "fill" that shows the level.
    rounded_rect(cr, 0, 0, width, height, width / 2.0);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.08);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Dark translucent track over the *unfilled* portion only (top down to
    // the level), so the current volume reads as "how much white is showing
    // through", not an accent-colored bar.
    const double fill_h = st->volume * height;
    const double unfilled_h = height - fill_h;
    if (unfilled_h > 0.0) {
        cairo_save(cr);
        rounded_rect(cr, 0, 0, width, height, width / 2.0);
        cairo_clip(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
        cairo_rectangle(cr, 0, 0, width, unfilled_h);
        cairo_fill(cr);
        cairo_restore(cr);
    }

    // Label and glyph draw last, in dark grey, on top of both the white
    // fill and the dark overlay so they stay legible at any level.
    cairo_set_source_rgba(cr, 0.25, 0.25, 0.25, 0.9);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_move_to(cr, width / 2.0 - 18, 18);
    cairo_show_text(cr, "Volume");

    // Speaker glyph near the bottom (simple triangle+arcs, no icon theme dep).
    const double gx = width / 2.0, gy = height - 20;
    cairo_move_to(cr, gx - 6, gy - 4);
    cairo_line_to(cr, gx - 2, gy - 4);
    cairo_line_to(cr, gx + 3, gy - 8);
    cairo_line_to(cr, gx + 3, gy + 8);
    cairo_line_to(cr, gx - 2, gy + 4);
    cairo_line_to(cr, gx - 6, gy + 4);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_arc(cr, gx + 3, gy, 6, -0.6, 0.6);
    cairo_set_line_width(cr, 1.4);
    cairo_stroke(cr);
}

void pill_send(PillState* st, bool final) {
    if (!st->throttle.should_emit(std::chrono::steady_clock::now(), final)) return;
    if (st->client) st->client->set_volume(st->volume);
}

void pill_update_from_point(GtkWidget* area, PillState* st, double py, bool final) {
    st->volume = pill_point_to_volume(py, gtk_widget_get_height(area));
    gtk_widget_queue_draw(area);
    pill_send(st, final);
}

void pill_drag_begin(GtkGestureDrag*, double, double y, gpointer data) {
    auto* pair = static_cast<std::pair<GtkWidget*, PillState*>*>(data);
    pair->second->dragging = true;
    pill_update_from_point(pair->first, pair->second, y, false);
}

void pill_drag_update(GtkGestureDrag* gesture, double, double offset_y, gpointer data) {
    auto* pair = static_cast<std::pair<GtkWidget*, PillState*>*>(data);
    double sx = 0, sy = 0;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    pill_update_from_point(pair->first, pair->second, sy + offset_y, false);
}

void pill_drag_end(GtkGestureDrag* gesture, double, double offset_y, gpointer data) {
    auto* pair = static_cast<std::pair<GtkWidget*, PillState*>*>(data);
    double sx = 0, sy = 0;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    pill_update_from_point(pair->first, pair->second, sy + offset_y, true);
    pair->second->dragging = false;
}

// --- Tone pad -------------------------------------------------------------

struct PadState {
    KeebyClient* client = nullptr;
    Tone tone{};
    bool dragging = false;
    Throttle throttle{std::chrono::milliseconds(static_cast<int>(kThrottleMs))};
};

void pad_draw(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
    auto* st = static_cast<PadState*>(data);
    rounded_rect(cr, 0, 0, width, height, 12.0);
    cairo_set_source_rgb(cr, 0x3D / 255.0, 0x3D / 255.0, 0x3D / 255.0);
    cairo_fill(cr);

    // Dot grid.
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    for (int gx = 12; gx < width; gx += 16) {
        for (int gy = 12; gy < height; gy += 16) {
            cairo_arc(cr, gx, gy, 1.0, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    // Crosshair axes.
    cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, width / 2.0, 0);
    cairo_line_to(cr, width / 2.0, height);
    cairo_move_to(cr, 0, height / 2.0);
    cairo_line_to(cr, width, height / 2.0);
    cairo_stroke(cr);

    // Corner labels.
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
    cairo_move_to(cr, 8, 14);
    cairo_show_text(cr, "Warm");
    cairo_move_to(cr, width - 38, 14);
    cairo_show_text(cr, "Bright");
    cairo_move_to(cr, 8, height - 8);
    cairo_show_text(cr, "Thock");
    cairo_move_to(cr, width - 38, height - 8);
    cairo_show_text(cr, "Clack");

    // Knob.
    const auto [px, py] = tone_to_pixel(st->tone, width, height);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_arc(cr, px, py, 9.0, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, px, py, 9.0, 0, 2 * M_PI);
    cairo_stroke(cr);
}

void pad_send(PadState* st, bool final) {
    if (!st->throttle.should_emit(std::chrono::steady_clock::now(), final)) return;
    if (st->client) st->client->set_tone(st->tone.x, st->tone.y);
}

void pad_update_from_point(GtkWidget* area, PadState* st, double px, double py, bool final) {
    st->tone = pixel_to_tone(px, py, gtk_widget_get_width(area), gtk_widget_get_height(area));
    gtk_widget_queue_draw(area);
    pad_send(st, final);
}

void pad_drag_begin(GtkGestureDrag*, double x, double y, gpointer data) {
    auto* pair = static_cast<std::pair<GtkWidget*, PadState*>*>(data);
    pair->second->dragging = true;
    pad_update_from_point(pair->first, pair->second, x, y, false);
}

void pad_drag_update(GtkGestureDrag* gesture, double offset_x, double offset_y, gpointer data) {
    auto* pair = static_cast<std::pair<GtkWidget*, PadState*>*>(data);
    double sx = 0, sy = 0;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    pad_update_from_point(pair->first, pair->second, sx + offset_x, sy + offset_y, false);
}

void pad_drag_end(GtkGestureDrag* gesture, double offset_x, double offset_y, gpointer data) {
    auto* pair = static_cast<std::pair<GtkWidget*, PadState*>*>(data);
    double sx = 0, sy = 0;
    gtk_gesture_drag_get_start_point(gesture, &sx, &sy);
    pad_update_from_point(pair->first, pair->second, sx + offset_x, sy + offset_y, true);
    pair->second->dragging = false;
}

void pad_double_click(GtkGestureClick*, gint n_press, double, double, gpointer data) {
    if (n_press != 2) return;
    auto* pair = static_cast<std::pair<GtkWidget*, PadState*>*>(data);
    pair->second->tone = Tone{0.0, 0.0};
    gtk_widget_queue_draw(pair->first);
    pad_send(pair->second, true);
}

} // namespace

GtkWidget* create_volume_pill(KeebyClient* client) {
    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, static_cast<int>(kPillWidth), static_cast<int>(kPillHeight));
    auto* st = new PillState{client};
    g_object_set_data_full(G_OBJECT(area), "pill-state", st, [](gpointer p) { delete static_cast<PillState*>(p); });
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), pill_draw, st, nullptr);

    auto* pair = new std::pair<GtkWidget*, PillState*>{area, st};
    g_object_set_data_full(G_OBJECT(area), "pill-pair", pair,
                            [](gpointer p) { delete static_cast<std::pair<GtkWidget*, PillState*>*>(p); });

    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(pill_drag_begin), pair);
    g_signal_connect(drag, "drag-update", G_CALLBACK(pill_drag_update), pair);
    g_signal_connect(drag, "drag-end", G_CALLBACK(pill_drag_end), pair);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));
    return area;
}

void volume_pill_set_value(GtkWidget* pill, double volume) {
    auto* st = static_cast<PillState*>(g_object_get_data(G_OBJECT(pill), "pill-state"));
    st->volume = std::clamp(volume, 0.0, 1.0);
    gtk_widget_queue_draw(pill);
}

bool volume_pill_is_dragging(GtkWidget* pill) {
    auto* st = static_cast<PillState*>(g_object_get_data(G_OBJECT(pill), "pill-state"));
    return st->dragging;
}

GtkWidget* create_tone_pad(KeebyClient* client) {
    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, static_cast<int>(kPadSize), static_cast<int>(kPadSize));
    auto* st = new PadState{client};
    g_object_set_data_full(G_OBJECT(area), "pad-state", st, [](gpointer p) { delete static_cast<PadState*>(p); });
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), pad_draw, st, nullptr);

    auto* pair = new std::pair<GtkWidget*, PadState*>{area, st};
    g_object_set_data_full(G_OBJECT(area), "pad-pair", pair,
                            [](gpointer p) { delete static_cast<std::pair<GtkWidget*, PadState*>*>(p); });

    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(pad_drag_begin), pair);
    g_signal_connect(drag, "drag-update", G_CALLBACK(pad_drag_update), pair);
    g_signal_connect(drag, "drag-end", G_CALLBACK(pad_drag_end), pair);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));

    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(pad_double_click), pair);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
    return area;
}

void tone_pad_set_value(GtkWidget* pad, double x, double y) {
    auto* st = static_cast<PadState*>(g_object_get_data(G_OBJECT(pad), "pad-state"));
    st->tone = clamp_tone(Tone{x, y});
    gtk_widget_queue_draw(pad);
}

bool tone_pad_is_dragging(GtkWidget* pad) {
    auto* st = static_cast<PadState*>(g_object_get_data(G_OBJECT(pad), "pad-state"));
    return st->dragging;
}

} // namespace keeby::ui
