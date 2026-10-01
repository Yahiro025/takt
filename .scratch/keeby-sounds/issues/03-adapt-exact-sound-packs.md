# Adapt verified exact recordings to KEEBY

Label: wayfinder:task
Type: task
Mode: AFK
Status: resolved
Assignee: none
Parent: [Keeby exact sounds and doubled volume](../map.md)
Blocked by: 01, 02

## Question

Can all available, reuse-supported exact recordings be loaded through the existing sound-pack format with their source key mappings, press/release pairs, and variants preserved? Import supported entries, verify every imported profile loads, and record any mapping or format gaps. Keep keyboard profiles separate from incidental sound effects. Do not substitute missing release recordings or silently flatten distinct special-key sounds.

## Comments

2026-10-01: Resolved after importing 25 DMG-based packs and one separate website Brown sprite pack into the user's pack directory. All 26 new packs passed import checks and load without warnings; development and package binaries accept 45/45 profiles. The source-audit log verifies all 1,113 copied WAVs and the Brown OGG against source hashes, and all 131 pre-existing regular pack files remained unchanged. Gaps remain explicit: Lizard has no keyboard release recordings; the Snappy profile uses generic right/middle mouse recordings; unavailable categories were not synthesized. See [report 018](../../../docs/018-keeby-sounds-and-volume.md), [research report 002](../../../docs/research/002-keeby-native-sound-catalog.md), and `build/research/import-20261001/`.
