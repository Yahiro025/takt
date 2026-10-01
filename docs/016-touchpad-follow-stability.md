# 016 — Touchpad follow stability: batching-independent linear gain, per-kind sends, package 0.1.0-7

## Goal

On installed 0.1.0-6 the user live-confirmed the 015 backdrop fix (no opaque
rectangle anymore) but reported a remaining cursor-follow issue: with the
touchpad the keyboard travels too far / jumps, and it stays far from a
stationary pointer parked near the center — while the Follow Speed control
does work. The user approved a stable touchpad fix. A reviewed isolated patch
at `build/wt/follow.patch` (6 paths) makes touchpad motion linear in
`follow_speed` and independent of batch arrival timing, sends mixed
mouse/touchpad batches as separate per-kind Motion messages, and keeps the
mouse adaptive-accel path untouched. This step: back up the tree, apply the
patch (no code changes of our own), verify Release + ASAN/UBSAN + TSAN +
profiles + whitespace, bump `pkgrel` to 7, and build the `0.1.0-7` package
(built, NOT installed). No live install, no screen captures, no raw motion
logging, no commits.

## Applied changes (patch only, all in the working tree)

- **`src/visualizer/cursor_estimator.hpp`** — new `MotionSource { Mouse,
  Touchpad }` (values match `VizMessage` kind / `PointerDeviceKind`);
  `accumulate(dx, dy, dt_ms, source = Mouse)`: touchpad returns early with
  `x += dx * follow_speed_` (dt ignored), mouse keeps the libinput-like
  `accel_gain(speed) * follow_speed_` curve. Comment corrected: edge clamping
  only re-syncs when real pointer and estimate reach the same edge.
- **`src/visualizer/viz_app.cpp`** — the arrival clock is now mouse-only
  (`has_last_mouse_motion` / `last_mouse_motion_time`); touchpad Motion
  accumulates with dt 0.0 and never shifts the interval the next mouse batch
  measures.
- **`src/visualizer_service.cpp` / `.hpp`** — the sender drain splits each
  batch by kind (mouse first, touchpad second, up to two Motion messages) so
  a fast mixed burst no longer collapses to a single last-kind-wins sum.
  No helper / privilege / wire change: `keeby-inputd` is untouched.
- **Tests** — `tests/visualizer_test.cpp::test_touchpad_coalescing_invariant`
  (touchpad 10x2 == coalesced 20-count at 1 ms and 10 ms arrival, exact linear
  `follow_speed` scaling, mouse batching still diverges through the accel
  curve, default arg stays mouse) and a mixed mouse+touchpad single-batch case
  in `tests/visualizer_service_test.cpp` (one Motion line per kind with exact
  per-kind totals).

## Red proof (from the reviewed patch, preserved here)

- Touchpad before the fix went through the accel gain: 10x2-count samples at
  1 ms moved ~21.7 px while one coalesced 20-count sample moved 44.0 — same
  finger travel, ~2x different displacement depending on batching. After the
  fix both land on exactly the summed counts (5020.0 in the test bounds).
- Mixed-kind batch before: one combined Motion message (last-kind-wins sum).
  After: two lines, mouse=(20,0) + touch=(0,20) exactly.

## Exact checks executed (2026-09-26, real repo)

- Backup `build/backups/follow-preapply-20260926-193109.tgz` (828694 bytes,
  220 entries, verified: no `.pkg.tar.zst`, no `build/` contents) taken before
  apply; `git apply --check build/wt/follow.patch` clean, then `git apply` —
  6 paths, all pre-existing tracked/untracked changes preserved, no
  staging/commit/push.
- `cmake --build build/integ-rel` — clean.
  `ctest --test-dir build/integ-rel --output-on-failure` — **18/18 pass**.
- Reconfigure + full build ASAN/UBSAN, then
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-asan` —
  **18/18 pass**.
- Reconfigure + full build TSAN (ALL targets), then
  `TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-tsan -E 'dbus_menu|control_service|kbus_client|visualizer_backdrop'` —
  **14/14 pass** (the 4 GTK/GLib exclusions are the known false-positive
  class; they pass under Release and ASAN).
- `./build/integ-rel/keeby --check-profiles` — **19 ok**.
- `git diff --check` clean; untracked-whitespace grep matches only binary
  `.wav` bytes (pre-existing assets), all 6 patched text files clean.
- `pkgrel` 6 → 7; `cd packaging/arch && makepkg -f --noconfirm` built
  `keeby-0.1.0-7-x86_64.pkg.tar.zst` (386552 bytes, `check()` passed);
  `bsdtar -tvf` confirms `-rwxr-s--- root/input usr/lib/keeby/keeby-inputd`
  and `-rwxr-xr-x` `usr/bin/keeby`, `keeby-settings`, `keeby-visualizer`.

## User-reported status carried in

- Backdrop: user-confirmed visual PASS on installed 0.1.0-6 (opaque
  rectangle gone) — reported live, no screenshot taken.
- Touchpad follow: too far/jumpy and offset from a stationary centered
  pointer; Follow Speed slider confirmed working by the user at 1.0.
- This patch does NOT (and cannot) guarantee the panel lands exactly on a
  stationary pointer: the estimator starts at the output center and Wayland
  gives clients no real cursor position, so an initial offset is expected and
  drift can only re-sync at a shared edge. Do not claim the follow bug fully
  fixed without the live test below.

## Limitations — live Niri check PENDING (0.1.0-7 built, NOT installed)

- Initial estimate is the output center (unknown real cursor): first summon
  may start offset until motion/edge contact corrects it.
- Drift remains possible mid-output; edge clipping still pins the estimate.
- User steps after install: `sudo pacman -U
  packaging/arch/keeby-0.1.0-7-x86_64.pkg.tar.zst`, `systemctl --user restart
  keeby.service`, `pacman -Q keeby` (expect 0.1.0-7); summon the overlay by
  typing, move the touchpad pointer to the screen center, stop, and after
  ~0.3 s check the panel position and its distance to the pointer, plus that
  travel stays proportional while moving.
