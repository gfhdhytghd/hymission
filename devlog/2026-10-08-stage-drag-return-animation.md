# Stage native drag return animation

Dragging a desktop window over a Stage card and leaving without releasing the
drag cleared `DragHover` immediately. That revealed the native full-size window
with no return transition, even when the entry transition was still running.

Keep the hover preview in a return phase, sampling its current box on reversal
and following the native window's actual geometry until the transition ends.
Re-entering a card samples the return frame before starting the next shrink.
The return endpoint is not fitted inside the monitor: native dragged windows may
extend beyond its edges. Releasing over the desktop retains the return phase so
native drag-end layout restoration can update its destination. Cancellation and
unavailable Stage/window state still clear the preview.

Validation: CMake build and existing CTest suite. Live compositor acceptance is
pending user-run `hyprpm update`; no plugin reload was performed.

Manual checks after updating:
- Drag a desktop window into a card and straight out while still holding.
- Reverse before the entry animation completes, then enter the card again.
- Release on the desktop before the return finishes, for tiled and floating windows.
- Drop into a card normally; check disabled animations and zero transition duration.
