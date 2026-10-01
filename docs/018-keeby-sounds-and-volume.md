# 018 — Keeby sounds and doubled volume

Date: 2026-10-01
Status: local import and package 0.1.0-10 evidence below are complete; package 0.1.0-11 is the current install target (see report 019); live installation, security, input, and listening checks remain open

## Goal

Research the sound profiles the current Keeby website and native macOS app expose. Use exact recordings where available, record gaps, and avoid claiming a native menu mapping where the evidence shows only an asset folder. Double the numerical output amplitude of every current KEEBY sound through the shared mixer.

The user chose: “Exact recordings where available; mark gaps.” A matching label or sound category alone does not prove two files are the same recording.

## What changed

- `SampleMixer` applies a 2× master-amplitude multiplier to built-in and imported press/release sounds. The existing per-voice gain range (0.35–0.65), master volume control, pan, mute, tone processing, and ±0.8 output clamp remain in place. The multiplier doubles sample amplitude before the existing clamp; it does not promise twice the perceived loudness.
- `keeby-inputd` forwards left, right, and middle button press/release events from pointer devices it already opens. It uses the existing post-drop libinput path, private helper socket, input queue, and mixer. No new device opens or device classes were added. The keyboard visualizer does not receive pointer-button codes or codes above 255.
- Mixer and pack mapping now also support `KEY_FN` (evdev code 464), so exact native Fn recordings can play where present. Other extended gaps 256–271 and 275–463 remain unsupported. Fn events use the already-open keyboard path; this adds no device access.
- The source and hash manifest, local importer, and detailed source catalog are documented in [research report 002](research/002-keeby-native-sound-catalog.md). The importer copied 26 local profiles into the user's pack directory: 25 based on DMG recordings and one separate website Brown sprite. It did not add source audio to this repository or to the Arch package.
- `pkgrel` increased from 9 to 10. The resulting package is `packaging/arch/keeby-0.1.0-10-x86_64.pkg.tar.zst`.

Package 0.1.0-10 build results and its SHA-256 below are historical. The settings-window scrolling change ships in package 0.1.0-11. That package is built and verified but not installed; its results are in [report 019](019-settings-window-scroll.md). Use the package 11 commands below.

## Catalog and exact-recording findings

The public [Keeby site](https://getkeeby.com/) and its captured [profile bundle](https://getkeeby.com/assets/index-BP4vxKiu.js) listed 22 selectable website sound profiles on 2026-10-01. Three displayed entries—Cherry MX Blue, Holy Panda X, and Razer Green—were marked “Soon” and are not counted as selectable. The website asset audit attempted 257 URLs: 199 profile audio files and four incidental audio files were valid audio; 54 other requests returned HTTP 200 with HTML, not audio.

The official [Keeby 1.10.4 macOS DMG](https://getkeeby.com/download/mac/Keeby.dmg), build 33, has SHA-256 `b970cf32d62da69fe38934640b8a7cad4e4547b4b8bc9d2e2be7d164e3b5ae4c`. Its Resources tree contains 861 WAV and 12 MP3 files. All 873 audio files were checked against the local manifest by size and SHA-256. It contains 24 keyboard folders: the 22 website profiles plus `blip` and `glyph`. The Mac binary has separate `quirky`, `blip`, and `glyph` strings, but no verified complete chooser labels or runtime selection for the last two. “Quirky Blip” and “Quirky Glyph” are local inferred labels.

The DMG-based local import contains 24 keyboard packs plus one Mouse Snappy pack paired with the NovelKeys Cream keyboard. A separate website Brown sprite pack (`keeby-web-keychron-k2-max-brown`) adds the exact website OGG recording under the label `Keychron K2 Max · K Pro Brown (website sprite)`; it is not claimed to be the same recording as the DMG Brown folder. The keyboard packs use exact native category recordings where present. Nine have exact Fn press/release files mapped to `KEY_FN` 464; actual Fn key emission and playback on hardware remain untested. Lizard has 13 press recordings and no keyboard release recordings, so its keyboard releases stay silent. Its missing releases were not replaced.

Each of the 24 native keyboard packs maps the seven native `mouse_down` and seven `mouse_up` source files to left, right, and middle Linux button codes 272–274. The same generic source series serves all three buttons; it is not three distinct recordings. The separate Mouse Snappy pack uses all 48 exact left-down and 48 exact left-up files for the left button. The native archive has no distinct Snappy right/middle recordings, so that profile explicitly uses the generic mouse series for those two buttons. This is a documented fallback, not an exact Snappy match.

All 198 website WAV assets have byte-identical SHA-256 values to same-path files in the DMG. The remaining website profile asset is a 1,941,031-byte Brown `sound.ogg` with SHA-256 `10390a4325fe218b005c509cac28c90bdd21c16bfcc303fe57d51d2588efd047`; it has no same-named DMG file. It was imported as a separate website-sprite pack. The native Brown folder contains 26 separate WAVs. No OGG-to-WAV equivalence was tested or claimed.

The DMG attribution file permits three contributors' recordings to be redistributed as part of the Keeby macOS app; that wording does not grant general redistribution in another app. It lists ten `tplai/kbsim` profiles under MIT. The website snapshot marks reuse permission as unverified, and the attribution file gives no blanket grant for all assets. The local importer therefore writes only to the user's personal pack directory. This is a record of the available terms, not a legal opinion.

The remaining seven tick WAVs, one stray mouse WAV, and 12 MP3 files with UI/notification-style names are inventoried but not mapped to current Linux events. No matching GTK event for them has been verified. The import does not claim full Keeby app-behavior replication. The complete category-by-category inventory, key mapping, pack IDs, source URLs, and gaps are in [research report 002](research/002-keeby-native-sound-catalog.md).

## Verification

The captured website catalog and official DMG are primary-source evidence. The optional Parallel CLI 0.9.3 could not complete its expiring OAuth device flow; no job launched, and no run ID or Parallel report was produced. The direct source and hash checks described above were used instead.

| Check | Result |
| --- | --- |
| Site profile/source inventory | 22 selectable website profiles recorded; three “Soon” entries excluded |
| Native audio manifest | 873/873 WAV/MP3 files match extracted size and SHA-256 records |
| Local import safety self-check | Passed exact-copy, skip-existing, no-replace race, HTML rejection, staging cleanup, and mapping checks |
| Temporary import | The 25 DMG-based packs and separate Brown website-sprite pack passed temporary import checks; no user directory was written in those temporary runs |
| User pack import and source audit | 26/26 new packs imported; all 1,113 copied WAVs and the copied Brown OGG match their source hashes; all 131 pre-existing regular pack files remained unchanged |
| Profile loading | Development and extracted package binaries each accept 45/45 profiles; the new packs have no warnings. Existing legacy-pack warnings remain: 10 unknown codes per affected legacy pack and six missing special-key files in `mxblue-travel` |
| Mouse and Fn tests | Press/release mapping, numeric mixer output, centered pan at nonzero stereo width, repeat suppression, visualizer filtering, and unsupported code gaps covered by tests |
| Release | 19/19 tests passed; all targets built |
| ASAN/UBSAN | 19/19 tests passed |
| TSAN | 15/15 tests passed; the four GLib tests are excluded as required by AGENTS.md |
| Package build and `check()` | `keeby 0.1.0-10` built; package `check()` passed 19/19 |
| Package SHA-256 | `0daa3268ac7a12fa68a0f9b6db4c719f43e602a17b642a87aef3c58bc573d8df` |
| Package contents | Helper is `root:input` mode 2750; binaries are mode 755; no external/native audio is bundled |
| Diff and whitespace checks | `git diff --check` passed; untracked text whitespace scan found no matches |

Logs are under `build/research/mouse-20261001/`, `build/research/import-20261001/`, and `build/research/volume-20261001/`. The source inventory and temporary extraction remain under the ignored `build/research/keeby-catalog-20261001/` directory. The archived package is not installed. A fresh `pacman -Q keeby` still reports `0.1.0-8`.

## Still open

- Install `0.1.0-11`, then perform the fresh live helper security check. The package build and old 0.1.0-5 security evidence do not establish the current helper's live privilege state. The user-run commands are in HANDOFF.md §6g.
- Confirm actual left/right/middle and Fn event capture on the user's devices. No `/dev/input` reads or synthetic live input were used for this work.
- Listen to built-in and imported profiles, keyboard releases, mouse buttons, ordinary and rapid typing, and volume values 0%, 50%, and 100%. Include Mouse Snappy left clicks and note that its right/middle sounds use the generic mouse recordings. Record the installed version and the user's observations. No listening or audible-playback PASS is claimed.
- Keep the exact cursor-tracking install, security, and visual checks in HANDOFF.md §6f open. This sound work does not resolve them.

## User live-check commands

Run these after installing package 11. Package 10's build and hash records above remain unchanged; package 11 is the current install target. These commands inspect the current processes and do not read input events.

```sh
sudo pacman -U packaging/arch/keeby-0.1.0-11-x86_64.pkg.tar.zst
systemctl --user restart keeby.service
pacman -Q keeby
keeby --check-profiles

engine_pid=$(systemctl --user show -p MainPID --value keeby.service)
case "$engine_pid" in ''|*[!0-9]*) printf 'No valid service MainPID\n' >&2; exit 1 ;; esac
test "$engine_pid" -gt 0 || exit 1
helper_pid=$(pgrep -P "$engine_pid" -x keeby-inputd) || exit 1
case "$helper_pid" in ''|*[!0-9]*) printf 'Expected exactly one helper PID\n' >&2; exit 1 ;; esac
test "$helper_pid" -gt 0 || exit 1
for pid in "$engine_pid" "$helper_pid"; do
  printf '\nPID %s\n' "$pid"
  grep -E '^(Name|Uid|Gid|Groups|Cap(Inh|Prm|Eff|Bnd|Amb)|NoNewPrivs):' "/proc/$pid/status"
done
```

Confirm `pacman -Q keeby` reports `keeby 0.1.0-11` and `keeby --check-profiles` accepts all 45 profiles. Record the `/proc` values for both processes and compare them with the prior live baseline in HANDOFF.md §6c. Expected values are UID/GID 1000 in all slots, no input-group gid 992, `CapPrm` and `CapEff` all zero, and helper `NoNewPrivs: 1`. The old baseline is not current-package evidence; do not mark the new security check complete until you record these values after installing package 11. The implementation boundary is in [docs/006 §48](006-step-2.5-security-permissions.md).

The negative-open probe is a separate, explicit user action. If the user chooses to run it, send exactly one `SIGUSR1` to the current helper, then inspect only the matching service journal line:

```sh
engine_pid=$(systemctl --user show -p MainPID --value keeby.service)
case "$engine_pid" in ''|*[!0-9]*) printf 'No valid service MainPID\n' >&2; exit 1 ;; esac
test "$engine_pid" -gt 0 || exit 1
helper_pid=$(pgrep -P "$engine_pid" -x keeby-inputd) || exit 1
case "$helper_pid" in ''|*[!0-9]*) printf 'Expected exactly one helper PID\n' >&2; exit 1 ;; esac
test "$helper_pid" -gt 0 || exit 1
kill -USR1 "$helper_pid"
journalctl --user -u keeby.service -b --since '2 minutes ago' --no-pager \
  | grep -F 'keeby-inputd: post-drop open() failed as expected (EACCES) — privilege drop verified live'
```

After installation, close and reopen any settings window that was already open. Check the General tab, scroll all 45 Sound profiles, and confirm the tab switcher remains visible; see report 019. Then listen to ordinary and special keys, press and release sounds, left/right/middle clicks, and Mouse Snappy at 0%, 50%, and 100% volume. Try rapid typing and record any silence, repeats, distortion, or delay. Test Fn only if the user's keyboard emits the mapped event. Exact cursor-follow behavior has its own separate visual check in HANDOFF.md §6f.
