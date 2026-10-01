# KEEBY — Design System (DSD)

Status: 2026-09-26. This is the design system future UI work (settings window, tray, visualizer) must follow. Values below are taken directly from the shipped CSS/cairo drawing code — **the code wins** over this document if they ever disagree; update this file, not the other way round. Values marked **pending (not merged)** live only in `build/wt/overlay/` and are not yet in `src/`.

## 1. Design principles

1. **Keeby-faithful, not Keeby-copied.** Match Keeby's (getkeeby.com) look, feel and structure — dark UI, floating keyboard, tone pad, tray — but every asset is original: no Keeby logo, wordmark, keycap art, or sound recordings (`docs/012` §6, `HANDOFF.md` §1).
2. **Dark only.** There is no light theme. Every surface is a shade of near-black/dark-grey; the only saturated colors are the accent blue, status colors, and the per-pack swatch palette.
3. **Quiet.** Flat fills, hairline dividers (8% white), no heavy borders or gradients except the one deliberate accent gradient on a selected position tile.
4. **Fast.** All motion is short (100–250 ms) and easing-based; nothing blocks a UI gesture on D-Bus — every control updates optimistically and throttles its outgoing calls.
5. **Original assets only.** Icon, glyphs, and switch-color palette are hand-built, not sourced from Keeby (`docs/012` §6).

## 2. Color tokens

| Token | Hex / value | Usage | Source |
|---|---|---|---|
| Window | `#222222` | Settings window background | `src/ui/app.cpp` CSS |
| Card | `#292929` | Cards, "not running" panel, unselected position tile | `app.cpp` CSS |
| Track | `#3D3D3D` | Slider trough, tone-pad background | `app.cpp` CSS; `sound_controls.cpp` `pad_draw` |
| Accent | `#197EF5` | Switch (on), slider fill/highlight, "Start" button | `app.cpp` CSS |
| Tab pill (rest) | `#28272A` | Segmented General/Sound/Visualizer switcher background | `app.cpp` CSS |
| Tab pill (active) | `#4B4B4B` | Active tab fill — **not** the accent color | `app.cpp` CSS |
| Divider | `alpha(#ffffff, 0.08)` | Hairline row dividers | `app.cpp` CSS |
| Primary text | `#ffffff` | Headings, body labels | `app.cpp` CSS |
| Secondary text | `alpha(#ffffff, 0.55)` | Subtext under a heading/row | `app.cpp` CSS `.kb-secondary` |
| Mono value text | `alpha(#ffffff, 0.7)` | Numeric value labels (`.kb-mono`) | `app.cpp` CSS |
| Error | `#E0665B` | Inline `SetProfile` failure text | `app.cpp` CSS `.kb-error` |
| Success dot | `#3ABE6E` | Keyboard Access "Granted" | `app.cpp:186` |
| Warning dot | `#E0A63A` | Keyboard Access "No keyboard" | `app.cpp:189` |
| Row hover | `alpha(#ffffff, 0.05)` | `.kb-row:hover` | `app.cpp` CSS |
| Tile hover | `#333333` | Position tile hover | `app.cpp` CSS |
| Tile selected | gradient `135deg #174079 → #28313C`, border `#3E8EF7` | Selected position tile | `app.cpp` CSS |
| Switch swatch palette (10) | `#5B8DEF #3ABEA0 #E0A63A #D0668A #8A6FD1 #4FB0C6 #E08A4F #6FA85B #C05B5B #5B7BC0` | Deterministic per-pack-id color (FNV-1a hash → palette index) | `src/ui/settings_logic.hpp::color_for_id` |
| Volume pill fill | `#ffffff`, outline `alpha(#000,0.08)` | Pill base | `sound_controls.cpp` `pill_draw` |
| Volume pill unfilled overlay | `alpha(#000, 0.35)` | Covers the unfilled portion from the top | `sound_controls.cpp` |
| Volume pill / tone pad label text | `alpha(0.25,0.25,0.25,0.9)` (pill), `alpha(#fff,0.5)` (pad) | Labels drawn on top of the fill | `sound_controls.cpp` |
| Tone-pad dot grid / crosshair | `alpha(#fff,0.12)` dots, `alpha(#fff,0.18)` axes | Guide marks | `sound_controls.cpp` `pad_draw` |
| Tone-pad knob | `#ffffff` fill, `alpha(#000,0.35)` stroke | Draggable knob | `sound_controls.cpp` |
| **Visualizer (live, merged)** panel | `#2B2B2B`, border `alpha(#fff,0.08)` | Current shipped overlay panel | `src/visualizer/viz_app.cpp:111,114` |
| **Visualizer (live, merged)** keycap | `#3A3A3A` rest → near-white pressed | Current shipped keycaps | `viz_app.cpp:127-128` |
| **Visualizer panel bg — pending (not merged)** | `#1C1C1E` | `build/wt/overlay/src/visualizer/viz_app.cpp:55` |
| **Visualizer panel border — pending (not merged)** | `#3A3A3C` | same file:56 |
| **Visualizer keycap bg — pending (not merged)** | `#2C2C2E` | same file:57 |
| **Visualizer ripple grey — pending (not merged)** | `#6A6A6E` (mid of a `#5A5A5E`–`#7A7A7E` band) | same file:58 |
| **Visualizer keycap lit/pressed — pending (not merged)** | `#F2F2F7` | same file:59 |
| **Visualizer legend — pending (not merged)** | `#8E8E93` rest, `#1C1C1E` on a lit key | same file:60-61 |

## 3. Typography

- No `font-family` is pinned for ordinary GTK labels — they inherit the system UI sans stack. **Design intent going forward: Inter, with fallback to the platform sans-serif stack** — the pending overlay code already requests `"Inter"` explicitly for the visualizer legend (`build/wt/overlay/.../viz_app.cpp:332`, falling back to cairo's default match if Inter isn't installed); the settings window CSS should adopt the same `font-family: Inter, sans-serif;` rule at its next touch.
- Cairo-drawn custom widgets currently hard-code `"sans-serif"` (volume pill label bold 11px, tone-pad corner labels normal 10px, live visualizer legend normal 8px).
- Monospace is reserved for numeric/value labels only: `.kb-mono` (`font-family: monospace; font-size: 12px; color: alpha(#fff,0.7)`), e.g. the Stereo Width and Dismiss-After value readouts.
- Sizes/weights: heading `13px/600`, body `13px/400`, secondary `11px/400`, error `11px/400`, switcher-tab label `13px/600`.

## 4. Spacing & radius

| Element | Radius | Padding/size |
|---|---|---|
| Card (`.kb-card`) | 10px | `4px 12px` |
| Segmented switcher pill | 999px (full) | outer `2px`, per-tab `4px 16px` |
| Row button (`.kb-row`) | 8px | — |
| "Not running" panel | 10px | 16px |
| Start button | 8px | `6px 16px` |
| Position tile | 10px | — |
| Slider trough | 6px | min-height 6px |
| Slider thumb | 50% (circle) | 16×16px |
| Tone pad | 12px | 176×176px square |
| Volume pill | full pill (`w/2`) | 64×176px |
| Visualizer panel (live) | 10px, key 3px | cell 30px/unit |
| Visualizer panel (pending) | 8px, key 3px | cell 18px/unit, ~290×110px panel |
| Settings window | fixed, non-resizable | 440×560px |

## 5. Elevation / shadow

- Settings window has no drop shadow of its own (GTK header bar + flat cards, no elevation system beyond the 8%-white hairline dividers).
- The visualizer panel is the one place with a real shadow: a single blurred black pass, `alpha 0.35`.
  - Live/merged: shadow drawn as an extra rounded-rect pass behind the panel (`viz_app.cpp:103-106`), offset only implicitly via padding.
  - Pending (not merged): a real box-blurred shadow surface, blur radius 24px, vertical offset +10px, alpha 0.35, rendered once at startup and re-blitted per frame (`build/wt/overlay/.../viz_app.cpp:62-64, 234-244`); the shadow itself is never tilted even when the panel is (deliberate simplification, noted in the overlay code).

## 6. Motion tokens

| Token | Value | Where | Status |
|---|---|---|---|
| Hover transitions (row, tile, switcher tab) | 100–150ms ease | `.kb-row`, `.kb-tile`, `.kb-switcher-tab` CSS `transition:` | merged |
| Switch toggle | 120ms ease | `switch` CSS | merged |
| Visualizer key ease-back (pressed→released) | 150ms (live) / 120ms (pending) | `viz_app.cpp` `kEaseMs` | merged / pending |
| Visualizer show/hide fade | 150ms opacity (live); 150ms opacity+scale (pending) | `kFadeMs` | merged / pending |
| Visualizer appear scale | — (live: none) / 0.96 → 1.0 (pending) | `kAppearScaleFrom` | pending |
| Animation tick | ~60fps (16ms) | `kTickMs` | merged / pending |
| Spring follow (cursor-follow) | critically damped, ω = 25 rad/s, settles ~230ms (within the 150–250ms target) | `viz_spring.hpp::Spring1D` | pending, not merged |
| 3D tilt mapping | gain 0.01°/(px/s), clamp ±14° (brief: "up to ~12–15°") | `viz_spring.hpp::tilt_from_velocity` | pending, not merged |
| Ripple duration | 400ms (brief: 350–450ms) | `viz_ripple.hpp::kRippleDurationMs` | pending, not merged |
| Ripple speed / falloff | 0.006 key-units/ms (~2.4 units over the duration); Gaussian ring thickness σ = 0.6 units | `viz_ripple.hpp` | pending, not merged |
| Ripple pool | 16 concurrent ripples, oldest recycled, never allocates | `viz_ripple.hpp::kMaxRipples` | pending, not merged |
| Cursor accel curve | 1:1 below 1.0 counts/ms, ramps to 2.2× at ≥15 counts/ms, times user Follow Speed | `cursor_estimator.hpp::accel_gain` | pending, not merged |
| Follow-cursor panel offset | panel top-left sits +12px x / +18px y from the estimated cursor tip (cursor reads as above-left of the panel) | `kFollowOffsetX/Y`, overlay `viz_app.cpp` | pending, not merged |
| D-Bus drag throttle | ~30/s (33ms), always flushes on drag-end | `Throttle` (`sound_controls.cpp`, `settings_logic.hpp`) | merged |

## 7. Components

- **Segmented tab switcher**: two-to-three linked `GtkToggleButton`s in a `.kb-switcher` pill (General / Sound / Visualizer), driving a `GtkStack`. Active tab = `#4B4B4B` fill, not accent (`docs/012` §3, `docs/013-visualizer-app.md`).
- **Card + rows**: `.kb-card` (`#292929`, 10px radius) hosting `.kb-row` buttons; an 8%-white `.kb-divider` separates rows within a card.
- **Toggle**: standard `GtkSwitch`, accent `#197EF5` when on, 120ms transition.
- **Slider**: `GtkScale` with a `#3D3D3D` trough, `#197EF5` fill/highlight, white 16px circular thumb (Stereo Width, Dismiss After).
- **Volume pill**: hand-drawn `GtkDrawingArea` + `GtkGestureDrag`, 64×176px, white base with a black translucent overlay covering the *unfilled* top portion (so level reads as "how much white shows", not an accent bar), label + speaker glyph drawn last in dark grey (`sound_controls.cpp`).
- **Tone pad**: 176×176px `#3D3D3D` square, dot grid + crosshair guides, white draggable knob. **Axis mapping**: `x ∈ [-1,1]` Thock (−1, left) ↔ Clack (+1, right); `y ∈ [-1,1]` Warm/Thock (−1, bottom) ↔ Bright (+1, top). **Corner labels**: top-left Warm, top-right Bright, bottom-left Thock, bottom-right Clack (`docs/012` §4, `settings_logic.hpp`).
- **Switch list rows**: brand-grouped (first word of the pack's display name), a colored square from the 10-color swatch palette, name, and a spinner (in flight) or checkmark (selected); a disabled brand-header row when a brand has 2+ packs, otherwise folded into a trailing "Other" group (`docs/011` §4).
- **Position tiles**: 6-tile grid (2 rows × 3, or a 4+3 layout when Follow Cursor is present, per `HANDOFF.md` §6b), each a `GtkButton` with a small cairo screen icon; selected tile gets the navy gradient + `#3E8EF7` border.
- **Tray menu structure** (`src/tray_service.cpp::build_tray_menu`, exact return order): **Control** — Sound on (checkmark) → Volume submenu (25/50/75/100% radios) → **Configure** — Switches submenu (brand-grouped radios + fetch hint) → Stereo width submenu (Mono/Narrow/Normal/Wide/Extra-wide radios) → Visualizer (checkmark) → Position submenu (6 radios) → separator → **App** — Settings… → Quit.
- **Visualizer keyboard**:
  - *Layout*: compact 60%-style key-rect table (`viz_layout.hpp`), cell size 30px/unit live (~a larger panel) vs. 18px/unit pending (~290×110px, "noticeably smaller" per the brief).
  - *Key states*: rest shade → highlight on press, easing back over 150ms (live) / 120ms (pending) after release; a key not in the layout table still resets the dismiss timer even with nothing to light.
  - *Ripple* (pending only): a water-drop ring expands outward in key-units from the pressed key's center, brightening nearby keycaps toward the ripple-grey band then fading, over ~400ms.
  - *Tilt* (pending only): panel yaw/pitch driven by cursor velocity, clamped ±14°.
  - *Shadow*: soft, offset down; the shadow itself is never tilted (pending implementation's deliberate simplification).
  - *Fade*: 150ms opacity fade on show/hide; pending version adds a 0.96→1.0 scale-up on appear.
  - *Cursor offset*: panel anchors 12px right / 18px down from the estimated cursor tip (pending, not merged).

## 8. Accessibility notes

- Text-on-fill contrast: primary white text on `#222222`/`#292929` cards is comfortably >10:1; secondary text is white at 55% alpha over the same dark backgrounds — still legible, but do not drop it further.
- Corner/legend labels drawn via cairo (tone-pad corner labels at `alpha(#fff,0.5)`, visualizer key legends at `alpha` shading) are small (7-10px) — keep them at or above 10px wherever a new label is cairo-drawn; the visualizer's 7-8px legends are an intentional exception scaled to the compact panel, not a pattern to copy into settings-window text.
- Status is never color-only: the Keyboard Access row pairs its green/amber dot with a text label ("Granted" / "No keyboard"), and switch-list selection pairs its checkmark with the row's own name text — don't add a color-only status indicator elsewhere.
- The volume pill and tone pad are drag-only custom widgets with no keyboard alternative today — a future pass should give them arrow-key stepping if built out further.

## 9. Do / Don't

**Do**
- Reuse the existing token values above (hex, radius, timing) instead of inventing new ones for a new control.
- Keep all new color/asset work original — no Keeby wordmark, logo, keycap art, or sound files.
- Keep the accent (`#197EF5`) reserved for on/active/primary-action states; don't reuse it for the active tab pill (that's `#4B4B4B` by design).
- Keep visualizer motion values inside the brief's ranges (hover 100-150ms, ripple 350-450ms, tilt ≤~15°) when finishing the pending overlay work.
- Mark anything sourced from `build/wt/overlay/` as "pending (not merged)" until the supervisor merges and re-verifies it live.

**Don't**
- Don't add a light theme or per-widget one-off colors outside this token table.
- Don't block a UI gesture on a synchronous D-Bus call — every control here is async + throttled.
- Don't give the RT audio/engine code any of this — GTK4 and this design system are confined to `keeby-settings` and `keeby-visualizer` only (`HANDOFF.md` §2).
- Don't treat the live-merged visualizer's simpler look (`#2B2B2B`/`#3A3A3A`, no ripple/tilt/cursor-follow) as final — it's the pre-overlay baseline, superseded once `build/wt/overlay/` merges.
