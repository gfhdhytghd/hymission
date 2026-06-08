# Dev Log: 2026-03-07 Hyprland Crash During Overview Workspace Transition

## Summary

Hyprland crashed on March 7, 2026 while `hymission` was committing an overview workspace transition. The crash was not caused by Hyprland's normal workspace switch path alone. The plugin entered a reentrant event path during transition commit and cleared its own transition state while still using it.

## User-Visible Symptom

- Hyprland crashed after the plugin triggered a workspace transition from overview mode.
- The first report was "Hyprland just got crashed by this plugin".

## Evidence Collected

- Crash report: `~/.cache/hyprland/hyprlandCrashReport1423548.txt`
- Coredump timestamp: 2026-03-07 03:15:13 EST
- Relevant stack chain:
  - `hymission::OverviewController::commitOverviewWorkspaceTransition`
  - `CMonitor::changeWorkspace`
  - Hyprland abort / crash reporting path

Journal and coredump output showed the plugin call chain entering `commitOverviewWorkspaceTransition()` and then reaching `CMonitor::changeWorkspace(...)`.

## Investigation Notes

1. The initial stack suggested the crash happened while the plugin was finalizing an overview workspace transition.
2. Reading Hyprland source from `~/data/Hyprland` clarified an important detail:
   - `CWindowTarget::assignToSpace(...)` calls `CWindow::moveToWorkspace(...)`
   - `CWindow::moveToWorkspace(...)` emits `Event::bus()->m_events.window.moveToWorkspace`
3. In the plugin, `commitOverviewWorkspaceTransition()` manually moved pinned windows before calling `changeWorkspace(..., internal=true, ...)`.
4. The plugin also had this behavior in `handleWindowSetChange(...)`:
   - if a workspace transition was active, it immediately called `clearOverviewWorkspaceTransition()`
   - then rebuilt visible state
5. A core inspection of frame 6 (`commitOverviewWorkspaceTransition`) showed that by the time execution reached the crashing line, the transition fields had already been cleared:
   - `m_workspaceTransition.active == false`
   - `m_workspaceTransition.monitor == null`
   - `m_workspaceTransition.targetWorkspaceId == -1`

## Root Cause

This was a reentrancy bug inside the plugin.

The sequence was:

1. `commitOverviewWorkspaceTransition()` started.
2. The plugin moved pinned windows to the target workspace.
3. Hyprland emitted `window.moveToWorkspace` synchronously.
4. The plugin's `handleWindowSetChange()` ran during the commit.
5. That handler cleared `m_workspaceTransition`.
6. Control returned to `commitOverviewWorkspaceTransition()`.
7. The function continued and dereferenced transition state that had already been wiped.

So the real failure was "transition state invalidated during its own commit path".

## Fix Implemented

Files changed:

- `src/overview_controller.cpp`
- `src/overview_controller.hpp`

Changes made:

1. Added a deferred rebuild flag:
   - `m_rebuildVisibleStateAfterWorkspaceTransitionCommit`
2. Updated `handleWindowSetChange(...)`:
   - if `m_applyingWorkspaceTransitionCommit` is true, do not clear the transition immediately
   - instead set the deferred rebuild flag and return
3. Updated `commitOverviewWorkspaceTransition(...)`:
   - snapshot transition-critical values into local variables before any reentrant calls
   - keep the "commit in progress" guard active across the reentrant section
   - rebuild visible state only after the commit finishes if deferred events were seen

## Why This Fix Is Correct

- The commit path is the only place where the plugin intentionally performs synchronous workspace-moving operations that can emit window-set events.
- Deferring state rebuild prevents self-invalidating the transition object while it is still in use.
- Copying the transition inputs into locals makes the commit path robust even if later state changes happen before the function returns.

## Validation

Build and test results after the fix:

- `cmake --build build-cmake -j4`
- `ctest --test-dir build-cmake --output-on-failure`

Result:

- Build succeeded
- Existing test suite passed

## Remaining Risk

- This fix addresses the confirmed reentrancy crash path during workspace transition commit.
- It does not guarantee that all overview-related event interleavings are safe.
- If more crashes appear, the next places to review are:
  - `handleWorkspaceChange(...)`
  - `handleMonitorChange(...)`
  - any path that clears transition state while synchronous Hyprland events are still on the stack

## Next Follow-Up

- Reproduce with pinned windows and overview workspace switching.
- Check whether additional guard logic is needed for monitor removal or close paths during transition commit.
