# Continuous card dragging and delayed drop focus

The user narrowed the animation reproduction to one held native floating drag:
desktop -> Stage card 1 -> Stage card 2. Native drags did not qualify for
`inGap`, so crossing inter-card space started a return-to-desktop transition and
entering the next card started a new edge transfer. All strip gaps now qualify
as Stage hover space. The existing direct Stage-to-Stage path therefore survives
the gap. Window transfers picked up inside Stage remain unchanged.

Live evidence for the separate unexpected workspace switch, read after the user
reproduced it with the `168e09d` source present in the hyprpm cache:

- native_drag_end, native_drag_ended: active 5, target 4, window workspace 5.
- move_window: active 5; place_window, restore_focus, complete: active 5, window
  workspace 4. All entries had follow=0 and floating=true.
- After complete: workspace_request 5 -> 4, then 4 -> 5.

This proves the unwanted switch was requested after the drop handler completed;
it does not identify the exact caller of that request. Add a narrowly scoped
rawWindowFocus guard against delayed automatic focus of the silently dropped
window while its destination remains hidden and the original workspace visible.
The guard expires after two seconds. New input and explicit focus/workspace
selection restore user control immediately. No broad workspace-switch blocking
or forced switch-back is used. Suppressed requests record the actual focus reason
in `last_drop_trace`, so the suspected focus path can be checked at runtime.

Validation: CMake build and existing CTest suites. The installed Hyprland binary
exports the expected rawWindowFocus signature (checked with nm/c++filt).
No plugin reload or simulated drag was performed. Live gap-animation and focus
guard acceptance remain pending user-run `hyprpm update` and reproduction.
