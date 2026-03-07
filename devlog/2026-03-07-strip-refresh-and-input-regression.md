# Dev Log: 2026-03-07 Workspace Strip Refresh And Input Regression

## Summary

March 7, 2026 focused on the first `hymission` workspace strip implementation and the regressions that followed while making it interactive and continuously refreshed.

The main outcomes were:

- the strip was moved onto the render-pass pipeline so it actually appeared on screen
- strip thumbnails were upgraded from lightweight box previews to real workspace-derived mini previews
- strip-specific input handling was stabilized after a broken low-level mouse path caused reopen regressions
- an overview freeze was traced to a self-rescheduling strip snapshot refresh loop
- dynamic strip refresh was restored after the freeze fix, but without reviving the unsafe input hook

## User-Visible Symptoms

- The strip reserved space in the overview layout but did not render.
- Early strip thumbnails showed raw workspace content instead of the intended overview-style mini preview.
- Strip click sometimes worked only once, then stopped working after closing and reopening overview.
- One debugging iteration caused Hyprland to freeze hard when opening overview, with no usable TTY fallback.

## Work Completed

### 1. Strip rendering path

The original strip implementation drew directly with `g_pHyprOpenGL->renderRect(...)` and `renderTexture(...)` during `RENDER_POST_WINDOWS`. That left layout space on screen but did not reliably land in the final frame.

This was changed to a render-pass overlay path:

- a custom `OverviewOverlayPassElement` is inserted into the active render pass
- strip and selection chrome render inside that element's `draw(...)`
- damage is expanded to cover the full monitor when the overlay is active

This aligned `hymission` with the same general solution used by `hyprspace` and `hyprexpo`: overview chrome must participate in Hyprland's pass pipeline instead of bypassing it late.

### 2. Strip preview semantics

The strip was initially a lightweight minimap. That was not acceptable for the intended UI.

The implementation was then extended so strip entries can render workspace-derived previews and later mini-overview previews built from the plugin's layout state, rather than only drawing simplified window boxes.

This also included:

- removing workspace number labels from strip entries
- making thumbnails full-bleed within each strip slot
- removing rounded corners and inner borders from strip visuals
- adding scale-aware overlay geometry so strip UI stops using logical coordinates as if they were physical render coordinates

### 3. Input regression and reopen behavior

Strip hover worked before click did. That narrowed the problem to the button path rather than hit testing.

Several iterations were tried:

1. Reusing the overview workspace transition pipeline for strip activation
2. Capturing hovered strip targets on press and activating them on release
3. Hooking lower-level mouse button handling

The low-level hook turned out to be the wrong direction. It introduced unstable reopen behavior and later contributed to compositor instability during debugging. The safer fix was to keep strip button handling on the event-bus input path and make the listener consume a copied `IPointer::SButtonEvent`, rather than relying on a more fragile raw hook chain.

### 4. Freeze root cause

The hard freeze was not a normal crash. Hyprland's main loop was being starved by the plugin.

Root cause:

- opening overview triggered strip snapshot refresh
- when overview was in a scrolling workspace / relayout / transition-sensitive state, the refresh path would mark strip snapshots dirty and reschedule itself with `doLater`
- the rescheduled callback saw the same condition and queued another refresh immediately
- that became a tight self-rescheduling loop on the compositor thread

The important distinction is:

- a per-frame refresh is bounded by the compositor frame rate
- a self-rescheduling idle callback is not frame-bounded and can spin effectively unbounded

That is why earlier "dynamic refresh every frame" behavior did not freeze the compositor, while the later `doLater`-driven retry loop did.

## Final Code Direction For This Session

The final state kept these decisions:

- keep dynamic strip refresh during overview, because live strip updates are required
- do not keep the unsafe raw mouse hook that caused reopen regressions
- only defer strip snapshot work when already inside the render stack, instead of self-rescheduling indefinitely on scrolling workspaces
- keep the earlier transition-commit crash fix that protects against reentrant workspace-change side effects

## Files Touched In This Iteration

- `src/main.cpp`
- `src/overview_controller.cpp`
- `src/overview_controller.hpp`
- `src/overview_logic.cpp`
- `src/overview_logic.hpp`
- `tools/overview_logic_test.cpp`
- `docs/workspace_strip_plan.md`
- `README.md`
- `docs/todo.md`
- build files and test registration updates

## Validation

Validation performed during this iteration:

- `cmake --build build-cmake -j"$(nproc)"`
- `build-cmake/hymission-overview-logic-test`
- repeated plugin reloads inside a live Hyprland session
- manual checks for strip visibility, hover, workspace switching, and reopen behavior

## Remaining Follow-Up

- strip click behavior still needs more real-session validation across repeated open/close cycles
- strip flash / interference during overview workspace transitions still needs cleanup
- if mini-overview strip previews remain too expensive, add explicit dirty-region refresh later rather than another unbounded retry loop
