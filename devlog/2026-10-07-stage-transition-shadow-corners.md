# Stage transition shadow corners

The Stage-origin overview shadow pass maps native window geometry into its
transparent intermediate layer. Hyprland also scales the window's shadow cutout
radius by that mapping, while overview surface rendering keeps its displayed
rounding independent of the window's animation scale. The resulting corner
contours diverge during the transition.

Capture the displayed surface radius with the deferred shadow pass, including
overview's radius cap. Use the existing Stage rounding hook to supply the inverse
scaled radius only while drawing that window's native shadow. Restore the hook
context after drawing; window rules and the outer shadow remain unchanged.

Validation: CMake build, existing CTest suite and diff whitespace check. No live
plugin reload or visual gesture acceptance is performed by this change; the user
must verify the reported corner after safely updating the committed plugin.
