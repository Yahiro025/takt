// keeby-visualizer: the floating on-screen-keyboard overlay. Spawned by the
// engine with a private SOCK_SEQPACKET socket on fd 3 (see
// ../visualizer_wire.hpp); reads key/motion/config datagrams and renders a
// compact, Keeby-styled keyboard panel via gtk4-layer-shell, never taking
// keyboard focus or intercepting clicks. See docs/013-visualizer-app.md.
//
// One full-output transparent layer-shell surface (anchored to all four
// edges, no exclusive zone, empty input region) hosts the panel, which is
// drawn at a spring-smoothed position inside it -- fixed-anchor placement
// or cursor-following, both share this one surface, so switching between
// them is just a different animation target rather than a different
// window.
//
// Keystrokes are confidential: this file must never log, print, or persist
// a key code anywhere -- only use it to look up a layout rect and flip a
// highlight/ripple.

#include <gtk4-layer-shell.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include <cairo.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "cursor_estimator.hpp"
#include "cursor_probe.hpp"
#include "viz_backdrop_css.hpp"
#include "viz_dismiss.hpp"
#include "viz_layout.hpp"
#include "viz_position.hpp"
#include "viz_ripple.hpp"
#include "viz_spring.hpp"
#include "../visualizer_wire.hpp"

using namespace keeby::viz;
using namespace std::chrono;

namespace {

// --- Look: Keeby's dark panel, per the brief's frame-by-frame colors. ----
constexpr uint32_t kPanelBg = 0x1C1C1E;
constexpr uint32_t kPanelBorder = 0x3A3A3C;
constexpr uint32_t kKeycapBg = 0x2C2C2E;
constexpr uint32_t kKeycapRipple = 0x6A6A6E; // mid of the brief's ~#5A5A5E-#7A7A7E band
constexpr uint32_t kKeycapLit = 0xF2F2F7;
constexpr uint32_t kLegend = 0x8E8E93;
constexpr uint32_t kLegendLit = 0x1C1C1E;

constexpr int kCellPx = 18;       // pixels per layout unit -- yields a ~290x110 panel, "noticeably smaller"
constexpr int kPanelPad = 10;     // panel inset around the keys
constexpr double kPanelRadius = 8.0;
constexpr double kKeyRadius = 3.0;
constexpr double kKeyGapPx = 2.0;
constexpr int kEaseMs = 120;      // pressed -> released ease-back duration
constexpr int kFadeMs = 150;      // panel show/hide opacity+scale fade
constexpr int kTickMs = 16;       // ~60fps animation tick while active
constexpr double kAppearScaleFrom = 0.96;

constexpr int kShadowBlurPx = 24;
constexpr int kShadowOffsetY = 10;
constexpr double kShadowAlpha = 0.35;

// Follow-cursor: panel's top-left sits this far below-right of the
// estimated cursor tip (frame-by-frame measurement from the brief).
constexpr double kFollowOffsetX = 12.0;
constexpr double kFollowOffsetY = 18.0;

constexpr double kMaxTiltDeg = 14.0; // keep in sync with viz_spring.hpp's tilt_from_velocity() clamp

int panel_w() { return static_cast<int>(kLayoutWidthUnits * kCellPx) + 2 * kPanelPad; }
int panel_h() { return static_cast<int>(kLayoutHeightUnits * kCellPx) + 2 * kPanelPad; }

void set_rgb_hex(cairo_t* cr, uint32_t hex) {
    cairo_set_source_rgb(cr, ((hex >> 16) & 0xFF) / 255.0, ((hex >> 8) & 0xFF) / 255.0, (hex & 0xFF) / 255.0);
}

uint32_t lerp_hex(uint32_t a, uint32_t b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto lerp8 = [t](uint32_t av, uint32_t bv) -> uint32_t {
        return static_cast<uint32_t>(av + (static_cast<double>(bv) - av) * t + 0.5);
    };
    const uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    const uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return (lerp8(ar, br) << 16) | (lerp8(ag, bg) << 8) | lerp8(ab, bb);
}

struct Pt {
    double x, y;
};

struct KeyVisual {
    bool pressed = false;
    steady_clock::time_point released_at{};
    bool has_released_at = false;
};

struct AppState {
    GtkWidget* window = nullptr;
    GtkWidget* area = nullptr;
    DismissTimer dismiss;
    std::unordered_map<uint16_t, KeyVisual> visuals;
    RippleEffect ripples;

    uint8_t position = static_cast<uint8_t>(kDefaultPosition);
    double follow_speed = kDefaultFollowSpeed;
    CursorEstimator cursor;
    Bounds bounds;

    // Absolute-position probe (see cursor_probe.hpp / the probe functions
    // below): anchor_time_ms is glue bookkeeping -- neither CursorEstimator
    // nor ProbePolicy stores it themselves, so there's exactly one copy.
    ProbePolicy probe;
    int64_t anchor_time_ms = 0;
    guint probe_timeout_source = 0;
    gulong probe_hook_id = 0; // g_signal_add_emission_hook id on GdkSurface::event -- see on_surface_event_hook
    bool logged_first_anchor = false;

    Spring1D spring_x{25.0}, spring_y{25.0};
    double panel_x = 0.0, panel_y = 0.0;
    double tilt_rx = 0.0, tilt_ry = 0.0;

    cairo_surface_t* shadow = nullptr;

    guint tick_source = 0;
    steady_clock::time_point last_tick{};
    bool shown = false;
    steady_clock::time_point shown_since{};
    steady_clock::time_point hidden_since{};
    GMainLoop* loop = nullptr;
};

// Linear ease-back factor for one key: 1.0 held/just released, decaying to
// 0.0 over kEaseMs after release.
double highlight_factor(const KeyVisual& v, steady_clock::time_point now) {
    if (v.pressed) return 1.0;
    if (!v.has_released_at) return 0.0;
    const double elapsed_ms = duration_cast<duration<double, std::milli>>(now - v.released_at).count();
    if (elapsed_ms >= kEaseMs) return 0.0;
    return 1.0 - elapsed_ms / kEaseMs;
}

// Opacity (and, via appear_scale(), a matching subtle scale) fade factor
// for the whole panel around a show/hide edge: 0 hidden, 1 fully shown.
double fade_factor(const AppState& s, steady_clock::time_point now) {
    if (s.shown) {
        const double elapsed_ms = duration_cast<duration<double, std::milli>>(now - s.shown_since).count();
        return elapsed_ms >= kFadeMs ? 1.0 : elapsed_ms / kFadeMs;
    }
    const double elapsed_ms = duration_cast<duration<double, std::milli>>(now - s.hidden_since).count();
    return elapsed_ms >= kFadeMs ? 0.0 : 1.0 - elapsed_ms / kFadeMs;
}

double appear_scale(double fade) { return kAppearScaleFrom + (1.0 - kAppearScaleFrom) * fade; }

void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI_2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI_2);
    cairo_close_path(cr);
}

// Backspace/arrow legends render as tofu under the fonts on this machine,
// so these draw as cairo paths instead. Backspace: a left-pointing
// pentagon (the standard "delete" pictogram). Arrows: a filled triangle.
void draw_glyph(cairo_t* cr, Glyph glyph, double x, double y, double w, double h, uint32_t color) {
    const double cx = x + w / 2.0, cy = y + h / 2.0;
    set_rgb_hex(cr, color);
    if (glyph == Glyph::Backspace) {
        const double gw = std::min(w, h) * 1.1, gh = std::min(w, h) * 0.62;
        const double left = cx - gw / 2.0, right = cx + gw / 2.0;
        const double top = cy - gh / 2.0, bottom = cy + gh / 2.0;
        const double notch = gh / 2.0;
        cairo_move_to(cr, left, cy);
        cairo_line_to(cr, left + notch, top);
        cairo_line_to(cr, right, top);
        cairo_line_to(cr, right, bottom);
        cairo_line_to(cr, left + notch, bottom);
        cairo_close_path(cr);
        cairo_fill(cr);
    } else if (glyph == Glyph::ArrowLeft || glyph == Glyph::ArrowRight) {
        const double tw = std::min(w, h) * 0.55, th = std::min(w, h) * 0.65;
        // dir is the direction the arrow's tip points: -1 left, +1 right.
        const double dir = glyph == Glyph::ArrowLeft ? -1.0 : 1.0;
        cairo_move_to(cr, cx - dir * tw / 2.0, cy - th / 2.0); // flat edge, opposite the tip
        cairo_line_to(cr, cx - dir * tw / 2.0, cy + th / 2.0);
        cairo_line_to(cr, cx + dir * tw / 2.0, cy); // tip
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

// One box-blur pass on the alpha channel of a CAIRO_FORMAT_ARGB32 surface,
// keeping premultiplied RGB at 0 (a pure black shadow). Runs once at
// startup on a small (~340x160) surface, so an O(w*h*radius) blur is fine
// -- no need for a sliding-window/two-pass optimization here.
void box_blur_shadow(unsigned char* data, int w, int h, int stride, int radius) {
    std::vector<unsigned char> tmp(static_cast<size_t>(w) * h);
    auto alpha_at = [&](int x, int y) {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return data[y * stride + x * 4 + 3];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += alpha_at(x + k, y);
            tmp[static_cast<size_t>(y) * w + x] = static_cast<unsigned char>(sum / (2 * radius + 1));
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) {
                const int yy = std::clamp(y + k, 0, h - 1);
                sum += tmp[static_cast<size_t>(yy) * w + x];
            }
            unsigned char* px = data + y * stride + x * 4;
            px[0] = px[1] = px[2] = 0;
            px[3] = static_cast<unsigned char>(sum / (2 * radius + 1));
        }
    }
}

cairo_surface_t* make_shadow_surface(int panel_w_px, int panel_h_px) {
    const int w = panel_w_px + 2 * kShadowBlurPx;
    const int h = panel_h_px + 2 * kShadowBlurPx;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(surf);
    cairo_set_source_rgba(cr, 0, 0, 0, kShadowAlpha);
    rounded_rect(cr, kShadowBlurPx, kShadowBlurPx, panel_w_px, panel_h_px, kPanelRadius);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    box_blur_shadow(cairo_image_surface_get_data(surf), w, h, cairo_image_surface_get_stride(surf), kShadowBlurPx);
    cairo_surface_mark_dirty(surf);
    return surf;
}

Pt fixed_target(const Bounds& b, Anchor a) {
    const PanelPt t = fixed_target_in(b, panel_w(), panel_h(), a);
    return {t.x, t.y};
}

Pt follow_target(const AppState& s) {
    const PanelPt t =
        follow_target_in(s.bounds, s.cursor.x(), s.cursor.y(), panel_w(), panel_h(), kFollowOffsetX, kFollowOffsetY);
    return {t.x, t.y};
}

bool is_follow_cursor(const AppState& s) { return s.position == static_cast<uint8_t>(Position::FollowCursor); }

Pt current_target(const AppState& s) {
    if (is_follow_cursor(s)) return follow_target(s);
    return fixed_target(s.bounds, anchor_for_position(s.position));
}

// The anchored layer-shell surface spans exactly one output, so all
// targets live in that surface's local 0..width/0..height coordinates.
// Prefer the live drawing-area allocation (the exact space draw_cb paints
// into); before first map, when there is no allocation yet, fall back to
// the compositor-assigned monitor via gdk_display_get_monitor_at_surface(),
// then the surface size itself. Returns false when nothing valid is known
// yet -- the caller keeps the previous bounds (startup default) instead
// of collapsing to a degenerate 0x0 that would clamp the cursor to a
// point and pin every target at the origin.
bool surface_local_bounds(AppState* s, Bounds& out) {
    if (s->area) {
        const int w = gtk_widget_get_width(s->area);
        const int h = gtk_widget_get_height(s->area);
        if (w > 0 && h > 0) {
            out = local_bounds_for_surface(static_cast<double>(w), static_cast<double>(h));
            return true;
        }
    }
    if (s->window) {
        if (GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(s->window))) {
            if (GdkDisplay* d = gdk_surface_get_display(surf)) {
                if (GdkMonitor* mon = gdk_display_get_monitor_at_surface(d, surf)) {
                    GdkRectangle geo{};
                    gdk_monitor_get_geometry(mon, &geo);
                    if (geo.width > 0 && geo.height > 0) {
                        out = local_bounds_for_surface(static_cast<double>(geo.width),
                                                       static_cast<double>(geo.height));
                        return true;
                    }
                }
            }
            const int sw = gdk_surface_get_width(surf);
            const int sh = gdk_surface_get_height(surf);
            if (sw > 0 && sh > 0) {
                out = local_bounds_for_surface(static_cast<double>(sw), static_cast<double>(sh));
                return true;
            }
        }
    }
    return false;
}

// Refresh s->bounds from the live surface/output size; clamps (never
// recenters) the cursor estimate so a resize can't strand it outside.
// Cheap enough to run every tick and on every show, which covers output
// resize and compositor output switches with no signal wiring.
void sync_surface_bounds(AppState* s) {
    Bounds nb{};
    if (!surface_local_bounds(s, nb)) return;
    if (nb.max_x <= nb.min_x || nb.max_y <= nb.min_y) return;
    if (nb.min_x == s->bounds.min_x && nb.min_y == s->bounds.min_y && nb.max_x == s->bounds.max_x &&
        nb.max_y == s->bounds.max_y)
        return;
    s->bounds = nb;
    s->cursor.set_bounds(nb);
}

void draw_cb(GtkDrawingArea*, cairo_t* cr, int, int, gpointer data) {
    auto* s = static_cast<AppState*>(data);
    const auto now = steady_clock::now();
    const double pw = panel_w(), ph = panel_h();
    const double px = s->panel_x, py = s->panel_y;

    // Soft drop shadow, offset down; untilted (a tilted shadow would need
    // its own transform -- skip it, the panel's own tilt already sells the
    // lift, revisit only if a side-by-side looks off).
    cairo_set_source_surface(cr, s->shadow, px - kShadowBlurPx, py - kShadowBlurPx + kShadowOffsetY);
    cairo_paint(cr);

    cairo_save(cr);
    const double cx = px + pw / 2.0, cy = py + ph / 2.0;
    cairo_translate(cr, cx, cy);
    // Fake 3D tilt: shear proportional to the tilt angles (perspective-like,
    // per the brief) plus a small scale-down that grows with tilt magnitude
    // so the panel reads as lifting/leaning rather than just skewing.
    const double shx = std::tan(s->tilt_ry * G_PI / 180.0) * 0.5;
    const double shy = std::tan(s->tilt_rx * G_PI / 180.0) * 0.5;
    const double tilt_mag = (std::fabs(s->tilt_rx) + std::fabs(s->tilt_ry)) / (2.0 * kMaxTiltDeg);
    const double scale = (1.0 - 0.02 * tilt_mag) * appear_scale(fade_factor(*s, now));
    cairo_matrix_t m;
    cairo_matrix_init(&m, scale, shy, shx, scale, 0, 0);
    cairo_transform(cr, &m);
    cairo_translate(cr, -cx, -cy);

    set_rgb_hex(cr, kPanelBg);
    rounded_rect(cr, px, py, pw, ph, kPanelRadius);
    cairo_fill_preserve(cr);
    set_rgb_hex(cr, kPanelBorder);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    for (const auto& key : kLayout) {
        const double x = px + kPanelPad + key.x * kCellPx + kKeyGapPx;
        const double y = py + kPanelPad + key.y * kCellPx + kKeyGapPx;
        const double w = key.w * kCellPx - 2 * kKeyGapPx;
        const double h = key.h * kCellPx - 2 * kKeyGapPx;

        double f = 0.0;
        if (auto it = s->visuals.find(key.code); it != s->visuals.end()) f = highlight_factor(it->second, now);
        const double ripple_b = s->ripples.brightness_at(key.x + key.w / 2.0, key.y + key.h / 2.0);

        const uint32_t base = lerp_hex(kKeycapBg, kKeycapRipple, ripple_b);
        const uint32_t shade = lerp_hex(base, kKeycapLit, f);
        rounded_rect(cr, x, y, w, h, kKeyRadius);
        set_rgb_hex(cr, shade);
        cairo_fill(cr);

        if (key.glyph != Glyph::None) {
            draw_glyph(cr, key.glyph, x, y, w, h, lerp_hex(kLegend, kLegendLit, f));
            continue;
        }
        if (key.legend[0] == '\0') continue; // spacebar: no legend, no glyph
        set_rgb_hex(cr, lerp_hex(kLegend, kLegendLit, f));
        cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 7.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, key.legend, &ext);
        cairo_move_to(cr, x + (w - ext.width) / 2.0 - ext.x_bearing, y + (h - ext.height) / 2.0 - ext.y_bearing);
        cairo_show_text(cr, key.legend);
    }
    cairo_restore(cr);
}

void ensure_ticking(AppState* s);
void set_probe_input_region(AppState* s, bool full); // defined below; tick_cb's hide path needs it early

gboolean tick_cb(gpointer data) {
    auto* s = static_cast<AppState*>(data);
    const auto now = steady_clock::now();
    const double dt_s = std::max(0.0, duration_cast<duration<double>>(now - s->last_tick).count());
    s->last_tick = now;

    const bool wants_visible = s->dismiss.tick(now);
    if (wants_visible != s->shown) {
        s->shown = wants_visible;
        if (wants_visible) {
            s->shown_since = now;
        } else {
            s->hidden_since = now;
        }
    }

    sync_surface_bounds(s);
    const Pt target = current_target(*s);
    s->spring_x.step(target.x, dt_s);
    s->spring_y.step(target.y, dt_s);
    s->panel_x = s->spring_x.value();
    s->panel_y = s->spring_y.value();

    if (is_follow_cursor(*s)) {
        const Tilt t = tilt_from_velocity(s->spring_x.velocity(), s->spring_y.velocity());
        s->tilt_rx = t.rx_deg;
        s->tilt_ry = t.ry_deg;
    } else {
        s->tilt_rx = s->tilt_ry = 0.0; // fixed positions never tilt, per the brief
    }
    s->ripples.tick(dt_s * 1000.0);

    // Drop fully-eased key visuals so the map doesn't grow forever.
    bool any_easing = false;
    for (auto it = s->visuals.begin(); it != s->visuals.end();) {
        if (it->second.pressed || highlight_factor(it->second, now) > 0.0) {
            any_easing = true;
            ++it;
        } else {
            it = s->visuals.erase(it);
        }
    }

    const double fade = fade_factor(*s, now);
    gtk_widget_set_opacity(s->window, fade);
    gtk_widget_queue_draw(s->area);

    const bool settled = std::fabs(s->spring_x.velocity()) < 0.5 && std::fabs(s->spring_y.velocity()) < 0.5 &&
                          std::fabs(s->panel_x - target.x) < 0.5 && std::fabs(s->panel_y - target.y) < 0.5;

    if (!s->shown && fade <= 0.0 && !any_easing && !s->ripples.any_active() && settled) {
        // Cancel any in-flight probe before unmapping: an open (whole-
        // surface-reactive) input region must never survive past the
        // surface being hidden -- there is nothing left to receive a
        // wl_pointer.enter for once it is, and it must not linger
        // click-intercepting into whatever the next visible thing is.
        if (s->probe_timeout_source) {
            g_source_remove(s->probe_timeout_source);
            s->probe_timeout_source = 0;
        }
        if (s->probe.in_flight()) {
            s->probe.end_probe();
            set_probe_input_region(s, /*full=*/false);
        }
        gtk_widget_set_visible(s->window, FALSE);
        s->tick_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void ensure_ticking(AppState* s) {
    if (s->tick_source == 0) {
        s->last_tick = steady_clock::now();
        s->tick_source = g_timeout_add(kTickMs, tick_cb, s);
    }
}

// Called whenever the panel transitions hidden -> shown: pop it in already
// at the right spot (no sliding in from a stale, long-idle position), so
// only *subsequent* motion produces the spring lag/tilt.
void snap_to_target(AppState* s) {
    sync_surface_bounds(s);
    const Pt t = current_target(*s);
    s->spring_x.reset(t.x);
    s->spring_y.reset(t.y);
    s->panel_x = t.x;
    s->panel_y = t.y;
}

int64_t steady_ms(steady_clock::time_point tp) {
    return duration_cast<milliseconds>(tp.time_since_epoch()).count();
}

#ifdef GDK_WINDOWING_WAYLAND
// Looks up the window's current wl_compositor/wl_surface fresh every call
// -- never cached across a hide/show cycle. GTK4 + gtk4-layer-shell 1.3.0
// are known to keep the same GdkSurface/wl_surface across a hide/show
// cycle (only "map"/"unmap" fire, never a second "realize"/"unrealize"),
// but that is not guaranteed by either library's own documentation, so
// deriving fresh is what's safe against a version/path that does recreate
// it -- and costs nothing (a few pointer lookups per probe, not a hot
// path).
bool lookup_wl_surface(GtkWidget* window, struct wl_compositor** out_compositor, struct wl_surface** out_surface) {
    GdkSurface* surface = window ? gtk_native_get_surface(GTK_NATIVE(window)) : nullptr;
    if (!surface || !GDK_IS_WAYLAND_SURFACE(surface)) return false;
    GdkDisplay* display = gdk_surface_get_display(surface);
    if (!GDK_IS_WAYLAND_DISPLAY(display)) return false;
    *out_compositor = gdk_wayland_display_get_wl_compositor(display);
    *out_surface = gdk_wayland_surface_get_wl_surface(surface);
    return *out_compositor && *out_surface;
}

// Flips the layer surface's Wayland input region between fully click-through
// (empty) and fully reactive (whole surface), for the absolute-position
// probe below. Uses the raw wl_surface_set_input_region -- the same call
// on_realize_make_click_through already uses below -- rather than the
// portable gdk_surface_set_input_region, which does not reach the
// compositor for this layer-shell-hacked surface. Also needs an explicit,
// immediate wl_surface_commit(): gtk_widget_queue_draw() does not reliably
// produce one. A bufferless commit (no new attach/damage, just the pending
// input-region state) is valid per the Wayland protocol.
void set_probe_input_region(AppState* s, bool full) {
    struct wl_compositor* compositor = nullptr;
    struct wl_surface* wl_surf = nullptr;
    if (!lookup_wl_surface(s->window, &compositor, &wl_surf)) return;
    if (full) {
        wl_surface_set_input_region(wl_surf, nullptr); // NULL == whole surface reactive
    } else {
        struct wl_region* region = wl_compositor_create_region(compositor);
        wl_surface_set_input_region(wl_surf, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(wl_surf);
}
#else
void set_probe_input_region(AppState*, bool) {} // no Wayland backend: nothing to toggle
#endif

gboolean probe_timeout_cb(gpointer data) {
    auto* s = static_cast<AppState*>(data);
    s->probe_timeout_source = 0; // this source is being removed regardless (G_SOURCE_REMOVE below)
    if (s->probe.in_flight()) {
        s->probe.end_probe();
        set_probe_input_region(s, /*full=*/false); // never leave the region open past the timeout
    }
    return G_SOURCE_REMOVE;
}

void start_probe(AppState* s, int64_t now_ms) {
    s->probe.begin_probe(now_ms);
    set_probe_input_region(s, /*full=*/true);
    s->probe_timeout_source = g_timeout_add(static_cast<guint>(kProbeTimeoutMs), probe_timeout_cb, s);
}

// Emission hook, not a normal signal handler -- and not a widget controller
// either: viz_app.cpp sets can_target FALSE on the window (see
// on_realize_make_click_through / gtk_widget_set_can_target below), and
// GTK4's gtk_widget_pick returns NULL for a non-targetable root, so
// widget-level crossing controllers never see this enter at all.
//
// A plain g_signal_connect() on GdkSurface's "event" signal does not work
// either, despite looking like the right level: GtkWindow's own realize
// (gtk/gtkwindow.c:4527, gtk_window_realize -- GtkWidget::realize is
// G_SIGNAL_RUN_FIRST, gtk/gtkwidget.c:1781-1788, so this class handler
// always runs before any normally-connected "realize" listener such as
// this file's on_realize_connect_probe) connects its own handler
// (surface_event -> gtk_main_do_event) to the very same "event" signal
// first. gtk_main_do_event (gtk/gtkmain.c) unconditionally returns TRUE
// for GDK_ENTER_NOTIFY, and GdkSurface::"event" accumulates with
// g_signal_accumulator_true_handled (gdk/gdksurface.c:706-712), whose
// semantics (gobject/gsignal.c:4152-4166) stop the emission the moment a
// handler returns TRUE -- confirmed in gobject/gsignal.c's handler-list
// loop (~3908-3957): once that happens, remaining normally-connected
// handlers, including ours if it were one, simply never run. Emission
// hooks are the one mechanism not subject to that: verified in the same
// gsignal.c, they run in their own phase (~3780-3906), strictly before
// the handler-list loop, regardless of any handler's return value -- so
// this is the one place left that still sees every GDK_ENTER_NOTIFY
// delivered to this surface.
gboolean on_surface_event_hook(GSignalInvocationHint*, guint n_param_values, const GValue* param_values,
                                gpointer data) {
    auto* s = static_cast<AppState*>(data);
    if (n_param_values < 2) return TRUE; // malformed emission; keep the hook installed regardless
    auto* surface = GDK_SURFACE(g_value_get_object(&param_values[0]));
    if (!s->window || surface != gtk_native_get_surface(GTK_NATIVE(s->window))) return TRUE; // not our surface
    if (!s->probe.in_flight()) return TRUE;
    auto* event = static_cast<GdkEvent*>(g_value_get_pointer(&param_values[1]));
    if (!event || gdk_event_get_event_type(event) != GDK_ENTER_NOTIFY) return TRUE;

    double x = 0.0, y = 0.0;
    gdk_event_get_position(event, &x, &y);
    if (s->probe_timeout_source) {
        g_source_remove(s->probe_timeout_source);
        s->probe_timeout_source = 0;
    }
    s->probe.end_probe();
    set_probe_input_region(s, /*full=*/false); // restore click-through immediately, per the spec
    s->cursor.anchor(x, y);
    s->anchor_time_ms = steady_ms(steady_clock::now());
    if (!s->logged_first_anchor) { // one line, no coordinates, only on the first success per process
        s->logged_first_anchor = true;
        std::fprintf(stderr, "keeby-visualizer: cursor anchored\n");
    }
    return TRUE; // keep the hook installed for subsequent probes
}

gboolean on_fd_ready(gint fd, GIOCondition, gpointer data) {
    auto* s = static_cast<AppState*>(data);
    VizMessage msg{};
    const ssize_t n = recv(fd, &msg, sizeof(msg), 0);
    if (n == 0) {
        g_main_loop_quit(s->loop); // EOF: engine closed the socket, quit cleanly
        return G_SOURCE_REMOVE;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return G_SOURCE_CONTINUE;
        g_main_loop_quit(s->loop);
        return G_SOURCE_REMOVE;
    }
    if (static_cast<size_t>(n) != sizeof(msg) || !is_valid_message(msg)) {
        return G_SOURCE_CONTINUE; // malformed/unknown: ignore per spec
    }

    if (msg.type == static_cast<uint8_t>(MessageType::Key)) {
        const auto now = steady_clock::now();
        const bool down = msg.kind == 1;
        s->dismiss.on_key(msg.code, down, now);
        auto& v = s->visuals[msg.code];
        v.pressed = down;
        if (!down) {
            v.released_at = now;
            v.has_released_at = true;
        } else if (const KeyRect* r = find_key(msg.code)) {
            s->ripples.trigger(r->x + r->w / 2.0, r->y + r->h / 2.0);
        }
        if (!s->shown) {
            s->shown = true;
            s->shown_since = now;
            snap_to_target(s);
            gtk_widget_set_visible(s->window, TRUE);
        }
        ensure_ticking(s);
    } else if (msg.type == static_cast<uint8_t>(MessageType::Motion)) {
        const auto now = steady_clock::now();
        const int64_t now_ms = steady_ms(now);
        const double dx = static_cast<double>(motion_dx(msg));
        const double dy = static_cast<double>(motion_dy(msg));
        // Exact, source-independent 1:1 accumulation (see
        // cursor_estimator.hpp) -- deltas are dt-independent, so there is
        // no mouse-vs-touchpad branch and no arrival clock here anymore.
        s->cursor.accumulate(dx, dy);

        // Continuity tracking runs always, shown or not -- only the probe
        // ITSELF (opening the input region) requires the overlay to
        // actually be shown/mapped: a permanently-mapped full-output
        // OVERLAY-layer surface would sit above fullscreen content and
        // block niri's direct scanout for video/games, for something only
        // needed about once per session, so the surface goes back to
        // being unmapped while hidden (see tick_cb/main()). That means a
        // probe can only succeed while the panel is visible; the anchor is
        // acquired the first time the pointer moves while it's shown, and
        // stays exact after that (deltas are exact, edges re-sync).
        s->probe.on_motion(now_ms);
        if (s->shown && gtk_widget_get_mapped(s->window) &&
            s->probe.should_probe(s->cursor.anchored(), now_ms - s->anchor_time_ms, now_ms))
            start_probe(s, now_ms);

        if (s->shown) {
            s->dismiss.touch(now); // motion alone never shows the panel, only extends it
            ensure_ticking(s);
        }
    } else { // Config
        s->dismiss.set_dismiss(milliseconds(msg.dismiss_ms));
        s->follow_speed = follow_speed_from_step(msg.reserved);
        s->cursor.set_follow_speed(s->follow_speed);
        const bool was_follow_cursor = is_follow_cursor(*s);
        s->position = msg.position;
        if (is_follow_cursor(*s) && !was_follow_cursor) {
            // InputCapture only wires the motion tap while position ==
            // follow-cursor (see input_capture.cpp), so motion stopped
            // arriving for however long a fixed position was selected;
            // whatever the estimate is frozen at is stale by an unknown
            // amount. Don't let a leftover "anchored" flag suppress the
            // next probe -- this is the "re-anchor after it resumes" fix.
            s->cursor.invalidate();
            s->probe = ProbePolicy{};
        }
    }
    return G_SOURCE_CONTINUE;
}

#ifdef GDK_WINDOWING_WAYLAND
void on_realize_make_click_through(GtkWidget* window, gpointer) {
    struct wl_compositor* compositor = nullptr;
    struct wl_surface* wl_surf = nullptr;
    if (!lookup_wl_surface(window, &compositor, &wl_surf)) return;
    struct wl_region* region = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(wl_surf, region);
    wl_region_destroy(region);
}
#endif

// Portable GDK (not Wayland-specific) hookup for the probe's own enter
// signal -- see on_surface_event_hook's comment on why this must be an
// emission hook rather than a normally-connected signal handler or a
// widget controller. Installed at most once: GTK4 + gtk4-layer-shell
// 1.3.0 are known to keep the same surface across a hide/show cycle (only
// "map"/"unmap" fire, never a second "realize"), but guarding against a
// second install costs one field check and is safe either way.
void on_realize_connect_probe(GtkWidget*, gpointer data) {
    auto* s = static_cast<AppState*>(data);
    if (s->probe_hook_id != 0) return;
    s->probe_hook_id = g_signal_add_emission_hook(g_signal_lookup("event", GDK_TYPE_SURFACE), 0,
                                                   on_surface_event_hook, data, nullptr);
}

// Defensive belt-and-suspenders: the probe's own timeout/success paths
// already restore click-through (set_probe_input_region(s, false)) before
// this could ever fire in practice, but the region must never stay open
// past the surface's lifetime for any reason, including one this file
// didn't anticipate. The emission hook is process-global (not tied to
// this one surface), so it is removed here rather than left installed.
void on_unrealize_restore_click_through(GtkWidget*, gpointer data) {
    auto* s = static_cast<AppState*>(data);
    set_probe_input_region(s, /*full=*/false);
    if (s->probe_hook_id != 0) {
        g_signal_remove_emission_hook(g_signal_lookup("event", GDK_TYPE_SURFACE), s->probe_hook_id);
        s->probe_hook_id = 0;
    }
}

// fd 3 must be exactly the private AF_UNIX SOCK_SEQPACKET socket the engine
// promises to hand us -- refuse to run against anything else.
bool validate_fd3() {
    struct stat st{};
    if (fstat(3, &st) != 0 || !S_ISSOCK(st.st_mode)) return false;
    int domain = 0;
    socklen_t len = sizeof(domain);
    if (getsockopt(3, SOL_SOCKET, SO_DOMAIN, &domain, &len) != 0 || domain != AF_UNIX) return false;
    int type = 0;
    len = sizeof(type);
    if (getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_SEQPACKET) return false;
    return true;
}

} // namespace

int main() {
    if (!validate_fd3()) {
        std::fprintf(stderr, "keeby-visualizer: fd 3 is not a SOCK_SEQPACKET AF_UNIX socket\n");
        return 1;
    }
    fcntl(3, F_SETFL, fcntl(3, F_GETFL, 0) | O_NONBLOCK);

    gtk_init();
    if (!gtk_layer_is_supported()) {
        std::fprintf(stderr, "keeby-visualizer: compositor has no zwlr-layer-shell support\n");
        return 1;
    }

    AppState state;
    // Startup default until the first sync_surface_bounds() (first tick /
    // show) measures the real surface: sync keeps these whenever the
    // surface has no allocation and no assigned monitor yet.
    state.cursor.set_bounds(state.bounds);
    state.cursor.recenter();
    state.shadow = make_shadow_surface(panel_w(), panel_h());

    state.window = gtk_window_new();
    gtk_layer_init_for_window(GTK_WINDOW(state.window));
    gtk_layer_set_namespace(GTK_WINDOW(state.window), "keeby-visualizer");
    gtk_layer_set_layer(GTK_WINDOW(state.window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(state.window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(state.window), 0);
    // One surface spanning the whole output: anchor all four edges, no
    // margin, and draw the panel wherever inside it the spring/cursor
    // estimate places it. No gtk_layer_set_monitor(): the compositor picks
    // the focused output, per the spec.
    gtk_layer_set_anchor(GTK_WINDOW(state.window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(state.window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(state.window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(state.window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

#ifdef GDK_WINDOWING_WAYLAND
    g_signal_connect(state.window, "realize", G_CALLBACK(on_realize_make_click_through), nullptr);
#endif
    g_signal_connect(state.window, "realize", G_CALLBACK(on_realize_connect_probe), &state);
    g_signal_connect(state.window, "unrealize", G_CALLBACK(on_unrealize_restore_click_through), &state);

    state.area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(state.area, TRUE);
    gtk_widget_set_vexpand(state.area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(state.area), draw_cb, &state, nullptr);
    gtk_window_set_child(GTK_WINDOW(state.window), state.area);
    gtk_widget_set_can_target(state.window, FALSE);
    // Full-output surface: only the painted panel may be visible, never the
    // theme's opaque window background (see viz_backdrop_css.hpp).
    apply_backdrop_style(state.window, state.area);
    gtk_widget_set_opacity(state.window, 0.0);
    gtk_widget_set_visible(state.window, FALSE); // hidden until the first key/activity

    state.loop = g_main_loop_new(nullptr, FALSE);
    g_unix_fd_add(3, G_IO_IN, on_fd_ready, &state);

    g_main_loop_run(state.loop);

    if (state.probe_timeout_source) g_source_remove(state.probe_timeout_source);
    if (state.tick_source) g_source_remove(state.tick_source);
    if (state.shadow) cairo_surface_destroy(state.shadow);
    g_main_loop_unref(state.loop);
    return 0;
}
