# Verify installed sound coverage and loudness by listening

Label: wayfinder:task
Type: task
Mode: HITL
Status: open
Assignee: none
Parent: [Keeby exact sounds and doubled volume](../map.md)
Blocked by: 03, 04

## Question

After the verified package is installed by the user, do all imported exact profiles play correctly, and are current sounds loud enough without unacceptable distortion? The user must listen to ordinary keys, special keys, releases, and rapid typing at several volume settings. Record the installed package version and the user's observations. Keep catalog gaps open; a listening check does not establish complete Keeby coverage.

## Comments

2026-10-01: The package and automated checks are ready, and 26 local packs are imported; development and packaged binaries accept 45/45 profiles. Package `0.1.0-10` is not installed; the current installed package is `0.1.0-8`. This ticket remains open until the user installs package 10, completes the read-only process security inspection and optional single SIGUSR1 probe, confirms real pointer-button/Fn capture, and listens to built-in/imported keyboard and mouse sounds at 0%, 50%, and 100% volume plus rapid typing. The exact-tracking live checks remain separate pending work. See [report 018](../../../docs/018-keeby-sounds-and-volume.md) and HANDOFF.md.
