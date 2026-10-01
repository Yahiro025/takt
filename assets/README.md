# Step 2.2 test samples

These files are byte-for-byte copies of the frozen Step 1.4 assets in
`experiments/input-reader/assets/`. The feasibility report identifies them
as synthetic ffmpeg-generated sine blips, not keyboard recordings.
The experiment originals remain unchanged.

Both WAVs contain 48 kHz stereo signed 16-bit PCM:

- `click_down.wav`: 1,681 frames (about 35 ms); physical A, S and D presses.
- `click_up.wav`: 1,201 frames (about 25 ms); physical Space and Enter presses.

The historical `click_up` filename does not imply release-triggered playback.
Both categories play only on presses. These short sounds are development
fixtures; no sound selection or profile system is included.

(As of Step 2.6 this key->category mapping is historical: every key presses
with `click_down*`, except the large stabilized keys -- Space, Enter, KP
Enter, Backspace -- which reuse `click_up*` as a deeper press sound. See
`load_default_bank` in `src/sample_mixer.cpp`.)

## Step 2.3 variants

Each category now has the original plus `_low.wav` and `_high.wav`.
`generate_variants.py` creates those four files from the unchanged originals:
linear interpolation at source-frame increments of 0.92 and 1.08, written back
as 48 kHz stereo PCM16. Pitch changes by about -8% / +8% (roughly 1.3-1.4
semitones), with duration changing inversely. These are three pitches of the
same synthetic blip, not different recorded keyboard strikes. Interpolation
is offline; the application only decodes the resulting WAVs before startup
and plays them at unity frame rate. The rates were widened from an initial
±3% after live listening found that smaller shift imperceptible.

```sh
python3 assets/generate_variants.py
python3 assets/generate_variants.py --check
```

The check validates interpolation cases and byte-compares all four shipped
variants. Runtime gain variation (0.35..0.65) is independent of variant choice.

## Step 2.6 release samples (placeholders)

`release.wav` is derived from `click_down.wav`, not recorded: keep the first
~40 ms, apply a first-difference high-pass (`y[n] = x[n] - x[n-1]` per
channel) to turn the low "thock" into a brighter tick, normalize the result
to about -10 dB relative to `click_down.wav`'s own peak, and fade the tail
out over the last quarter to avoid a truncation click. `release_low.wav` and
`release_high.wav` are then generated from `release.wav` by the same 0.92 /
1.08 interpolation as every other pitch variant. All three are covered by
`generate_variants.py --check`.

Every key's release range points at these three files (see
`load_default_bank` in `src/sample_mixer.cpp`). They exist so key-up has
*some* distinct, quieter sound in Step 2.6; they are not meant to sound like
a real keyswitch release and are expected to be replaced wholesale by
recorded release samples in a Step 2.7 sound pack.
