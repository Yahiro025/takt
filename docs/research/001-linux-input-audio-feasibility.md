# 001 — Linux Input-to-Audio Feasibility (CachyOS / Niri / Wayland)

Status: research only, no product code. Target machine: CachyOS (Arch-based), Wayland, Niri compositor, PipeWire, x86_64, Dell Precision 7670 (i5-12600HX, RTX A1000).

Legend used throughout:
- **[D]** documented Linux/Wayland/PipeWire behavior (kernel docs, project docs, man pages)
- **[C]** conclusion derived from [D] facts, not itself directly documented
- **[H]** implementation hypothesis — plausible, but must be verified on the real machine before relying on it

---

## 1. Recommended architecture

```
Physical keyboard
  → Linux kernel input core (drivers/input, evdev)
  → /dev/input/eventN node(s), opened read-only, NOT grabbed
  → libinput (preferred) or libevdev, in a dedicated input-worker thread
  → normalize to internal KeyEvent { keycode: KEY_*, kind: Down|Up|Repeat, ts_monotonic_ns }
  → lock-free bounded SPSC ring buffer
  → PipeWire RT audio thread (owned by PipeWire, we supply the process() callback)
      - drains ring buffer (wait-free)
      - triggers pre-decoded PCM voices in a fixed-size voice pool
      - mixes active voices into the output buffer
  → PipeWire graph → hardware sink → speakers
```

Two threads of our own at minimum:
1. **Input worker** — normal scheduling (`SCHED_OTHER`), blocks on `epoll_wait`/`libinput_dispatch`, does no audio work.
2. We do **not** own the audio thread — PipeWire creates and schedules it (`SCHED_FIFO`, via rtkit or rlimits). We only register a callback. This is important: the "audio engine" in the candidate diagram is not a separate thread we manage, it's code that runs *inside* PipeWire's real-time thread, which changes what's legal to do there (see §6, §8).

This satisfies all 10 requirements in the prompt: evdev-level keycodes are layout-independent and pre-text (req. 1, 2, 8, 9), reading a shared device node without `EVIOCGRAB` leaves every other client — including the focused app — untouched (req. 4, 7), the kernel fans chord events out per-key so simultaneity is inherent (req. 5), pre-decoded samples plus a lock-free trigger path keep audio latency low (req. 6), and seat-based permission (§4) avoids root (req. 10).

## 2. Alternatives considered

| Option | Verdict | Why |
|---|---|---|
| **Raw evdev + libevdev** (candidate architecture) | **Recommended**, paired with libinput's permission plumbing | libevdev is a thin, correct wrapper over the raw `ioctl`/`read` evdev ABI — one device, one fd, no session handling of its own. Good enough on its own if you handle device permission and hotplug yourself. |
| **libinput** | **Recommended**, for permission handling | libinput sits above evdev, multiplexes several devices, and — critically — already knows how to acquire device fds through logind/seatd without any udev rule work on our part [D]. Its keyboard events (`LIBINPUT_EVENT_KEYBOARD_KEY`) carry the same Linux `KEY_*` codes as raw evdev [D], so nothing is lost. Tradeoff: libinput is designed for compositors and pulls in some pointer/gesture machinery we don't need, but that's inert if we only listen for keyboard events. |
| **Compositor-level keyboard APIs / Wayland protocols** (`xdg-desktop-portal` GlobalShortcuts, Niri's new `vicinae-hotkey-v1`) | **Rejected** as the primary path | Wayland's security model deliberately has no analog of X11's global key listener — this is intentional, to make keylogging harder [D]. The portal's GlobalShortcuts API only delivers *pre-registered* shortcut combinations chosen by the user through a portal dialog, not a full down/up/repeat stream for every key [D]. Niri's `vicinae-hotkey-v1` (in-flight PR at time of writing, not yet an upstream `wayland-protocols` proposal) is the same shape — bind/rebind/remove specific shortcuts, not passive full-keyboard capture [D]. Neither can satisfy "every key, unregistered, with repeat" (req. 1–3). Useful later for app-level hotkeys (e.g. a mute toggle), not for the core sound-on-keypress feature. |
| **X11/XInput (`XGrabKeyboard`, `XInput2` raw events)** | **Compatibility note only** | Would work under Xwayland-rootful setups or a pure X11 fallback, but Niri is Wayland-only; there is no X11 server to grab against. Not relevant unless a non-Wayland fallback session is explicitly supported later. |
| **`chmod 666 /dev/input/*`** | **Rejected** | Explicitly excluded by the brief; also a system-wide keylogger-enablement for every local user, not just our app. |
| **Global suid-root helper reading evdev, dropping privileges after open** | **Rejected for now** | Works, but adds an attack-surface/root-handling burden the seat-based mechanisms (§4) make unnecessary on a systemd/logind desktop. Revisit only if the logind/uaccess path fails on the real machine. |

## 3. Niri/Wayland constraints

- **[D]** Wayland compositors do not expose raw, unfiltered, cross-application keyboard input to arbitrary clients — this is a deliberate design choice across the protocol family, not a Niri-specific gap.
- **[D]** Niri uses `libxkbcommon` for keymap handling and (per its own docs) can source XKB settings from `systemd-localed` over D-Bus if not configured locally. This layer is downstream of evdev and irrelevant to us if we stop at the evdev/libinput keycode layer — another reason to stay below it.
- **[C]** Because our path never asks the compositor for input, Niri's window/focus model is a non-issue: we are not a Wayland client requesting keyboard focus at all, we are a second reader of the same kernel device node the compositor's own libinput instance is reading. Focus, workspace, fullscreen state, etc. do not affect us.
- **[H]** Whether a *second*, independent process can hold its own logind/seatd device handle on the same evdev node concurrently with the compositor's handle needs to be confirmed on this machine (§10, §12). It is expected to work — this is exactly how tools like `evtest`, `libinput debug-events`, and on-screen-keyboard/accessibility daemons coexist with a running compositor — but "expected" is not "verified."
- **[H]** Whether Niri's libinput instance leaves kernel-level autorepeat (`EVIOCGREP`) enabled on the device is unverified — see §6/§8/§12, it directly affects how we implement requirement 3 (repeat detection).

## 4. Input permission model

Do **not** use `chmod 666`. In order of preference:

1. **Seat-managed access via logind, through libinput** *(preferred)*. On a systemd desktop, `systemd-logind` (or `seatd` as its non-systemd equivalent) owns "seats" and grants the actively logged-in graphical user's session ACL-based read/write access to that seat's input devices. libinput's `libinput_udev_create_context()` + an `open_restricted` callback that calls logind's `TakeDevice` (or the seatd equivalent) is the standard, documented way every Wayland compositor already gets device access without running as root or suid [D]. Using the same mechanism means our app needs **no udev rules, no group membership, and no root** — it just needs to be a process running inside the user's active graphical session.
2. **`uaccess`-tagged device nodes** *(fallback / what actually backs #1 at the udev layer)*. systemd ships default udev rules (`70-uaccess.rules`) that tag seat-owned input device nodes with the `uaccess` tag; logind then dynamically chmods/ACLs those nodes to the active local user for the duration of their graphical session [D]. In practice this usually means `/dev/input/eventN` for seat0 devices is already `rw` for the logged-in user with no extra configuration, even for a process that opens the node directly with `open(2)` rather than going through libinput. This should be confirmed on the real machine with `getfacl /dev/input/eventN`.
3. **`input` group + custom udev rule** *(legacy fallback, avoid unless 1–2 fail)*. Older, coarser: add the user to an `input` group and ship a rule like `KERNEL=="event*", SUBSYSTEM=="input", GROUP="input", MODE="0640"`. This grants blanket access independent of session/seat state (even over SSH, no logind session), which is both unnecessary for a desktop GUI app and a strictly worse security posture than dynamic ACLs. Keep as a documented fallback only.

Either way, run the GUI application as the normal desktop user. Nothing in this design needs root, suid, or a persistent group grant.

## 5. PipeWire audio design

Constraints from the brief, restated as design rules:
- No disk I/O in the RT callback → **decode every sample fully into memory at startup** (or on a background loader thread before first use), never at trigger time.
- No blocking synchronization in the RT callback → **communicate key events into the audio thread only through a lock-free/wait-free SPSC ring buffer**; the callback drains it non-blockingly at the top of each `process()` invocation.
- No unnecessary allocation in the RT callback → **preallocate a fixed-size voice pool** at startup (e.g. 32–64 voices sized for the max realistic simultaneous key-sound budget); "playing a sound" is just marking a pooled voice active and resetting its read cursor to 0, not allocating a buffer.
- No process-per-sound, no re-loading a file per keypress → **preloaded, pre-decoded PCM (float32) samples held in RAM for the lifetime of the app**, referenced (not copied) by each voice when triggered.

Mechanics:
- Use `pw_stream` (or `pw_filter`) in `PW_DIRECTION_OUTPUT` with a negotiated 32-bit float interleaved format.
- Request low latency via the stream's node latency property (e.g. `PW_KEY_NODE_LATENCY = "128/48000"`), then let PipeWire's graph converge on an achievable quantum — [D] PipeWire supports native quantum sizes down into the tens of samples, but the *actually achievable* stable quantum is hardware/driver dependent and must be measured on this machine [H].
- Real-time scheduling for the audio thread is arranged by PipeWire itself, either via `RLIMIT_RTPRIO` (preferred, lower overhead) or a fallback through the Portal Realtime D-Bus API / rtkit if rlimits aren't set up [D]. This is infrastructure we configure (session limits) rather than code we write.
- Mixing: in the callback, sum the active voices' current sample into the output frame, advance each voice's cursor, deactivate voices that reach end-of-sample. This is O(active voices) per frame, bounded by the pool size, no branches that allocate or lock.
- Voice-stealing policy for when trigger rate exceeds pool size: drop-the-quietest or drop-the-oldest, with a counter for diagnostics — never block waiting for a free voice.

## 6. Threading/concurrency model

| Thread | Owner | Priority | Responsibilities | Must never do |
|---|---|---|---|---|
| Input worker | us | `SCHED_OTHER` | Poll evdev/libinput fd(s) via `epoll`, normalize events, push to ring buffer | Touch audio state directly, block on audio thread |
| PipeWire audio callback | PipeWire | `SCHED_FIFO` (rtkit/rlimits) | Drain ring buffer, advance/mix voices, write output buffer | malloc/free, disk I/O, mutex lock, syscalls other than what PipeWire itself does, unbounded loops |
| Startup/loader (transient) | us | `SCHED_OTHER` | Decode sample files into RAM once at launch | Run after startup completes |

The ring buffer is the only shared structure between the input worker and the audio callback; it must be a bounded SPSC lock-free queue (single producer = input worker, single consumer = audio callback). If multiple physical keyboards are fanned into one input worker thread, that thread remains the single producer — no need for an MPSC structure at this stage. **[H]** whether a genuinely separate thread per input device is warranted is a later scaling question, not needed for the minimal experiment.

## 7. Privacy/security risks

- **[C]** Read access to `/dev/input/eventN` for a keyboard is, structurally, keylogger-equivalent capability: any process with that access sees every physical key on the system, in every application, including password fields and terminals. This is inherent to requirement 1 ("passively detect... globally") — it cannot be designed away, only bounded by policy:
  - Open only keyboard-class device nodes (filter by `EV_KEY` capability bitmap / `ID_INPUT_KEYBOARD` udev property), not every input node indiscriminately.
  - Never persist key codes to disk, logs, or network — each `KeyEvent` should be read, used to pick a sound, and discarded; no buffer of key history beyond what a repeat-detection timer strictly needs (a single "currently held key" per physical key, not a sequence).
  - Because evdev delivers physical `KEY_*` codes, not layout-mapped characters, the app structurally cannot reconstruct typed text even if it wanted to (req. 8) — this is a property of stopping *below* the XKB/layout translation layer, not a policy we have to enforce separately. It does not, however, prevent keystroke-timing side-channel inference (e.g. approximate typing cadence/word-length correlation) — worth a line in any published privacy documentation, since the capability is real even if unused.
  - Keep the input-handling code small, auditable, and ideally open-source, since "trust us, we don't log" is a much weaker claim than "the code that could log is 200 lines anyone can read."
- No new attack surface is added by the audio path itself (no network, no dynamic code loading, no file writes at runtime beyond initial sample loading from the app's own asset directory).
- Optional future hardening (not needed for the minimal experiment): isolate the evdev-reading component in a small helper process with no network access, communicating only key-identity/kind/timestamp to the GUI/audio process over a local socket or shared memory, so a compromised GUI process can't itself become the keylogging surface. This is an architectural nice-to-have for a later phase, flagged here rather than designed now.

## 8. Failure modes

| Failure | Cause | Mitigation |
|---|---|---|
| Audible crackle / xrun | RT callback exceeds its quantum budget (allocation, lock, syscall, too many voices) | Keep callback O(active voices), preallocated pool, no locks/allocation; monitor via `pw-top` xrun counter during testing |
| Ring buffer overflow | Audio thread stalls (system suspend, RT starvation) while input worker keeps producing | Bounded buffer with drop-oldest/drop-newest + counter; input worker must never block on a full buffer |
| Missed repeat events | **[H]** Niri's libinput instance may disable kernel-level `EVIOCGREP` autorepeat on the device and emulate repeat purely inside libinput/the compositor, meaning a raw evdev tap would see no `value == 2` events at all | Do not rely on kernel-emitted repeat; implement our own repeat timer in the input worker (start-of-hold timestamp + configurable delay/rate), verified independent of what the compositor does |
| Device hot-unplug/replug mid-session | User unplugs/replugs a USB keyboard | Use libinput's native `LIBINPUT_EVENT_DEVICE_ADDED/REMOVED` (if using libinput) or a `udev` monitor (if using raw evdev) to re-enumerate rather than assuming a static fd set |
| Seat/logind ACL revoked mid-session | Session switch (VT switch, screen lock in some configs), logind re-arbitrating seat devices | Handle `TakeDevice`/`ReleaseDevice`-equivalent revoke callbacks from libinput; reacquire on resume rather than crashing |
| PipeWire daemon restart | `pipewire.service`/`pipewire-pulse.service` restarted or crashes | Handle `pw_stream` disconnect/error state, reconnect rather than treating it as fatal |
| Double `EVIOCGRAB` conflict | Any code path (ours or a dependency) accidentally calls `EVIOCGRAB` | Explicit code-review rule: never call `EVIOCGRAB`/libinput's grabbing equivalents; this is the one mistake that would silently break requirement 7 |

## 9. Exact packages/libraries likely required on CachyOS

Arch package names (verify exact versions on the real machine, CachyOS mirrors Arch's `extra` repo closely for these):

- `libinput` — usually already present as a Niri/Wayland dependency; provides `libinput.h`/`pkg-config libinput`
- `libevdev` — if going the raw-evdev route instead of/alongside libinput
- `pipewire` — core library + headers (`libpipewire-0.3.pc`), Arch does not split a separate `-devel` package
- `pipewire-audio` — meta-package pulling in the audio session pieces (WirePlumber etc.) if not already installed as part of the desktop
- `wireplumber` — session/policy manager PipeWire depends on for routing
- `seatd` — only if Niri on this install is *not* using logind's built-in seat management; check which one is active before adding this (`loginctl seat-status` vs `seatd` service status)
- `systemd` (`logind`) — base component, already present
- `base-devel` + `pkgconf` — toolchain and `pkg-config` for locating `libpipewire-0.3`, `libinput`, `libevdev` at build time
- Optional, if the implementation language is chosen later: `pipewire-rs`/`input`/`evdev` Rust crates, or the C headers directly for a C/C++ prototype

**[H]** exact presence/absence of `seatd` vs pure-`logind` seat management on this specific CachyOS+Niri install needs to be checked directly (`pacman -Qi seatd`, `systemctl status seatd`, `loginctl list-seats`).

## 10. Minimal experiment to build next

A single CLI proof-of-concept, no GUI, roughly two halves:

1. **Input tap**: open one keyboard evdev node (discover via `/dev/input/by-id/*-event-kbd` or a libinput context), read events *without* `EVIOCGRAB`, print `KEY_*` code, event kind (down/up/repeat), and a monotonic timestamp to stdout — while simultaneously typing into a separate focused text editor, to directly confirm the editor still receives every keystroke untouched.
2. **Audio trigger**: preload one short sample into memory; on each `KEY_DOWN` from the tap, push an event into a ring buffer; a `pw_stream` process callback pops it and plays the sample through a single voice, then extended to 2–3 overlapping voices to confirm polyphony.

This is deliberately the smallest slice that exercises every architectural risk (permission model, non-interference, repeat semantics, RT-safe triggering) without building any product surface.

## 11. Success/failure criteria for that experiment

**Success**, all of the following:
- Typing in the other focused app while the tap runs produces zero missed or duplicated characters (proves req. 4, 7 — no grab leakage).
- The tap process runs as the normal desktop user, no root, no `chmod 666`, no manual group edit if the `uaccess`/logind path works (proves req. 10).
- Console output shows distinct down and up events per physical key; holding a key either shows native `value == 2` repeat events or clearly shows their absence — either result answers the open repeat question from §8 and informs whether we need our own repeat timer (req. 1–3).
- Holding two keys simultaneously produces two independently interleaved down events in the correct order (req. 5).
- Measured input-event-to-audible-onset latency stays consistently under ~20 ms (stretch target <10 ms) with zero `pw-top`-reported xruns across a 60-second rapid/overlapping-typing stress run (req. 6).
- `strace -e trace=openat,write,connect` on the running process shows no file writes beyond initial asset loading and no network connections at all during the test (req. 8, 9 — no logging, no exfiltration surface).

**Failure indicators**: any missed/duplicated keystrokes in the other app, any requirement to fall back to the `input`-group/root path because seat-based access didn't work, audible crackle/xrun under the stress test, or any code path that ends up needing `EVIOCGRAB`.

## 12. Open questions requiring the real machine

1. Does this CachyOS+Niri install grant `uaccess`-based RW on `/dev/input/eventN` to the logged-in graphical user automatically, or does something (seatd config, a missing rule) block it? (`getfacl /dev/input/eventN`)
2. Can a second unprivileged process hold its own read handle on an evdev node that Niri's libinput instance already has open, with zero conflict/"device busy" errors?
3. Does Niri's libinput leave kernel-level autorepeat (`EVIOCGREP`) enabled on keyboard devices, or does it disable it and do repeat purely in userspace — determining whether our repeat detection can be kernel-driven or must be self-implemented?
4. What PipeWire quantum/latency is actually stable on this laptop's audio hardware (Intel HDA via the 12600HX platform, not the NVIDIA A1000, which has no bearing on audio) — measured with `pw-top`, not assumed from docs?
5. Does `logind`/`seatd` on this machine permit a second concurrent seat client (our experiment) to `TakeDevice` alongside the compositor's own claim, or is that restricted to one active client per seat?
6. Subjective/perceived latency at whatever quantum turns out to be stable — a measured number under 20 ms is necessary but not sufficient; it needs to actually feel instant when typing normally.
