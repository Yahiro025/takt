# 014 — Cursor-follow + ripple overlay: merge, verify, package 0.1.0-5

## Goal

Merge the two in-flight pieces from HANDOFF §6 (pointer-input patch + overlay/ripple
worker) plus the multi-monitor confinement fix, re-verify Release + ASAN/UBSAN + TSAN,
and build the `0.1.0-5` package. No live install, no new screen captures, no commits.

## Merged changes (all already in the working tree; this step verified + packaged)

- **Pointer input (ex-6a):** `src/pointer_input.hpp` (100 lines), `src/pointer_event.hpp`.
  `keeby-inputd` opens up to 4 pointer devices (REL_X/Y mice, single-finger touchpads)
  before the privilege drop and forwards only summed per-frame dx/dy as
  `wire::MessageType::PointerMotion = 2`. `InputCapture` routes motion only to the
  visualizer tap, and only when the visualizer is enabled with `follow-cursor`.
  `src/visualizer_wire.hpp`: `Position::FollowCursor = 6` (new default),
  `MessageType::Motion = 3`, config `reserved` byte carries follow_speed in 0.1 steps
  (1..40, default 10). D-Bus `GetVisualizer()` / `SetVisualizerFollowSpeed(d)`,
  settings key `visualizer_follow_speed`, `keeby ctl visualizer speed <x>`,
  "Follow cursor" tray item, fd-3 CLOEXEC fix in both spawners. `pkgrel` = 5.
- **Overlay/ripple (ex-6b):** `src/visualizer/cursor_estimator.hpp` (83 lines),
  `src/visualizer/viz_spring.hpp` (71 lines), `src/visualizer/viz_ripple.hpp`
  (89 lines); `src/visualizer/viz_app.cpp` spring-smoothed follow + velocity tilt +
  fade-in/scale; `src/ui/visualizer_tab.cpp` 7-tile picker (Follow Cursor first,
  4+3 layout) with Follow Speed slider shown only for follow-cursor.
- **Multi-monitor confinement fix:** `src/visualizer/viz_position.hpp:83-104`
  (`local_bounds_for_surface`, `fixed_target_in`, `follow_target_in`) keeps every
  target in the anchored layer-shell surface's local coordinates instead of the
  monitor-union rectangle; `viz_app.cpp` derives the local bounds from the live
  surface allocation. Regression test in `tests/visualizer_test.cpp:325-368`.
- **Finalizer formatting fix (this step):** `src/visualizer/viz_app.cpp:411-412`
  split the shared-line `spring_x.step(...); spring_y.step(...);` onto two lines.
- **docs/006 §46** ("Pointer motion for the visualizer", line 1227) records the
  user-approved privilege-scope change, closing the 6a doc item.

## Exact checks executed (2026-09-26, real repo)

- `cmake --build build/integ-rel` — clean.
  `ctest --test-dir build/integ-rel --output-on-failure` — **17/17 pass**.
- Reconfigure + full build ASAN/UBSAN, then
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-asan` —
  **17/17 pass**.
- Reconfigure + full build TSAN (all targets), then
  `TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-tsan -E 'dbus_menu|control_service|kbus_client'` —
  **14/14 pass** (3 GLib tests excluded, known false positives).
- `./build/integ-rel/keeby --check-profiles` — 19 `ok` lines, exit 0.
- `git diff --check` — clean. Untracked text files
  (`git ls-files --others --exclude-standard` + `grep -InP '[ \t]+$'`, binaries
  excluded) — no trailing whitespace.
- `cd packaging/arch && makepkg -f --noconfirm` — built
  `packaging/arch/keeby-0.1.0-5-x86_64.pkg.tar.zst` (386169 bytes).
  `bsdtar -tvf` confirms:
  `-rwxr-s--- root/input usr/lib/keeby/keeby-inputd`,
  `-rwxr-xr-x root/root usr/bin/keeby`, `usr/bin/keeby-settings`,
  `usr/bin/keeby-visualizer`.

## Screenshots (existing, no new captures taken)

Cropped overlay/window captures in `build/wt/overlay-shots/`: `still.png`,
`moving.png`, `moving_01.png`–`moving_06.png`, `ripple_01.png`–`ripple_06.png`,
and the corrected `visualizer_tab.png` (re-captured 2026-09-26, 7 tiles + slider).
They were not re-judged against the recording in this step.
Evidence handling: during this session the overlay worker once captured browser
content by mistake due to bad `grim` geometry; the file was shredded immediately
and was not retained or shared. The corrected `visualizer_tab.png` was then
captured with the compositor `screenshot-window` action (window-only by
construction), with no further `grim -g` used. Finalization in this step took
no new captures.

## Limitations — not verified, user steps mandatory

- Cursor position is an estimate from relative motion: expect drift; screen edges
  re-sync it; Follow Speed slider calibrates it.
- The overlay surface spans exactly one output (focused output); no multi-monitor
  spanning.
- Security check passed live on 0.1.0-5 (see below); visual pending. Still mandatory
  before 0.1.0-5 counts as done: live look-and-feel review (spring follow, tilt,
  ripple, drift, slider).

## Live security re-check — passed 2026-09-26 on 0.1.0-5

- `pacman -Q keeby => keeby 0.1.0-5`; service active/running MainPID 221310
  `/usr/bin/keeby`; sole direct helper PID 221339 (`comm keeby-inputd`, PPid 221310).
- Both processes Uid/Gid 1000 in all slots, Groups with no `input` gid 992,
  CapPrm/CapEff `0000000000000000`; helper `NoNewPrivs 1`.
- Installed helper stat root:input mode 2750, ACL `group:keeby:--x`.
- `/proc/221339/exe` readlink Permission denied (non-dumpable; path not asserted verified).
- User-authorized single SIGUSR1 at 17:05:39 (kill exit 0); filtered journal
  `keeby-inputd: post-drop open() failed as expected (EACCES) — privilege drop verified live`;
  service and helper stayed on the same PIDs afterward.
