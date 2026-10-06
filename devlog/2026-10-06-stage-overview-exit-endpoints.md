# Return overview windows directly to Stage cards

Overview previously used Stage card geometry only when opening. Its gesture
prediction and committed close still sent inactive workspaces off-screen. Stage
also relinquished native workspace-animation ownership while overview was open,
allowing a native slide beneath the overview transition.

Stage now supplies a settled card destination for inactive windows, using the
same card collection and preview layout as the resting sidebar. The collection
is evaluated against the exit workspace, so hiding the active card, named
workspaces, empty slots and monitor ownership follow the existing Stage rules.
Gesture prediction and both normal and settled close use this destination before
considering the ordinary off-screen endpoint. Native workspace slides are made
instant while overview owns motion on a Stage-enabled monitor.

Stage-disabled outputs, pinned/special windows, and destinations without a
visible Stage card retain their existing endpoint behavior. Fullscreen targets
that cover the sidebar do not produce card endpoints.

Validation: plugin CMake build and five existing CTest tests. Live Stage state
was read back as enabled with hooks ready; no plugin reload was performed.
Physical gesture and visual acceptance remain pending after a user-run update:

- From A, select a window on B in global overview and swipe closed. A should
  shrink into its resulting Stage card while B enters the desktop, without a
  second native workspace slide.
- Reverse the gesture before committing; previews should retrace their path.
- Repeat using click/Return, then verify Stage-disabled and fullscreen exits.
- Cross the hidden gap into B's compact overview and confirm it remains on B.
