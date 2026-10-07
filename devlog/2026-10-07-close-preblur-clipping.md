# Closing-window pre-blur clipping

The supplied `Recording-2026-10-07-023814.mp4` shows rectangular blur
artifacts during a native window close with Stage enabled, particularly at
approximately 0.833 seconds.

`captureWindowPasses()` retains the native fadeout's pre-blur rectangle and
snapshot texture. `drawWindowPasses()` supplied both with a viewport-sized
clip. The native rectangle-blur implementation draws a full-output blurred
texture with damage restricted to the rectangle. The texture implementation
uses a nonempty clip box in preference to that damage. Consequently, supplying
the Stage clip can expose blur outside the closing window, including pixels
outside the blur update region.

Intersect blurred rectangle clips with their render-modified rectangle before
drawing. Preserve any original clip and the Stage edge/sample limit. Skip an
empty intersection because native empty clips mean unrestricted drawing.
The subsequent snapshot texture keeps its existing clip and animation.

Validation: plugin build succeeded; all five CTest tests passed; diff whitespace
check passed. These existing tests do not reproduce the native fadeout path.
No live plugin reload was performed. Visual acceptance requires the user to
update from a safe context and repeat the close operation from the recording.
