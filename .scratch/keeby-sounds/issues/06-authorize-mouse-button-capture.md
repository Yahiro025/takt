# Authorize local mouse-button sound capture

Label: wayfinder:grilling
Type: grilling
Mode: HITL
Status: resolved
Assignee: none
Parent: [Keeby exact sounds and doubled volume](../map.md)
Blocked by: none

## Question

Did the user authorize global left, right, and middle button press/release events from pointer devices that `keeby-inputd` already opens, for local sound playback? A yes applies only to those three button codes over the existing private helper socket and audio event path. It does not authorize new device types or opens, scrolling, absolute coordinates, device writes, `EVIOCGRAB`, D-Bus, or logging. Touchpad button events are included only when an already-open touchpad emits one through libinput; this does not promise a sound for every touch gesture.

## Comments

2026-10-01: The user explicitly answered, “Yes, add mouse button events.” The approved scope is left/right/middle press and release from already-open pointer devices. The helper uses the existing post-drop libinput path and private socket; no device open, privilege, write/grab, D-Bus, or logging scope changes. See [docs/006 §48](../../../docs/006-step-2.5-security-permissions.md) for the implementation boundary and [report 018](../../../docs/018-keeby-sounds-and-volume.md) for tests and remaining live checks.
