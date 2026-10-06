# Gesture handoff to the selected workspace

The continuous `recommand` gesture can close all-workspace overview, cross the
hidden gap, and open `onlycurrentworkspace` without lifting the fingers.
`retargetGestureScope()` already activates the preferred window's workspace and
passes a workspace override into `beginOpen()`. However, `beginOpen()` preserves
the original overview's `focusBeforeOpen`. Releasing below the compact opening
threshold, or cancelling that opening, then focuses the original workspace while
the compact overview still contains only the target workspace's windows. This
provides a source-level path to the reported workspace jump and blank view.

After a successful compact handoff, use the handoff window as the new opening
focus baseline. Suppress the initial hover pass during retargeting so pointer
coordinates from the previous scope do not replace the handoff selection.

Validation: CMake plugin build and existing CTest suite. These do not exercise
physical trackpad input or establish that the running plugin has been updated.

Pending live acceptance after a user-run `hyprpm update`:

- From workspace A, open global overview and select a window on B. With
  `gesture_close_restores_focus = 0`, close without crossing the gap: land on B.
- Repeat and cross into compact overview, then release before its opening
  threshold at low speed: land on B, without flashing A or an empty overview.
- Repeat and fully open compact overview: show B's windows and active strip item.
- Reverse or cancel after crossing: return to B, rather than the original A.
- Check same-workspace selection and a subsequent compact-to-global gesture.
