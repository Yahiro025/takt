// Backdrop transparency regression for keeby-visualizer: headless GSK
// render-node check under a private gtk4-broadwayd display. Without the
// shipped CSS the theme paints an opaque full-surface color node (the
// reported dark rectangle); with it that node is gone and the painted
// panel stays. No screen capture, no live service.
//
// Isolation: a mkdtemp private runtime dir hosts our daemon sockets, and
// XDG_RUNTIME_DIR points at it for our children only, so our paths can
// neither collide with nor remove anyone else's. The daemon is exec'd with
// --unixsocket inside that dir, so it opens no TCP listener at all. Only
// our own pids, sockets and dir are ever signalled or removed, after both
// children exit; the daemon child also takes PR_SET_PDEATHSIG (no setsid,
// so CTest timeout signals still reach it) to guarantee no detached
// process. GTK runs in a forked worker that _exit()s, keeping third-party
// one-time globals out of the parent's LeakSanitizer. Exit 77 without
// gtk4-broadwayd.

#include "../src/visualizer/viz_backdrop_css.hpp"

#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

namespace {

constexpr int kDisplay = 19; // private to this test; never the session's display
constexpr int kW = 800;
constexpr int kH = 600;

// Worker results, one per phase.
enum : int {
    WORKER_OK = 0,
    WORKER_GTK_INIT_FAILED = 3,
    WORKER_VACUOUS = 4, // bug did not reproduce without the CSS: check proves nothing
    WORKER_STILL_OPAQUE = 5,
    WORKER_PANEL_LOST = 6,
    WORKER_CSS_PARSE_ERROR = 7,
    WORKER_NO_SNAPSHOT = 8, // bounded wait below expired: fail, never assume pumps render
};

// Small opaque rect only -- like viz_app's draw_cb, which paints just the
// panel/shadow and never a full-surface background.
void draw_panel(GtkDrawingArea*, cairo_t* cr, int, int, gpointer) {
    cairo_set_source_rgb(cr, 0.11, 0.11, 0.12);
    cairo_rectangle(cr, 255, 245, 290, 110);
    cairo_fill(cr);
}

int g_parse_errors = 0;
void on_parse_error(GtkCssProvider*, GtkCssSection*, GError* error, gpointer) {
    ++g_parse_errors;
    std::fprintf(stderr, "backdrop CSS parse error: %s\n", error->message);
}

struct WalkStats {
    int opaque_fullscreen_colors = 0; // alpha==1 color nodes covering >=95% of the surface
    int opaque_halfscreen_colors = 0; // alpha==1 color nodes covering >=50% of the surface
    int cairo_nodes = 0;
};

bool covers(const graphene_rect_t& b, double fraction) {
    return (b.size.width * b.size.height) >= fraction * kW * kH;
}

void walk(GskRenderNode* node, WalkStats& stats) {
    if (!node) return;
    if (gsk_render_node_get_node_type(node) == GSK_COLOR_NODE) {
        const GdkRGBA* c = gsk_color_node_get_color(node);
        if (c->alpha == 1.0f) {
            graphene_rect_t b;
            gsk_render_node_get_bounds(node, &b);
            if (covers(b, 0.95)) ++stats.opaque_fullscreen_colors;
            if (covers(b, 0.50)) ++stats.opaque_halfscreen_colors;
        }
    } else if (gsk_render_node_get_node_type(node) == GSK_CAIRO_NODE) {
        ++stats.cairo_nodes;
    }
    // Borrowed in GTK 4.22 (freeing corrupts the heap): never g_free.
    gsize n = 0;
    GskRenderNode** kids = gsk_render_node_get_children(node, &n);
    for (gsize i = 0; i < n; ++i) walk(kids[i], stats);
}

WalkStats snapshot_stats(GtkWidget* window, bool& ok) {
    GdkPaintable* paint = gtk_widget_paintable_new(window);
    GtkSnapshot* snap = gtk_snapshot_new();
    gdk_paintable_snapshot(paint, snap, kW, kH);
    GskRenderNode* node = gtk_snapshot_to_node(snap);
    WalkStats stats{};
    ok = node != nullptr;
    if (ok) walk(node, stats);
    if (node) gsk_render_node_unref(node);
    g_object_unref(snap);
    g_object_unref(paint);
    return stats;
}

void pump(int rounds = 50) {
    for (int i = 0; i < rounds; ++i) g_main_context_iteration(nullptr, FALSE);
}

// Bounded wait for a real snapshot: pumps until the window is mapped and
// yields a render node (~10s cap), failing explicitly if it never does.
bool await_snapshot(GtkWidget* window, WalkStats& stats) {
    for (int i = 0; i < 100; ++i) {
        pump(5);
        if (gtk_widget_get_mapped(window)) {
            bool ok = false;
            stats = snapshot_stats(window, ok);
            if (ok) return true;
        }
        usleep(50 * 1000);
    }
    return false;
}

// viz_app.cpp installs the backdrop CSS at startup, before the first show;
// apply it here pre-present too, never as a dynamic restyle mid-run.
GtkWidget* make_window(bool with_css) {
    GtkWidget* window = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(window), kW, kH);
    // Layer-shell surfaces (zwlr_layer_surface, not xdg_toplevel) never get
    // CSD headerbars -- the live recording shows none -- so emulate the
    // undecorated target explicitly; broadway would decorate by default.
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_panel, nullptr, nullptr);
    gtk_window_set_child(GTK_WINDOW(window), area);
    if (with_css) {
        GtkCssProvider* probe = gtk_css_provider_new();
        g_signal_connect(probe, "parsing-error", G_CALLBACK(on_parse_error), nullptr);
        gtk_css_provider_load_from_string(probe, keeby::viz::kBackdropCss);
        g_object_unref(probe);
        keeby::viz::apply_backdrop_style(window, area);
    }
    gtk_window_present(GTK_WINDOW(window));
    return window;
}

int run_worker() {
    setenv("GDK_BACKEND", "broadway", 1);
    setenv("BROADWAY_DISPLAY", (":" + std::to_string(kDisplay)).c_str(), 1);
    if (!gtk_init_check()) {
        std::fprintf(stderr, "gtk_init_check failed on the private broadway display\n");
        return WORKER_GTK_INIT_FAILED;
    }

    GtkWidget* bare_window = make_window(/*with_css=*/false);
    WalkStats bare{};
    if (!await_snapshot(bare_window, bare)) {
        std::fprintf(stderr, "no snapshot rendered without CSS; check is vacuous\n");
        return WORKER_NO_SNAPSHOT;
    }
    std::printf("bare: fullscreen opaque colors=%d halfscreen=%d cairo=%d\n", bare.opaque_fullscreen_colors,
                bare.opaque_halfscreen_colors, bare.cairo_nodes);
    if (bare.opaque_fullscreen_colors < 1) {
        std::fprintf(stderr, "harness did not reproduce the opaque backdrop; check is vacuous\n");
        return WORKER_VACUOUS;
    }
    gtk_window_destroy(GTK_WINDOW(bare_window));
    pump();

    GtkWidget* fixed_window = make_window(/*with_css=*/true);
    WalkStats fixed{};
    if (!await_snapshot(fixed_window, fixed)) {
        std::fprintf(stderr, "no snapshot rendered with CSS; check is vacuous\n");
        return WORKER_NO_SNAPSHOT;
    }
    std::printf("fixed: parse_errors=%d fullscreen opaque colors=%d halfscreen=%d cairo=%d\n", g_parse_errors,
                fixed.opaque_fullscreen_colors, fixed.opaque_halfscreen_colors, fixed.cairo_nodes);
    (void)fixed_window;
    if (g_parse_errors != 0) return WORKER_CSS_PARSE_ERROR;
    if (fixed.opaque_halfscreen_colors != 0) {
        std::fprintf(stderr, "opaque backdrop remains with the fix applied\n");
        return WORKER_STILL_OPAQUE;
    }
    if (fixed.cairo_nodes < 1) {
        std::fprintf(stderr, "panel content vanished with the fix applied\n");
        return WORKER_PANEL_LOST;
    }
    return WORKER_OK;
}

pid_t spawn_daemon(const std::string& explicit_sock) {
    const pid_t parent = getpid();
    pid_t pid = fork();
    if (pid != 0) return pid;
    // No setsid: stay in CTest's process group so its timeout signal still
    // reaches us. Death-signal plus the ppid check below guarantee no
    // detached daemon if the parent dies first; only our own pid is ever
    // signalled (by our parent's waitpid/kill path or the kernel).
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) _exit(127);
    if (getppid() != parent) _exit(127);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }
    // --unixsocket overrides --address/--port: no TCP listener is opened.
    // The daemon still serves its default display socket too; both live in
    // our private runtime dir and both are removed at the end.
    execlp("gtk4-broadwayd", "gtk4-broadwayd", "--unixsocket", explicit_sock.c_str(),
           (":" + std::to_string(kDisplay)).c_str(), nullptr);
    _exit(127);
}

} // namespace

int main() {
    gchar* broadwayd = g_find_program_in_path("gtk4-broadwayd");
    if (!broadwayd) {
        std::printf("visualizer_backdrop_test: SKIP (no gtk4-broadwayd)\n");
        return 77;
    }
    g_free(broadwayd);

    std::string runtime_tmpl = std::string(g_get_tmp_dir()) + "/keeby-backdrop-test-XXXXXX";
    if (!mkdtemp(runtime_tmpl.data())) {
        std::perror("mkdtemp");
        return 1;
    }
    // Seen by our daemon/worker children only; the socket path below can
    // therefore name no pre-existing resource.
    setenv("XDG_RUNTIME_DIR", runtime_tmpl.c_str(), 1);
    const std::string socket_path =
        runtime_tmpl + "/broadway" + std::to_string(kDisplay + 1) + ".socket";
    const std::string explicit_sock = runtime_tmpl + "/broadway-test.sock";

    const pid_t self = getpid();
    pid_t daemon = spawn_daemon(explicit_sock);
    if (daemon < 0) {
        std::fprintf(stderr, "fork failed\n");
        rmdir(runtime_tmpl.c_str());
        return 1;
    }
    bool ready = false;
    for (int i = 0; i < 200 && !ready; ++i) {
        if (access(socket_path.c_str(), F_OK) == 0) ready = true;
        else usleep(50 * 1000);
    }
    int exit_code = 1;
    if (!ready) {
        std::fprintf(stderr, "broadwayd did not serve %s\n", socket_path.c_str());
    } else {
        pid_t worker = fork();
        if (worker < 0) {
            std::fprintf(stderr, "fork failed\n");
        } else if (worker == 0) {
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) _exit(127);
            if (getppid() != self) _exit(127);
            _exit(run_worker()); // _exit: keep GTK one-time globals out of LSAN
        } else {
            int status = 0;
            waitpid(worker, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == WORKER_OK) {
                std::printf("visualizer_backdrop_test: OK\n");
                exit_code = 0;
            } else if (WIFEXITED(status)) {
                std::fprintf(stderr, "worker failed, code %d\n", WEXITSTATUS(status));
            } else {
                std::fprintf(stderr, "worker did not exit normally\n");
            }
        }
    }
    // Only our own children and paths, reaped/removed after both exited.
    kill(daemon, SIGTERM);
    int status = 0;
    waitpid(daemon, &status, 0);
    unlink(explicit_sock.c_str());
    unlink(socket_path.c_str());
    rmdir(runtime_tmpl.c_str());
    return exit_code;
}
