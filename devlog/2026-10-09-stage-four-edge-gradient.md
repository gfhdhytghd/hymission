# Four-edge gradients in Stage card viewports

Extend EdgeViewport with top/bottom widths, normalized independently against
height. Per-window overflow classification leaves complete edges sharp. Stage
cards enable all four bands for scrolling tiled and floating previews; desktop
seam behavior is unchanged. Edge frame interpolation, scrolling flights and
Overview reveals carry the vertical widths, using vertical scale on reveals.

The two-pass shader combines per-edge blur radii with max and opacity with min,
so corners do not multiply fade twice. Rendering considers vertical bands when
selecting the shader path and expanding sample support. Hard clip and premultiplied
compositing remain in place. Width zero disables gradients but retains clipping.

Validation: logic tests for vertical overflow, corner classification, opacity,
normalization and transition/reveal widths. GPU tests cover all 16 combinations
of active edges, 8 output transforms, 3 fractional offsets, 3 output scales and
3 source patterns including alpha/mirror/checkerboard. CMake/CTest validation;
live acceptance pending user-run hyprpm update for vertical scrolling and float
windows crossing top/bottom card edges. No live reload performed.
