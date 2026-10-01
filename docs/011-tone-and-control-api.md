# Step: tone filter, settings-window D-Bus API, Keeby-ordered tray menu

Engine-side half of KEEBY's Keeby-style settings UI. A parallel worker
builds the GTK4 `keeby-settings` window against the D-Bus contract below;
another changes `src/sound_pack.*` and adds `--check-profiles` to
`main.cpp`. This step: the tone filter DSP, `org.keeby.Control1` additions,
and the tray menu reorder/regrouping.

## 1. Tone filter DSP

Two params, `tone_x`/`tone_y` in `[-1, 1]`, default `(0, 0)`. Three RBJ
shelving biquads in series, stereo, applied to the mix bus after voice
summation, before the output clamp (`src/sample_mixer.cpp`,
`compute_tone_coeffs`/`apply_biquad`/`rbj_shelf`):

- Stage 0: low-shelf ~250 Hz, gain `-6*x` dB
- Stage 1: high-shelf ~2500 Hz, gain `+6*x` dB
- Stage 2: high-shelf ~7000 Hz, gain `+6*y` dB

`x` is a tilt (stage 0 + 1 together): `x=-1` (Thock) boosts low/cuts high;
`x=+1` (Clack) is the reverse. `y` (stage 2 alone) is an independent
high-shelf: `y=+1` (Bright) boosts the top end without touching the tilt.
Coefficients are the standard RBJ Audio EQ Cookbook shelf formulas, shelf
slope `S=1`, normalized so `a0 == 1` (not stored). Chosen over a shallower
design (e.g. two first-order shelves) because it directly matches Keeby's
2D pad language (independent tilt + brightness axes) with three small,
well-understood biquads -- no new dependency, no custom filter design.

**Bypass exactness**: `ToneCoeffs::bypass = (x == 0 && y == 0)` (checked
before clamping matters, since both are already clamped by then). `mix()`
skips the filter loop entirely when `bypass`, so at `(0, 0)` output is
bit-identical to no filter existing -- verified by `test_tone_bypass_exact`,
which explicitly round-trips through a non-zero tone and back to `(0, 0)`
and diffs the rendered buffer byte-for-byte against a mixer that never
touched tone at all. Every pre-existing golden/sample-exact test in
`sample_mixer_test.cpp` is untouched because none of them call `set_tone`.

**RT-safety audit**: coefficients are computed off-RT in `set_tone()` (a
control-thread call, e.g. from a D-Bus handler or startup). The RT thread
(`mix()`) loads a published pointer once per callback and reads through it
for that whole callback; its own persistent per-channel biquad state
(`tone_state_l_`/`tone_state_r_`, direct-form-II-transposed `z1`/`z2`)
carries across callbacks unchanged. A coefficient change is applied
directly with no crossfade -- acceptable per the brief ("no zipper noise or
clicks worth worrying about for small moves"); a settings-window drag
produces many small steps, not one large jump.

**Publish mechanism, and why it changed mid-implementation**: the first
version used a fixed 2-slot double buffer (`tone_coeffs_[2]` +
`std::atomic<int> tone_index_`), the design the brief suggested. TSAN
immediately proved it racy: `mix()` holds a `const ToneCoeffs&` into one
slot for the entire callback (up to 4096 frames), and a writer thread
calling `set_tone()` faster than the reader calls `mix()` can flip the
index twice (0->1->0) inside that window, so the writer's *second* publish
overwrites the exact slot the reader is still reading from. A larger fixed
slot count only raises the bar, it doesn't eliminate the class of bug
(the concurrent-publish test deliberately hammers `set_tone()` as fast as
possible from a second thread specifically to catch this).

The fix reuses this codebase's own already-reviewed pattern for the exact
same problem: `SoundBank` hot-swapping (`request_bank`/`active_bank`/
`sync_bank`, docs/008). Each `set_tone()` call heap-allocates one immutable
`ToneCoeffs` (never mutated again after construction -- the only allocation
in the whole tone path, always off-RT) and publishes a pointer via
`requested_tone_` (release store). `mix()` loads it once per callback
(acquire) into its own RT-local cache (`tone_cache_`, a plain pointer,
touched by no other thread) and, on change, stores it back into
`active_tone_` -- mirroring `bank_`/`requested_bank_`/`active_bank_`
exactly. Retired `ToneCoeffs` objects live in `retired_tone_`
(off-RT-only, mutex-guarded) until GC'd.

GC correctness (this is the part that took two attempts): `retired_tone_`
is in publish order, and `mix()` only ever advances `tone_cache_` *forward*
through that order, never backward. The first GC attempt freed anything
that was neither `active_tone_`'s last-observed value nor the just-pushed
pointer -- also TSAN-caught: an *in-flight* entry (published after the
stale `active_tone_` a concurrent writer observed, but before the newest
one) is exactly what `mix()` can be transitioning onto locally (it sets
`tone_cache_` before it stores `active_tone_`, a few instructions apart) at
the moment the writer takes its snapshot. The fix: only erase entries
*strictly before* `active_tone_`'s position in the list; never touch it or
anything after. Whatever `mix()` is using or about to switch to is always
at or after that position, so this is always safe -- conservative (an
entry a fast-moving writer skips over stays alive a little longer than
strictly necessary) but correct, and self-corrects as `active_tone_`
catches up on later calls. No bounded wait is needed (unlike
`swap_bank()`'s 500 ms poll) since nothing here needs the switch to be
visible before `set_tone()` returns.

Verified clean under TSAN (`test_tone_concurrent_publish`: a writer thread
flips tone as fast as possible while the main thread triggers+mixes in a
tight loop, 8 consecutive clean runs) and functionally sane
(`test_tone_response_sanity`: crude low-pass/high-pass energy proxies on a
rendered white-noise burst confirm Thock raises low energy and lowers high
energy relative to flat, Clack the reverse, and Bright raises high energy).

## 2. `EngineController`/`Settings` surface

`EngineController::set_tone(float, float)` / `tone() -> pair<float,float>`,
thin forwards through `AudioBoundary` to `SampleMixer`, same shape as
`stereo_width`. Not `noexcept` (the only allocating control setter in this
surface -- matches `set_profile`'s existing allocating precedent).

`Settings` gained `tone_x`/`tone_y` (`float`, `[-1, 1]`, default `0`),
persisted as `tone_x = ...`/`tone_y = ...` lines, same clamp-with-warning
load path as `stereo_width`. `main()` applies them at startup
(`engine.set_tone(settings.tone_x, settings.tone_y)`) and saves them via
the existing `current_settings()`/dirty-flag machinery -- no new save path.

## 3. `org.keeby.Control1` additions (exact contract)

All at `/org/keeby/Keeby`, bus name `org.keeby.Keeby`, interface
`org.keeby.Control1`. Existing methods (`Toggle`, `SetEnabled`,
`SetVolume`, `SetStereoWidth`, `SetProfile`, `ListProfiles`, `GetState`,
`Quit`) are unchanged.

- `ListProfilesDetailed() -> a(ss)`: `(id, display name)` pairs, same order
  as `ListProfiles`.
- `GetTone() -> (dd)` / `SetTone(d x, d y)`: non-finite argument ->
  `org.keeby.Error.InvalidArgument` (same `require_finite` helper
  `SetVolume`/`SetStereoWidth` already use); otherwise clamped to
  `[-1, 1]`, never rejected.
- `GetInfo() -> (s version, b device_connected)`: version `"0.1.0"`, from
  `project(keeby VERSION 0.1.0 LANGUAGES CXX)` in `CMakeLists.txt` via a
  `KEEBY_VERSION` compile definition on `keeby_core` (so every translation
  unit linking it, including every test binary, gets the same literal).
  `device_connected` forwards `EngineController::device_connected()`.
- Signal `StateChanged()`: no arguments. `ControlService::notify_state_changed()`
  emits it (best-effort, never throws, mirrors `TrayService::notify_changed`).
  `main()`'s single `refresh_and_mark_dirty` closure -- already the one
  place every state-changing path (tray Activate/Scroll/menu-click, every
  `ControlService` mutating handler, a successful profile switch) already
  funnels through -- now also calls `control->notify_state_changed()`, so
  every source is covered by one line instead of threading a callback
  through each call site individually.

`keeby ctl tone <x> <y>` (each `-100..100` percent, reusing the existing
`parse_percent` helper): calls `SetTone(x/100, y/100)`, prints
`tone=X,Y` (percents, rounded). `keeby ctl status` now appends
` tone=X,Y` to its existing `enabled=... volume=... width=... profile=...`
line (two separate `GetState`/`GetTone` calls -- `GetState`'s reply shape
is unchanged, per "existing methods unchanged").

## 4. Tray menu: Keeby's order and grouping

Top level, in order: **Sound on** (checkmark, id 1) -> **Volume** submenu
(id 10, unchanged presets 11-14) -> **Switches** submenu (id 30,
regrouped, see below) -> **Stereo width** submenu (id 20, unchanged presets
21-25, moved after Switches) -> separator (id 40) -> **Settings…** (new,
id 42) -> **Quit** (id 41, unchanged). The tray's own styling (checkmarks,
radios, disabled greying) is drawn by the host shell; only structure,
order and labels are this step's responsibility.

**Switches submenu regrouping** (`build_tray_menu` in `tray_service.cpp`):
"Default (built-in)" always first (the `id == "default"` entry, never
grouped). Every other pack is grouped by brand = the first word of its
display name (`"Cherry MX Blue"` -> `"Cherry"`). A brand with 2+ packs gets
a disabled header item (its brand name) followed by its packs, sorted by
full name. A brand with exactly one pack is not
worth its own header -- it's collected into a final, sorted `"Other"`
group instead. Groups themselves are ordered alphabetically by brand
(`std::map<std::string, ...>` iterates sorted). The existing
`kTrayIdProfileBase + index` scheme is preserved exactly (`index` is the
pack's position in the *original*, ungrouped `TrayMenuState::profiles`
snapshot, not a group-local position) so `dispatch_tray_menu_click`'s
existing snapshot-resolution logic needs no changes. Header items are
`MenuItem::Kind::Standard`, `enabled = false`, ids from
`kTrayIdBrandHeaderBase` (1000) up -- disabled and non-clickable, per the
brief; `dispatch_tray_menu_click`'s existing bounds check
(`idx < profiles.size()`) already makes these (and any other out-of-range
id) inert with no special-casing needed. The fetch hint (`"More sounds:
run keeby-fetch-packs"`) is unchanged: still the last child of Switches,
still only shown when exactly one profile (`"default"`) is installed.

**Settings…**: `TrayActions::open_settings`, wired in
`TrayService::handle_menu_click`, launches `keeby-settings` via
`posix_spawnp` (argv-only, `{"keeby-settings", nullptr}`, no shell). This
process never `waitpid()`s on it (truly "detached" would need a
double-fork/session-detach; here "detached" means this process doesn't
track or wait for it) -- `main()` sets `SIGCHLD` to `SIG_IGN` at startup
(one line, next to the existing `SIGINT`/`SIGTERM` handlers) so the kernel
reaps the child directly instead of it becoming a zombie under this
long-running service. A failed spawn prints to stderr only, never fatal.

## 5. Tests added/updated

- `tests/sample_mixer_test.cpp`: `test_tone_default_and_clamping`,
  `test_tone_bypass_exact`, `test_tone_response_sanity`,
  `test_tone_concurrent_publish` (the TSAN target).
- `tests/settings_test.cpp`: tone round-trip (added to the existing
  round-trip test) and `test_tone_clamping`.
- `tests/control_service_test.cpp`: `test_tone` (ctl subcommand + status
  line), `test_list_profiles_detailed`, `test_get_info`,
  `test_state_changed_signal` (uses `sdbus::createProxy`, not the
  lightweight proxy every other test here uses, since signal delivery
  needs a real event-loop thread), plus a `SetTone` NaN case in the
  existing `InvalidArgument`/`ProfileFailed` test. `FakeState` gained
  `tone_x`/`tone_y`/`profiles_detailed`/`connected`.
- `tests/tray_menu_test.cpp`: replaced the old single switch-menu test with
  `test_switch_menu_default_and_fetch_hint` and
  `test_switch_menu_brand_grouping` (multi-pack brand + singleton "Other"
  + id-follows-original-index assertions), renamed
  `test_separator_and_quit` -> `test_separator_settings_and_quit` (adds
  the Settings item and the new top-level order), and extended the
  dispatch tests for `open_settings` and a disabled brand-header id.

## 6. Verification (inside `$WT`)

- Release: `ctest --test-dir build/rel` -- 12/12 passed, no new warnings.
- ASAN+UBSAN (Debug, `-fsanitize=address,undefined -fno-omit-frame-pointer`):
  12/12 passed, no sanitizer reports.
- TSAN (`-fsanitize=thread`, all 38 targets built including
  `fake_inputd_test_helper`): full `ctest --test-dir build/tsan` -- 12/12
  passed; `sample_mixer_test` additionally run 8 times directly, all
  clean (the concurrent-publish test is what originally caught both races
  described in section 1).
- `git add -A -- ':!build' && git diff --cached --check HEAD`: clean.

## 7. Limitations / out of scope

- No crossfade on a tone change -- acceptable per the brief; a settings
  window drag is many small steps, not one jump.
- `retired_tone_`'s GC is conservative (see section 1): under a
  pathological writer that always outruns the RT thread, the list can grow
  unboundedly. In practice `set_tone()` is called from UI/D-Bus interaction
  (at most tens of times per second even during a drag), vastly slower
  than `mix()`'s callback rate, so this is a non-issue -- not fixed further
  here (`ponytail`: no bounded-wait/priority scheme added without a
  demonstrated need; add one if a future profiling run shows otherwise).
- `keeby-settings` itself (the GTK4 window) is a separate parallel work
  stream; this step only implements the D-Bus contract and the tray's
  launcher.
