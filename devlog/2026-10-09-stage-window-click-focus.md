# Explicit Stage window clicks override saved canvas position

A card-window click used WORKSPACE_CHANGE focus, so the canvas activation guard
restored the previously scrolled position immediately after focus and again from
its idle callback. Discard that workspace's saved canvas entry before activation
and use SWITCH_TO_WINDOW_HARD for the selected window. This avoids the native
CLICK path's desktop-coordinate hit test, since the pointer is in a thumbnail.
If activation has already focused the selected window, explicitly fit its native
scrolling column: rawWindowFocus otherwise returns before emitting focus events.
Do not force the fit when native focus selected a different window (e.g. a modal).
Blank-card clicks and workspace gestures retain their canvas preservation path.

Validation: CMake build and existing CTest suite. Live acceptance pending user
update: scroll a Stage card, click a partially visible window, and confirm focus
and canvas move to it; repeat with its previously focused window and follow_focus
on/off. Blank-card activation and workspace swipes must retain the saved offset.
No live reload was performed.
