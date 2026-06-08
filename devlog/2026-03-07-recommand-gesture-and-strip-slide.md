# Dev Log: 2026-03-07 Recommand Gesture and Strip Slide

## Summary

March 7, 2026 focused on the overview gesture path that had become awkward in real use, plus a pair of animation mismatches that made multi-workspace overview feel less spatially correct than intended.

The main outcomes were:

- a new gesture-only `recommand` mode for `hymission:toggle`
- a two-stage transfer gap so crossing from one overview scope to the other must pass through hidden state instead of switching immediately at zero
- `forceall` opening and closing paths now use workspace scene positions, so hidden workspaces animate in from and back to their off-screen locations
- the workspace strip now slides out during close instead of only sliding in during open
- the documented local build and reload path is now standardized on `build-cmake/libhymission.so`

## User-Visible Symptoms

- A bidirectional toggle gesture could not express "up for forceall, down for compact" as one continuous interaction.
- After reaching the hidden midpoint, the gesture could immediately spill into the opposite scope without any extra travel, which made the handoff feel accidental.
- In `forceall`, windows from other workspaces appeared to originate from the current workspace, and on close they did not return to their off-screen workspace positions.
- The workspace strip entered with a slide but disappeared abruptly on close because its progress was pinned fully open outside the opening phase.

## Work Completed

### 1. Gesture-only `recommand` mode

`gesture = ..., dispatcher, hymission:toggle,recommand` is now supported as a toggle-only gesture mode.

Its semantics are:

- from hidden state, the positive direction opens `forceall`
- from hidden state, the negative direction opens `onlycurrentworkspace`
- the compact side is always `onlycurrentworkspace`, independent of `only_active_workspace`
- when starting from a visible overview, reversing direction closes back toward hidden first

This gives one gesture a stable "bigger scope on one side, compact scope on the other side" interaction model.

### 2. Two-stage transfer gap between scopes

Crossing from one visible scope to the opposite scope no longer happens the moment signed progress crosses zero.

The gesture state machine now keeps a short hidden transfer gap:

- first drag the current scope fully shut
- continue dragging through a small hidden-only buffer
- only then start opening the opposite scope

This preserves the "pass through workspace first" feel and avoids accidental scope flips on small reversals.

### 3. Forceall animation origin and restore fixes

The overview state snapshot for hidden workspaces now includes workspace render offset when it needs the off-screen scene position, while the live and goal geometry paths also use scene-space positions that include workspace offset.

In practice this fixes both ends of the `forceall` motion:

- opening: other-workspace windows fly in from their actual off-screen workspace positions
- closing: they fly back out to those positions instead of collapsing inside the current workspace

### 4. Strip close animation now slides out

The workspace strip enter offset is now driven by the same `visualProgress()` value for both open and close phases.

That means:

- open still slides the strip in
- gesture close and normal close now slide the strip back out instead of popping it away

## Files Touched In This Iteration

- `src/overview_controller.cpp`
- `src/overview_controller.hpp`
- `src/overview_logic.cpp`
- `src/overview_logic.hpp`
- `tools/overview_logic_test.cpp`
- `README.md`
- `docs/spec.md`

## Validation

Validation performed during this iteration:

- `cmake --build build-cmake`
- `ctest --test-dir build-cmake --output-on-failure`
- `hyprctl plugin unload /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
- `hyprctl plugin load /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
- `hyprctl plugin list`
- `hyprctl dispatch hymission:debug_current_layout`

## Remaining Follow-Up

- the transfer gap constant is still hardcoded; it may need tuning after more live trackpad use
- `recommand` currently remains a gesture-only entry point and is intentionally not part of dispatcher syntax
