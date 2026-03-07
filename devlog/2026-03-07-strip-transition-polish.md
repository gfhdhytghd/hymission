# Dev Log: 2026-03-07 Workspace Strip Transition Polish

## Summary

March 7, 2026 continued the first `hymission` workspace strip pass, but focused on the transition polish issues that became obvious in live use.

The main outcomes were:

- selection chrome was reordered so strip thumbnails stay visually above overview borders during workspace transitions
- `show_focus_indicator = 0` was tightened so overview preview borders and selected-title chrome stop rendering entirely
- strip snapshot reuse was added so workspace switches no longer drop to a single-frame card-only fallback while async thumbnails are being repainted
- switching into a newly created empty workspace now rebuilds the final overview state against the real workspace, so the strip updates immediately on the first switch

## User-Visible Symptoms

- During overview workspace transitions, the selected preview border could appear in front of the strip even after the moving preview had gone behind it.
- With `show_focus_indicator = 0`, focus chrome was still partially visible because the hover outline path remained active.
- Strip thumbnails flashed during workspace switches because the new state could render before replacement snapshots were ready.
- Swiping into a brand new empty workspace did not immediately add that workspace to the strip; it appeared only after another workspace change.

## Work Completed

### 1. Overlay ordering and focus-indicator gating

The overview overlay pass had been drawing the strip first and the selection chrome second. That made the preview border logically correct from the code's perspective, but visually wrong for the intended layering.

This was changed so:

- selection chrome draws first
- strip chrome and thumbnails draw after it
- the strip now consistently covers preview borders during transition overlap

At the same time, `show_focus_indicator` was expanded from only guarding the selected outline to guarding both selected and hovered preview chrome. That made the config behave the way the user expected: when disabled, the overview no longer paints focus-indicator borders or the selected title label.

### 2. Strip snapshot carry-over to remove flash

The strip thumbnails are rendered asynchronously into per-entry framebuffers. On a workspace commit, the overview state could switch to the new `stripEntries` immediately, while the snapshot refresh was deferred with `doLater(...)`.

That created a one-frame gap:

- new strip entry geometry was already active
- replacement snapshots were not ready yet
- render fell back to the blurred card background without the thumbnail texture

The fix was to carry matching snapshots from the previous strip state into the new strip state before replacing `m_state`. That preserves the old thumbnail texture until the async refresh paints the updated one.

This was applied on:

- normal overview open
- overview rebuilds while visible
- workspace-transition commit

The result is not a new strip animation system. It is a state handoff fix that removes the visible flash while keeping the current snapshot pipeline.

### 3. Newly created empty workspace strip update

The transition path for a synthetic empty workspace built its target overview state before the target workspace existed as a real Hyprland workspace object.

That meant the transition could commit with a final state that still reflected the pre-creation workspace list. In practice, the strip lagged one switch behind.

The commit path now detects that case and rebuilds the final overview state after `changeWorkspace(...)` when:

- the target was synthetic before commit
- or the rebuilt state still does not contain the real target workspace
- or the final `ownerWorkspace` does not match the actual target workspace

This makes the just-created empty workspace appear in the strip immediately on the first switch.

## Files Touched In This Iteration

- `src/overview_controller.cpp`
- `src/overview_controller.hpp`

## Validation

Validation performed during this iteration:

- `ninja -C build-meson libhymission.so hymission-overview-logic-test`
- `meson test -C build-meson hymission-overview-logic-test --print-errorlogs`
- `cmake --build build-cmake -j"$(nproc)"`
- repeated live plugin reloads with `hyprctl plugin unload ...` and `hyprctl plugin load ...`
- manual checks for overview workspace switching, strip layering, `show_focus_indicator = 0`, strip flash, and newly created empty workspaces

## Remaining Follow-Up

- strip transitions still do not have dedicated motion; current polish only removes the flash and incorrect layering
- if strip animation is added later, it should be built on explicit source/target strip state instead of relying on snapshot refresh timing
