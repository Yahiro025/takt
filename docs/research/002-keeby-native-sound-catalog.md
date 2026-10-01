# 002 — Keeby native sound catalog and local import

Status: source inventory and isolated local importer prepared. This report does not claim a live audio/listening test or prove runtime menu availability in Keeby 1.10.4.

## Sources and method

The official [Keeby website](https://getkeeby.com/) describes the Mac sound app. The site profile list was captured from its public [JavaScript bundle](https://getkeeby.com/assets/index-BP4vxKiu.js) on 2026-10-01. That snapshot contains 22 selectable website profiles; the three entries shown as “Soon” (Cherry MX Blue, Holy Panda X, and Razer Green) are outside that selectable set. The site's asset URL, size, and SHA-256 records are in the ignored local research directory; the native source manifest proposed here is [keeby-native-sound-manifest.json](../../scripts/keeby-native-sound-manifest.json).

For the richer native recordings, the official [Keeby macOS DMG](https://getkeeby.com/download/mac/Keeby.dmg) was downloaded and hashed. SHA-256: `b970cf32d62da69fe38934640b8a7cad4e4547b4b8bc9d2e2be7d164e3b5ae4c`. The DMG identifies Keeby **1.10.4**, build **33**. It was extracted with the cached 7-Zip 26.03 executable at `build/research/keeby-catalog-20261001/archive-tool/usr/lib/7zip/7z`. The extracted files and source archive are local research material under `build/research/keeby-catalog-20261001/`; they are not part of this patch.

The full source inventory is [keeby-native-sound-manifest.json](../../scripts/keeby-native-sound-manifest.json). It contains 873 file records with source path, byte length, SHA-256, format, and use/exclusion role. The DMG Resources tree has **861 WAV and 12 MP3 files**. A separate local verification pass checked the size and SHA-256 of all 873 records against the extracted tree. The manifest also records `Sounds/ATTRIBUTION.md` SHA-256 `6eaa8d8fddad2ab08bf6c2b26f560aff6bd1081f34eb4b8f74a1badd10704edd`. The importer checks each file it copies against this manifest; it does not fetch files from the network.

## Catalog coverage

The native Resources tree has 24 keyboard sound folders: the 22 site profiles plus the two additional folders `blip` and `glyph`. A folder in the DMG does not by itself prove that its name appeared in the Mac app's chooser. The import gives these folders local Keeby pack IDs; the display labels below are local mappings from the manifest. In particular, the Mach-O contains separate strings `quirky`, `blip`, and `glyph`, but no verified contiguous menu text “Quirky Blip” or “Quirky Glyph”.

| Source folder | Local display name | Keyboard WAVs | Recorded categories | Missing release category |
|---|---|---:|---|---|
| `iqunix-mq80` | MQ80 | 32 | `alpha`, `arrow`, `backspace`, `caps_lock`, `command`, `control`, `enter`, `escape`, `fn`, `function`, `modifier`, `number`, `option`, `shift`, `space`, `tab` | — |
| `lofree-flow-2-surfer` | Flow 2 Surfer | 32 | `alpha`, `arrow`, `backspace`, `caps_lock`, `command`, `control`, `enter`, `escape`, `fn`, `function`, `modifier`, `number`, `option`, `shift`, `space`, `tab` | — |
| `lofree-flow-2-void` | Flow 2 Void | 32 | `alpha`, `arrow`, `backspace`, `caps_lock`, `command`, `control`, `enter`, `escape`, `fn`, `function`, `modifier`, `number`, `option`, `shift`, `space`, `tab` | — |
| `lofree-flow-2-pulse` | Flow 2 Pulse | 32 | `alpha`, `arrow`, `backspace`, `caps_lock`, `command`, `control`, `enter`, `escape`, `fn`, `function`, `modifier`, `number`, `option`, `shift`, `space`, `tab` | — |
| `akko-piano-pro` | Piano Pro | 14 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `akko-cs-jelly-black` | CS Jelly Black | 42 | `alpha`, `arrow`, `backspace`, `caps_lock`, `command`, `control`, `enter`, `escape`, `fn`, `function`, `modifier`, `number`, `option`, `shift`, `space`, `tab` | — |
| `akko-v3-pro-cream-yellow` | V3 Cream Yellow Pro | 48 | `alpha`, `arrow`, `backspace`, `caps_lock`, `command`, `control`, `enter`, `escape`, `fn`, `function`, `modifier`, `number`, `option`, `shift`, `space`, `tab` | — |
| `akko-clicky-pink` | Clicky Pink | 44 | `alpha`, `arrow`, `backspace`, `caps_lock`, `command`, `control`, `enter`, `escape`, `fn`, `function`, `modifier`, `number`, `option`, `shift`, `space`, `tab` | — |
| `keychron-k2-max-red` | K2 Max · K Pro Red | 40 | `alpha`, `arrow`, `backspace`, `caps_lock`, `command`, `control`, `enter`, `escape`, `fn`, `function`, `modifier`, `number`, `option`, `shift`, `space`, `tab` | — |
| `keychron-k2-max-brown` | K2 Max · K Pro Brown | 26 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `aflion-carrot` | Carrot Orange | 44 | `alpha`, `arrow`, `backspace`, `caps_lock`, `command`, `control`, `enter`, `escape`, `fn`, `function`, `modifier`, `number`, `option`, `shift`, `space`, `tab` | — |
| `durock-alpaca` | Alpaca | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `gateron-ink-black` | Ink Black | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `gateron-ink-red` | Ink Red | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `gateron-turquoise-tealios` | Turquoise Tealios | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `novelkeys-cream` | Cream | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `drop-holy-panda` | Holy Panda | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `kailh-box-navy` | Box Navy | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `ibm-buckling-spring` | Buckling Spring | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `topre-classic` | Classic | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `alps-skcm-blue` | SKCM Blue | 20 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | — |
| `lizard` | Lizard | 13 | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` | `alpha`, `arrow`, `backspace`, `enter`, `modifier`, `space`, `tab` |
| `blip` | Quirky Blip | 24 | `alpha`, `arrow`, `backspace`, `enter`, `escape`, `modifier`, `space`, `tab` | — |
| `glyph` | Quirky Glyph | 24 | `alpha`, `arrow`, `backspace`, `enter`, `escape`, `modifier`, `space`, `tab` | — |

The categories are the source folder's recorded categories, not a claim that the Mac app maps every Linux key identically. The generated v2 configs use these Linux evdev codes:

| Source category | Linux key codes | Keys |
|---|---|---|
| `arrow` | 103, 108, 105, 106 | Up, Down, Left, Right |
| `backspace` | 14, 111 | Backspace, forward Delete |
| `caps_lock` | 58 | Caps Lock |
| `command` | 125, 126 | Left/Right Meta |
| `control` | 29, 97 | Left/Right Ctrl |
| `enter` | 28, 96 | Main and keypad Enter |
| `escape` | 1 | Escape |
| `fn` | 464 | Fn |
| `function` | 59–68, 87, 88 | F1–F12 |
| `number` | 2–11 | Number row |
| `option` | 56, 100 | Left/Right Alt |
| `shift` | 42, 54 | Left/Right Shift |
| `space` | 57 | Space |
| `tab` | 15 | Tab |
| `modifier` | 42, 54, 29, 97, 56, 100, 125, 126 when no more specific category exists | Generic modifier sound |

For ordinary keyboard packs, alpha is the default; exact category samples are used where present, with alpha as the existing pack fallback. Exact `fn` files map only to Linux `KEY_FN` 464 when present. Nine source profiles have `fn_down` and `fn_up`; actual physical Fn event emission and audible behavior have not been tested.

Lizard has **13 press WAVs and no keyboard release WAVs**. Its pack intentionally omits a keyboard release default, so the loader's normal no-release behavior applies; no replacement recording was invented. Several profiles have fewer recorded categories than the detailed profiles above. Those unmapped keys use the profile alpha sound. The table lists every category family found per source folder.

### Mouse recordings

The Resources root contains seven `mouse_down` and seven `mouse_up` WAVs. Each of the 24 keyboard profiles attaches these exact generic source series to Linux BTN_LEFT 272, BTN_RIGHT 273, and BTN_MIDDLE 274. Thus the same source series is used for all three buttons; this is a local mapping to available input events, not evidence that Keeby has distinct native recordings for each button.

The native `snappy` folder has 48 `left_down` and 48 `left_up` WAVs. A separate `Mouse Snappy (NovelKeys Cream keyboard)` local pack maps those 96 exact files to BTN_LEFT. It uses the exact generic `mouse_down/up` series as an explicit fallback for right and middle buttons. The archive has no distinct Snappy right/middle recordings. The 96 files under `snappy.pre-denoise.bak` are inventoried but excluded from imports as backup copies.

### Remaining packaged audio

The 12 MP3s (`faahh-enter`, `glass-pop-smooth`, `keeby-disable`, `keeby-enable`, five `notch-*` clips, `toaster-ding`, and two `typewriter-*` clips) appear to be UI or notification effects by filename. They are recorded in the manifest as unmapped. No matching current Linux GTK event was established, so the importer does not connect them to UI actions. Seven `tick_*.wav` files and `mouse__02.wav` are also recorded but left unmapped. No event association was verified for them.

The website asset manifest contains 199 files: 198 WAVs and one root-level `sounds/sound.ogg` sprite associated with the Keychron K2 Max Brown website profile. Comparing each of the 198 same-folder/same-filename site WAVs to its counterpart in the extracted DMG shows byte-identical SHA-256 values for all 198. The Brown sprite has no same-named DMG file; the native Brown folder instead has 26 separate WAV files. Their byte/content equivalence to the website OGG has not been established, so the native Brown profile uses those WAVs while a separate optional website profile preserves the OGG itself.

The cached website sprite at `https://getkeeby.com/sounds/sound.ogg` is **1,941,031 bytes**, SHA-256 `10390a4325fe218b005c509cac28c90bdd21c16bfcc303fe57d51d2588efd047`, and begins with the Ogg `OggS` marker. The prepared sprite map contains 85 press and 85 release slices. Its prior config had 84 Linux key-code pairs because Fn was outside the then-supported range; the checked-in [website Brown config](../../scripts/keeby-web-keychron-k2-max-brown.json) adds the exact sprite-map Fn slices at code 464 (`[8036, 92]` down and `[8128, 76]` up), making 85 paired codes and 170 definitions. No mouse defines are included. The source map has no separate numpad-key slices. The checked config SHA-256 is `ae724987139b13fabc88dfed11a4066dcf944ff74a0d8ce8b46a9183c6d9591e`; the ignored research sprite-map SHA-256 is recorded in the manifest.

## Exact local pack IDs

Without the optional website sprite flag, the importer creates the 25 native packs below. With `--web-brown-sprite PATH`, it adds one separate `keeby-web-keychron-k2-max-brown` pack labelled **Keychron K2 Max · K Pro Brown (website sprite)**. Thus the importer creates 25 packs by default or 26 with the exact website Brown OGG added; the optional profile has no mouse mapping.

- `keeby-native-iqunix-mq80` — MQ80
- `keeby-native-lofree-flow-2-surfer` — Flow 2 Surfer
- `keeby-native-lofree-flow-2-void` — Flow 2 Void
- `keeby-native-lofree-flow-2-pulse` — Flow 2 Pulse
- `keeby-native-akko-piano-pro` — Piano Pro
- `keeby-native-akko-cs-jelly-black` — CS Jelly Black
- `keeby-native-akko-v3-pro-cream-yellow` — V3 Cream Yellow Pro
- `keeby-native-akko-clicky-pink` — Clicky Pink
- `keeby-native-keychron-k2-max-red` — K2 Max · K Pro Red
- `keeby-native-keychron-k2-max-brown` — K2 Max · K Pro Brown
- `keeby-native-aflion-carrot` — Carrot Orange
- `keeby-native-durock-alpaca` — Alpaca
- `keeby-native-gateron-ink-black` — Ink Black
- `keeby-native-gateron-ink-red` — Ink Red
- `keeby-native-gateron-turquoise-tealios` — Turquoise Tealios
- `keeby-native-novelkeys-cream` — Cream
- `keeby-native-drop-holy-panda` — Holy Panda
- `keeby-native-kailh-box-navy` — Box Navy
- `keeby-native-ibm-buckling-spring` — Buckling Spring
- `keeby-native-topre-classic` — Classic
- `keeby-native-alps-skcm-blue` — SKCM Blue
- `keeby-native-lizard` — Lizard
- `keeby-native-blip` — Quirky Blip
- `keeby-native-glyph` — Quirky Glyph
- `keeby-native-mouse-snappy` — Mouse Snappy (NovelKeys Cream keyboard)
- `keeby-web-keychron-k2-max-brown` — Keychron K2 Max · K Pro Brown (website sprite), optional

The 24 keyboard folders have 647 unique source WAVs. The active mouse sources add 96 Snappy and 14 generic files, for 757 distinct active WAV sources. The complete manifests also retain 96 excluded backup WAVs, 8 other root WAVs, and 12 MP3s. Common generic mouse files are copied into each keyboard pack, so the local packs contain duplicate references/copies of those 14 source assets; this is why per-pack file totals exceed the count of distinct source assets.

## Reuse and attribution evidence

The packaged `Sounds/ATTRIBUTION.md` names three community contributors—Akko Piano Pro, Keychron K2 Max K Pro Brown, and IQUNIX MQ80—and says redistribution as part of the Keeby macOS application is permitted. That wording does not establish a general redistribution grant for another app. The same file lists these ten `tplai/kbsim` packs under MIT:

- Alps SKCM Blue
- Drop Holy Panda
- Durock Alpaca
- Gateron Ink Black
- Gateron Ink Red
- Gateron Turquoise Tealios
- IBM Buckling Spring
- Kailh Box Navy
- NovelKeys Cream
- Topre Classic

The attribution file points to [tplai/kbsim](https://github.com/tplai/kbsim) and [thock-soundpacks](https://github.com/kamillobinski/thock-soundpacks). The site snapshot marks website reuse permission as `not_verified`, and the attribution file does not establish blanket permission for every recording. Therefore the importer copies recordings only to the user's local pack directory; no WAV, MP3, or OGG audio is added to the source tree or package. This is an evidence statement, not a legal opinion.

## Importer and verification

The dependency-free Python importer is [import-keeby-native-packs.py](../../scripts/import-keeby-native-packs.py). It accepts an already extracted `Contents/Resources` directory and the checked manifest. The optional `--web-brown-sprite PATH` flag imports only the hash-pinned Brown website OGG and its checked-in single-sprite config. It requires the exact size and SHA-256, a regular file with no symlink in its path, and the Ogg `OggS` header. The native WAV limit remains 1 MB; the 1,941,031-byte OGG is allowed only through separate fixed metadata checks. The importer rejects unsafe paths, source symlinks, non-regular files, size/hash mismatches, HTML presented as audio, invalid WAV data, duplicate output names, and native packs over their existing limits. It copies audio bytes unchanged, stages complete packs on the destination filesystem, and publishes each directory with Linux `renameat2(RENAME_NOREPLACE)`. Existing IDs are skipped; even a directory appearing during publication is never replaced. No source audio is embedded in this repository patch.

Offline check:

```sh
python3 scripts/import-keeby-native-packs.py --self-check
```

Result on 2026-10-01: **passed** exact-copy, keyboard/mouse/Fn mappings, all 85 Brown down/up pairs, exact Fn sprite offsets, mouse-map exclusion, skip-existing, refusal to replace a pre-existing empty directory, HTML rejection, source-symlink rejection, and staging cleanup. Fresh isolated imports confirmed 25 packs without the optional flag and 26 with it. In the 26-pack run, the copied `sound.ogg` had the pinned 1,941,031-byte size and SHA-256 and the Ogg marker; `config.json` matched the checked-in config byte-for-byte and contained 170 definitions across 85 key codes, including Fn 464. Neither run wrote to the user's actual pack directory or tested playback.

To repeat the local import with the already downloaded DMG and cached extractor from the research directory:

```sh
RESEARCH_DIR="$PWD/build/research/keeby-catalog-20261001"
SEVEN_ZIP="$RESEARCH_DIR/archive-tool/usr/lib/7zip/7z"
"$SEVEN_ZIP" x -y "-o$RESEARCH_DIR/native-extracted" "$RESEARCH_DIR/Keeby.dmg"
python3 scripts/import-keeby-native-packs.py \
  --native-resources "$RESEARCH_DIR/native-extracted/Keeby 1.10.4 Installer/Keeby.app/Contents/Resources" \
  --web-brown-sprite "$RESEARCH_DIR/packs/keeby-web-keychron-k2-max-brown/sound.ogg"
```

The final command targets the standard user pack directory (`$XDG_DATA_HOME/keeby/packs`, or `~/.local/share/keeby/packs`). Review the generated configs with `keeby --check-profiles` after building the current mouse/Fn input changes. The copied audio bytes remain exact, but the existing loader peak-normalizes each source and makes its ±8% pitch variants; this changes playback from the original Mac/browser path. No sound was auditioned in this research step.
