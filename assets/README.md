# Step 2.2 test samples

These files are byte-for-byte copies of the frozen Step 1.4 assets in
`experiments/input-reader/assets/`. The feasibility report identifies them
as synthetic ffmpeg-generated sine blips, not keyboard recordings.
The experiment originals remain unchanged.

Both WAVs contain 48 kHz stereo signed 16-bit PCM:

- `click_down.wav`: 1,681 frames (about 35 ms); physical A, S and D presses.
- `click_up.wav`: 1,201 frames (about 25 ms); physical Space and Enter presses.

The historical `click_up` filename does not imply release-triggered playback.
Step 2.2 plays both files only on presses. These short sounds are development
fixtures; no sound selection or profile system is included.
