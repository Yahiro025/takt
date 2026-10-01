# KEEBY — Handoff (2026-09-26)

For the next agent continuing this project. Read this whole file before touching anything.

## Current next step (2026-10-01)

The source audit and imported-playback findings are in [research report 003](docs/research/003-keeby-playback-fidelity.md) and [report 020](docs/020-imported-playback-fidelity.md). The merged playback correction passed Release 20/20, ASAN/UBSAN 20/20, TSAN 16/16 (four documented exclusions), 45/45 profile-load checks, the independent RED/GREEN fixture, the 2,700-row render/source checks, and the 26-profile mapping/render audit (12,864 mapped key-direction entries; 25,480 mapped variant references; zero failures). Package `0.1.0-12` is built and verified but not installed. Package `0.1.0-11`, including the settings-window scrolling change in [report 019](docs/019-settings-window-scroll.md), remains installed and `keeby.service` is active; `pacman -Q keeby` reports `0.1.0-11`. Package 12 SHA-256 is `7c68cbb1468c872eb668fb17c6a873ae78f3e57413b566c8a500df6c8610238f`. Any future code package must use `pkgrel=13`. Headless geometry QA verified the bounded view and full Sound-row reachability, while the user's desktop visual check remains pending. Fresh helper-security evidence, live button/Fn capture, exact cursor-tracking visual acceptance, and controlled listening remain open. Read §6g for the live install and acceptance steps.

## 1. What KEEBY is

A Linux-native clone of **Keeby** (https://getkeeby.com/ — mechanical-keyboard typing sounds, macOS/Windows) for the user's own machine: **CachyOS (Arch) + Niri Wayland compositor + noctalia (quickshell) bar**. C++23, CMake + Ninja, GCC 16, PipeWire, libevdev, sdbus-c++ 2.3.1, GTK4 (UI only), gtk4-layer-shell (overlay), nlohmann-json, libsamplerate, libsndfile.

Goal set by the user: "build this keeby for my cachyOS", and later: make the UI and the pop-up keyboard "look exactly like keeby", replicating **everything in their screen recording** of getkeeby.com's pop-up keyboard (`~/Videos/recording_2026-09-25_22.12.31.mp4`), including cursor-following, the 3D tilt, and the "water-drop" ripple effect.

## 2. How the user works (follow these — they have enforced them repeatedly)

- **Never stage, commit, or push unless the user explicitly says so.** The last commit is still `0e0d8bd feat: complete step 2.2 sample mixer`; everything since (69 paths) is uncommitted in the working tree. Offer to commit; don't do it.
- **No PASS without live evidence.** Unit tests are not enough. When a check needs the user (sudo, installing, rebooting, listening, looking at the screen), stop, give exact commands, and ask for the output.
- **The PipeWire realtime path must stay free** of allocation, locks, syscalls, IPC and exceptions. Verify every merge with Release + ASAN/UBSAN + TSAN, plus `git diff --check`.
- **Security matters**: raw keystrokes are confidential. Only `keeby-inputd` touches `/dev/input`. Never send keystrokes over D-Bus or any named socket. Scope changes to privileges need the user's explicit approval (see docs/006).
- **Ask before killing any process** you didn't start.
- **No Qt.** GTK4 is allowed only for the UI processes (`keeby-settings`, `keeby-visualizer`), never in the engine.
- **Working style the user asked for**: the top model acts as **orchestrator/supervisor only** — it spawns worker subagents (in parallel when independent), then verifies their work (reads diffs, reruns tests, checks screenshots) before merging. Workers get self-contained briefs with file paths, constraints and acceptance criteria.
- **Screenshots: never capture the user's whole screen** (a worker once captured private content). Crop to the window/overlay (`grim -g`, and focus the window with `niri msg action focus-window --id <id>` right before capturing).

## 3. Architecture (current)

```
keyboard ─▶ keeby-inputd (setgid input, drops privilege after opening devices)
              │ 16-byte wire msgs over a private socketpair (fd 3)
              ▼
            keeby (unprivileged main process, systemd user service)
              ├─ InputCapture thread ─▶ SPSC EventTransport ─▶ PipeWire RT callback
              │                                               (AudioBoundary → SampleMixer:
              │                                                per-key press/release samples,
              │                                                stereo pan, tone filter)
              ├─ tap ─▶ VisualizerService ─▶ keeby-visualizer (child, private socketpair fd 3,
              │                               8-byte VizMessages; GTK4 + layer-shell overlay)
              ├─ D-Bus session bus: org.keeby.Keeby
              │     /org/keeby/Keeby  org.keeby.Control1 (ctl + settings window)
              │     StatusNotifierItem + DBusMenu tray at /MenuBar
              └─ settings: ~/.config/keeby/settings.ini
keeby-settings (GTK4 window, separate process, GDBus client of Control1)
keeby ctl ...  (CLI client), keeby --list-profiles / --check-profiles
```

Key docs (all in `docs/`, read the relevant one before changing an area):
- 006 security model (Steps 2.5, 2.5B, 2.5C); needs a pointer-scope section (see §6)
- 007 engine v2 (all keys, press/release, pan)
- 008 sound packs (Mechvibes config.json v1/v2, null/missing-file handling, `--check-profiles`)
- 009 desktop control (tray menu, `keeby ctl`, single instance, settings)
- 010 packaging (PKGBUILD, `keeby` group ACL, reboot-not-logout note, service start limits)
- 011 tone filter + Control1 API
- 012 settings window
- 013-visualizer.md (engine half) and 013-visualizer-app.md (overlay half)

## 4. Installed state on the user's machine

- Package **`keeby 0.1.0-11`** is installed (`pacman -Q keeby => keeby 0.1.0-11`) and
  `keeby.service` is active/running (verified 2026-10-01). PIDs from older
  sessions are stale; do not reuse them.
- The helper is `/usr/lib/keeby/keeby-inputd`, root:input 2750, with ACL `group:keeby:--x`. The user is in group `keeby` (gid 948) and not in `input` (gid 992).
- `~/.local/share/keeby/packs` contains 18 older Mechvibes packs and 26 imported Keeby source packs. The development and packaged binaries accept 45 profiles total (44 pack profiles plus the built-in profile), verified with `keeby --check-profiles`.
- `gtk4-layer-shell` is installed.
- Live-verified 2026-09-26 on 0.1.0-5: Step 2.5 security — both processes Uid/Gid 1000 in all slots, Groups with no gid 992, CapPrm/CapEff `0000000000000000`, helper `NoNewPrivs 1`; user-authorized single SIGUSR1 at 17:05:39 (kill exit 0), filtered journal `keeby-inputd: post-drop open() failed as expected (EACCES) — privilege drop verified live`; service and helper stayed on the same PIDs afterward. `/proc/<helper-pid>/exe` readlink Permission denied (non-dumpable; path not asserted verified). Sound and tray live-verified earlier. (Historical 0.1.0-5 evidence, kept — the privilege-drop mechanism itself is unchanged, though 0.1.0-8 adds a post-drop libinput load on top of it — see §6f.)
- Live-verified 2026-09-26 on installed 0.1.0-6: **backdrop transparency visual PASS** — the opaque fullscreen rectangle from the 0.1.0-5 recording is gone (user eyeballed on Niri, no screenshot).
- Live-verified 2026-09-26 on installed 0.1.0-7 via the user's own recording
  (`~/Videos/recording_2026-09-26_20.16.04.mp4`): **cursor-follow visual FAIL**
  — the panel drifts from the real pointer instead of tracking it (e.g. t=4s
  cursor top-center vs. panel bottom-left; t=12s panel pinned in the
  top-right corner while the cursor is mid-right; short intervals show the
  panel moving ~2.7x-3.4x farther than the cursor over the same span).
  Root-caused and fixed by the exact-tracking change — see §6f and
  docs/017-exact-cursor-tracking.md.
- The settings window's bounded geometry and full Sound-list reachability passed headless QA, but the user's desktop visual check is still pending.
- Package 0.1.0-11 includes the exact cursor-tracking code from docs/017, but its user visual acceptance and the fresh helper-security check remain pending. Real mouse-button/Fn capture and controlled listening also remain pending.

## 5. Build / verify / package

Dev build dirs used by the supervisor (all gitignored under `build/`):
```
cmake --build build/integ-rel && ctest --test-dir build/integ-rel --output-on-failure
cmake -S . -B build/integ-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-asan
cmake -S . -B build/integ-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-tsan -E 'dbus_menu|control_service|kbus_client|visualizer_backdrop'
```
- Build ALL targets for TSAN, because tests launch `fake_inputd_test_helper`.
- Exclude the four GTK/GLib-based tests from TSAN: GLib's internal thread pool gives known false positives (the `visualizer_backdrop` test additionally forks a full-`gtk_init` worker plus an external daemon). They still run under Release and ASAN.
- `git diff --check` skips untracked files, so also grep new files for trailing whitespace.

Package: `cd packaging/arch && makepkg -f --noconfirm` builds `keeby-<ver>-<rel>-x86_64.pkg.tar.zst` from the **working tree** (`$startdir/../..`, on purpose: the work is uncommitted). The user installs it with `sudo pacman -U …` and then `systemctl --user restart keeby.service`. Bump `pkgrel` for every release. Historical note (2026-09-26): package 0.1.0-8 was built while 0.1.0-7 was installed.

Current state (2026-10-01): package `0.1.0-11` is installed and verified; the next code package must use `pkgrel=12`. See §6g. The package 10 install instructions in report 018 and package 8 instructions in §6f are historical.

Worker isolation pattern used so far: each worker rsyncs the tree into `build/wt/<name>/`, makes a throwaway `git init` there, and outputs `build/wt/<name>.patch` (`git diff --cached --binary HEAD`). The supervisor verifies that the patch equals the copy, backs up the tree to `build/backups/*.tgz`, runs `git apply --check` and `git apply`, rebuilds, and reruns the checks.

## 6. MERGED + PACKAGED 0.1.0-5 (HISTORICAL, SECURITY VERIFIED, 0.1.0-5 VISUAL FAIL ON RECORDING) + 0.1.0-6 INSTALLED (BACKDROP PASS) + 0.1.0-7 INSTALLED (FOLLOW STILL FAIL ON RECORDING) + 0.1.0-8 BUILT + VERIFIED, NOT INSTALLED (resume here — see §6f)

The user's current request: **replicate Keeby's pop-up keyboard exactly** — it follows the cursor with a springy lag, tilts in 3D while moving, has a big soft shadow, and is a small, darker, Mac-like 60% keyboard. Also the **"water-drop" ripple**: every key press sends a circular wave across neighboring keys, which brighten to light grey and fade, while the pressed key goes near-white. It fades in with a slight scale-up and fades out after the dismiss delay. The cursor sits just above-left of the panel's top-left corner.

Wayland gives no global cursor position, so the user chose **"Estimate the cursor from mouse movement"**. That approves widening `keeby-inputd` to also read pointer devices (motion only). Expect drift; an edge only re-syncs the estimate when the real pointer and the estimate reach that same edge (clamping alone does not); there is a Follow Speed calibration.

### 6a. Pointer patch — MERGED (was `build/wt/pointer.patch`)
- Adds `src/pointer_input.hpp`. `keeby-inputd` opens up to 4 pointer devices (mouse via REL_X/Y, or touchpad) **before** the privilege drop (lines ~326–328 of src/inputd_main.cpp — the supervisor verified the order). It forwards only summed per-frame dx/dy as `wire::MessageType::PointerMotion = 2` (dx/dy packed in `reserved`); touchpad motion counts only with exactly one finger down, in mm × 1000/25.4 units. It never forwards buttons, scroll or absolute positions.
- `InputCapture` routes motion only to the visualizer tap, and only when the visualizer is enabled with position `follow-cursor`.
- `visualizer_wire.hpp`: `Position::FollowCursor = 6` ("follow-cursor", the new default), `MessageType::Motion = 3` (dx in `code`, dy in `dismiss_ms`, both int16 bit-cast), and the config message's `reserved` byte carries follow_speed in 0.1 steps (1..40, default 10).
- D-Bus: `GetVisualizer() → (b s u d)`, `SetVisualizerFollowSpeed(d)`; settings key `visualizer_follow_speed`; `keeby ctl visualizer speed <x>`; a "Follow cursor" tray item.
- Fixed the fd-3 CLOEXEC edge case in both spawners (if the socketpair end is already fd 3, clear FD_CLOEXEC instead of dup2).
- The worker reported 17/17 Release and ASAN, TSAN clean. `pkgrel` → 5.
- docs/006 §46 ("Pointer motion for the visualizer", line 1227) records the user-approved scope change — the 6a doc item is done.

### 6b. Overlay worker — MERGED (work was in `build/wt/overlay/`, patch at `build/wt/overlay.patch`)
- New files: `src/visualizer/cursor_estimator.hpp`, `src/visualizer/viz_spring.hpp`, `src/visualizer/viz_ripple.hpp`; changes in `src/visualizer/*`, `src/visualizer_wire.hpp`, `src/ui/*` and tests. Screenshots in `build/wt/overlay-shots/` (still, moving_01–06, ripple_01–06, corrected visualizer_tab.png re-captured 2026-09-26) were kept as evidence only, not re-judged here.
- Merge note: both pieces edited `src/visualizer_wire.hpp`, `src/ui/kbus_client.*`, `tests/fake_keeby_service.cpp` and `tests/kbus_client_test.cpp`; reconciled to ONE `visualizer_wire.hpp`. Additionally merged the multi-monitor confinement fix (`viz_position.hpp:83-104` surface-local targets, regression test `tests/visualizer_test.cpp:325-368`) and a formatting fix (`viz_app.cpp:411-412` one statement per line).

### 6c. Finalized 2026-09-26 (docs/014-cursor-follow-ripple.md) — installed, security VERIFIED live
Release 17/17, ASAN/UBSAN 17/17, TSAN 14/14 (3 GLib exclusions), `--check-profiles` 19 ok, `git diff --check` clean, no trailing whitespace in untracked text files. `makepkg -f --noconfirm` built `packaging/arch/keeby-0.1.0-5-x86_64.pkg.tar.zst` (386169 bytes); `bsdtar -tvf` confirms `-rwxr-s--- root/input` helper plus keeby/keeby-settings/keeby-visualizer. The user installed 0.1.0-5 and restarted the service (`pacman -Q keeby => keeby 0.1.0-5`; service active/running MainPID 221310 `/usr/bin/keeby`; sole direct helper PID 221339 `comm keeby-inputd` PPid 221310).
**Live security re-check passed 2026-09-26** (mandatory because the helper now opens pointer devices): both processes Uid/Gid 1000 in all slots, Groups with no gid 992, CapPrm/CapEff `0000000000000000`, helper `NoNewPrivs 1`; installed helper root:input 2750 with ACL `group:keeby:--x`; user-authorized single SIGUSR1 at 17:05:39 (kill exit 0), filtered journal `keeby-inputd: post-drop open() failed as expected (EACCES) — privilege drop verified live`; service and helper stayed on the same PIDs afterward. `/proc/221339/exe` readlink Permission denied (non-dumpable; path not asserted verified).

### 6d. Backdrop transparency — MERGED + PACKAGED + INSTALLED as 0.1.0-6, backdrop user-confirmed PASS (historical; next: §6e)

User chose backdrop transparency first. Reviewed patch `build/wt/backdrop.patch`
applied 2026-09-26 (backup `build/backups/backdrop-preapply-20260926-181534.tgz`).
Pointer/overlay patches NOT re-merged. Change: new `src/visualizer/viz_backdrop_css.hpp`
(class-scoped transparent window/canvas CSS, APPLICATION priority) applied in
`viz_app.cpp` at startup; new headless red/green test
`tests/visualizer_backdrop_test.cpp` (private mkdtemp runtime, `--unixsocket`
only, no setsid, no TCP listener) + one safety fix in this step (both forked
children check `prctl(PR_SET_PDEATHSIG)` return, `_exit(127)` on failure).
Verified: Release 18/18, ASAN/UBSAN 18/18, TSAN 14/14 (4th exclusion is the new
GTK/fork test — same GLib false-positive class; covered by Release+ASAN),
`--check-profiles` 19 ok, `git diff --check` + untracked-whitespace clean,
`pkgrel` → 6, `packaging/arch/keeby-0.1.0-6-x86_64.pkg.tar.zst` (386354 bytes,
helper `-rwxr-s--- root/input`, binaries `-rwxr-xr-x`) BUILT, `check()` passed.
Full report: docs/015-visualizer-backdrop.md.
Recording visual status: FAIL on 0.1.0-5 (opaque fullscreen rectangle behind
the panel; the desired look is the floating panel alone) — the user then
installed 0.1.0-6 and live-confirmed the backdrop PASS (opaque rectangle gone,
reported live, no screenshot; read-only `pacman -Q keeby => keeby 0.1.0-6`
verified 2026-09-26). Pointer-tracking drift from 014 stayed open and became
the §6e touchpad-follow issue; the Follow Speed slider itself was confirmed
working by the user.
Historical install commands for 0.1.0-6 (already done — do NOT rerun; the live
step is now §6e's 0.1.0-7 install): `sudo pacman -U
packaging/arch/keeby-0.1.0-6-x86_64.pkg.tar.zst`, then `systemctl --user
restart keeby.service`.

### 6e. Touchpad follow stability — MERGED + PACKAGED 0.1.0-7, installed; live check FAILED (superseded by §6f)

User installed 0.1.0-6 and live-confirmed the backdrop PASS (opaque rectangle
gone, read-only `pacman -Q keeby => keeby 0.1.0-6` verified 2026-09-26), but
reported touchpad follow still wrong: travels too far/jumps, stays far from a
stationary pointer near the center; Follow Speed slider DOES work (at 1.0).
User approved a stable touchpad fix. Reviewed patch `build/wt/follow.patch`
applied 2026-09-26 (backup
`build/backups/follow-preapply-20260926-193109.tgz`, 828694 bytes, 220
entries). Change: touchpad deltas go linear (`gain = 1.0 * follow_speed`,
dt ignored) so batching/arrival timing can't change displacement; mouse keeps
the adaptive-accel curve; a per-kind mouse-clock in `viz_app.cpp`; the sender
drain emits up to two Motion messages per batch (mouse first, touchpad
second); default `accumulate()` arg stays mouse. Red proof kept in the patch:
touchpad 10x2 @1ms moved ~21.7 vs coalesced 20-count 44.0 old (now exact);
mixed-kind batch was one combined message old (now two exact per-kind lines).
No helper/privilege/wire change (`keeby-inputd` untouched). Verified: Release
18/18, ASAN/UBSAN 18/18, TSAN 14/14 (same 4 GTK/GLib exclusions),
`--check-profiles` 19 ok, `git diff --check` + patch-file whitespace clean,
`pkgrel` → 7, `packaging/arch/keeby-0.1.0-7-x86_64.pkg.tar.zst` (386552 bytes,
helper `-rwxr-s--- root/input`, binaries `-rwxr-xr-x`) BUILT, `check()` passed.
Full report: docs/016-touchpad-follow-stability.md. The live test was then
run: the user installed 0.1.0-7 and recorded the result
(`~/Videos/recording_2026-09-26_20.16.04.mp4`) — the follow bug was NOT
fixed. See the FAIL evidence in §4 and the root cause + fix in §6f and
docs/017-exact-cursor-tracking.md.

### 6f. Exact cursor tracking — MERGED + PACKAGED 0.1.0-8, verified; NOT installed (resume here)

The §6e live test failed (see §4): the panel drifts from the real cursor
instead of tracking it, confirmed at multiple timestamps in the user's own
recording (e.g. ~2.7x-3.4x more panel travel than cursor travel over the
same interval, and the panel pinned in a corner while the cursor sat
mid-screen). Root-caused on the 0.1.0-7 code to three compounding issues:
(1) the estimator only ever recentered at startup, with no way to re-anchor
to the real cursor afterward; (2) touchpad deltas were forwarded at gain 1.0
while libinput itself applies `TP_MAGIC_SLOWDOWN` 0.2968 (libinput
`src/filter-touchpad.c:37`) — i.e. the real cursor moves ~3.37x slower than
what was being forwarded; (3) the edge clamp pinned the estimate to a screen
edge while the real cursor was still mid-screen. Cross-checked against niri
26.04 (commit `8ed0da4`) source: pointer focus is re-hit-tested every loop
tick after a client commits (`src/niri.rs:1022-1098`); the user's own
`hide-when-typing` niri setting hides the pointer on every keypress, and a
contents change while hidden demotes it to `Disabled` with no focus
(`src/input/mod.rs:366-392,595-615`, `niri.rs:1070-1081`); real cursor motion
is `pos + event.delta()` (libinput's already-accelerated delta, 1:1 logical
px, `mod.rs:2405`) clamped to `loc..loc+size-1` (`mod.rs:2506-2511` — one
pixel inside the far edge, not the width/height itself); `apply_libinput_
settings` is `mod.rs:4674-4962`.

User decision (2026-09-26): shown three options — "Exact tracking
(Recommended)" (`keeby-inputd` runs libinput itself, read-only,
post-privilege-drop only), "Snap on typing start" (no helper change; a
zero-length virtual-pointer nudge plus a probe at the first keypress after
motion, with the side effect that `hide-when-typing`'s cursor briefly
reappears), and "Gain fix only". Chose exact tracking, instead of the
visualizer continuing to guess a gain curve. Reviewed patches
`build/wt/exacttrack-h.patch` (helper: a
libinput path-backend context over the already-opened, already-validated
pointer fds; see docs/006 §47 for the full security case) and
`build/wt/exacttrack-v.patch` (visualizer: `CursorEstimator::accumulate()`
now applies each Motion message's delta 1:1 with an immediate per-event
clamp to `[0,w-1]x[0,h-1]`, matching niri's own per-event, not per-batch,
clamp; a new absolute-position probe, `cursor_probe.hpp`, briefly opens the
overlay's Wayland input region to capture a real `wl_pointer.enter` and
anchor the estimate to it — gated so it never runs on a key press, since
`hide-when-typing` hides the real pointer on every keystroke, and only once
motion has been continuous for 60ms, at most once per 10 minutes once
anchored, 150ms probe timeout) were reviewed and required two changes before
merge:
- **post-drop `dlopen()` instead of linking.** An initial version linked
  libinput directly, which pulled its transitive deps (libwacom, libgudev,
  GLib/GObject, liblua5.4, libpcre2, libffi, mtdev, libudev) into the
  pre-privilege-drop ELF image as `NEEDED` entries whose constructors would
  run before `main()` while the process is still setgid-`input` — GLib does
  not support setuid/setgid use. Fixed via `src/libinput_loader.hpp`
  (`dlopen()`, post-drop only); `ldd` on the built helper now lists only
  libevdev plus libstdc++/libm/libgcc_s/libc.
- **reverted permanent overlay mapping.** A full-output OVERLAY-layer
  surface mapped at all times blocks niri's direct scanout of fullscreen
  windows (video/games). The overlay goes back to unmapped-while-hidden, so
  the absolute-position anchor is only acquired the first time the pointer
  moves while the panel is actually visible.

Known limits (full list in docs/017): before the first anchor in a session
the panel can start offset; pointer warps (games, pointer-lock) drift until
the next edge contact or the 10-minute re-probe; the libinput accel settings
applied are hardcoded to mirror the user's own niri config (`ponytail`
comment at `apply_niri_pointer_settings()` — pass settings via argv instead,
if this ever needs to serve a different config); the sender only
carries/sums a motion sample under socket backpressure, never otherwise; the
GTK/Wayland safety of the explicit bufferless `wl_surface_commit()` and of
`wl_surface` persistence across a hide/show cycle is empirical only (GTK
4.22.5 + gtk4-layer-shell 1.3.0), not source-cited.

Full report: docs/017-exact-cursor-tracking.md. Exact checks, real tree, 2026-09-26
(backup `build/backups/exacttrack-preapply-20260926-233256.tgz`, 682962 bytes, 340
entries; `git apply --check` + `apply` for exacttrack-h, exacttrack-v, then
exacttrack-docs, all clean): Release **19/19**, ASAN/UBSAN **19/19**, TSAN **15/15**
(all targets built first, same 4 GTK/GLib exclusions), `--check-profiles` **19 ok**,
`git diff --check` + whitespace **clean**; `ldd keeby-inputd` confirmed libevdev +
libstdc++/libm/libgcc_s/libc/ld-linux only (no libinput or its dependency chain);
`pkgrel` -> 8, `packaging/arch/keeby-0.1.0-8-x86_64.pkg.tar.zst` (389006 bytes, helper
`-rwxr-s--- root/input`, binaries `-rwxr-xr-x`, `depends` includes `libinput`) BUILT,
`check()` **19/19** passed.

A throwaway harness check the user ran earlier reported no `wl_pointer.enter` while
moving the touchpad; that result is **invalid**: the harness window had opacity 0, so
GTK never attached a buffer, the layer surface was never actually mapped, and niri
never hit-tests an unmapped surface — the harness never gave the probe mechanism a
chance to run. The live mechanism check moved to a driver against the real
`keeby-visualizer` (`build/wt/exacttrack-live/`), and found the probe mechanism itself
was broken on the merged code: a 15s `WAYLAND_DEBUG` run showed 78 real
`wl_pointer.enter` events delivered to the panel's surface but 0
`keeby-visualizer: cursor anchored` lines. Root-caused to GTK's own `GdkSurface`
"event" handler (connected first, during the widget's `RUN_FIRST` "realize" class
handler) always returning `TRUE` for `GDK_ENTER_NOTIFY` and stopping the signal
before this file's later, normally-connected handler ever ran — full source
citations (GTK 4.22.5 + GLib 2.88.3) in docs/017's "Probe capture bug found + fixed".
Fixed by capturing the enter with `g_signal_add_emission_hook()` instead of a plain
`g_signal_connect()` (`build/wt/enterfix.patch`, `src/visualizer/viz_app.cpp` only,
68 insertions / 29 deletions — **MERGED into the real tree 2026-09-27**). Backup
before applying: `build/backups/enterfix-preapply-20260927-004044.tgz` (705857 bytes,
verified: no `build/` or `.pkg.tar.zst` entries); `git apply --check` + `git apply`:
clean. Post-merge real tree: Release **19/19**, ASAN/UBSAN **19/19**, TSAN **15/15**
(same 4 exclusions), `--check-profiles` **19 ok**, `git diff --check` + whitespace
**clean**.

Live-verified against the fixed build over a real niri session (no synthetic input):
the driver printed `ANCHORED after 296 ms` and, on a repeat run, `329 ms`; a
`WAYLAND_DEBUG=1` run against the fixed build showed exactly 1
`set_input_region(nil)`, 1 real `wl_pointer.enter` on the panel's surface, and 1
`cursor anchored` line, matched 1:1. Reconfirmed after merge: the supervisor's own
re-run on the worker copy printed `ANCHORED after 320 ms`; a run against the merged
real tree's `build/integ-rel/keeby-visualizer` printed `keeby-visualizer: cursor
anchored` and `ANCHORED after 447 ms`, with no leftover visualizer processes
afterward (only the installed `/usr/bin/keeby-visualizer` remained).

Repackaged as `0.1.0-8` (never installed, so `pkgrel` stays 8):
`keeby-0.1.0-8-x86_64.pkg.tar.zst`, 389199 bytes, sha256
`31c0e2d6e5da14f363bbcf89ec5a7f10243c8e9c5b3a1962bcb79ba0bc168508` — replaces the
earlier 389006-byte build, which lacked the fix. `bsdtar` confirms
`-rwxr-s--- root/input usr/lib/keeby/keeby-inputd`, `-rwxr-xr-x root/root` on `keeby`,
`keeby-settings`, `keeby-visualizer`, `keeby-fetch-packs`; `.PKGINFO` `depend =
libinput`; the packaged `keeby-visualizer` binary contains
`g_signal_add_emission_hook` and the `"cursor anchored"` string.

Honest open item: the first `makepkg` attempt after the merge failed `check()` with 1
of 19 tests (log not retained, test unknown); the second attempt passed 19/19. It did
not reproduce in 20 serial runs of the exact `check()` command (`ctest
--output-on-failure` in `packaging/arch/src/build`), nor in 15 parallel (`-j8`)
repeats in each of `build/integ-rel` and `packaging/arch/src/build`. Treat as an
unexplained, unreproduced intermittent failure to watch for.

The user's own install + live-check below is still **PENDING**, now against the
merged, repackaged build.

**User install + live-check commands:**
1. Supervisor PASS (see above). Optional for the user: `python3
   build/wt/exacttrack-live/viz_live_probe_test.py --binary
   build/integ-rel/keeby-visualizer --seconds 6` — the panel appears, then prints
   ANCHORED.
2. `sudo pacman -U packaging/arch/keeby-0.1.0-8-x86_64.pkg.tar.zst`
3. `systemctl --user restart keeby.service`, then `pacman -Q keeby` (expect
   `keeby 0.1.0-8`).
4. `journalctl --user -u keeby.service -b --no-pager | grep -E
   'keeby-inputd: libinput|keeby-visualizer: cursor anchored'`
5. Visual: type, then move the pointer while the panel is visible — its
   top-left should sit just below-right of the cursor and follow it 1:1.
6. Mandatory security re-check (helper changed, see docs/006): Uid/Gid/
   Groups/Cap*/NoNewPrivs of both processes, plus a user-authorized single
   SIGUSR1 to the helper expecting the journal line `post-drop open()
   failed as expected (EACCES)`.

Do NOT re-merge `build/wt/exacttrack-h.patch`, `build/wt/exacttrack-v.patch`,
`build/wt/exacttrack-docs.patch`, or `build/wt/enterfix.patch` (all already applied).

### 6g. Keeby sounds, volume, and mouse buttons — 0.1.0-12 BUILT, NOT INSTALLED; LIVE ACCEPTANCE OPEN

The exact-source catalog and import are in [report 002](docs/research/002-keeby-native-sound-catalog.md), volume and input work are in [report 018](docs/018-keeby-sounds-and-volume.md), and playback-path research is in [report 003](docs/research/003-keeby-playback-fidelity.md). The bounded imported-playback correction is merged and checked; see [report 020](docs/020-imported-playback-fidelity.md). Package 11 does not contain that correction, and package 12 is not evidence of an audible match.

Package `packaging/arch/keeby-0.1.0-11-x86_64.pkg.tar.zst` (SHA-256 `ce85e103154a265fb1c415963cb345598c8ca7222242a93abde7753292966c1b`) remains installed; `pacman -Q keeby` reports `keeby 0.1.0-11` and `keeby.service` is active. Package 11 includes the settings scroll fix from [report 019](docs/019-settings-window-scroll.md). Package `packaging/arch/keeby-0.1.0-12-x86_64.pkg.tar.zst` is built, passed its 20/20 package check, and its binary accepts 45/45 profiles; SHA-256 `7c68cbb1468c872eb668fb17c6a873ae78f3e57413b566c8a500df6c8610238f`. Package 12 is not installed. Any future code package must use `pkgrel=13`.

Still-open user checks on the installed package:

1. Open or reopen `keeby-settings`. Confirm the General page fits, the Sound page scrolls to every profile, and the tab controls stay visible. Headless geometry and reachability passed; desktop visual acceptance has not been reported.
2. Confirm real pointer-button and Fn events are captured by the user's devices. This is a live hardware check; no synthetic input was sent during development.
3. Perform the fresh helper-security review required by docs/006 §48. Read `/proc/<pid>/status` fields `Uid`, `Gid`, `Groups`, `CapInh`, `CapPrm`, `CapEff`, `CapBnd`, `CapAmb`, and `NoNewPrivs` for both `keeby` and `keeby-inputd`. Compare with report 018 and docs/006. Both processes should run as the user, have no `input` group membership, and have zero effective/permitted capabilities; the helper should have `NoNewPrivs: 1`.
4. The negative-open probe is user-run only: send exactly one `SIGUSR1` to the current helper and filter the service journal for `keeby-inputd: post-drop open() failed as expected (EACCES) — privilege drop verified live`. The supervisor did not send this signal. Use the commands in report 018.
5. Install the checked package and confirm the profile catalog:
   ```sh
   sudo pacman -U /home/yahiro/Documents/PROJECTS/KEEBY_LINUX/packaging/arch/keeby-0.1.0-12-x86_64.pkg.tar.zst
   systemctl --user restart keeby.service
   pacman -Q keeby  # expect keeby 0.1.0-12
   keeby --check-profiles
   ```
6. For comparison with the website's standard key path, record the current settings, then set neutral controls with `keeby ctl tone 0 0` and `keeby ctl width 0`. Listen to ordinary and special key presses/releases, mouse buttons, Mouse Snappy, and rapid typing at 0%, 50%, and 100% volume. Restore preferred tone/width afterward if needed. Record the installed version and observations. Do not call this a PASS until the user reports the result. This comparison does not establish Mac-app parity.

The desktop visual acceptance of exact cursor tracking from docs/017 remains pending. No live listening, fresh security result, or real hardware capture is recorded here.

## 7. Backlog / known items

- docs/006: append the live Step 2.5 evidence from 2026-09-25, which passed (§4). Also fix section 43's line "unexecutable by anyone but root", since `input` members could run it too.
- Polish nits: each tray action rebuilds the menu twice; `list_packs()` rescans on every refresh; the tray title shows the pack id instead of its name; `main.cpp` has an unused `input_capture.hpp` include; the retired tone-coefficient list only shrinks while audio is running.
- License is not chosen (`license=('custom')` placeholder) — ask the user.
- Not built yet: Keeby's per-key configurator (switch, tone and volume per key) and visualizer themes (Keeby has 10). Mouse-click capture and local sound packs are implemented; live hardware capture, fresh security review, and listening remain pending in §6g.
- `build/` holds many backups and worker copies (`build/wt/*`, `build/backups/*`). They're safe to prune once everything is merged, but ask first.

## 8. Memory

Persistent notes for Claude sessions are in `~/.claude/projects/-home-yahiro-Documents-PROJECTS-KEEBY-LINUX/memory/`: the orchestrator-only role, the project goal, and the step discipline.
