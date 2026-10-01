# 017 — Exact cursor tracking: libinput-driven pointer motion + absolute-position probe, package 0.1.0-8

## Goal

The docs/016 touchpad-follow fix was packaged as `0.1.0-7`, installed, and live-checked
by the user via their own screen recording
(`~/Videos/recording_2026-09-26_20.16.04.mp4`). The recording showed the follow bug was
NOT fixed: the panel drifts away from the real cursor instead of tracking it. This step
root-causes that drift on the 0.1.0-7 code, replaces the hand-estimated pointer tracking
with **exact tracking** sourced from libinput itself (a user-approved, docs/006 §47
privilege-relevant change), adds an absolute-position probe to correct residual drift, and
packages the result as `0.1.0-8` (built, NOT installed — see the live-check steps below).

## Root cause (with evidence)

Frame coordinates below are read off the recording at its native 1024x640 export scale;
multiply by 1.875 for real screen pixels (noted inline where it clarifies scale).

- **t=4s**: cursor at frame (335, 170) — top-center of frame (real px ≈ (628, 319)).
  Panel spans frame x 125-275, y 410-465 — bottom-left of frame (real px ≈ x 234-516,
  y 769-872). The panel is nowhere near the cursor.
- **t=12s**: the panel is pinned in the top-right corner while the cursor is mid-right —
  consistent with the estimate having hit an edge clamp and stuck there while the real
  cursor kept moving independently.
- **t=17s->18s**: the cursor moved ~230 frame units left; the panel moved ~620 frame
  units in the same direction — a ~2.7x ratio.
- **t=20s->21s**: the cursor moved (+96, +68) frame units; the panel moved (+335, +230)
  frame units — a ~3.4x ratio (per-axis: 335/96 ≈ 3.49, 230/68 ≈ 3.38).

Confirmed on the 0.1.0-7 code, three compounding causes:

1. **No absolute anchor.** `CursorEstimator` only ever recenters at startup
   (`recenter()`, called once from `viz_app.cpp`'s `main()`); nothing in 0.1.0-7 ever
   corrects the running estimate against the real cursor position again, so any
   per-event error accumulates without bound for the life of the process.
2. **Touchpad gain mismatch.** 0.1.0-7 forwarded touchpad motion as
   `mm * 1000/25.4` at gain 1.0, but libinput itself applies `TP_MAGIC_SLOWDOWN = 0.2968`
   to touchpad deltas (libinput `src/filter-touchpad.c:37`) before niri ever sees them —
   i.e. the real cursor moves at `1/0.2968 ≈ 3.37x` *slower* than the raw finger travel
   0.1.0-7 was forwarding. This alone predicts almost exactly the ~2.7x-3.4x drift ratios
   measured above.
3. **Edge clamp pinning.** Once the (too-fast) estimate ran off an edge, the clamp held
   it there — explaining the t=12s corner-pinning while the real cursor sat mid-screen.

## Research facts

niri 26.04 (commit `8ed0da4`) source, and the user's own niri config, checked directly:

- Pointer focus is re-hit-tested every event-loop tick after a client commits
  (`src/niri.rs:1022-1098`) — a client that changes its input region and commits is
  re-evaluated for pointer focus essentially immediately, not on some other cadence.
- `hide-when-typing` hides the real pointer on every key press
  (`src/input/mod.rs:366-392,595-615`); a surface content change while the pointer is
  hidden demotes it to `Disabled` and clears focus entirely (`niri.rs:1070-1081`) — so a
  probe that fires from a key-press handler cannot observe a real pointer-enter at all.
- Real cursor motion is `pos + event.delta()`, i.e. libinput's own already-accelerated
  delta applied 1:1 in logical pixels (`mod.rs:2405`), clamped every event to
  `loc..loc+size-1` (`mod.rs:2506-2511` — one pixel inside the far edge, never the
  width/height itself).
- `apply_libinput_settings()` is `mod.rs:4674-4962`.
- The user's own niri config: touchpad `tap` enabled with
  `tap-button-map "left-right-middle"`; mouse `accel-profile "flat"`; cursor
  `hide-when-typing` ON; no `ext-image-copy-capture` protocol and no cursor IPC of any
  kind exposed to clients — confirming there is no compositor-side API this process
  could simply query instead of tracking motion itself.

Probe implementation history (why the mechanism looks the way it does): a first attempt
fired the probe from the key-press handler and used the portable
`gdk_surface_set_input_region()`. It failed for three independent reasons at once:
`hide-when-typing` had already hidden the real pointer by the time the probe ran; the
overlay's `can_target` is FALSE (see Design below), so GTK's widget-level pick returns
NULL and no widget-level crossing controller ever sees an enter; and the portable GDK
call never reached the compositor for this layer-shell-hacked surface, with
`gtk_widget_queue_draw()` producing no matching `wl_surface.commit` either (traced at
119 `set_input_region` calls against only 2 unrelated startup-frame commits over an
entire run). The fix used in the merged design: probe only while shown/moving (never on
a key press), the raw `wl_surface_set_input_region()` call, and an explicit
`wl_surface_commit()` right after it — re-traced afterward at 119 `set_input_region` and
121 `commit` calls, every one immediately paired.

## Design + user decision

**User decision (2026-09-26):** shown three options — **"Exact tracking (Recommended)"**
(`keeby-inputd` runs libinput itself, read-only, post-privilege-drop only, over the same
already-opened pointer fds), **"Snap on typing start"** (no helper change; a
zero-length virtual-pointer nudge plus a probe at the first keypress after motion,
with the side effect that `hide-when-typing`'s cursor briefly reappears), and **"Gain
fix only"**. The user chose exact tracking, instead of the visualizer continuing to
guess a gain curve. Full security case in docs/006 §47.

**Supervisor review required two changes before merge:**

- **Post-drop `dlopen()` instead of linking.** An initial version linked libinput
  directly (`target_link_libraries(keeby-inputd PkgConfig::LIBINPUT)`), which pulled
  its transitive dependencies (libwacom, libgudev, GLib/GObject, liblua5.4, libpcre2,
  libffi, mtdev, libudev) into the pre-privilege-drop ELF image as `NEEDED` entries —
  their own ELF constructors (e.g. GLib's) would then run before `main()`, inside a
  still-setgid-`input` process; GLib's own documentation says it does not support
  setuid/setgid use. Fixed via `src/libinput_loader.hpp`: `CMakeLists.txt` now supplies
  `${LIBINPUT_INCLUDE_DIRS}` only (headers, no `IMPORTED_TARGET`, never linked), and
  `libinput.so.10` is `dlopen()`'d with every symbol `dlsym()`'d into a typed
  `LibinputApi` struct, called only from `setup_libinput()`, strictly after
  `drop_privilege_or_die()`. `ldd` on the built helper now lists only libevdev plus
  libstdc++/libm/libgcc_s/libc — none of libinput or its dependency chain.
- **Reverted permanent overlay mapping.** A full-output OVERLAY-layer surface mapped at
  all times blocks niri's direct scanout of fullscreen windows (video/games). The
  overlay stays unmapped-while-hidden as before; the absolute-position probe can
  therefore only run — and the anchor can only be acquired — the first time the pointer
  moves while the panel happens to be visible, not continuously in the background.

**Helper side** (`keeby-inputd`): post-drop, a libinput path-backend context is created
over the pointer fds already opened and validated pre-drop (`setup_libinput()`).
`open_restricted()` never calls `open()` — it only hands back
`fcntl(existing_fd, F_DUPFD_CLOEXEC, 0)` for a path that matches one already-opened fd
exactly, or `-EACCES`. Acceleration settings are hardcoded to mirror the user's own niri
config exactly (`ponytail` comment at `apply_niri_pointer_settings()`: flat/speed-0 for
mice, tap-enabled/`left-right-middle`/dwt+dwtp disabled/no drag-lock for touchpads —
upgrade path is to pass settings via `argv` instead of mirroring a constant, if this
ever needs to serve a different config). Only `LIBINPUT_EVENT_POINTER_MOTION` is ever
read back out; every event's already-accelerated `dx`/`dy` goes through
`carry_and_truncate()` (a per-device, per-axis carried fractional remainder, so
sub-pixel deltas at low speed are never silently rounded away) and is sent as exactly
one `wire::PointerMotion` per libinput event — no summing across events.

**Visualizer side**: `CursorEstimator::accumulate(dx, dy)` applies each Motion message's
delta 1:1 (no accel curve, no per-source branch) and clamps immediately after every
single delta to `[min, max-1]` on each axis, matching niri's own per-event (never
per-batch) clamp exactly. A new absolute-position probe (`cursor_probe.hpp`'s
`ProbePolicy`) periodically opens the overlay's Wayland input region to capture a real
`wl_pointer.enter` and `anchor()` the estimate to it: gated to never run on a key press
(`hide-when-typing` hides the real pointer on every keystroke), only once motion has
been continuous for `kProbeMinContinuousMs`, at most once per `kProbeStaleMs` once
anchored, and abandoned after `kProbeTimeoutMs` if no enter arrives. The probe itself
uses the raw `wl_surface_set_input_region()` (not the portable GDK call, which does not
reach the compositor for this surface) plus an explicit `wl_surface_commit()` (GTK's
`queue_draw()` does not reliably produce one). The enter is captured on the `GdkSurface`
"event" signal, not a widget-level crossing controller, because the overlay's
`can_target` is FALSE (its normal click-through requirement) and GTK4's
`gtk_widget_pick()` returns NULL for a non-targetable root, so no widget ever sees a
crossing event. It is captured via `g_signal_add_emission_hook()`, not a plain
`g_signal_connect()` — see "Probe capture bug found + fixed" below for why the plain
connect doesn't work and how this was found. Switching away
from follow-cursor mode and back invalidates the anchor and resets the probe policy,
since motion stops arriving entirely while a fixed position is selected (`InputCapture`
only wires the tap in follow-cursor mode) and the frozen estimate is stale by an
unknown amount by the time it resumes.

**Probe policy constants** (shown-only; never triggered by a key press):
`kProbeMinContinuousMs = 60`, `kProbeGapResetMs = 50`, `kProbeStaleMs = 10 * 60 * 1000`,
`kProbeTimeoutMs = 150`.

## What changed (per file)

- **`CMakeLists.txt`** — `pkg_check_modules(LIBINPUT REQUIRED libinput)` for headers
  only (no `IMPORTED_TARGET`); `${CMAKE_DL_LIBS}` added to `keeby-inputd`'s link line;
  new `libinput_loader_test` target + `add_test` (15s timeout).
- **`packaging/arch/PKGBUILD`** — `pkgrel` 7 -> 8; `libinput` added to `depends`.
- **`docs/006-step-2.5-security-permissions.md`** — new §47 "Exact pointer tracking via
  libinput", the full security case for this change (superseding §46's mechanism, not
  its scope decision).
- **`src/libinput_loader.hpp`** (new) — `LibinputApi`, a struct of `libinput_*` function
  pointers each `dlsym()`'d by name; `load_libinput_api()` `dlopen()`s
  `"libinput.so.10"` and resolves every symbol, falling back to an unloaded/default
  `LibinputApi` (never dying) if the library or any symbol is missing.
- **`src/inputd_main.cpp`** — `OpenedPointer` now stores the device's path (for
  `open_restricted()` matching) instead of its own `libevdev*`/resolution/state;
  `try_open_pointer()` only uses `libevdev` transiently to classify the device, then
  frees it. New libinput block: `kPointerInterface`, `pointer_open_restricted()`/
  `pointer_close_restricted()`, `PointerDeviceState` (per-device kind + carry
  remainders), `apply_niri_pointer_settings()`, `setup_libinput()`. `main()` calls
  `setup_libinput()` post-drop; the poll set now has one aggregate libinput fd instead
  of one fd per pointer device; the event loop drains
  `LIBINPUT_EVENT_POINTER_MOTION` only and forwards through `carry_and_truncate()`.
- **`src/pointer_input.hpp`** — removed `TouchpadFingerTracker`,
  `units_to_mouse_equivalent()`, `kDefaultResolutionUnitsPerMm` (libinput now does this
  work); added `carry_and_truncate(delta, remainder)`.
- **`tests/libinput_loader_test.cpp`** (new) — resolves every symbol
  `keeby-inputd` calls against the real installed `libinput.so`; creates and tears down
  a minimal path-backend context end-to-end (no device added, no root, no fork).
- **`tests/pointer_input_test.cpp`** — device-classification tests kept; the
  touchpad-tracker/unit-conversion tests replaced with `carry_and_truncate()` cases
  (sub-count deltas, crossing a whole count, many small deltas, negative symmetry,
  mixed-sign conservation).
- **`src/visualizer/cursor_estimator.hpp`** — removed `MotionSource`/`accel_gain()`;
  `accumulate(dx, dy)` is now 1:1 with no dt/source parameters; added
  `anchor()`/`anchored()`/`invalidate()`; `clamp()` now bounds to `[min, max-1]` per
  axis (was `[min, max]`), matching niri's own clamp.
- **`src/visualizer/cursor_probe.hpp`** (new) — `ProbePolicy` (continuity/staleness/
  timeout state machine) and the `kProbe*` constants.
- **`src/visualizer/viz_app.cpp`** — new probe wiring: `lookup_wl_surface()` (shared by
  the existing click-through setup and the new probe), `set_probe_input_region()`,
  `start_probe()`/`probe_timeout_cb()`, `on_surface_event()` (raw GDK enter capture),
  `on_realize_connect_probe()`/`on_unrealize_restore_click_through()`. `tick_cb` cancels
  any in-flight probe and restores click-through before unmapping. The Motion handler
  in `on_fd_ready()` calls `cursor.accumulate()` 1:1, feeds `ProbePolicy::on_motion()`,
  and starts a probe when eligible; the position-mode handler invalidates the anchor
  and resets the probe policy on a transition back into follow-cursor mode.
- **`src/visualizer_service.cpp` / `.hpp`** — the sender now forwards one Motion message
  per libinput event instead of coalescing a drained batch into one per-kind sum (niri
  clamps every event, not every batch); a new `MotionCarry` (per-kind pending delta)
  ensures a sample that hits `EAGAIN` is never dropped, only carried into the next send
  attempt for that same kind.
- **`tests/visualizer_service_test.cpp`** — coalescing tests rewritten to assert
  one-message-per-event forwarding and in-order mixed-kind delivery; new pure-logic
  test for `MotionCarry`.
- **`tests/visualizer_test.cpp`** — `accel_gain`/touchpad-coalescing tests removed;
  added exact 1:1 accumulation, `anchor()`/`anchored()`/`invalidate()`, per-event-vs-
  batch clamp divergence, and `ProbePolicy` coverage (continuity, gap reset, stale
  re-probe, in-flight suppression, timeout-then-retry).

## Exact checks

All run against the real tree, 2026-09-26. Backup
`build/backups/exacttrack-preapply-20260926-233256.tgz` (682962 bytes, 340 entries,
verified: no `build/` contents, no `.pkg.tar.zst`) taken before applying; `git apply
--check` then `git apply`, in order, for `build/wt/exacttrack-h.patch`,
`build/wt/exacttrack-v.patch`, then `build/wt/exacttrack-docs.patch` — all clean.

```
cmake --build build/integ-rel && ctest --test-dir build/integ-rel --output-on-failure
```
Result: **19/19 pass**

```
cmake -S . -B build/integ-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/integ-asan
```
Result: **19/19 pass**

```
cmake -S . -B build/integ-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/integ-tsan
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-tsan -E 'dbus_menu|control_service|kbus_client|visualizer_backdrop'
```
Result: **15/15 pass** (all targets built first, per the `fake_inputd_test_helper`
requirement; the 4 GTK/GLib exclusions are the known false-positive class, covered by
Release + ASAN above)

```
./build/integ-rel/keeby --check-profiles
```
Result: **19 ok, exit 0**

```
git diff --check
git ls-files --others --exclude-standard -z | xargs -0 -r grep -nP '[ \t]+$'
```
Result: **clean** (both)

```
ldd build/integ-rel/keeby-inputd
```
Expected: only libevdev + libstdc++/libm/libgcc_s/libc/ld-linux — no libinput,
libwacom, libgudev, glib, gobject, lua, pcre2, ffi, mtdev, or libudev.
Result: **confirmed** — `linux-vdso.so.1`, `libevdev.so.2`, `libstdc++.so.6`,
`libm.so.6`, `libgcc_s.so.1`, `libc.so.6`, `ld-linux-x86-64.so.2`. None of libinput or
its dependency chain present.

```
cd packaging/arch && makepkg -f --noconfirm
bsdtar -tvf keeby-0.1.0-8-x86_64.pkg.tar.zst
```
Result: built `keeby-0.1.0-8-x86_64.pkg.tar.zst` (389006 bytes); `check()` **19/19
pass**. `bsdtar -tvf` confirms `-rwxr-s--- root/input usr/lib/keeby/keeby-inputd` and
`-rwxr-xr-x root/root` on `usr/bin/keeby`, `keeby-settings`, `keeby-visualizer`,
`keeby-fetch-packs`; `depends` includes `libinput`.

### `enterfix.patch` merge checks (2026-09-27)

Backup before applying: `build/backups/enterfix-preapply-20260927-004044.tgz` (705857
bytes, verified: no `build/` or `.pkg.tar.zst` entries). `git apply --check` then
`git apply build/wt/enterfix.patch`: clean. Post-merge real tree: Release **19/19**,
ASAN/UBSAN **19/19**, TSAN **15/15** (same 4 exclusions), `--check-profiles` **19 ok**,
`git diff --check` clean, untracked-whitespace grep clean.

Live anchor proof, driver `build/wt/exacttrack-live/viz_live_probe_test.py`: worker
copy `ANCHORED after 329 ms` and, a repeat, `296 ms`; supervisor re-run on the worker
copy `ANCHORED after 320 ms`; supervisor run against the merged real tree's
`build/integ-rel/keeby-visualizer` printed `keeby-visualizer: cursor anchored` and
`ANCHORED after 447 ms`; no leftover visualizer processes afterward (only the
installed `/usr/bin/keeby-visualizer` remained).

Repackaged as `0.1.0-8` (never installed, so `pkgrel` stays 8):
`keeby-0.1.0-8-x86_64.pkg.tar.zst`, 389199 bytes, sha256
`31c0e2d6e5da14f363bbcf89ec5a7f10243c8e9c5b3a1962bcb79ba0bc168508` — replaces the
earlier 389006-byte 0.1.0-8 build, which lacked the fix. `bsdtar` confirms
`-rwxr-s--- root/input usr/lib/keeby/keeby-inputd`, `-rwxr-xr-x root/root` on `keeby`,
`keeby-settings`, `keeby-visualizer`, `keeby-fetch-packs`; `.PKGINFO` `depend = libinput`;
the packaged `keeby-visualizer` binary contains `g_signal_add_emission_hook` and the
`"cursor anchored"` string.

Honest open item: the first `makepkg` attempt after the merge failed `check()` with 1
of 19 tests (log not retained, test unknown); the second attempt passed 19/19. It did
not reproduce in 20 serial runs of the exact `check()` command (`ctest
--output-on-failure` in `packaging/arch/src/build`), nor in 15 parallel (`-j8`) repeats
in each of `build/integ-rel` and `packaging/arch/src/build`. Recorded as an
unexplained, unreproduced intermittent failure to watch for.

## Limitations

- The GTK/Wayland safety of the explicit bufferless `wl_surface_commit()`, and of the
  overlay's `wl_surface` persisting across a hide/show cycle, is empirical only (GTK
  4.22.5 + gtk4-layer-shell 1.3.0: the same `wl_surface` survives a hide/show cycle in
  practice) — not confirmed against either library's own source or documentation.
- Before the first anchor in a session (or after a long enough gap to go stale), the
  panel can start or remain visibly offset from the real cursor.
- Pointer warps that don't go through libinput's own accelerated motion path (pointer-
  lock games, programmatic warps) drift the estimate until the next edge contact or the
  10-minute stale re-probe — whichever comes first.
- The libinput acceleration settings applied (`apply_niri_pointer_settings()`) are
  hardcoded to mirror the user's own niri config, not read from it (`ponytail`-marked;
  upgrade path is passing settings via `argv` if this ever needs to serve a different
  config).
- The visualizer-service sender only sums a motion sample into the next send when the
  socket is under backpressure (`MotionCarry`); on the normal path every event is
  forwarded individually, one message at a time.

## Probe capture bug found + fixed (2026-09-27)

The mechanism above never actually anchored on the merged code. A 15s `WAYLAND_DEBUG`
run of `build/integ-rel/keeby-visualizer` on the panel's layer surface `wl_surface#40`
showed `attach` 886, `commit` 1077, `set_input_region(nil)` 95 and its `restore` 95 (95
probes opened and closed), and 78 real `wl_pointer.enter` events on that surface — but
`keeby-visualizer: cursor anchored` 0 times. niri was delivering the enter;
`on_surface_event` in `src/visualizer/viz_app.cpp` never saw it.

Root cause, verified directly against GTK 4.22.5 and GLib 2.88.3 source (both match the
installed versions exactly): `GtkWidget::"realize"` is `G_SIGNAL_RUN_FIRST`
(`gtk/gtkwidget.c:1781-1788`), so `gtk_window_realize()` — which connects GTK's own
handler (`surface_event`) to the new `GdkSurface`'s "event" signal
(`gtk/gtkwindow.c:4527`) — always runs before any normally-connected "realize"
listener, including this file's `on_realize_connect_probe`. GTK's handler
(`gtk/gtkwindow.c:4949-4954` calling `gtk_main_do_event`, `gtk/gtkmain.c`)
unconditionally returns `TRUE` for `GDK_ENTER_NOTIFY`. `GdkSurface::"event"`
accumulates with `g_signal_accumulator_true_handled` (`gdk/gdksurface.c:706-712`),
whose semantics (`gobject/gsignal.c:4152-4166`) stop the emission the instant a
handler returns `TRUE` — confirmed in the handler-list loop
(`gobject/gsignal.c:3908-3957`): a second, normally-connected handler on the same
signal (what this file had) simply never runs for this event type, no matter how many
enters arrive.

Fix: capture the enter with `g_signal_add_emission_hook()` on `GdkSurface::"event"`
instead of a plain `g_signal_connect()`. Verified in the same `gobject/gsignal.c`: the
emission-hook phase (~3780-3906) runs unconditionally, strictly before the
handler-list loop that implements the accumulator's stop — the one place left that
still sees every `GDK_ENTER_NOTIFY` delivered to this surface. Installed from
`on_realize_connect_probe`, filtered to our own surface and to `GDK_ENTER_NOTIFY`
while a probe is in flight; removed with `g_signal_remove_emission_hook()` from
`on_unrealize_restore_click_through`. Everything else (probe policy/constants, the raw
`wl_surface_set_input_region` + `wl_surface_commit`, shown-only gating, hide-path
cancel) is unchanged. `src/visualizer/viz_app.cpp` only, 68 insertions / 29 deletions
(`build/wt/enterfix.patch`, **MERGED into the real tree 2026-09-27 — do NOT
re-merge**).

Live-verified against the fixed build over a real niri session (no synthetic input):
the driver (`build/wt/exacttrack-live/viz_live_probe_test.py --binary <fixed
keeby-visualizer>`) printed `keeby-visualizer: cursor anchored` and `ANCHORED after
296 ms` (a repeat run: 329 ms). A `WAYLAND_DEBUG=1` run against the same build showed
exactly 1 `set_input_region(nil)`, 1 real `wl_pointer.enter` on `wl_surface#40`, and 1
`cursor anchored` line, 7 log lines apart — then no further probes for the rest of the
15s run, since the estimate was already anchored and not yet stale (`kProbeStaleMs`).
Reconfirmed after merge (2026-09-27, see "`enterfix.patch` merge checks" above):
supervisor re-run on the worker copy `ANCHORED after 320 ms`; a run against the merged
real tree's `build/integ-rel/keeby-visualizer` printed `keeby-visualizer: cursor
anchored` and `ANCHORED after 447 ms`, with no leftover visualizer processes. Repackaged
as `0.1.0-8` (389199 bytes, sha256
`31c0e2d6e5da14f363bbcf89ec5a7f10243c8e9c5b3a1962bcb79ba0bc168508`) — still not
installed on the user's machine, see User live-check steps below.

## User live-check steps

A throwaway harness check the user ran earlier (2026-09-26) reported no
`wl_pointer.enter` while moving the touchpad. That result is **invalid as evidence**:
the harness window was built with opacity 0, so GTK never attached a buffer to it
(traced: 0 `wl_surface.attach` calls), the layer surface was therefore never actually
mapped, and niri never hit-tests pointer focus against an unmapped surface — the
harness never gave the probe mechanism a chance to run, independent of whether the
real `keeby-visualizer` probe works. The live mechanism check moved to a driver
against the real `keeby-visualizer` (`build/wt/exacttrack-live/`); it found and this
step fixed the emission-hook bug above (see "Probe capture bug found + fixed"). The
user's own install + live-check below is still **PENDING**, now against the fixed
build.

1. Supervisor PASS (see "`enterfix.patch` merge checks" above). Optional for the
   user: `python3 build/wt/exacttrack-live/viz_live_probe_test.py --binary
   build/integ-rel/keeby-visualizer --seconds 6` (the panel appears, then prints
   ANCHORED).
2. `sudo pacman -U packaging/arch/keeby-0.1.0-8-x86_64.pkg.tar.zst`
3. `systemctl --user restart keeby.service`, then `pacman -Q keeby`, which should show
   `keeby 0.1.0-8`.
4. `journalctl --user -u keeby.service -b --no-pager | grep -E 'keeby-inputd: libinput|keeby-visualizer: cursor anchored'`
5. Visual: type to summon the panel, then move the pointer while it's visible — the
   panel's top-left should sit just below-right of the cursor and follow it 1:1.
6. Mandatory security re-check (the helper changed — see docs/006): Uid/Gid/Groups/
   Cap*/NoNewPrivs of both `keeby` and `keeby-inputd`, plus a user-authorized single
   `SIGUSR1` to the helper, expecting the journal line
   `keeby-inputd: post-drop open() failed as expected (EACCES)`.
