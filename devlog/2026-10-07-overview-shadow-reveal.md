# Uncropped, fading overview shadows

The reported first-frame corner shows a rectangular cut through the outer
shadow. Stage captures the overview window's passes into a cropped edge-effect
layer; that layer also captured the overview shadow, although the shadow extends
outside the visible card/window crop. The live configuration has Stage window
decorations disabled, so introducing the full-strength overview shadow at entry
also creates a sudden appearance.

Keep the outer overview shadow outside the Stage content layer, preserving its
position in native pass order. That shadow pass owns full-output clipping and
restores it after drawing. The window body retains the card/strip crop and edge
gradient, and the shadow retains the existing mapped rounded-window cutout.

Multiply overview shadow opacity by the existing ease-in-out cubic of visual
openness. Opening, closing, reversed gestures and settle frames therefore use
the same continuous fade without a separate timer or a new configuration knob.

Validation: CMake plugin build and all five existing CTest targets (including
the EGL edge-render test). No plugin reload was performed. Live acceptance of
the first-frame corner and fade remains pending after the user updates.
