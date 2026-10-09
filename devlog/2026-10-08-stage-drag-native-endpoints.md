# Native rounding and unclamped desktop drag endpoints

Follow-up to `100fa98`, also present in the local hyprpm source cache when this
issue was reported:

- Desktop hover drawing still used `previewRounding`, so adding decorations
  did not restore native window rounding. Desktop hover and release continuation
  now use `window->rounding()` immediately, including per-window rules.
- A thumbnail drag could opt back into edge transfers after passing over the
  desktop on its way between Stage cards. Edge transfer is now restricted to
  native floating drags that start on the desktop. Stage-origin drags retain the
  direct animation throughout. Desktop/card identity is checked separately from
  workspace identity when retargeting.
- Desktop thumbnail targets were both scaled/clamped into the desktop and sent
  through `transitionBoxWithin`. Release flights also fitted their native
  destination into output bounds. They now keep the native size and pointer
  center, interpolate directly to the real endpoint, and use only the physical
  output as a drawing clip. Native-return hover uses that same direct path.

Added regression checks using a window larger than the desktop: the target
retains its native size and pointer anchor, the final animation frame equals it,
and Stage-card targets still fit. CMake build and all CTest suites are required.
Actual rounding, Stage-to-Stage float dragging, and large tiled-window handoff
remain live acceptance checks after user-run `hyprpm update`. No reload was run.
