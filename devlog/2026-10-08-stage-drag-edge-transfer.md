# Drag transfers through fixed Stage edges

Scope: Stage drag-in/drag-out and their release continuation only. Ordinary
workspace switch/swipe flights retain their existing interpolation.

The previous drag preview scaled across the desktop/card boundary. With edge
clipping enabled this produced an awkward moving crop. Drag transfers now use
two equal phases on the existing `stage_transition_ms` timeline:

1. Translate the source-size window out through its region's facing edge.
2. Translate the destination-size window in through the target's facing edge.

Each phase retains that region's viewport, hard clip and edge-effect widths.
Size changes happen while hidden; fully clipped frames do not draw a residual
shadow. Left/right sidebars are mirrored. Transfers between vertically aligned
cards use their facing top/bottom edges. Moving within one region settles
directly. Already hidden columns remain hidden.

Interruptions sample both the displayed box and viewport. Returning to the
desktop follows live native geometry, including release before completion.
Drop placement uses the intended pointer preview center, independently of the
temporarily offscreen animated box. Drag-release flights opt into the new path;
workspace-switch flights do not. Overview handoff samples the actual drag clip.

Validation: CMake plugin build and all five CTest suites, including new geometry
checks for both sidebar sides, both directions, fixed clips/sizes, the hidden
midpoint, interrupted reversal, hidden columns, same-card settling, vertical
card transfer and zero blur widths.

Live validation remains pending user-run `hyprpm update`: hold a desktop window
over a card, drag out without release, reverse during each phase, release during
transfer, and drag a thumbnail onto the desktop. Repeat for tiled/floating
windows and disabled animations. No live reload was performed.
