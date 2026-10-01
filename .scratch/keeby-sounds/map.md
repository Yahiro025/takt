# Keeby exact sounds and doubled volume

Label: wayfinder:map
Status: open

## Destination

Identify every sound currently offered by Keeby, import exact recordings where reuse is supported, and identify all remaining gaps. Double the output amplitude of every current KEEBY sound while keeping the existing volume control and output limit.

## Notes

- Use the [Wayfinder skill](/home/yahiro/.gemini/config/plugins/matt-pocock/skills/wayfinder/SKILL.md), [Parallel Deep Research skill](/home/yahiro/.agents/skills/parallel-deep-research/SKILL.md), and the local Markdown tracker.
- The user explicitly requests implementation. This map therefore includes execution of the volume change and import of verified exact recordings, rather than planning alone.
- User choice: “Exact recordings where available; mark gaps.” Do not insert substitute sounds or infer exact equivalence from matching switch names.
- Keep research findings separate from local build evidence and the user's listening check. Availability and permission to reuse are separate questions.
- Raw key events stay private. Preserve the existing helper privilege scope and real-time audio rules in AGENTS.md.
- On 2026-10-01 the user approved left/right/middle button press/release events from pointer devices the helper already opens. The implementation uses existing post-drop libinput and the private helper socket; it does not expand device opens, privileges, writes/grabs, D-Bus, or logging. See docs/006 §48. Other pointer event classes remain outside scope.
- `KEY_FN` (evdev 464) is now an explicitly supported sound slot so exact native Fn recordings can be mapped. Codes 256..271 and 275..463 remain unsupported; the visualizer does not receive codes >=256.
- The optional Parallel CLI 0.9.3 authentication expired before any research job launched. No job launched; no run ID or Parallel report was produced. Research uses the direct site bundle, downloaded official DMG, source hashes, and attribution file.
- The user pack directory now contains 26 imported profiles: 25 DMG-based packs and a separate website Brown OGG sprite pack. The OGG was copied with its source hash; it is not claimed equivalent to the 26 native Brown WAV files. User import audit and 45/45 profile checks are in report 018 and research report 002.
- “Double” means 2× sample amplitude before the existing output limit. It is not a promise that the sound will seem twice as loud to a listener.
- Child tickets live in `issues/`; dependencies use their `Blocked by:` lines. Resolve tickets only with recorded evidence.

## Decisions so far

- User choice remains: “Exact recordings where available; mark gaps.” Do not infer identical audio from matching names.
- The mouse decision ticket was resolved by the user's exact answer, “Yes, add mouse button events.” Its data/capture boundary is recorded in section 48 of docs/006.
- The user's approved source handling is local import only. Public access does not by itself establish reuse rights. The captured attribution terms and their limits are in [research report 002](../../docs/research/002-keeby-native-sound-catalog.md).
- The 25 DMG-based packs and one website Brown sprite pack passed import and profile checks. The new profiles load without warnings; source hashes, exact mapping gaps, and limits are recorded in report 018 and research report 002. Live installation, security, and listening acceptance remain open.
- Amplitude means twice the sample amplitude before the existing output clamp, not a promise of twice perceived loudness.

## Not yet specified

The `blip` and `glyph` source folders are confirmed, but their complete Mac chooser labels and runtime selection are not. Local labels are marked as inferred. UI/notification MP3s and tick/stray mouse WAVs also have no verified current Linux event mapping.

## Out of scope

Similar recordings presented as exact Keeby sounds; UI redesign; helper privilege changes; account creation, purchases, or access-control bypass; system-wide audio changes; committing or publishing this work.
