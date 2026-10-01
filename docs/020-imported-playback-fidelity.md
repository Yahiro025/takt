# 020 — Imported playback fidelity

Date: 2026-10-01
Status: imported-playback fix is merged and automated checks/package build passed; package 0.1.0-12 is built but not installed; live listening remains pending

## Goal

Investigate why exact imported recordings sound weak or uneven. Compare the current website browser path, local pack files, the Linux loader, and what can be verified from the native Mac binary. Keep the fix limited to the source of the demonstrated processing mismatch.

## Finding

All 1,114 copied files in 26 local pack directories match their pinned sources; this is copy integrity, not proof every file is selected or played. The confirmed processing mismatch is that the Linux playback baseline shipped in installed package 0.1.0-11 normalized a whole bank from one peak and created synthetic ±8% pitch variants, while the website's standard WAV path normalizes each decoded WAV separately from its largest 10 ms RMS window and uses fixed down/up profile gains. Several imported profiles have very quiet alpha files next to a source-bank peak near 0.9; the existing whole-bank calculation therefore applies almost no gain to those alpha clips. See [research report 003](research/003-keeby-playback-fidelity.md) for the primary-source trace, exact formulas, 26-pack hashes, category maps, and limits.

This is a verified code-path difference and a credible cause of the reported low and uneven output. The user's audible symptom has not been reproduced with a controlled listen, and this does not prove that the mismatch explains every sound difference. The Mac binary exposes a separate audio pipeline and normalization getter, but its numeric runtime behavior is not known. The website browser path is not evidence for the Mac app's output.

## Bounded change

The authorized change applies only to imported pack playback:

- Apply the verified website B7 per-WAV RMS rule to native WAV source clips.
- Keep each source clip as one source variant; remove generated ±8% pitch variants and randomized per-trigger gain from the imported path.
- Use the verified website down/up phase gains and profile factors recorded in report 003.
- Apply the verified F7-style 3 ms attack and 20 ms tail envelope to imported WAV playback.
- Keep the Brown OGG sprite unnormalized, because the website decodes that sprite without B7.
- Keep the existing 2× master-volume multiplier and user tone/width controls. Leave built-in and legacy pack playback unchanged.

No claim of sound matching follows from these settings. The implementation applies the website F7-style envelope, but it does not add the browser compressor or reduce Linux's 32-voice pool to the website's six-voice cap. Linux also retains user tone/width controls. For a direct comparison with the website's standard key path, set neutral controls with `keeby ctl tone 0 0` and `keeby ctl width 0`: the Linux tone filter is bypassed at `(0, 0)`, and width zero centers samples. The website's standard path has no tone filter or panner. These commands change saved controls, so note current preferences first and restore them after the comparison if needed. Non-neutral settings intentionally differ. Mac app runtime values remain unknown.

## Automated verification and remaining checks

The root merged and checked the implementation. Release passed 20/20, ASAN/UBSAN 20/20, and TSAN 16/16 with the four documented GTK/GLib exclusions. Development and packaged profile checks accepted 45/45 profiles. The independent RED/GREEN policy fixture now passes at output `0.16` with one selected source; 2,700 render rows across 27 profiles, 25 keys, two directions, and two settings had zero mapped-silent results; 67 source-waveform checks passed. A same-command baseline comparison confirmed all 100 legacy Turquoise rows were unchanged. A separate imported-catalog audit loaded 26 profiles, checked 12,864 mapped key-direction entries and 25,480 variant/source/render references, covered 1,249 unique loaded sample entries, and reported zero failures; its 264 empty mappings are documented in report 003 and are outside the representative mapped-silent check. Logs and CSV evidence are in `build/research/fidelity-20261001/`.

Package `packaging/arch/keeby-0.1.0-12-x86_64.pkg.tar.zst` built successfully; its package check passed 20/20 and its binary accepted 45/45 profiles. SHA-256: `7c68cbb1468c872eb668fb17c6a873ae78f3e57413b566c8a500df6c8610238f`. Package 0.1.0-12 is not installed; `pacman` still reports 0.1.0-11. The package contains the merged playback correction. For profiles without an official website `c1` entry (Blip, Glyph, and local Mouse Snappy), the website JavaScript fallback is `1.0`; this is a local compatibility fallback, not a verified Mac profile trim.

Live listening remains a separate acceptance gate. Install package 0.1.0-12, restart the service, confirm `pacman -Q keeby` reports `0.1.0-12`, and run `keeby --check-profiles`. For a neutral browser-path comparison, use the commands above, then compare ordinary and special key presses/releases, mouse buttons, and Mouse Snappy at 0%, 50%, and 100% volume, plus rapid typing. Record the installed version and observations. Do not report a PASS for listening, exact Mac output, or complete Keeby behavior replication without that evidence.

Fresh helper-security evidence, actual pointer/Fn event capture, the desktop settings-window check, and exact cursor-tracking acceptance also remain separate open items in [HANDOFF.md](../HANDOFF.md).
