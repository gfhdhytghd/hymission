# Drag decorations and original tiled-window policy

The live options read during diagnosis were `stage_window_decorations = 0` and
`stage_drop_follow = 0`. Preview drawing used the former without a desktop-drag
override, explaining why decorations appeared only when native rendering
resumed on release.

Desktop-bound hover previews and thumbnail release continuations now request
decorations explicitly. With card decorations disabled, this override preserves
the content coordinate mapping and draws native decorations around it instead
of shrinking the content into a new decoration bounding box.

Native tiled move drags temporarily float their targets. Use `draggingTiled()`
and retain that classification in the hover state. Tiled drags use the original
direct animation above Stage, without Stage-region clipping. Outside hover
ownership, the native render hook submits Stage before the dragged tiled window
and bypasses the resting desktop clip for that window. Stage-to-Stage direct
motion remains unchanged; floating desktop/card transfers keep edge animation.

The occasional workspace switch was NOT reproduced or conclusively attributed.
The no-follow option is disabled in the live session. Local compositor sources
show that native drag-end restores tiling and focus, and focusing a hidden
workspace's window can activate it, but those sources are not the exact running
compositor revision. Do not treat that candidate path as proven runtime cause.
Added `last_drop_trace` to the existing state dispatcher to capture the actual
release phase and workspace-change requests on the next occurrence, without
changing focus policy speculatively.

Validation: plugin CMake build and existing CTest suite. Live decoration/layering
acceptance and unexpected-switch diagnosis remain pending user-run update and
reproduction. No live reload, workspace switch, or simulated drag was performed.
