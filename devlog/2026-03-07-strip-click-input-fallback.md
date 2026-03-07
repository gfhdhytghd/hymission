# Dev Log: 2026-03-07 Workspace Strip Click Input Fallback

## Summary

March 7, 2026 continued the `hymission` workspace strip stabilization work, but focused on a live-session regression where strip hover worked while strip click still did nothing.

The main outcomes were:

- the strip click failure was reproduced end-to-end with live logging, screenshots, and simulated pointer input
- the regression was narrowed to corrupted mouse button payloads reaching `handleMouseButton(...)`
- strip click handling was hardened with a local primary-button fallback so corrupted button callbacks still produce a usable press/release pair
- the fix was validated in-session by switching the active workspace through the strip without closing overview

## User-Visible Symptoms

- Hovering the workspace strip highlighted the expected target.
- Clicking the same strip entry did not switch workspaces.
- Live logs showed the callback arriving, but `button` was corrupted to `32767`, so the overview input path treated it as a non-left click and returned early.

## Work Completed

### 1. Reproduced the failure with live input and screenshots

The problem was verified against the real loaded plugin, not only from code inspection.

Validation steps included:

- enabling `plugin:hymission:debug_logs`
- opening `hymission:open onlycurrentworkspace`
- moving the cursor to the `ws=2` strip tile with `hyprctl dispatch movecursor 2250 2128`
- confirming hover in `/tmp/hymission-debug.log`
- issuing a synthetic left click with `ydotool click 0xC0`
- checking `hyprctl activeworkspace`
- capturing screenshots with `grim`

This established a concrete failure sequence:

- strip hover resolved to `strip=1:2:2`
- the click callback still arrived
- but the raw payload logged as `state=1 button=32767`
- no workspace transition began

### 2. Kept the listener-side event copy

The mouse button listener continues to copy the signal payload immediately before forwarding it into overview logic.

That remains the safest listener boundary for the current Hyprland build, but it was not sufficient by itself in this environment: the callback still reached overview with a corrupted button code.

### 3. Added a local primary-button fallback for corrupted callbacks

The actual fix was applied in `handleMouseButton(...)`.

When the callback delivers a valid left-button payload, overview behaves exactly as before.

When the payload arrives with an invalid button code, the handler now:

- normalizes the button to the primary button
- synthesizes the missing press/release edge from a tiny local `m_primaryButtonPressed` state machine
- logs both the raw and effective values for later debugging

This preserves the existing strip and window activation logic while making the broken callback stream usable for the left-click interactions that overview currently depends on.

The local button state is reset on overview open and deactivate so stale state does not leak across sessions.

## Files Touched In This Iteration

- `src/overview_controller.cpp`
- `src/overview_controller.hpp`

## Validation

Validation performed during this iteration:

- `cmake --build build-cmake`
- `ctest --test-dir build --output-on-failure`
- live plugin reloads with `hyprctl plugin unload ...` and `hyprctl plugin load ...`
- live reproduction with:
  - `hyprctl dispatch hymission:open onlycurrentworkspace`
  - `hyprctl dispatch movecursor 2250 2128`
  - `ydotool click 0xC0`
  - `hyprctl activeworkspace`
- screenshot capture with `grim`

The final live result was:

- hover hit `ws=2` in the strip
- the synthesized press/release pair was logged
- strip activation started an overview workspace transition
- `hyprctl activeworkspace` changed from workspace `1` to workspace `2`
- overview stayed open through the transition

## Remaining Follow-Up

- The underlying reason why this environment still delivers `IPointer::SButtonEvent.button = 32767` is not fully explained yet.
- The fallback is intentionally scoped to the primary-button interactions used by overview today; if right-click behavior is added later, the raw callback corruption should be investigated separately instead of broadening the fallback blindly.
