# Use the desktop work area for scrolling follow visibility

Native scrolling soft focus checks follow_min_visible against the monitor box,
while Stage has reduced CSpace::workArea to reserve its sidebar. Hook only the
scrolling focusOnInput soft-input percentage path for interactive Stage outputs.
Use the target's global position and its workspace's global work area for both
overlap and denominator, selecting width or height from the tape direction.
Clamp disjoint overlap to zero, rather than treating a negative gap as visibility.

Keep native early-out for already visible columns and native centerOrFitCol plus
recalculate. Hard focus/click input, disabled thresholds, fullscreen/noninteractive
Stage and other layouts retain original dispatch. Hook registration and removal
follow the existing Stage lifecycle; no global configuration or monitor geometry
is temporarily changed. The installed binary exports the required hook symbol;
its visible/center/fit/recalculate calls were also checked read-only.

Validation: CMake build and CTest. Geometry tests cover both horizontal edges,
top/bottom thresholds, offscreen and oversized targets, empty viewports, negative
monitor origins, and the configured 14 percent boundary. Live compositor
acceptance remains pending a user-run hyprpm update; no reload was performed.
