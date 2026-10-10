# Extend workspace activation through Stage padding and gaps

Use a separate activation hit policy for button presses/releases. Card activation
extends horizontally across the sidebar to the physical output edge. Gaps belong
to the nearest visible card, split at the midpoint, with the last painted card
winning exact ties and overlap. Top/bottom strip padding uses the nearest visible
card; card rectangles are clipped to the visible card region before selection.

Shared strip input interception includes the outer monitor-to-strip margin, so
an edge click cannot pass through to a desktop client. Native pinned floating
input priority and transition blocking are unchanged. Window-preview selection
and Super thumbnail drags additionally require the strict card hit; extended
activation therefore cannot select invisible content outside the card clip.
Window drop targets, gap drags, and card reorder targeting retain their existing
strict/drag policies.

Validation: CMake build and CTest, with geometry regressions for both horizontal
edges, midpoint ties, outer padding, desktop exclusion, partially/fully hidden
cards, overlaps and empty strips. Live user-side update still required; no reload.
