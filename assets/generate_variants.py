#!/usr/bin/env python3
"""Build the fixed development WAV variants (Step 2.3 pitch variants plus the
Step 2.6 placeholder release samples); stdlib only.

Run with --check to check interpolation/derivation and byte-compare the
shipped files. Never invoked by the production executable or audio callback.
"""
import argparse
import io
import math
from pathlib import Path
import struct
import wave


def interpolate(frames, rate):
    """Linear stereo interpolation, fixed output length, last-frame extension."""
    result = []
    for f in range(math.ceil(len(frames) / rate)):
        position = f * rate
        index = int(position)
        fraction = position - index
        first = frames[index]
        second = frames[min(index + 1, len(frames) - 1)]
        result.append(tuple(round(a + (b - a) * fraction) for a, b in zip(first, second)))
    return result


def variant_bytes(source, rate):
    with wave.open(str(source), "rb") as wav:
        assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) == (48000, 2, 2)
        frames = list(struct.iter_unpack("<hh", wav.readframes(wav.getnframes())))
    assert 0 < len(frames) < 48000  # fixed short fixtures only, not a general converter
    converted = interpolate(frames, rate)
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
        wav.writeframes(b"".join(struct.pack("<hh", *frame) for frame in converted))
    return output.getvalue()


def high_pass(frames):
    """First-difference high-pass per channel: y[n] = x[n] - x[n-1]. Cheap
    way to turn a low "thock" into a brighter, transient-heavy "tick"."""
    result = []
    previous = (0, 0)
    for frame in frames:
        result.append(tuple(a - b for a, b in zip(frame, previous)))
        previous = frame
    return result


def peak(frames):
    return max((abs(v) for frame in frames for v in frame), default=0) or 1


def release_bytes(source):
    """Step 2.6 placeholder release sample: a short, quieter, brighter tick
    derived from a press sample -- not a recording. Keeps the first ~40 ms,
    high-passes it, normalizes to ~-10 dB relative to the source's own peak,
    and fades the tail out to avoid a truncation click. Step 2.7 replaces
    this with real recorded release samples."""
    with wave.open(str(source), "rb") as wav:
        assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) == (48000, 2, 2)
        frames = list(struct.iter_unpack("<hh", wav.readframes(wav.getnframes())))
    source_peak = peak(frames)
    short_frames = frames[:int(48000 * 0.040)]
    filtered = high_pass(short_frames)
    target_peak = source_peak * (10 ** (-10 / 20))  # ~-10 dB relative to source
    scale = target_peak / peak(filtered)
    fade_start = int(len(filtered) * 0.75)
    fade_len = max(1, len(filtered) - fade_start)
    result = []
    for i, frame in enumerate(filtered):
        fade = 1.0 if i < fade_start else max(0.0, 1.0 - (i - fade_start) / fade_len)
        result.append(tuple(max(-32768, min(32767, round(v * scale * fade))) for v in frame))
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
        wav.writeframes(b"".join(struct.pack("<hh", *frame) for frame in result))
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    # Known values, stereo independence, unity, endpoint, and single-frame cases.
    frames = [(0, 8), (8, 0)]
    assert interpolate(frames, 1) == frames
    assert interpolate(frames, 0.5) == [(0, 8), (4, 4), (8, 0), (8, 0)]
    assert interpolate(frames, 1.5) == [(0, 8), (8, 0)]
    assert interpolate([(5, -5)], 0.97) == [(5, -5), (5, -5)]
    folder = Path(__file__).resolve().parent
    # release.wav must exist on disk before the variants loop below derives
    # release_low/high.wav from it, exactly like click_down/click_up.
    release_target = folder / "release.wav"
    release_data = release_bytes(folder / "click_down.wav")
    if args.check:
        assert release_target.read_bytes() == release_data, f"release mismatch: {release_target}"
    else:
        release_target.write_bytes(release_data)
    for category in ("click_down", "click_up", "release"):
        for name, rate in (("low", 0.92), ("high", 1.08)):
            target = folder / f"{category}_{name}.wav"
            data = variant_bytes(folder / f"{category}.wav", rate)
            if args.check:
                assert target.read_bytes() == data, f"variant mismatch: {target}"
            else:
                target.write_bytes(data)
    print("sample_variants: OK (interpolation, stereo, endpoints, seven reproducible assets)")


if __name__ == "__main__":
    main()
