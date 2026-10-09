# Scroll the hovered Stage canvas

The registered `hymission:scroll, layout` gesture previously always resolved the
active layout workspace. Stage now exposes its existing input hit test, respecting
pinned floating windows and noninteractive cards. At gesture begin the controller
captures a weak reference to the hovered workspace. Direction, algorithm, bounds
and gesture sensitivity use that target for the entire session. An expired target
never falls back to the desktop, and empty/sidebar chrome or incompatible layouts
consume no canvas movement.

Stage scrolling directly updates the hidden scrolling controller and recalculates
its geometry, which the existing Stage refresh timer renders into the card. It
never activates or focuses the hidden workspace, and skips the desktop gesture's
end refocus and global follow-focus override. The existing desktop and Overview
routes and sidebar wheel navigation remain intact. This targets the configured
Hymission layout gesture; native Hyprland scrollmove hooks are unchanged.

Validation: CMake build and CTest. Live acceptance remains pending user-run update:
- Four-finger horizontal canvas scroll over a scrolling Stage card moves only its
  canvas; active workspace and focus remain unchanged.
- Moving the pointer to another card during a gesture retains the initial target.
- Empty/dwindle cards and sidebar gaps never scroll the desktop behind them.
- Scroll over the desktop and Overview as before; two-finger sidebar scrolling
  still moves the card list. Check reversed/vertical workspace directions and a
  secondary output, and ensure deletion of the target mid-gesture is harmless.
