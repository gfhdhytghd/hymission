# Dev Log: 2026-03-09 Expand-Selected Live-Focus Stability

## Summary

March 9, 2026 focused on turning `expand_selected_window` from an experimental hover effect into a stable overview interaction that can keep up with real mouse movement.

The main outcomes were:

- `expand_selected_window` is now a real user-facing config with layout-engine support and regression coverage
- selected-preview growth was changed from global reshuffle to local push-away, so neighboring previews move without the whole grid reordering
- hover retargeting can now happen while the previous relayout animation is still in flight, with frame-coalesced layout and real-focus commits
- multi-workspace overview no longer hard-cuts when hover-driven real focus crosses workspaces; the overview grid stays anchored instead of rebuilding around the new active workspace

## User-Visible Symptoms

- The first hover after opening overview could reshuffle preview order instead of only enlarging the hovered target.
- Two nearby previews could oscillate or swap during expand-selected animation because hover targeting and relayout kept chasing each other.
- After immediate hover-follow was enabled, some animations looked truncated because repeated mouse-move events restarted relayout too aggressively.
- In a mixed-workspace `3x3` overview, the bottom row could animate smoothly while upper rows hard-cut, because cross-workspace live focus kept triggering full overview rebuilds.

## Work Completed

### 1. Added selected-preview emphasis to the layout engine

The mission layout input model now accepts a per-window `layoutEmphasis` multiplier.

That made `expand_selected_window` a first-class layout concern instead of a later paint-only hack:

- config registration was added in plugin init
- mission-layout preparation now multiplies the layout weight by the emphasis value
- the mission-layout test binary gained coverage for "selected grows, neighbor yields, no overlap"

### 2. Reworked expand-selected from reorder-heavy relayout to local push-away

The selected preview no longer relies on a full overview reorder whenever hover changes.

The resulting path is:

- build the base slot layout first
- enlarge the selected preview in place
- push nearby previews away based on distance and bearing
- keep the visible slot order stable instead of reinterpreting the whole grid as a new pack problem on every hover

This also fixed the "open overview with the initially focused window already double-expanded" regression by separating base-slot creation from the active selected-preview relayout.

### 3. Made hover-follow and real focus responsive without per-event hard resets

Immediate hover response was kept, but the commits were split into two layers:

- hover updates `selected` immediately for UI responsiveness
- relayout requests are queued and flushed once per render frame
- real focus sync is also queued and flushed once per render frame

That preserves the "move fast, UI follows fast" feel while avoiding repeated same-frame relayout / focus commits from every raw mouse event.

### 4. Anchored multi-workspace overview against live-focus workspace churn

The final hard-cut issue came from real focus following hover across workspaces.

In multi-workspace overview, the old path did this:

- hover preview on another workspace
- real focus changes
- Hyprland emits workspace-active change
- overview rebuilds against the new active workspace
- owner workspace and preview anchors drift, so the row looks like a hard cut instead of a smooth local relayout

The new path keeps real focus follow, but changes the overview reaction:

- active-workspace overview still uses the dedicated overview-to-overview workspace transition path
- multi-workspace overview skips the rebuild for hover-driven live-focus workspace changes
- strip active state can still refresh
- `ownerWorkspace` stays anchored during passive rebuilds so mixed-workspace rows do not inherit the real focus workspace as a new layout baseline

## Files Touched In This Iteration

- `src/main.cpp`
- `src/mission_layout.cpp`
- `src/mission_layout.hpp`
- `src/overview_controller.cpp`
- `src/overview_controller.hpp`
- `tools/mission_layout_test.cpp`
- `README.md`
- `docs/spec.md`

## Validation

Validation performed during this iteration:

- `cmake --build build-cmake -j"$(nproc)"`
- `./build-cmake/hymission-overview-logic-test`
- repeated live plugin reloads with `hyprctl plugin unload ...` and `hyprctl plugin load ...`
- targeted debug-log capture for relayout, hover retarget, scrolling spot state, and live-focus workspace-change paths
- live manual validation of `expand_selected_window = 1` on a mixed-workspace `3x3` overview, including the previously failing upper-row transitions

## Remaining Follow-Up

- the current local push-away tuning is intentionally conservative; if the visual ripple should feel stronger or more directional, only the displacement parameters should need retuning
- right-click close inside overview is still unimplemented, so the new hover/focus semantics have only been validated against activation, close, fullscreen, and toggle flows
