# KEEBY — Idea & Research Summary

Status: 2026-09-26. One-stop crystallized summary of what was researched and decided while building a Linux-native clone of Keeby.

## The idea

KEEBY is a personal, Linux-native clone of Keeby (getkeeby.com) built for one user's own CachyOS (Arch) machine running the Niri Wayland compositor with a noctalia/quickshell bar. It passively detects physical keyboard presses/releases at the kernel evdev level and triggers low-latency mechanical-switch sounds through PipeWire, replicating Keeby's sound, feel and look — per-key pitch variation, stereo panning, a Thock/Clack/Bright tone pad, switchable sound profiles, a system tray, a settings window, and a floating cursor-following on-screen-keyboard visualizer with 3D tilt and a "water-drop" ripple — without copying any of Keeby's actual assets, and without compromising keystroke privacy or PipeWire's real-time guarantees (see `docs/PRD.md` §1, `HANDOFF.md` §1).

## What Keeby is (researched)

| Fact | Detail | Source |
|---|---|---|
| Product | $4.99 one-time macOS/Windows app; mechanical-keyboard typing sounds, does not run on Linux | `docs/PRD.md` §1 |
| Profiles | 21 recorded switch profiles | `docs/PRD.md` FR-07 |
| Sound features | Per-key pitch variation, stereo panning by key position, Thock/Clack 2D tone control | `docs/PRD.md` §1, `docs/011` §1 |
| Visualizer | Floating on-screen keyboard: cursor-follow with springy lag, 3D tilt while moving, big soft shadow, water-drop ripple on keypress, ~10 shipped themes | `HANDOFF.md` §6, §7; `docs/PRD.md` FR-15 |
| Surfaces | System tray menu, settings window (General/Sound tabs equivalent), per-key configurator | `docs/012` §1; `docs/PRD.md` FR-09 (not built here) |
| Not researched/confirmed | Exact spatial-audio DSP internals, per-key configurator UI, and the 10 visualizer themes' individual designs were inferred from the user's screen recording and product description, not from Keeby's own source | `docs/013-visualizer-app.md`; `HANDOFF.md` §1 |

## Linux/Wayland/Niri realities discovered

- **No global cursor position on Wayland.** No protocol lets an arbitrary client ask "where is the mouse." Chosen workaround: estimate it from relative pointer-motion deltas (mouse `REL_X/Y` or single-finger touchpad), accumulated with an adaptive gain curve and clamped/re-synced at screen edges. See `docs/PRD.md` NFR-05, §6; `HANDOFF.md` §6; `build/wt/overlay/src/visualizer/cursor_estimator.hpp` (pending, not merged).
- **No global raw keyboard protocol.** Wayland deliberately has no analog of X11's global key grab; the portal `GlobalShortcuts` API only delivers pre-registered combinations, not a full down/up/repeat stream. The only way to get requirement 1-3 (every key, unregistered, with repeat) is to stay *below* the compositor, at the evdev/kernel layer. See `docs/research/001` §3.
- **logind `uaccess` doesn't cover keyboards.** systemd's seat-based dynamic ACL (`70-uaccess.rules`) does not extend read access to keyboard event nodes the way it does for some other seat devices — confirmed, not assumed, on the real machine. See `docs/006` §5 option A ("does not apply, verified").
- **`input`-group membership is a standing keylogger-equivalent grant.** Every process the user's account runs — not just KEEBY — can read every `/dev/input/event*` node for as long as membership exists. This is the condition that forced the privileged-helper design. See `docs/006` §6, `docs/research/001` §7.
- **Layer-shell + SNI/DBusMenu, not a native systray.** Niri/Wayland has no native systray; the tray is a StatusNotifierItem + `com.canonical.dbusmenu` D-Bus service (sdbus-c++), verified against the user's noctalia/quickshell bar specifically, not a generic implementation. The visualizer overlay uses `gtk4-layer-shell` (`GTK_LAYER_SHELL_KEYBOARD_MODE_NONE`, no exclusive zone, empty input region for click-through). See `docs/PRD.md` §6; `docs/013-visualizer-app.md`.
- **Lingering breaks group changes.** With `loginctl … Linger=yes`, the systemd `--user` manager (and everything it starts) survives logout, so a mere relogin after `usermod -aG keeby` does not refresh its groups — only a reboot (or restarting `user@<uid>.service`, which kills the whole graphical session) does. Verified live; this is now the standard remedy text everywhere the docs mention group changes. See `docs/010` §12.
- **Second concurrent seat client works.** A second, independent unprivileged process can hold its own read handle on an evdev node the compositor's libinput instance already has open, with no conflict — the same way `evtest` coexists with a running compositor. See `docs/research/001` §3, §12.

## Sound-pack research

- **Mechvibes is the format, not a KEEBY-specific schema.** `config.json` v1 (`key_define_type: single|multi`, no per-key release, no fallback for undefined keys) and v2 (adds a pack-wide default `sound`/`soundup` and per-key release via a `"<code>-up"` key). `version >= 3` is rejected outright as an evolving draft KEEBY doesn't implement. See `docs/008` §"Supported format".
- **iohook → evdev key mapping.** Mechvibes packs use the iohook/libuiohook code space: unprefixed codes equal evdev `KEY_*` directly; a fixed table of ~20 "extended" iohook codes (numpad Enter, right modifiers, nav cluster, arrows, etc.) maps onto their evdev equivalents; anything else (or anything landing at evdev code ≥256) is unknown and silently skipped, with one summary diagnostic line per pack rather than one per key. See `docs/008` §"Key mapping".
- **Licensing: packs are mostly unlicensed, so user-supplied only.** "Most Mechvibes community packs carry no explicit license" — KEEBY never bundles, commits, or auto-fetches any third-party pack; `keeby-fetch-packs` is opt-in, asks for confirmation, and is documented as a personal-use convenience only. See `docs/008` §"Limitations", `docs/PRD.md` §7 (non-goals).
- **Real-world compatibility gaps found and fixed.** Live-testing against the user's 18 downloaded packs found Mechvibes accepts `null` define values (meaning "no sound"/"use pack default") and tolerates a per-key file that's simply missing upstream — both are now downgraded from hard errors to warnings rather than a full pack-load failure. See `docs/008` §"Real-pack compatibility fixes".

## Key decisions and their reasons

| Decision | Reason | Source |
|---|---|---|
| Setgid `input` helper (`keeby-inputd`) + a **group ACL**, not a per-user ACL | A privileged helper is the only option that lets the interactive account leave `input` entirely; a per-user `setfacl` (the first fix) couldn't be expressed in a PKGBUILD scriptlet without hardcoding a username, so the final design uses a dedicated `keeby` system group instead — anyone added to it can run the helper, matching the same execute-permission boundary, but packageable | `docs/006` §7 (decision checkpoint), §41-43 (per-user ACL, superseded); `docs/010` §7 (why a group) |
| Execute permission, not the setgid bit, is the real security boundary | `keeby-inputd` doesn't authenticate its peer on fd 3 — anyone who can *execute* the helper can reproduce the handshake and receive keystrokes; the setgid bit only controls where the privilege to *open the device* comes from | `docs/006` §40 |
| Mechvibes as KEEBY's native pack format | No second schema needed; the community pack ecosystem is Mechvibes-shaped already, so importing it directly maximizes compatible content with no format design of KEEBY's own | `docs/008` §"Supported format" |
| GTK4 without libadwaita | libadwaita's GNOME look fights Keeby's own dark/pill/card look; plain GTK4 C API lets nothing else impose styling | `docs/012` §1, §6 |
| Cursor estimation via relative pointer-motion accumulation | Wayland gives no absolute cursor position to any client; this is the only workaround, accepted with known drift, edge re-sync, and a user-tunable Follow Speed | `docs/PRD.md` NFR-05; `HANDOFF.md` §6 |
| Widening `keeby-inputd` to read pointer devices requires explicit user approval first | Any privilege-scope change is gated by the user's own out-of-band sign-off, never implied by a feature request | `docs/PRD.md` NFR-06; `HANDOFF.md` §6a |
| Privately, unnamed sockets only for both the input helper and the visualizer | Keystrokes must never cross D-Bus or any named/pathname socket another process could connect to or eavesdrop on | `docs/013-visualizer.md` §1; `docs/PRD.md` NFR-03, NFR-08 |

## Differentiating vs. out of scope

**Differentiating (in scope, built or being built):** passive global evdev capture with a privilege-separated helper; RT-safe PipeWire playback; Mechvibes-compatible pack import with real-world compatibility fixes; a 2D tone pad (Thock/Clack/Bright) implemented as three RBJ shelving biquads; a StatusNotifierItem tray matching Keeby's Control/Configure/App grouping; a GTK4 settings window; a cursor-following, tilting, rippling visualizer overlay (in progress). See `docs/PRD.md` §3-4.

**Out of scope (explicit non-goals):** copying Keeby's actual sound recordings, logo, or artwork; bundling any third-party sound pack in the package; Windows/macOS builds or any non-Wayland/non-Niri target; multi-user or system-wide deployment, monetization, or public release packaging; an updater, licensing/DRM, or account system. See `docs/PRD.md` §7.

**Backlog (acknowledged, not scheduled):** mouse-click sounds (FR-08); per-key configurator for switch/tone/volume (FR-09); visualizer themes, Keeby ships 10 (FR-15); merging and live-verifying the pointer-motion patch (`build/wt/pointer.patch`) and the overlay ripple/spring/cursor work (`build/wt/overlay/`), both done and worker-tested but not yet merged or seen live by the user. See `docs/PRD.md` §4 notes, §9; `HANDOFF.md` §6-7.

## Open ideas / backlog

- License for the project itself is still unchosen (`license=('custom')` placeholder) — an open question for the user, not a technical one. `docs/010` §11, `docs/PRD.md` §9.
- Whether to build out the 10 visualizer themes, and at what priority relative to finishing cursor-follow/tilt/ripple, is undecided. `docs/PRD.md` §9.
- Per-key configurator and mouse-click sounds are backlog with no scheduled priority. `docs/PRD.md` §9, `HANDOFF.md` §7.
- Minor polish nits noted but not fixed: tray menu rebuilds twice per action, `list_packs()` rescans every refresh, tray title shows pack id instead of display name, an unused include in `main.cpp`. `HANDOFF.md` §7.
- Merge order matters: both in-flight patches touch `src/visualizer_wire.hpp`, `src/ui/kbus_client.*`, and two test files — apply the pointer patch first, then reconcile the overlay patch against it. `HANDOFF.md` §6b, `docs/PRD.md` §9.
