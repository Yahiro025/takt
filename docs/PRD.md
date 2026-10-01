# KEEBY — Product Requirements Document

Status snapshot date: 2026-09-26. Source of truth for status claims: `HANDOFF.md` §4, §6, §7 and `docs/001-core-feasibility-spec.md`.

## 1. Problem and goals

Keeby (getkeeby.com) is a $4.99 one-time macOS/Windows app that plays mechanical-switch sounds on keystrokes, with per-key pitch variation, stereo panning, a Thock/Clack tone control, a floating on-screen keyboard visualizer, a tray menu, and a settings window. It does not run on Linux.

The user runs CachyOS (Arch) with the Niri Wayland compositor and wants the same experience — sound, feel, and look — as a personal daily-driver tool on their own machine, built and iterated on by AI agents.

Goals:
1. Global, passive, low-latency keystroke-to-sound with the same character as Keeby (per-key press/release sounds, pitch variety, stereo pan, tone shaping).
2. A look-alike floating keyboard visualizer (cursor-follow, 3D tilt, shadow, water-drop ripple).
3. A tray menu and settings window comparable in structure to Keeby's.
4. None of this may compromise keystroke privacy or Wayland/PipeWire real-time guarantees.

## 2. Target user and persona

Single persona: the project owner, a power user running CachyOS + Niri + noctalia (quickshell bar) on one known machine (Dell Precision 7670). They are simultaneously the sole end user, the product owner, and the person who approves scope changes (e.g. widening what `keeby-inputd` reads). There is no multi-user, multi-machine, or general-public audience.

## 3. Scope

In scope: a background engine service, a privileged input helper, PipeWire-based audio, Mechvibes-compatible sound packs, a tray menu, a GTK4 settings window, a floating GTK4 layer-shell visualizer, pacman packaging and a systemd user service — all for one Linux desktop.

Out of scope: see §7 Non-goals.

## 4. Functional requirements

Status legend: **Done & live-verified** (confirmed working on the user's real machine, not just tests) · **Done (tests only)** (implemented, unit/integration tests pass, not yet confirmed live by the user) · **In progress** (partially built and/or merged) · **Not started**.

| ID | Requirement | Status |
|---|---|---|
| FR-01 | Passive global keyboard capture (press/release/repeat), never grabs the device, never injects or blocks input | Done & live-verified |
| FR-02 | Low-latency RT-safe playback of a preloaded sample on keypress via PipeWire | Done & live-verified |
| FR-03 | Per-key stereo panning by physical key position with a global stereo-width control | Done (tests only) |
| FR-04 | Pitch/gain variant randomization so repeated keystrokes don't sound identical | Done (tests only) |
| FR-05 | Thock/Clack + brightness tone control (2D tone pad DSP + D-Bus API) | Done (tests only) |
| FR-06 | Mechvibes sound-pack import (v1/v2 `config.json`, slicing, resampling, dedup) | Done & live-verified |
| FR-07 | Runtime switch-profile selection (equivalent to Keeby's 21 recorded profiles) | Done & live-verified |
| FR-08 | Mouse-click sounds | Not started |
| FR-09 | Per-key configurator (switch/tone/volume per individual key) | Not started |
| FR-10 | System tray menu (sound on/off, volume, stereo width, switch profile, quit; left/right/middle-click, scroll-to-adjust-volume) | Done & live-verified |
| FR-11 | `keeby ctl` CLI control client | Done (tests only) |
| FR-12 | GTK4 settings window (General / Sound tabs, tone pad, brand-grouped switch list) | Done (tests only) |
| FR-13 | Settings persistence across restarts (`~/.config/keeby/settings.ini`) | Done (tests only) |
| FR-14 | Floating pop-up keyboard visualizer: static rendering, position options, auto-dismiss timer | Done (tests only) |
| FR-15 | Visualizer themes (Keeby ships 10) | Not started |
| FR-16 | Cursor-follow with springy lag, via pointer-motion estimation (no absolute cursor on Wayland) | In progress |
| FR-17 | 3D tilt of the visualizer panel while moving | In progress |
| FR-18 | "Water-drop" ripple on keypress (ring wave over neighboring keys, pressed key near-white, fade in/out) | In progress |
| FR-19 | Privilege-separated input helper (`keeby-inputd`): opens `/dev/input`, drops privilege immediately after, zero capabilities, `NoNewPrivs` | Done & live-verified |
| FR-20 | Pacman package + systemd user service with autostart | Done & live-verified |
| FR-21 | Mechvibes pack fetch helper (`keeby-fetch-packs`), packs stored under `~/.local/share/keeby/packs`, never bundled in the package | Done & live-verified |

Notes on FR-16–18: the pointer-motion patch (`build/wt/pointer.patch`) is built, worker-tested (17/17 Release, ASAN/TSAN clean) but **not merged**. The overlay half (spring follow, tilt, ripple: `build/wt/overlay/`) was interrupted mid-task; its screenshots exist but are **unreviewed** by the supervisor. Neither has been seen running live by the user.

## 5. Non-functional requirements

| ID | Requirement |
|---|---|
| NFR-01 | Latency: keystroke to audible onset must feel immediate. Feasibility spike measured ~5–8 ms software-side (evdev→RT callback 1–2.8 ms, ALSA ring-buffer delay ~3.8–5.25 ms) at a 128/48000 PipeWire quantum, live-verified with zero xruns. |
| NFR-02 | RT safety: the PipeWire `process()` callback must stay free of allocation, locks, syscalls (beyond what PipeWire itself does), disk I/O, and unbounded loops. Every merge is checked under Release, ASAN/UBSAN, and TSAN. |
| NFR-03 | Keystroke privacy: only `keeby-inputd` opens `/dev/input`; it drops privilege immediately after opening the device (setgid `input`, zero capabilities, `NoNewPrivs=1`). Raw keystrokes never cross D-Bus or any named/pathname socket — only private, unnamed `socketpair()` channels between parent and child. No key sequence is ever persisted or logged. |
| NFR-04 | Footprint: one `systemd --user` service plus two optional GUI child processes (`keeby-settings`, `keeby-visualizer`). No Qt anywhere. GTK4 is confined to the two UI processes; the engine and `keeby-inputd` link no GUI toolkit. |
| NFR-05 | Wayland/Niri constraints: no protocol exposes raw global keyboard input or absolute cursor position to arbitrary clients (deliberate Wayland security design). Global key capture works only because it stays below the compositor, at the evdev/kernel layer. Cursor position for the visualizer is therefore an **estimate** from relative pointer motion, not a true reading — expect drift, mitigated by screen-edge resync and a user-tunable follow speed. |
| NFR-06 | Any widening of `keeby-inputd`'s device access (e.g. adding pointer devices) requires the user's explicit, out-of-band approval before merge — not implied by a feature request alone. |
| NFR-07 | No feature is marked done on user-facing behavior without live evidence (the user running it, listening, or looking at the screen); passing unit tests alone downgrades a feature to "Done (tests only)". |
| NFR-08 | Visualizer IPC isolation: `keeby-visualizer` receives only two message shapes (key event, config update) over a private `SOCK_SEQPACKET` socketpair — no settings blob, no filesystem paths, no D-Bus connection of its own. |

## 6. Platform constraints

- **No global cursor position on Wayland.** There is no protocol-level way to ask "where is the mouse on screen" from an arbitrary client. The chosen workaround is relative pointer-motion accumulation inside `keeby-inputd` (mouse `REL_X/REL_Y`, or single-finger touchpad deltas), forwarded only as summed per-frame deltas, never absolute position, never buttons/scroll. This is an **estimate**, calibrated by a Follow Speed setting, not a precise cursor reader.
- **No Qt.** GTK4 only, and only in the two standalone UI processes (`keeby-settings`, `keeby-visualizer`); the engine, `keeby-inputd`, and all RT code are GUI-toolkit-free.
- **Niri/Wayland has no native systray**; the tray is implemented directly as a StatusNotifierItem D-Bus service (sdbus-c++), independent of any specific bar, though verified against the user's noctalia/quickshell bar.
- Target is one specific machine/session (CachyOS, Niri, PipeWire, systemd `--user`, this hardware) — no portability testing across distros, compositors, or other init systems is in scope.

## 7. Non-goals

- Copying Keeby's actual sound recordings, logo, or artwork. KEEBY replicates *behavior and look-and-feel*, not literal Keeby assets.
- Bundling any third-party (Mechvibes) sound pack inside the KEEBY package. Packs are always user-downloaded via `keeby-fetch-packs` at runtime.
- Windows/macOS builds, X11-only fallback, or any non-Wayland/non-Niri target.
- Multi-user or system-wide deployment; commercial distribution, monetization, or public release packaging (see BRD).
- An updater mechanism, licensing/DRM, or account system (explicitly excluded even in the original feasibility spec).
- Visualizer themes, per-key configurator, and mouse-click sounds are *acknowledged backlog*, not silently dropped scope — see FR-08, FR-09, FR-15 (Not started).

## 8. Acceptance criteria per major feature

- **Core sound engine (FR-01–07):** typing in any focused application is unaffected (no dropped/duplicated keystrokes); every key press and release triggers a sound; pitch/gain vary across repeats; pan follows key position; profile switching is audible and doesn't glitch; zero PipeWire xruns during a sustained typing/rapid-typing session.
- **Tray + CLI (FR-10–11):** left-click toggles sound; right-click menu matches the documented item list; `keeby ctl` commands take effect immediately and are reflected in the tray/settings window.
- **Settings window (FR-12–13):** window opens even when `keeby` isn't running (shows a "not running" card); every control matches the live engine state; settings survive a `systemctl --user restart keeby.service`. **Not yet accepted** — needs a live user pass per NFR-07.
- **Visualizer static behavior (FR-14):** panel appears near a keypress, shows the right key(s) highlighted, auto-dismisses on the configured timer, in the configured screen position. **Not yet accepted.**
- **Visualizer motion (FR-16–18):** cursor-follow tracks real mouse movement with visible spring lag and re-syncs at screen edges; 3D tilt responds to movement direction/speed; ripple radius/timing/colors match the reference recording (ring over 2–3 neighboring keys, ~350–450 ms, pressed key near `#F2F2F7`, fade-in scale 0.96→1.0). **Blocked on merging `build/wt/pointer.patch` and `build/wt/overlay/`, then a live user check — not accepted yet.**
- **Packaging/security (FR-19–21):** `makepkg` produces an installable package; post-install, both `keeby` and `keeby-inputd` show no `input`-group membership and zero capabilities; `keeby-inputd` has `NoNewPrivs=1`; `SIGUSR1` to the helper logs the expected post-drop `EACCES` self-test line.

## 9. Open questions

1. **License**: `packaging/arch/PKGBUILD` still has `license=('custom')` as a placeholder — the user hasn't chosen one.
2. **Commit policy**: everything since commit `0e0d8bd` (69 paths) is uncommitted by design (user rule: never commit unless asked). PRD/BRD authorship does not change this.
3. **Visualizer themes**: Keeby ships 10; scope, style, and priority relative to finishing cursor-follow/tilt/ripple is undecided.
4. **Per-key configurator and mouse-click sounds**: acknowledged as backlog (HANDOFF §7) but not scheduled — priority relative to visualizer completion is undecided.
5. **Merge order for the two in-flight patches**: both touch `src/visualizer_wire.hpp`, `src/ui/kbus_client.*`, and two test files; HANDOFF specifies applying the pointer patch first and reconciling the overlay patch against it, but this hasn't happened yet.
