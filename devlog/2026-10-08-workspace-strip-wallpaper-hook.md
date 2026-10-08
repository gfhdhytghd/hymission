# Workspace strip wallpaper hook reuse

Stage's `renderBackgroundIntoFramebuffer()` activates the overview-owned
`renderLayer` hook on demand before overview opens. `activateHooks()` then
attempted to activate the same hook again. Hyprland rejects a hook whose source
is already active; the overview failure branch removed the hook and cleared
`m_renderLayerOriginal`. Workspace strip snapshots consequently skipped their
background layers, including in `onlycurrentworkspace` mode.

Reuse the existing original-function pointer as the activation guard. Closing
overview still unhooks and clears it; a subsequent Stage capture or overview
entry can activate the retained hook normally.

Live inspection found a mapped, alpha-1 `awww-daemon` background layer on DP-4.
Validation: plugin build and the five existing CTest tests; these do not exercise
the live hook lifecycle. User-run update and repeated overview entry remain the
visual acceptance step. No compositor reload is performed by Codex.
