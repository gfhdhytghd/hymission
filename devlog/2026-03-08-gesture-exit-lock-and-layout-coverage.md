# Dev Log: 2026-03-08 Gesture Exit Lock And Layout Coverage

## Summary

March 8, 2026 focused on tightening the overview gesture model so it behaves more predictably in live use, while also landing a few release-facing fixes around strip scope, delayed window geometry, and layout test coverage.

The main outcomes were:

- visible overview `toggle` gestures can now exit in either swipe direction
- `recommand` only keeps its continuous two-stage handoff in the side-changing direction; the other direction now just exits back to hidden
- hidden-start gesture sessions no longer chain into a brand-new visible-start exit or cross-side return without lifting first
- workspace strip empty-gap slots are now opt-in via `workspace_strip_empty_mode = continuous`, with background-backed synthetic empty thumbnails
- overview rebuilds now retry briefly when a newly relevant window has not reached usable geometry yet
- the mission layout got dedicated regression tests and a few scoring / scaling fixes

## User-Visible Symptoms

- When overview was already visible, one swipe direction could be ignored even though the user only wanted to exit.
- `recommand` could continue too far in a single touch sequence and feel like it was chaining state transitions more aggressively than intended.
- Some windows that should have entered overview were still skipped if their geometry had not settled by the first rebuild.
- The strip behavior for empty gaps was too eager for a default path; synthetic empty slots needed to become configurable.
- The mission layout had uncovered edge cases around row-height contamination, premature candidate rejection, and negative scale clamps.

## Work Completed

### 1. Visible overview gesture exit model

The trackpad gesture session now records the swipe direction it should treat as "forward" for the current session instead of assuming visible overview always closes in one hardcoded direction.

That changed the interaction model to:

- hidden overview still opens only in the configured direction
- visible `hymission:toggle,*` overview can close in either swipe direction
- `hymission:open,*` remains open-only and still does nothing while overview is visible

For `recommand`, visible gestures are now classified into two modes:

- `TransferCapable`: the direction that points through hidden toward the opposite side
- `CloseOnly`: the other direction, which can only exit to hidden

This keeps the bidirectional feel for leaving overview without letting every visible swipe become a cross-side handoff.

### 2. Hidden-start gesture locking

The previous gesture path could treat a long continuous interaction too much like multiple separate gestures.

The updated session model keeps a hidden-start gesture as a single opening / cancel session:

- you can still pull back before release to cancel the open
- but you cannot keep the same touch alive, treat the now-visible overview as a fresh visible-start gesture, and immediately chain into another exit or return phase

That preserves the responsive "pull back" behavior without letting one touch sequence act like multiple committed state changes.

### 3. Strip empty-mode and snapshot polish

The strip now has an explicit empty-gap policy:

- `workspace_strip_empty_mode = existing` is the default and only shows real workspaces plus the trailing new-workspace card
- `workspace_strip_empty_mode = continuous` progressively exposes one synthetic empty workspace slot per positive-id gap

Synthetic empty slots now prefer rendering the monitor background / wallpaper layers, while the trailing new-workspace slot keeps its dedicated `+` card styling.

### 4. Geometry retry and layout coverage

Overview rebuilds now tolerate a short "window became relevant before geometry settled" window by retrying rebuild a couple of times when the window matches scope but still lacks usable size.

Separately, the mission layout gained a standalone test binary and targeted fixes for:

- rejected-row height contamination
- row-search prematurely stopping after a local score drop
- clamping additional scales so oversized spacing cannot produce negative preview sizes
- preserving original natural aspect ratios while still using padded geometry only for layout weighting

## Files Touched In This Iteration

- `src/overview_controller.cpp`
- `src/overview_controller.hpp`
- `src/overview_logic.cpp`
- `src/overview_logic.hpp`
- `src/mission_layout.cpp`
- `src/main.cpp`
- `tools/overview_logic_test.cpp`
- `tools/mission_layout_test.cpp`
- `README.md`
- `docs/spec.md`
- `docs/workspace_strip_plan.md`
- build test registration updates

## Validation

Validation performed during this iteration:

- `cmake --build build-cmake`
- `ctest --test-dir build-cmake --output-on-failure`
- live `hyprctl plugin unload/load` against `build-cmake/libhymission.so`
- `hyprctl plugin list`

## Remaining Follow-Up

- live manual trackpad validation is still needed for the exact gesture feel across forceall / compact transitions
- if the geometry retry path still misses real-world windows, it should be replaced with a more explicit event-driven settle hook later instead of increasing retry depth
