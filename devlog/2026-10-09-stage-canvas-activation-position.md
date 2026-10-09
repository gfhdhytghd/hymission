# Retain the real Stage canvas position on activation

The hover gesture already calls the workspace's native scrolling controller
setOffset and recalculates real target geometry. Native workspace activation can
subsequently fit the previously focused column back into view. Source inspection
shows both focus callbacks and workspace-change layout recalculation paths; the
live configuration has scrolling follow_focus enabled. This is a source-backed
reset path, not a captured live reproduction of the user's symptom.

Record the controller's actual offset after each successful Stage canvas move.
For the next workspace activation, restore that offset after native focus and
layout recalculation, clamped to the current layout extent. Select a mapped
window in the current viewport as the workspace's remembered focus without
focusing a hidden workspace during the gesture. Keep the restoration guard
through the card click's subsequent focus call, then consume it in the next
idle callback. Explicit focus navigation discards the guard. Weak workspace
references, Stage disable and plugin destruction clean up retained state.

Validation: CMake build and existing CTest suite. Live acceptance pending:
scroll a hidden Stage far from its old focused column, activate via a card click
and workspace swipe, and verify native canvas/window positions match the preview.
Then deliberately focus another column and verify normal focus-driven scrolling.
Also cover multiple scrolled cards, closing windows before activation, target
workspace deletion and disabling Stage. No live plugin reload was performed.
