# Atomic Stage publication after changing overview focus

Reported case: exit global overview to a window on another workspace. The old
occupant of a sidebar slot briefly covers the workspace flying into that slot.

## Source-level causes

- Stage drawing was gated by `overviewProgress > 0`. At zero, overview still
  owned the compositor hooks and was waiting for deferred teardown, but Stage
  could draw its old live card list. Render/snapshot contexts also made the
  progress callback disappear, accidentally changing ownership.
- Exit rectangles were calculated independently for each window from a new
  card list. The actual Stage renderer still used its asynchronously refreshed
  list, including card `shift` animations from the previous arrangement.
- Stage preview refresh is suspended during overview. Merely changing card
  geometry does not ensure the first resumed frame has new preview content.

## Replacement contract

Rendering ownership is now `Inactive`, `Active` or `Releasing`, independent of
animation progress and snapshot rendering. Active overview owns even its
zero-progress and reversed gesture frames. A releasing overview permits only a
prepared Stage scene, never the stale live scene.

Prepare one complete per-output destination scene: workspace identity/order,
geometry, scroll and native window previews. Every exit endpoint reads this
scene. Refresh it for the final focus before native window collection begins;
Stage draws that prepared scene during deferred teardown. Its card shifts,
departing pane and flights are cleared rather than restarted.

After final focus/fullscreen restoration, deferred teardown seeds Stage with
the prepared card list and synchronously refreshes native layout and previews.
That transaction disables Stage reorder/flight/side-change animations. Only
then are prepared scenes discarded and live Stage rendering resumed.

Unrelated outputs retain normal Stage rendering. Session-lock hiding remains
authoritative. A missing prepared scene never authorizes stale Stage rendering.

## Verification

- CMake plugin build and all five CTest targets.
- Added render-ownership regression cases for entry/zero/reversal, teardown
  with/without a prepared scene, and return to live Stage.
- `hymission-stage-state` now exposes `overview_render_owner`, prepared target
  workspace/card order, and live card shifts for subsequent frame diagnosis.
- No live plugin reload or claimed visual acceptance. After the user updates,
  check A -> B focus exit with `stage_show_active = 0`, including nonadjacent
  workspaces, reversed gestures, click/Return, and the compact handoff. The first
  Stage frame should have the target card order and no old card occupying the
  outgoing workspace's destination slot.
