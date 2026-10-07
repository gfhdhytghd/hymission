# Gesture release integrated with Stage viewport transitions

The earlier isolated b3b3a87 prototype captured only window rectangles. The
integrated implementation uses one Stage transition sample containing window
position, viewport, clip and edge widths, so two interpolation systems do not
compete.

Both swipe-close commit paths now keep the gesture active until beginClose has
sampled the displayed frame. Previously clearing it first made sampling read
Active-phase progress (1) rather than the actual finger progress. The sample
includes ordinary destination windows, not only windows with Stage endpoints.
ClosingSettle freezes both geometry and clipping, including a gesture released
at zero openness, until the final desktop/card layout is ready.

Stage entry/exit endpoints retain their visible card crops and edge gradients.
Interrupted transitions preserve the sampled frame before heading toward a new
endpoint. The separate native desktop fadeout change remains outside this
integration.

Validation: build the exact integrated source in an isolated checkout and run
all five CTest targets, including release crop/viewport/edge continuity checks.
No live reload was performed; physical release and visual acceptance remain
pending after a user-run update. Recheck A-to-B focus exit at partial and zero
openness, and confirm the first timed/settle frame matches the final finger frame.
