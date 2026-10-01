# Step: `keeby-settings` — GTK4 settings window

## 1. Goal

A settings window that looks like the macOS Keeby app's settings window,
built for CachyOS + Niri (Wayland). User instruction (verbatim): "I want it
to look exactly like keeby." Two explicit choices from that discussion:
plain **GTK4, no libadwaita** (libadwaita's GNOME look fights Keeby's own
look), and **window + tone pad now, visualizer later** (no third tab yet).

`keeby-settings` is a new, independent executable. It links no engine code
(`keeby_core` is never linked in) — it only talks to the running `keeby`
engine over D-Bus (`org.keeby.Control1`), same as `keeby ctl`. If `keeby`
isn't running, the window still opens and shows a "KEEBY isn't running"
card instead of crashing or refusing to launch.

## 2. Structure

```
src/ui/
  settings_logic.hpp     pure logic: brand grouping, switch color, tone-pad
                          coordinate mapping, update throttling (no GTK/GLib)
  kbus_client.hpp/.cpp    async GDBus client for org.keeby.Control1
  systemctl.hpp/.cpp      g_spawn wrappers for `systemctl --user ...`
  sound_controls.hpp/.cpp  volume pill + tone pad (GtkDrawingArea + gestures)
  tabs.hpp/.cpp           General tab, Sound tab, switches-list rebuild
  app.hpp/.cpp            window/header-bar/CSS, refresh-from-engine glue
  keeby_settings_main.cpp main(): GtkApplication "org.keeby.Settings"

assets/icons/keeby.svg    original app icon (installed to the hicolor theme)

tests/
  settings_logic_test.cpp    pure-logic unit tests, no display/D-Bus
  fake_keeby_service.cpp     test-only full org.keeby.Control1 fake, GDBus,
                              bus name from argv[1]/$KEEBY_BUS_NAME
  kbus_client_test.cpp       headless client test against the fake, SKIP 77
                              with no session bus
```

`KeebyClient::default_bus_name()` reads `$KEEBY_BUS_NAME`, falling back to
`org.keeby.Keeby` — this is how every test points the client at a private
fake instead of a real running KEEBY, and it's also how a developer could
point the whole app at a second instance.

## 3. Design spec, as built

Dark only. Window `#222222`, cards `#292929` (10px radius), hairline
dividers (`alpha(#ffffff, 0.08)`) between rows, accent `#197EF5` for the
active switcher pill, switches and slider fill. Small fixed window
(440×560, `gtk_window_set_resizable(FALSE)`). Title bar: `GtkHeaderBar`
whose title widget is a hand-built segmented control (two linked
`GtkToggleButton`s in a `.kb-switcher` pill, `gtk_toggle_button_set_group`
for mutual exclusion) driving a `GtkStack`'s visible child — General |
Sound, no third tab.

**General tab**: header card with our own SVG icon (a rounded keycap with
three original blue bars — not Keeby's logo/wordmark/keycap art),
"KEEBY", and a version label filled from `GetInfo()`. Below: Launch at
Login (reads `systemctl --user is-enabled keeby.service` once at build
time, toggles `enable`/`disable` via `g_spawn_async` with an argv array —
no shell involved anywhere), Keyboard Access (green dot + "Granted" /
amber dot + "No keyboard" from `GetInfo().device_connected`), Sound
(`GetState().enabled` / `SetEnabled`). If the bus name has no owner
(`g_bus_watch_name` vanished/never appeared), a "KEEBY isn't running" card
appears with a Start button (`systemctl --user start keeby.service`) and
the rest of the window (`ctx.stack`) is set insensitive; it recovers on
its own via the same watch when the name reappears.

**Sound tab**: a volume pill and tone pad side by side (both hand-drawn
`GtkDrawingArea` + `GtkGestureDrag`, not boxed in a card, matching the
"floating controls" look), a Stereo Width card (0–200% slider, mono value
label), then a Switches section grouped by brand with a colored square,
name, and either a spinner (SetProfile in flight) or a checkmark
(selected) per row; clicking an unselected row calls `SetProfile` and
shows the spinner until the callback lands, with an inline red error label
under the row on failure. A trailing hint: "More sounds: run
keeby-fetch-packs".

## 4. Tone-pad mapping (the one genuinely ambiguous part of the spec)

Corner labels are Warm (top-left), Bright (top-right), Thock
(bottom-left), Clack (bottom-right) — i.e. **Thock↔Clack runs along the
x-axis** (bottom edge) and **Warm↔Bright is the top edge**, both varying
with x, while y separates "top" (brighter overall) from "bottom". Decision
implemented in `settings_logic.hpp`:

- `x ∈ [-1, 1]`: Thock at -1 (left), Clack at +1 (right).
- `y ∈ [-1, 1]`: Bright/top-brighter side at +1 (top of the pad), Thock/Warm
  side at -1 (bottom) — pixel y grows downward, so `tone_y = 1 - 2*py/h`.

`pixel_to_tone`/`tone_to_pixel` are pure, clamp to `[-1, 1]` on both ends
(overshooting a drag past the pad edge still gives a valid tone), and are
covered by `settings_logic_test.cpp` (corners, center, round-trip,
clamping). `SetTone(x, y)` is called with this same (x, y) — matches the
`GetTone()->(dd)`/`SetTone(dd)` contract's stated x/y semantics exactly.

## 5. D-Bus usage

Async only (`g_dbus_proxy_call`/`_finish`), integrated with the GLib main
loop GTK already runs — no blocking round trip ever happens on a UI
gesture. `KeebyClient` watches the bus name (`g_bus_watch_name`) and
creates/destroys a `GDBusProxy` as the name appears/vanishes; a `g-signal`
handler filters for `StateChanged` and calls back into `refresh_from_engine`,
which re-issues `GetState`/`GetTone`/`GetInfo`/`ListProfilesDetailed` and
updates every widget that isn't mid-drag (`volume_pill_is_dragging`/
`tone_pad_is_dragging` guard the pill/pad; the width slider has no
drag-end signal of its own, so a StateChanged echo of our own just-sent
value is harmless — see the comment in `app.cpp::refresh_from_engine`).

Outgoing pad/pill updates are throttled via `keeby::ui::Throttle` to at
most ~30/s while dragging, always flushing the drag-end (or double-click
reset) value immediately regardless of timing. The width slider has no
drag-end signal, so it pairs the same throttle with a 60ms trailing
`g_timeout_add` flush to guarantee the final value is always sent even if
the last `value-changed` landed inside the throttle window.

## 6. What differs from Keeby, and why

- No libadwaita styling — GTK4's plain C API only, so nothing fights the
  custom dark/pill/card look (per the user's explicit choice).
- Original icon, name and artwork throughout: `assets/icons/keeby.svg` (a
  rounded keycap with an original glyph, our own blue), and a from-scratch
  switch-color palette (`color_for_id` in `settings_logic.hpp`) rather than
  Keeby's own switch colors.
- No Visualizer tab (explicitly deferred, per the user's second choice).
- `KEEBY_SETTINGS_INITIAL_TAB=sound` is an unadvertised, code-commented
  dev/screenshot convenience (defaults to General otherwise) — not part of
  the design spec, added only so a verification script can land on either
  tab without Wayland input automation.

## 7. Packaging

`KEEBY_BUILD_SETTINGS_UI` (default `ON`) gates the whole feature; with
GTK4 not found via pkg-config it's skipped with a warning and the engine
still builds. `keeby-settings` installs to `bin/`, the icon to
`share/icons/hicolor/scalable/apps/keeby.svg`. `packaging/keeby.desktop`
now launches `keeby-settings` (`Icon=keeby`); `gtk4` was added to the
PKGBUILD's `depends` (pkgrel left for the supervisor to bump).

## 8. Tests

- `settings_logic_test`: brand grouping (including the "Default
  (built-in)"-sorts-first rule), deterministic per-id color, tone-pad
  mapping/clamping/round-trip, throttle timing — no display, no D-Bus.
- `fake_keeby_service`: a from-scratch GDBus implementation of the full
  contract (existing methods + `ListProfilesDetailed`/`GetTone`/`SetTone`/
  `GetInfo`/`StateChanged`) on a caller-given bus name, printing `READY`
  once it owns the name.
- `kbus_client_test`: spawns the fake on a unique per-run bus name, drives
  `KeebyClient` through every call plus a successful and a failing
  `SetProfile`, and asserts the `StateChanged` signal was delivered. SKIPs
  (exit 77) with no session bus; never touches `org.keeby.Keeby`.
- Both new tests plus all 12 pre-existing ones pass under `ctest`
  (Release) and under an ASan+UBSan debug build of the two new binaries,
  clean (no leaks, no sanitizer reports). No new compiler warnings from
  any file this step added (`-Wall -Wextra` on every new target).

## 9. Screenshots

Captured with the real `keeby-settings` binary against `fake_keeby_service`
only (never the real `org.keeby.Keeby`), via `grim`, saved to
`build/wt/settingsui-shots/general_tab.png` and `.../sound_tab.png`. The
Sound-tab capture landed with the floating window partially past this
machine's single monitor's right edge (a Niri floating-window placement
quirk in that particular run, unrelated to the app), so its rightmost
~80px (the far edge of the tone pad's "Bright"/"Clack" labels and the
width slider's value label) is clipped in that one PNG; the rest of the
tab is intact and representative.
