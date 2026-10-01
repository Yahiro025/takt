# 019 — Settings window scrolling

Date: 2026-10-01
Status: implementation, automated checks, package 0.1.0-11, and headless GTK geometry/scroll reachability are verified; user desktop visual acceptance is pending

## Goal

Keep the settings window at its existing 440×560 default size and keep the tab
switcher visible while allowing long tab content to scroll. With 45 profiles,
the Sound tab's list must not make the General tab force the window taller than
the screen.

## What changed

- Put the tab stack in a `GtkScrolledWindow` below the fixed title bar. Vertical
  scrolling is automatic and horizontal scrolling is disabled.
- Turned off the stack's vertical homogeneity so the inactive Sound page does
  not set the General page's height.
- Bumped the package release from 10 to 11.

## What was verified

- Release: all targets built; 19/19 tests passed in 5.62 seconds.
- ASAN/UBSAN: all targets built; 19/19 tests passed in 7.22 seconds.
- TSAN: all targets built; 15/15 tests passed in 9.75 seconds. The four
  GTK/GLib tests are excluded as required by AGENTS.md.
- Package `0.1.0-11` built; its retry of the package check passed 19/19 in
  5.34 seconds. SHA-256:
  `ce85e103154a265fb1c415963cb345598c8ca7222242a93abde7753292966c1b`.
- The packaged `keeby --check-profiles` accepts all 45 profiles.
- The merged Release UI passed a headless geometry probe using 45 actual
  profile buttons over private TCP Broadway and headless Chromium. General,
  Sound, Visualizer, then General again were checked at 472×560. Before the
  change, General measured 3148px high.
- In Sound, the upper content measured 3100px and the page 512px; scroll travel
  was 2588px. The last row was at y=435 with height 34px inside the 512px
  viewport, and `last_row_fully_visible` was `1`.
- Cropped after-change views are `root-after-general-initial.png` and
  `root-after-sound-bottom.png`; logs and images are under
  `build/research/settings-scroll-20261001/`. This headless check does not
  establish visual acceptance on the user's desktop.
- The first package check timed out in `audio_boundary_test` after 15.02
  seconds with no output. A focused recheck passed in 0.54 seconds, then a
  controlled full package retry passed. The cause of the first timeout is
  unknown.
- Extracted `keeby` and `keeby-inputd` binaries are byte-identical between
  package 10 and 11; only the settings binary changed. Package 11 is not
  installed; `pacman -Q keeby` reports `0.1.0-8`.
- Release, sanitizer, package, and timeout-recheck logs are under
  `build/research/settings-scroll-20261001/`.
- GTK geometry and scroll-reachability QA is still pending. No desktop visual
  PASS is claimed.

## Still open

- Install package 11 and close and reopen any settings window that was already
  open. On the user's desktop, confirm General fits at the normal window size
  and the Sound list scrolls through all 45 profiles while the title bar stays
  visible. Check the Visualizer tab at the same size.
- Complete the live helper security, pointer/Fn, listening, and exact
  cursor-tracking checks in HANDOFF.md §§6f–6g.
