# 015 — Visualizer backdrop transparency: apply reviewed patch, verify, package 0.1.0-6

## Goal

The user's 48 s screen recording of installed 0.1.0-5 shows our overlay rendering
an opaque fullscreen rectangle (the GTK theme's window background painted across
the whole full-output layer-shell surface) instead of the getkeeby.com look —
only the small keyboard panel floating over the desktop. The user chose backdrop
transparency first. A reviewed patch at `build/wt/backdrop.patch` (4 paths:
`CMakeLists.txt`, `src/visualizer/viz_app.cpp`, new
`src/visualizer/viz_backdrop_css.hpp`, new
`tests/visualizer_backdrop_test.cpp`) fixes it and adds a headless red/green
regression test. This step: back up the tree, apply the patch (no re-merge of
pointer/overlay work), apply one tiny safety fix inside the new test, verify
Release + ASAN/UBSAN + TSAN + profiles + whitespace, bump `pkgrel` to 6, and
build the `0.1.0-6` package. No live install, no new screen captures, no
commits.

## Applied changes (patch + one safety fix, all in the working tree)

- **Fix:** `src/visualizer/viz_backdrop_css.hpp` (new, 37 lines) — class-scoped
  (`keeby-viz-window`, `keeby-viz-canvas`) CSS opting the window and its
  drawing area out of every themed background, image and shadow
  (`background-color: transparent; background-image: none; box-shadow: none`),
  installed at APPLICATION priority like `src/ui/app.cpp`. `viz_app.cpp`
  calls `apply_backdrop_style(state.window, state.area)` at startup before the
  first show — only the painted panel/shadow stays visible.
- **Test:** `tests/visualizer_backdrop_test.cpp` (new) + CMake registration
  (`TIMEOUT 60`, `SKIP_RETURN_CODE 77`). Headless GSK render-node check under
  a private `gtk4-broadwayd` display: a bare window must show an opaque
  fullscreen color node (bug reproduced, else `WORKER_VACUOUS` fail), then the
  CSS window must show zero halfscreen-opaque color nodes while keeping its
  cairo panel node (fix confirmed) with zero CSS parse errors.
- **Safety fix (this step, test-only):** both forked children now check the
  `prctl(PR_SET_PDEATHSIG, SIGTERM)` return and `_exit(127)` on failure
  (`tests/visualizer_backdrop_test.cpp:202,261`) instead of proceeding
  without timeout protection.

## Isolation pre-check (before first run — confirmed by grep)

- Daemon exec'd with `--unixsocket` inside an `mkdtemp` private runtime dir;
  `XDG_RUNTIME_DIR` points there for our children only.
- No `setsid` call anywhere (only comments stating it is deliberately not
  used, so CTest timeout signals still reach the daemon).
- `kill` targets only our own daemon pid; `unlink`/`rmdir` touch only the two
  private socket paths and the private dir, after both children exit.

## Exact checks executed (2026-09-26, real repo)

- Backup `build/backups/backdrop-preapply-20260926-181534.tgz` (420097 bytes,
  key files verified inside) taken before apply; `git apply --check` clean,
  then `git apply` — 4 paths, no other tree changes.
- `cmake --build build/integ-rel` — clean.
  `ctest --test-dir build/integ-rel --output-on-failure` — **18/18 pass**
  (was 17; `visualizer_backdrop_test` passes in ~0.15 s, verdict
  `visualizer_backdrop_test: OK`).
- Reconfigure + full build ASAN/UBSAN, then
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-asan` —
  **18/18 pass** (LSAN clean; the worker's `_exit()` keeps GTK one-time
  globals out of the parent's leak checker).
- Reconfigure + full build TSAN (all targets), then
  `TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-tsan -E 'dbus_menu|control_service|kbus_client|visualizer_backdrop'` —
  **14/14 pass**. Exclusion rationale: the new test forks a worker that runs
  full `gtk_init` (GDK/GSK broadway backend) plus an external daemon —
  GTK/GLib's internal thread pools are the same known-false-positive class as
  the three excluded GLib tests (AGENTS.md §2); it is still covered by Release
  and ASAN/UBSAN with leak detection.
- `./build/integ-rel/keeby --check-profiles` — 19 `ok` lines, exit 0 (the
  `sound_pack: missing ... BACKSPACE/ENTER/SPACE.mp3` warnings are the
  pre-existing partial-pack notices, unchanged from 0.1.0-5).
- `git diff --check` — clean. Untracked text files
  (`git ls-files --others --exclude-standard` + `grep -nP '[ \t]+$'`) — no
  trailing whitespace (only binary `.wav` matches, excluded).
- `cd packaging/arch && makepkg -f --noconfirm` — built
  `packaging/arch/keeby-0.1.0-6-x86_64.pkg.tar.zst` (386354 bytes);
  `check()` (full ctest under fakeroot) passed as part of the build.
  `bsdtar -tvf` confirms:
  `-rwxr-s--- root/input usr/lib/keeby/keeby-inputd`,
  `-rwxr-xr-x root/root usr/bin/keeby`, `usr/bin/keeby-settings`,
  `usr/bin/keeby-visualizer`, `usr/bin/keeby-fetch-packs`.

## Notes and caveats

- The worker child's `bare:`/`fixed:` diagnostic prints are lost by design:
  it `_exit()`s (keeping GTK one-time globals out of LSAN), which skips stdio
  flush. The verdict travels via the exit-code protocol (`WORKER_OK` requires
  the bare phase to reproduce opacity AND the fixed phase to remove it while
  keeping panel content), so the green run is a genuine red→green, not a
  vacuous pass — `WORKER_VACUOUS`/`WORKER_NO_SNAPSHOT` fail loudly instead.
- An earlier diagnostic Broadway session (default TCP listener) ran briefly
  during patch review and was stopped; the merged test is Unix-socket-only
  (see isolation pre-check) and opens no TCP listener.
- No service, helper, or install changes: the helper binary is byte-identical
  in role (same setgid `root/input` mode); only `keeby-visualizer` and the new
  test are affected by this step.

## Limitations — update 2026-09-26: backdrop live PASS, follow-up moved to 016

- Live compositor check DONE — user-confirmed visual PASS on installed
  0.1.0-6 (`pacman -Q keeby => keeby 0.1.0-6`): the opaque rectangle is gone
  (reported live, no screenshot). Unit/headless evidence below stays as the
  build-time record, not the visual verdict.
- Pointer-tracking drift from 014 stayed open and became the §6e
  touchpad-follow issue (too far/jumpy, offset from a stationary centered
  pointer — visual follow still unresolved); the Follow Speed slider itself
  was confirmed working by the user (at 1.0), and the original recording's
  overall look-and-feel review is still open. Tracked in docs/016, with
  0.1.0-7 built (NOT installed) and the live Niri stationary-middle check
  PENDING.
