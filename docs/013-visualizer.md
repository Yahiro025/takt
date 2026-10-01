# Step: floating on-screen keyboard visualizer (engine half)

Engine-side half of KEEBY's Keeby-style on-screen keyboard visualizer. A
parallel worker builds the `keeby-visualizer` overlay app and the
settings-window tab against the contracts documented here. This step: the
IPC boundary keeby spawns the overlay over, the non-RT key-event tap,
`org.keeby.Control1` additions, settings persistence, and the tray menu.

## 1. Security design

Keystrokes are confidentiality-sensitive (see
docs/006-step-2.5-security-permissions.md's threat model) and must never
cross D-Bus or any named/pathname socket another process could connect to
or eavesdrop on. The same holds for pointer motion, forwarded by the same
`keeby-inputd` helper (widened from "one keyboard" to "one keyboard plus
pointer devices" -- see docs/006's "Pointer motion for the visualizer"
section for the full security audit of that change) and routed to the
visualizer through the same private socket, never D-Bus. `VisualizerService` (`src/visualizer_service.hpp/.cpp`)
spawns `keeby-visualizer` the same way `InputCapture` spawns `keeby-inputd`:

- `socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)` -- a private,
  unnamed, process-pair-only channel, not `bind()`-able by anything else.
- `fork()` + the child's end `dup2`'d onto fd 3, then `execv(helper_path,
  argv)` with **argv containing only the program path** -- no shell, no
  string interpolation, nothing attacker-controlled reaches an `exec*` that
  could be shell-expanded.
- The helper path is resolved the same way as `keeby-inputd`'s: a
  test-only env override (`KEEBY_VISUALIZER_PATH`), else next to `keeby`'s
  own executable (`/proc/self/exe`), else the compiled-in installed bin dir
  -- **never** a `$PATH` search, so a malicious `PATH` entry can't
  substitute a different binary.
- Only two message *shapes* ever cross the socket (Contract A below): a key
  event (evdev code + down/up) and a config update (position/dismiss_ms).
  Nothing else -- no settings blob, no profile data, no filesystem paths.

Unlike `keeby-inputd`, `keeby-visualizer` needs **no privilege at all**: it
never touches `/dev/input/*`, so there is no setgid step, no `keeby` group,
nothing for `docs/010-step-2.9-packaging.md` to grant it. It runs as
KEEBY's own child, at KEEBY's own privilege level.

## 2. Contract A: IPC wire format

`src/visualizer_wire.hpp`, shared by both halves:

```cpp
struct VizMessage {
    uint8_t type;         // 1 = Key, 2 = Config, 3 = Motion
    uint8_t kind;         // Key: 1 down, 0 up (repeat never sent).
                           // Motion: 0 mouse, 1 touchpad (PointerKind).
    uint16_t code;        // Key: raw evdev KEY_* code.
                           // Motion: dx, int16 bit-cast (motion_dx()).
    uint8_t position;     // Config only: Position (0..6)
    uint8_t reserved;     // Config only: follow-speed step (1..40, 0.1x
                           // each; see follow_speed_to_step/from_step).
                           // Zero for every other type.
    uint16_t dismiss_ms;  // Config: 250..5000.
                           // Motion: dy, int16 bit-cast (motion_dy()).
};
static_assert(sizeof(VizMessage) == 8);
```

One fixed 8-byte message per `SOCK_SEQPACKET` datagram -- no framing needed,
`recv()` on a seqpacket socket returns exactly one datagram per call.
`Position` is `TopLeft=0, TopCenter=1, TopRight=2, BottomLeft=3,
BottomCenter=4, BottomRight=5, FollowCursor=6` (the default); `viz::to_string`/
`viz::position_from_string` convert to/from the tray/settings/D-Bus string
form (`"top-left"`..`"bottom-right"`, `"follow-cursor"`).

### 2b. Contract A2: pointer motion (docs/006's "Pointer motion for the
visualizer" section)

Wayland has no global-cursor-position query, so `follow-cursor` is an
*estimate* built from forwarded mouse/touchpad motion, not a real position.
`keeby-inputd` forwards summed per-`SYN_REPORT` `(dx, dy)` samples from up
to 4 auto-discovered pointer devices to `InputCapture` as
`wire::MessageType::PointerMotion` (see `src/input_wire.hpp` and docs/006);
`InputCapture` routes each decoded sample to a second tap
(`set_visualizer_motion_transport`) that `EngineController` wires only
while the visualizer is enabled AND its position is `follow-cursor` --
otherwise the sample is decoded and dropped, never queued. `VisualizerService`
owns that second transport (`motion_transport()`) and its existing sender
thread coalesces (sums) whatever is pending into at most one
`type == Motion` `VizMessage` per loop iteration -- never one message per
sample, and never blocking, same policy as the `Key` path. `dx`/`dy` are an
int16 bit-cast into `code`/`dismiss_ms` (`motion_dx()`/`motion_dy()` in
`visualizer_wire.hpp`) so the full clamped range survives the two uint16
wire fields.

Config's `reserved` byte additionally carries the follow-cursor speed
multiplier as a step of 0.1x, `1..40` (`0.1x`..`4.0x`, default `10` ==
`1.0x`) -- `follow_speed_to_step()`/`follow_speed_from_step()` in
`visualizer_wire.hpp`. Settings key `visualizer_follow_speed` (float,
`[0.1, 4.0]`, default `1.0`); D-Bus `SetVisualizerFollowSpeed(d)`; CLI
`keeby ctl visualizer speed <x>`. Rendering the estimate itself (turning
motion samples into an on-screen position, and applying the speed
multiplier) is the overlay's own concern, left to the parallel worker
building `keeby-visualizer` -- see section 9.

keeby sends exactly one `Config` message right after spawning (before any
`Key` message can race ahead of it -- `spawn_locked()` calls
`send_config_locked()` synchronously before starting the sender thread) and
one more on every position/dismiss_ms change. All sends are
`MSG_DONTWAIT | MSG_NOSIGNAL`: **never blocking**, and never raising SIGPIPE
into a process that isn't expecting it.

## 3. Key-event tap (non-RT side only)

`InputCapture::set_visualizer_transport(KeyEventTransport*)` wires an
optional second `EventTransport<KeyEvent, 256>` (the same SPSC template
`KeyEventTransport` already uses for the audio path) that the existing
forwarding thread also pushes every **non-repeat** `KeyEvent` into,
drop-on-full, right alongside its push into the primary (audio) transport.
The pointer is an `std::atomic`, wired/unwired with release/acquire
ordering, so it can be turned on and off at runtime without restarting
input capture. Nothing in `AudioBoundary` or `SampleMixer` changes, and the
RT `process()` callback is untouched -- this tap lives entirely in the
forwarding thread that already runs off-RT.

`VisualizerService` owns that second transport (`transport()`) and a
dedicated sender thread (started by `set_enabled(true)`) that drains it and
writes `VizMessage`s. The sender never blocks: a full socket send buffer
(the child not reading) means `EAGAIN`/`EWOULDBLOCK`, and the event is
simply dropped -- exactly like the primary transport's own drop-on-full
policy, just one hop later.

## 4. Lifecycle

- **Enable**: `VisualizerService::set_enabled(true)` spawns the child (if
  not already running), sends the first `Config`, and starts the sender
  thread.
- **Config change** (`set_position`/`set_dismiss_ms`, called from a live
  D-Bus/settings/tray thread): re-sends `Config` to a live child
  immediately; if the child had died, this *also* triggers a respawn.
- **Child death or socket error**: detected by the sender thread itself
  (a `send()` failure other than `EAGAIN`, or a `waitpid(WNOHANG)` hit
  checked every loop iteration even with no key traffic) -- it stops
  sending, reaps the child (`waitpid`, no zombie left behind) and marks
  `child_alive() == false`. **No automatic respawn**: the next explicit
  `set_enabled(true)` or a position/dismiss_ms change is what respawns it,
  exactly mirroring how a user would notice ("the visualizer stopped
  working") and fix it (toggle it, or just keep using the settings tab).
- **Disable/quit**: `set_enabled(false)` (also called from
  `EngineController::stop()`) requests the sender thread stop, joins it,
  then terminates the child with the same bounded SIGTERM-then-SIGKILL
  pattern `InputCapture` uses for `keeby-inputd` (`terminate_and_reap`:
  SIGTERM, ~1s bounded poll, SIGKILL, blocking `waitpid`), and closes the
  socket.

`EngineController::set_visualizer_enabled()` mediates between the two: it
wires `InputCapture`'s tap pointer to `VisualizerService::transport()`
before enabling, and clears it after disabling, so `InputCapture` only ever
pushes into that queue while the visualizer is actually meant to be running.

## 5. Contract B: `org.keeby.Control1` additions

- `GetVisualizer() -> (b enabled, s position, u dismiss_ms, d follow_speed)`
- `SetVisualizer(b enabled)`
- `SetVisualizerPosition(s position)` -- an unrecognized string throws
  `org.keeby.Error.InvalidArgument` (checked in `ControlService` itself,
  before the handler ever runs, via `viz::position_from_string`).
  `"follow-cursor"` is valid here (and everywhere else a position string is
  accepted: settings, ctl, tray radio items) and is the default.
- `SetVisualizerDismiss(u ms)` -- clamped to `[250, 5000]` by the handler
  (same "callee clamps" contract as `SetVolume`/`SetStereoWidth`).
- `SetVisualizerFollowSpeed(d speed)` -- clamped to `[0.1, 4.0]` by the
  handler, same contract.
- `StateChanged` is emitted after each, via the existing
  `refresh_and_mark_dirty()` glue in `main.cpp` (docs/011).

CLI: `keeby ctl visualizer on|off`, `keeby ctl visualizer position <p>`,
`keeby ctl visualizer dismiss <ms>`, `keeby ctl visualizer speed <x>`;
`keeby ctl status` now prints an additional
`visualizer=on,follow-cursor,1000,1.0` line.

## 6. Settings

Four keys in `settings.ini` (`src/settings.hpp/.cpp`): `visualizer` (bool,
default `true`), `visualizer_position` (string, default `follow-cursor`,
validated against the seven position strings), `visualizer_dismiss_ms`
(integer, default `1000`, clamped `[250, 5000]`), and
`visualizer_follow_speed` (float, default `1.0`, clamped `[0.1, 4.0]`).
Same load-time contract as every other setting: a missing file yields
defaults silently; an invalid value keeps the default/clamps and appends a
warning rather than failing the whole load. `main.cpp` applies all four at
startup (position, dismiss_ms and follow_speed before enabling, so a
startup spawn's first `Config` message already reflects them) and persists
the live state back through the existing `settings_dirty` debounce.

## 7. Tray

"Configure" group gains a "Visualizer" checkmark and a "Position" submenu
of seven radio items (`kTrayIdVisualizer`, `kTrayIdVisualizerPositionMenu`,
`kTrayIdVisualizerPos*`, including `kTrayIdVisualizerPosFollowCursor`),
between the stereo-width menu and the separator. `TrayActions` gained
`toggle_visualizer`/`set_visualizer_position`, wired in
`TrayService::handle_menu_click` to `EngineController::set_visualizer_enabled`/
`set_visualizer_position`. Follow speed has no tray control (only a
position radio item) -- settings/D-Bus/ctl only.

## 8. Testing

`tests/fake_keeby_visualizer.cpp` is a test-only stand-in (same shape as
`tests/fake_inputd.cpp`): pointed to via `KEEBY_VISUALIZER_PATH`, it logs
every `VizMessage` it receives as a text line to `$FAKE_VISUALIZER_OUTPUT`
(`config <position> <dismiss_ms> speed=<step>`, `key <up|down> <code>`, or
`motion <mouse|touchpad> <dx> <dy>`), with modes for a normal run, a child
that never reads (`no_read`, to exercise the non-blocking-drop path), and a
child that exits mid-session (`die_after_2`, to exercise death
detection/reap/deferred respawn). `tests/visualizer_service_test.cpp`
exercises: config-sent-first on spawn (incl. follow-speed), end-to-end
forwarding through `InputCapture`'s real tap (repeats filtered),
non-blocking drop against a non-reading child, child-death reap with no
automatic respawn until the next enable/config-change, bounded/idempotent
disable, follow-speed re-sent on change, and pointer-motion coalescing
(summed, not one-message-per-sample) plus non-blocking motion forwarding.
`tests/fake_inputd.cpp` gained a `pointer_motion` mode (a KeyEvent, a mouse
PointerMotion, a touchpad PointerMotion, another KeyEvent) that
`tests/input_capture_test.cpp` uses to verify PointerMotion is decoded
correctly for both device kinds, reaches a wired motion tap, and never
reaches the key transport or an unwired tap.
`tests/pointer_input_test.cpp` covers the helper-side pure logic directly
(device classification incl. tablet/touchscreen/keyboard exclusion, the
touchpad finger-count/position state machine incl. scroll and
lift-and-replace resets, and unit conversion incl. the zero-resolution
fallback) with no fd, libevdev, or fork involved.
`tests/control_service_test.cpp`, `tests/settings_test.cpp`,
`tests/tray_menu_test.cpp` and `tests/kbus_client_test.cpp`
(+ `tests/fake_keeby_service.cpp`) gained matching coverage for the D-Bus
methods (incl. `SetVisualizerFollowSpeed` and the widened `GetVisualizer`
tuple), settings round-trip/validation, tray items/dispatch, and the
settings-window's own D-Bus client, respectively.

## 9. Overlay app

*(left for the parallel worker building `keeby-visualizer` and the
settings-window tab: how the overlay renders, its window/compositor
integration on Niri, and how the settings tab surfaces
position/dismiss_ms/enabled through the `org.keeby.Control1` methods
above.)*
