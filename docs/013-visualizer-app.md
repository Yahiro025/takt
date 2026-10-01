# Step: `keeby-visualizer` overlay app + Settings Visualizer tab

## Overlay app

`keeby-visualizer` (src/visualizer/) is a new, independent executable: GTK4 +
[gtk4-layer-shell](https://github.com/wmww/gtk-layer-shell) (pkg-config
`gtk4-layer-shell-0`). It links no engine code and owns no D-Bus connection
of its own -- the engine (a parallel worker's change, see
docs/013-visualizer.md) spawns it with a private `SOCK_SEQPACKET` socket on
fd 3 and talks to it purely over that one fd, one 8-byte `VizMessage`
datagram (`src/visualizer_wire.hpp`) per key or config change.

### Structure

```
src/visualizer_wire.hpp    the 8-byte wire struct, identical on both sides
                            of the engine/overlay boundary (deliberately
                            duplicated rather than shared, per the brief --
                            the supervisor dedups the two copies at merge)
src/visualizer/
  viz_layout.hpp    pure key-rect table for the compact 60%-style panel
                    (key units, GTK-free, unit-tested)
  viz_dismiss.hpp   pure show/hide state machine driven by key up/down
                    events + a periodic tick (GTK-free, unit-tested)
  viz_position.hpp  pure position(0..5) <-> anchor-edge/halign mapping and
                    VizMessage validation (GTK-free, unit-tested)
  viz_app.cpp       main(): fd 3 validation, GLib fd source, layer-shell
                    window, cairo rendering, animation tick

tests/visualizer_test.cpp   covers all three pure-logic headers above --
                            layout table (every key has a rect, codes
                            unique, standard keys present), message
                            validation, the dismiss timer, and the
                            position/anchor mapping. No display, no GTK; it
                            always builds and runs regardless of whether
                            gtk4-layer-shell is installed.
```

### IPC

fd 3 is validated (`fstat` + `SO_DOMAIN`/`SO_TYPE`) to be an `AF_UNIX`
`SOCK_SEQPACKET` socket before anything else runs; a mismatch is a hard
`exit(1)`, never a silent fallback. Reads happen via `g_unix_fd_add` on the
GLib main loop -- never a blocking `recv()` on the UI thread. Malformed or
unknown messages (wrong size, out-of-range `type`/`kind`/`position`/
`dismiss_ms`) are dropped via `is_valid_message()`; EOF on the socket (the
engine exited or closed it) quits the process cleanly. A key event for a
code that isn't in `viz_layout.hpp`'s table still feeds `DismissTimer`,
resetting the fade-out clock, exactly as the brief requires, even though it
has no visual to light up.

**Confidentiality**: `viz_app.cpp` never logs, prints, or persists a key
code anywhere; a code is used only to look up a `KeyRect` and flip a
highlight flag, and only within the one message-handling function.

### Layer-shell placement

Overlay layer, `GTK_LAYER_SHELL_KEYBOARD_MODE_NONE` (never takes focus), no
exclusive zone, anchored per the `Anchor{VEdge, HAlign}` `viz_position.hpp`
computes from the config message's `position` byte, with a 24px margin on
whichever edge(s) it's anchored to. No monitor is set explicitly
(`gtk_layer_set_monitor` is never called), so the compositor places it on
the focused output, per the brief.

**Click-through**: on the window's `realize` signal (Wayland backend only,
guarded by `GDK_WINDOWING_WAYLAND`), the surface's `wl_surface` gets an
empty `wl_region` via `wl_surface_set_input_region()` -- the standard
technique for a layer-shell surface that must never intercept a click, since
`gtk_widget_set_can_target(FALSE)` alone only affects GTK's own hit-testing,
not what the compositor delivers to the Wayland surface underneath.

### Rendering and animation

A single `GtkDrawingArea` draws the whole panel each frame with cairo:
rounded panel background (~#2B2B2B, a hairline border, a soft drop-shadow
pass), then each keycap from `viz_layout.hpp`'s table, shaded from
`#3A3A3A` at rest to near-white when its `highlight_factor()` (1.0 held,
easing linearly to 0.0 over 150ms after release) is high, with the 8px grey
legend darkening the same way. Cairo draws are resolution-independent, so
this stays crisp at scale factors 1 and 2 without any bitmap assets.

A single `g_timeout_add` (~60fps) drives both this ease-back and the panel's
own ~150ms show/hide opacity fade, plus `DismissTimer::tick()`; it is only
armed while the panel is visible or a key is still easing back, and cancels
itself (`G_SOURCE_REMOVE`) the moment both are false -- so an idle overlay
holds zero timers and costs zero CPU, per the brief.

### Packaging

`KEEBY_BUILD_VISUALIZER` (default `ON`) gates the whole feature; if either
`gtk4` or `gtk4-layer-shell-0` is missing via pkg-config it's skipped with a
`message(WARNING ...)` and the rest of the build (including
`visualizer_test`, which has no such dependency) is unaffected.
`keeby-visualizer` installs to `bin/`; `gtk4-layer-shell` was added to the
PKGBUILD's `depends` (pkgrel left for the supervisor to bump).

`gtk4-layer-shell-0` is not installed on this machine as of this step, so
`keeby-visualizer` itself could not be built or screenshotted here --
`visualizer_test` (which needs neither GTK nor the layer-shell library)
built, ran, and passed under both Release and ASan/UBSan. `viz_app.cpp` was
written and reviewed against the documented gtk4-layer-shell/GTK4-Wayland
API but is otherwise unverified by a real compile; re-run
`cmake --build . --target keeby-visualizer` once the package is installed to
confirm.

## Settings window: Visualizer tab

See docs/013-visualizer.md (the engine-side worker's doc) for the
`GetVisualizer`/`SetVisualizer`/`SetVisualizerPosition`/
`SetVisualizerDismiss` D-Bus contract this tab drives. On the UI side:

- `src/ui/kbus_client.hpp/.cpp` gained `VisualizerState` and the four
  matching async `KeebyClient` methods, following the exact async-call/
  `StateChanged`-refresh pattern the existing Sound tab uses.
- `src/ui/visualizer_tab.cpp` (new file, declared in `tabs.hpp` alongside
  the existing tab builders) builds the "Show Visualizer" switch, the
  "Dismiss After" slider (250-5000ms range, `Throttle`-debounced exactly
  like the Stereo Width slider, monospace `"1.0s"`-style value label), and
  the 6-tile Position grid. Each tile is a `GtkButton` with a small
  cairo-drawn screen icon (outline + a filled bar at the edge/corner that
  tile represents) over a label; the selected tile gets the
  `#174079`->`#28313C` navy gradient and blue border via the
  `button.kb-tile.kb-tile-selected` CSS class added in `app.cpp`'s
  stylesheet.
- `app.cpp`'s three-way switcher (`General`/`Sound`/`Visualizer`) and
  `refresh_from_engine()` (now also pulling `GetVisualizer`) wire it into
  the existing window; `KEEBY_SETTINGS_TAB=visualizer` is the same
  dev/screenshot convenience the existing tabs use to land on this one
  without Wayland input automation.
- `tests/fake_keeby_service.cpp` grew `GetVisualizer`/`SetVisualizer`/
  `SetVisualizerPosition`/`SetVisualizerDismiss` (defaults: disabled,
  `"top-center"`, 1000ms) and `tests/kbus_client_test.cpp` exercises a full
  get/set/get round trip through all four before continuing into the
  pre-existing profile-switching assertions.
- Keeby's theme grid is intentionally not built yet, per the brief.

## Tests

- `visualizer_test`: layout table, message validation, dismiss timer,
  position/anchor mapping -- see above. Passes under Release and under
  ASan+UBSan, no leaks or sanitizer reports.
- `kbus_client_test`: extended as described above; still SKIPs (exit 77)
  with no session bus and never touches a real running KEEBY (unique
  per-run bus name, as before).
- Full `ctest` (Release): all 15 tests pass, including both new/extended
  ones. No new compiler warnings from any file this step added.

## Screenshots

`build/wt/vizapp-shots/visualizer_tab.png`: the real `keeby-settings`
binary's Visualizer tab against a private `fake_keeby_service` instance on a
unique test-only bus name (`KEEBY_SETTINGS_TAB=visualizer`), captured with
`grim` -- never the real `org.keeby.Keeby`.

The overlay screenshot (`overlay.png`) could not be captured this run:
`gtk4-layer-shell-0` is not yet installed on this machine (confirmed via
`pkg-config --modversion gtk4-layer-shell-0` both at the start and again at
the end of this step), so `keeby-visualizer` itself isn't built. Once it is,
a small test driver (`socketpair(AF_UNIX, SOCK_SEQPACKET, ...)`, exec
`keeby-visualizer` with fd 3 = the child end, send one config message plus a
few scripted key down/up pairs on the parent end) plus `grim` will produce
it the same way the Sound-tab shot was taken -- see the brief's Screenshots
section for the exact scripted key sequence to send.
