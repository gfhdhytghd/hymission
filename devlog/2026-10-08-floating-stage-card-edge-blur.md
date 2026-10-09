# Gradient blur for floating card previews

Resting card previews previously passed a gradient viewport only for scrolling
tiled windows. Include floating windows in that selection, independently of the
workspace layout. Reuse the existing card's left/right bands and
`stage_scrolling_preview_edge_width`. The renderer still classifies each edge
against the window's actual content bounds, so fully visible edges stay sharp.
Hard clipping remains active when the configured blur width is zero.

Apply the same selection in `cardEndpoint` so Overview entry/exit starts and
finishes with the same clip and gradient. Do not opt floating workspace flights
into the scrolling flight interpolation or change direct drag motion.

Validation: CMake build and existing CTest suite, including edge-render coverage.
Live visual acceptance remains pending user-run `hyprpm update`; no reload was
performed. The reused bands affect left/right edges; vertical card edges retain
their existing hard clip.
