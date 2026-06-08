# Dev Log: 2026-03-09 Multi-Workspace Recent-First Sorting

## Summary

March 9, 2026 added an optional "recently used first" ordering mode for multi-workspace overview.

The main outcomes were:

- multi-workspace overview can now place more recently used windows earlier in the grid
- the behavior is guarded behind a new config, `multi_workspace_sort_recent_first`, so existing layouts stay unchanged by default
- ordering is based on plugin-maintained window activation history instead of raw workspace target order
- overview-internal hover and live-focus changes no longer scramble the MRU order while overview is visible

## User-Visible Symptoms

- `forceall` and other multi-workspace overview scopes followed compositor/workspace storage order, which did not match recent usage
- even if the focused window happened to appear early, the rest of the grid could still feel arbitrary from a user perspective
- switching hover targets during overview could not be used as a reliable proxy for "recently used", because that would make the grid reorder in surprising ways

## Work Completed

### 1. Added a user-facing config gate

Plugin init now registers:

- `multi_workspace_sort_recent_first = 0`

README example config and behavior docs were updated so the new knob is discoverable and clearly scoped to multi-workspace overview.

### 2. Added plugin-side window MRU tracking

Hyprland exposes workspace history, but not a directly consumable global window MRU list for this use case.

The plugin now maintains its own lightweight activation sequence:

- subscribe to `events.window.active`
- assign a monotonically increasing serial to mapped, visible windows
- prune closed and unmapped windows from the history

This makes the overview order derive from actual focus history rather than workspace target iteration order.

### 3. Applied recent-first ordering only where it is visually meaningful

The new ordering is only used when:

- overview is building a fresh multi-workspace state
- the new config is enabled
- the current rebuild is not preserving the already visible overview order

When active, candidate windows are stable-sorted so higher MRU serials come first, and the layout engine is switched into input-order-preserving mode so the sorted order becomes the visual left-to-right / top-to-bottom order.

### 4. Prevented overview-internal focus churn from corrupting MRU state

Overview can temporarily move real focus during hover-follow and selection sync.

That focus churn is useful for interaction, but it should not rewrite the global MRU ordering every frame.

The final behavior is:

- normal desktop focus changes update the MRU history
- opening overview records the currently focused window so the active window is seeded correctly
- while overview is visible, window-active events do not rewrite MRU order
- when overview closes onto a selected window, that final activation is committed back into the MRU history

## Files Touched In This Iteration

- `src/main.cpp`
- `src/overview_controller.cpp`
- `src/overview_controller.hpp`
- `README.md`

## Validation

Validation performed during this iteration:

- `cmake --build build-cmake -j4`
- `ctest --test-dir build-cmake --output-on-failure`
- code inspection of the layout path to confirm that `preserveInputOrder` is enabled when MRU sorting is active

## Remaining Follow-Up

- the MRU history starts from the current plugin lifetime, so after a fresh reload the first ordering is only as good as the focus events observed since that reload
- if a future Hyprland build exposes a stronger native global window-history API, the plugin-side serial table could be replaced with that source
