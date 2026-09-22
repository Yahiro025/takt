# Core Input-to-Audio Feasibility Spec

## Goal

Determine whether a Linux application running on CachyOS + Niri can

passively detect physical keyboard press/release events globally and

trigger low-latency local audio without interfering with normal keyboard

input.

## Target Environment

- CachyOS

- Wayland

- Niri compositor

- PipeWire audio

- x86_64

## Initial Technology

- C++23

- CMake

- Ninja

- libevdev

- PipeWire native API

## Required Behavior

1. Detect physical keyboard press events globally.

2. Detect release events separately.

3. Detect key repeat separately.

4. Never suppress or inject keyboard input.

5. Trigger a preloaded WAV sample on key press.

6. Work regardless of which application has focus.

7. Keep keyboard-to-audio latency low enough to feel immediate.

## Non-Goals

Do not implement:

- GUI

- tray

- switch profiles

- visualizer

- cursor tracking

- mouse sounds

- tone controls

- favorites

- configuration

- autostart

- updater

- licensing

## Privacy Constraint

The prototype should operate on physical Linux key codes only.

It must not reconstruct text, persist keystroke sequences, or transmit

keyboard information over the network.

## Success Criteria

The experiment succeeds when:

- normal typing is unaffected;

- press and release are observed globally;

- multiple simultaneous keys work;

- rapid typing produces corresponding sound events;

- the audio path does not require disk reads after startup;

- the program remains stable for at least several minutes of continuous typing.

## Deliverable

A technical report stating:

PASS / FAIL

plus:

- measured latency;

- permissions required;

- PipeWire configuration used;

- evdev devices detected;

- observed limitations under Niri/Wayland;

- recommended production architecture.

## Results — Step 1.4 Feasibility Spike (Completed)

**Verdict: PASS.**

physical keyboard event -> passive evdev capture -> RT-safe PipeWire
playback is viable on this machine (CachyOS, Niri, Wayland, this exact
hardware). This is a feasibility result, not a production-readiness claim
— see Architectural Limitations below.

Implementation: `experiments/input-reader/` (C++23, CMake+Ninja,
libevdev, native PipeWire, libsndfile for WAV decode). Not the product
codebase.

### Environment tested

- CachyOS, Wayland, Niri compositor, PipeWire (user session), x86_64,
  this machine (Dell Precision 7670, i5-12600HX).

### Input path

- Device tested: `/dev/input/event3`, "AT Translated Set 2 keyboard"
  (internal laptop keyboard, i8042/serio).
- Observation is passive: opened read-only via libevdev, `EVIOCGRAB` is
  never called anywhere in the code. Verified both by code inspection and
  by live testing — normal typing in other applications, including after
  switching window focus, was confirmed unaffected while the spike was
  running and reading the same device node.
- Verified behaviors, on real hardware, with the user physically typing:
  - press (value 1) and release (value 0) correctly distinguished;
  - **key repeat (value 2) confirmed as real kernel-emitted events** — 55
    observed in one session; this machine's Niri/libinput stack does
    **not** disable kernel-level autorepeat on the shared device node, so
    no userspace repeat timer was needed for this spike;
  - modifier + key (e.g. Left Shift + A) captured correctly, in order;
  - chords (overlapping key-down states across two physical keys)
    captured with correct per-key independence and ordering;
  - rapid typing sustained without issue;
  - focus changes mid-typing did not affect capture or normal typing.
- Ring buffer (SPSC, capacity 256) between the input-worker thread and
  the PipeWire RT thread: **zero dropped events observed**, across
  multiple live sessions totaling roughly 1,000 real key events.

### Audio path

- PipeWire native API, `pw_thread_loop` + `pw_stream`, output direction,
  32-bit float, negotiated at 48 kHz.
- Sample rate: 48000 Hz, confirmed both by our stream's negotiated format
  and by the system's PipeWire clock (`clock.rate = 48000`).
- Preloaded WAV samples (via libsndfile) decoded fully into RAM at
  startup; the RT `process()` callback does no disk I/O, no allocation,
  no locking — only lock-free ring-buffer drain, fixed-size voice-pool
  mixing, and buffer submission.
- `pw-cat` playback of the generated asset, and audible clicks heard by
  the user on the mapped keys (A, S, D, Space, Enter) during live tests,
  confirm the PipeWire output path actually reaches the speakers.

### PipeWire runtime configuration (live-verified)

- We requested `PW_KEY_NODE_LATENCY = "128/48000"`.
- **Directly observed while KEEBY was actively running and playing
  sounds**: KEEBY's own stream node and the physical ALSA sink node
  (`alsa_output.pci-0000_00_1f.3.analog-stereo`) were both running at
  **128 frames / 48000 Hz** (`pw-top`, `pw-cli info`). `node.latency` on
  the KEEBY node read back literally as `"128/48000"` — the request was
  honored, not silently downgraded.
  - This is **not** a claim that the entire PipeWire graph runs at 128
    frames: a concurrently-connected node (a browser's audio stream) was
    observed at its own 1024-frame quantum at the same time. Only
    KEEBY's stream and the physical ALSA sink were verified at 128
    frames.
- Zero PipeWire xruns observed (`pw-top` `ERR` column was 0 across every
  sample taken during active playback).
- Underlying ALSA hardware period size is 1024 frames (`buffer_size`
  32768), a separate, lower-level fact from the PipeWire graph quantum
  above. Despite that large period/buffer configuration, the live ALSA
  ring-buffer delay — read directly from
  `/proc/asound/card0/pcm0p/sub0/status` while KEEBY was playing — was
  measured at **183, 209, and 238 frames** across three samples (~3.8 to
  ~5.25 ms, average ≈ 4.4 ms), confirming PipeWire keeps the hardware
  buffer nearly empty rather than filling it to the period/buffer size.

### Latency

- Kernel evdev event -> PipeWire RT callback dequeue (measured inside
  the app, using `EVIOCSCLOCKID(CLOCK_MONOTONIC)` on our own fd so the
  timestamp domain matches `clock_gettime`): **0.994–1.437 ms average,
  1.781–2.819 ms max**, across two live sessions.
- Live ALSA ring-buffer delay (measured separately, see above): **≈3.8
  to 5.25 ms**, average ≈4.4 ms.
- Combined estimate, kernel keypress -> audible output:
  **approximately 5–8 ms**. This figure is software-observed only — it
  is **not** an acoustic or oscilloscope measurement of actual
  sound-onset time, and no such physical measurement was taken.

### Permissions

- No `chmod 666`, no root, no new udev rule, no group change was
  required or performed. The test user was already a member of the
  system `input` group, which already owns `/dev/input/event*` at mode
  `0660` on this machine — that pre-existing group membership is what
  granted access.
- The code defensively checks `EACCES` on open and prints guidance
  toward `input`-group membership (never `chmod 666`) for a machine
  where that membership doesn't already exist, but this was not
  exercised on the test machine since access already worked.
- **Security implication, unresolved for production**: read access to a
  keyboard's `/dev/input/event*` node is, structurally, equivalent to
  keylogger capability — any process with that access (via `input`-group
  membership or another mechanism) observes every physical key on the
  system, in every application, including password fields. This spike
  relied on pre-existing group membership on one development machine;
  it did **not** design or validate a production-appropriate permission
  model (e.g. scoping access via a dedicated helper process, seat/logind
  integration, or narrower device selection). That design question
  remains open and must be resolved before this becomes a
  distributable, non-development build.

### Architectural limitations (carried over, still unresolved)

- Device discovery is "first match wins" over `/dev/input/event0..31`,
  not "most confident match" — untested with multiple physical
  keyboards attached.
- No resampling: WAV assets must already be 48 kHz/stereo, or loading
  fails at startup by design; production needs startup-time resampling
  for arbitrary assets.
- No hot-unplug re-enumeration: a disconnect (`ENODEV`) is detected and
  logged, but the input thread simply exits rather than retrying.
- Test WAV assets are synthetic `ffmpeg`-generated sine-blips, not real
  mechanical-keyboard recordings.
- Total perceived latency including any additional fixed downstream
  hardware delay (HDA controller FIFO, DAC conversion) was not
  independently measured; the ~5–8 ms figure above is the software-side
  estimate only.
- Production permission model is unresolved (see Security above).

### Recommended production architecture

Carry the proven shape forward unchanged: passive libevdev/libinput tap
(never grabbing) feeding a lock-free SPSC ring buffer into a PipeWire
`process()` callback that is allocation-, lock-, and I/O-free, with all
samples pre-decoded at startup. Before productionizing, resolve: the
permission-scoping question above, startup-time resampling for
arbitrary WAV assets, multi-keyboard-aware device discovery, and
hotplug re-enumeration. See `docs/research/001-linux-input-audio-feasibility.md`
for the fuller architectural analysis this spike was built to validate.
