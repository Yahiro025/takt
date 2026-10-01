# AGENTS.md — KEEBY

Read this first. It is the entry point for any coding agent (OpenCode, Codex, Claude) working in this repo.

## 1. What this is

KEEBY is a Linux-native clone of **Keeby** (https://getkeeby.com/ — mechanical-keyboard typing sounds) built for the user's own machine: **CachyOS (Arch) + Niri (Wayland) + noctalia (quickshell) bar**.

Stack: C++23, CMake + Ninja, GCC 16, PipeWire, libevdev, sdbus-c++ 2.3.1, GTK4 (UI processes only), gtk4-layer-shell, nlohmann-json, libsamplerate, libsndfile.

### Repo map

```
src/
  main.cpp                keeby entrypoint: CLI, profile switching, wires the engine
  inputd_main.cpp          keeby-inputd: privileged helper, only thing that opens /dev/input
  input_capture.cpp/.hpp   spawns/talks to keeby-inputd over a socketpair (wire protocol)
  input_wire.hpp           wire structs shared with keeby-inputd (KeyEvent, PointerMotion)
  key_event.hpp            KeyEvent type
  key_layout.hpp           keycode -> physical key layout table (pan, visualizer)
  event_transport.hpp      lock-free SPSC queue: InputCapture thread -> PipeWire RT callback
  audio_boundary.cpp/.hpp  PipeWire stream setup + the RT audio callback boundary
  sample_mixer.cpp/.hpp    in-memory WAV mixing (press/release samples, pan, tone filter)
  sound_pack.cpp/.hpp      Mechvibes + native sound-pack loading (config.json v1/v2)
  engine_controller.cpp/.hpp  composes InputCapture+AudioBoundary+SampleMixer, profile control
  control_service.cpp/.hpp    D-Bus org.keeby.Control1 (settings + `keeby ctl` API)
  dbus_menu.cpp/.hpp          StatusNotifierItem + DBusMenu tray implementation
  tray_service.cpp/.hpp       tray menu build/dispatch logic
  settings.cpp/.hpp           ~/.config/keeby/settings.ini read/write
  visualizer_service.cpp/.hpp spawns keeby-visualizer, sends it VizMessages
  visualizer_wire.hpp         wire protocol for the visualizer overlay process
  paths.hpp                   asset-dir / helper-path resolution (dev vs installed)
  ui/            keeby-settings GTK4 window (app, tabs, sound_controls, visualizer_tab,
                 systemctl.cpp, kbus_client.cpp = GDBus client of Control1)
  visualizer/    keeby-visualizer GTK4 + layer-shell overlay (viz_app, viz_layout,
                 viz_dismiss, viz_position, viz_backdrop_css)
tests/           one *_test.cpp per src module, plus fake_inputd.cpp, fake_keeby_service.cpp,
                 fake_keeby_visualizer.cpp (test doubles so no root/hardware/session-bus needed),
                 visualizer_backdrop_test.cpp (headless broadway red/green for the backdrop CSS)
assets/          built-in click/release .wav samples + icons/, generate_variants.py
packaging/       keeby.service, keeby.desktop, keeby.conf (sysusers), packaging/arch/PKGBUILD
scripts/         fetch-mechvibes-packs.sh, install-/uninstall-keeby-inputd.sh
docs/            numbered step reports (001-013, see below) plus:
  PRD.md, BRD.md, SDD.md, DSD.md, idea.md   — product/business/software/design docs (parallel work)
HANDOFF.md       current status + exactly where to resume — READ THIS NEXT
```

Doc index (`docs/`): 001 core feasibility spec · 002 input->audio boundary (Step 2.1) ·
003 sample mixer (2.2) · 004 sound fidelity/variant selection (2.3) · 005 desktop shell (2.4) ·
006 security/permissions model (2.5, long — the privilege-drop design) · 007 sound engine v2,
all keys + pan (2.6) · 008 sound packs / Mechvibes import (2.7) · 009 tray+ctl+settings wiring
(2.8) · 010 pacman packaging (2.9) · 011 tone filter + Control1 API · 012 keeby-settings window ·
013-visualizer.md engine half / 013-visualizer-app.md overlay half of the on-screen keyboard ·
014-cursor-follow-ripple.md (0.1.0-5) · 015-visualizer-backdrop.md (0.1.0-6) ·
016-touchpad-follow-stability.md (0.1.0-7) · 017-exact-cursor-tracking.md (0.1.0-8).
`docs/research/001-linux-input-audio-feasibility.md` is background research, not a step report.

## 2. Build, test, verify

Dev build dirs live under `build/` (gitignored):

```
cmake --build build/integ-rel && ctest --test-dir build/integ-rel --output-on-failure

cmake -S . -B build/integ-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/integ-asan

cmake -S . -B build/integ-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/integ-tsan   # build ALL targets — tests launch fake_inputd_test_helper
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/integ-tsan -E 'dbus_menu|control_service|kbus_client|visualizer_backdrop'
```

The four GTK/GLib-based tests (`dbus_menu`, `control_service`, `kbus_client`,
`visualizer_backdrop`) are excluded from TSAN only — GLib's internal thread pool
produces known false positives there (the backdrop test additionally forks a
full-`gtk_init` worker plus an external daemon). They still run under Release
and ASAN.

Whitespace / diff hygiene (`git diff --check` only covers already-tracked content):
```
git diff --check
git ls-files --others --exclude-standard -z | xargs -0 -r grep -nP '[ \t]+$'
```

Sound-pack sanity check: `./build/integ-rel/keeby --check-profiles` (loads every profile in
`~/.local/share/keeby/packs`, must succeed for all of them).

Packaging (builds from the **working tree**, not git HEAD — see the PKGBUILD comment):
```
cd packaging/arch && makepkg -f --noconfirm
```
Bump `pkgrel` in `packaging/arch/PKGBUILD` before every package build that ships new code.
`check()` runs the full ctest suite under fakeroot with no real hardware/session bus required.

## 3. Hard rules — non-negotiable

- Never `git add`/`commit`/`push` unless the user explicitly asks. Offer, don't act.
- No PASS without live evidence. Unit tests passing is not a PASS for anything that needs sudo,
  install, reboot, listening to audio, or looking at the screen — stop and hand the user the
  exact commands to run, then wait for their output.
- The PipeWire RT audio path (`audio_boundary.cpp`'s callback, everything it calls) must do
  zero allocation, zero locks, zero syscalls, zero IPC, zero exceptions. Verify with
  Release + ASAN/UBSAN + TSAN before calling it done.
- Keystrokes are confidential. Only `keeby-inputd` may touch `/dev/input`. Never send raw key
  data over D-Bus, named sockets, or into logs.
- Any privilege-scope change (e.g. widening what `keeby-inputd` opens) needs explicit user
  approval first — see docs/006.
- Ask before killing any process you didn't start yourself.
- No Qt, anywhere. GTK4 is allowed only in `keeby-settings` and `keeby-visualizer` — never in
  the engine (`keeby`, `keeby-inputd`).
- Never capture the whole screen. Crop screenshots to the window/overlay (e.g. `grim -g`,
  focusing the window first with `niri msg action focus-window --id <id>`).
- Minimal comments: only a non-obvious "why", never a restatement of the code.
- No new dependencies without justification — check `CMakeLists.txt`'s existing
  `pkg_check_modules`/`find_package` list first.
- Errors are returned as `std::expected`, not exceptions or raw bools, on any new API surface.

## 4. Working style

Orchestrator/worker pattern: the top-level agent plans and verifies only; it delegates actual
edits to worker subagents. Each worker gets an isolated copy under `build/wt/<name>/` (rsync
of the tree + a throwaway `git init`), works there, and outputs `build/wt/<name>.patch`
(`git diff --cached --binary HEAD`). The supervisor backs up the real tree
(`build/backups/*.tgz`), runs `git apply --check` then `git apply`, rebuilds, and reruns the
checks in §2 before treating anything as merged.

Step discipline: work is broken into numbered steps; each step gets a report at
`docs/NNN-step-name.md` (see the existing 001–013 files for the format: goal, what was built,
what was verified and how, what's still open).

## 5. Where to start right now

Read `HANDOFF.md` in full, then go straight to **§6f** of it: pointer-input, overlay/ripple,
the multi-monitor confinement fix, backdrop transparency, and touchpad-follow
linearization are merged; `keeby 0.1.0-7` was installed and live-tested, but the user's
own recording (`~/Videos/recording_2026-09-26_20.16.04.mp4`) showed the follow estimate
still drifting from the real cursor by ~2.7-3.4x and pinning to a corner — root-caused
to the touchpad gain not matching libinput's own slowdown constant, and to the estimator
having no absolute anchor after startup. The user approved **"exact tracking"**:
`keeby-inputd` now runs libinput itself (`dlopen()`'d, post-privilege-drop only, never
linked) and the visualizer applies its deltas 1:1 with a shown-only absolute-position
probe (on continuous motion, when unanchored or the anchor is >10 min old)
— see docs/017-exact-cursor-tracking.md. Do NOT re-merge `build/wt/pointer.patch`,
`build/wt/overlay.patch`, `build/wt/backdrop.patch`, `build/wt/follow.patch`,
`build/wt/exacttrack-h.patch`, `build/wt/exacttrack-v.patch`,
`build/wt/exacttrack-docs.patch`, `build/wt/enterfix.patch`, or
`build/wt/finaldocs.patch` (all already applied). The live security re-check in
HANDOFF §6c is VERIFIED on 0.1.0-5 (kept as historical baseline; §6f needs a fresh one,
since the helper now also loads libinput post-drop). `enterfix.patch` (the
`GdkSurface::"event"` emission-hook fix for the cursor-anchor probe) is merged, and the
live anchor test has passed against the merged build (`keeby-visualizer: cursor
anchored`, `ANCHORED after 447 ms`, no leftover visualizer processes) — see HANDOFF
§6f. `keeby 0.1.0-8` (exact cursor tracking, repackaged with the fix) is BUILT and
VERIFIED, not installed; the next step is the user's live install, the mandatory
security re-check, and the follow-accuracy visual check, all in HANDOFF §6f.

## 6. Environment quirks

- A "Fact-Forcing Gate" hook may ask you to state facts before your first Bash/Edit/Write call
  in a session. Answer it, then retry the same call — it is not an error.
- A Read-tool hook caps file reads at roughly 350 lines. For anything longer (several docs/
  files, CMakeLists.txt is fine at 333 lines but check first), use `offset`/`limit` and read in
  chunks. For screenshots, downscale large PNGs first with `magick <in>.png -resize 50% <out>.png`
  before reading them.
