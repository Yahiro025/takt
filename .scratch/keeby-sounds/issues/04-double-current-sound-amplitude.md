# Double every current sound's output amplitude

Label: wayfinder:task
Type: task
Mode: AFK
Status: resolved
Assignee: volume
Parent: [Keeby exact sounds and doubled volume](../map.md)
Blocked by: none

## Question

Apply a 2× amplitude increase in the shared mixer so built-in and imported profiles, including press and release sounds, use the same change. Preserve the existing volume range, mute, output limit, panning, and real-time safety. Record a deterministic regression check and Release, ASAN/UBSAN, and TSAN results. Do not claim twice the perceived loudness or a listening PASS from numerical tests.

## Comments

2026-10-01: The shared mixer now doubles sample amplitude before the existing ±0.8 output clamp. Press/release assertions cover built-in and imported sounds at master levels 0, 0.25, and 1; a mutation check that removed the multiplier failed those assertions. The original per-voice gain range, volume control, mute, pan, tone, and clamp remain. Merged Release 19/19, ASAN/UBSAN 19/19, and TSAN 15/15 passed; TSAN excludes the four GLib tests as required. Package `0.1.0-10` and its 19/19 `check()` were built. Logs and exact limits are recorded in [report 018](../../../docs/018-keeby-sounds-and-volume.md). This does not establish twice perceived loudness or a listening PASS.
