# Dev Log: 2026-03-09 Toggle Switch Mode And Release Tracking

## Summary

March 9, 2026 added a toggle-only switch mode for `hymission:toggle` and then hardened its release-exit behavior against real desktop input edge cases.

The main outcomes were:

- `hymission:toggle` can now behave like a lightweight Alt-Tab style switch session
- repeated toggle presses cycle the current overview selection instead of closing overview
- releasing a configured modifier key commits the current selection and exits overview
- release tracking now survives window-specific focus changes and multi-keyboard / virtual-keyboard input paths

## User-Visible Symptoms

- a normal `toggle` binding could only open or close overview; it could not stay open as a transient switch session
- after the first implementation, repeated `Tab` presses worked, but release-exit could still fail for some target windows
- release-exit was especially fragile when the selected target changed real focus, because the plugin was relying too heavily on one keyboard state path and one release-event path

## Work Completed

### 1. Added a user-facing toggle-only switch mode

Plugin init now registers:

- `toggle_switch_mode = 0`
- `switch_toggle_auto_next = 1`
- `switch_release_key = Alt_L`

When enabled:

- the first `hymission:toggle` opens overview as a switch session
- subsequent `toggle` calls cycle forward through the current overview order
- releasing `switch_release_key` commits the selected target and closes overview

The mode is scoped to `hymission:toggle`; `open`, `close`, and gesture paths keep their existing behavior.

### 2. Reused overview order instead of inventing a second task-switch list

The cycle path follows Hymission's existing window order rather than introducing a compositor-global Alt-Tab list.

That means switch mode automatically inherits:

- multi-workspace ordering
- optional recent-first ordering
- the current selected / live-focus synchronization rules

### 3. Added explicit switch-session state and circular selection logic

The controller now tracks whether the visible overview is currently inside a toggle switch session.

While that state is active:

- hidden `toggle` enters switch mode
- visible `toggle` advances to the next window with wraparound
- close/abort/deactivate paths clear the switch-session state so stale release events do not trigger later

The circular step is implemented as overview logic rather than a reuse of directional keyboard navigation.

### 4. Hardened release-exit against missed per-window release events

The first cut depended too much on a single keyboard state and a direct key-release match.

That was not robust enough once live focus moved across real windows or when virtual keyboards were involved.

The final behavior is now backed by two layers:

- release-key held state is resolved across all active keyboards instead of only the first keyboard object
- the switch session starts a lightweight polling timer that watches the configured release key and closes overview once that key is no longer held

This keeps the direct release-event fast path, but no longer depends on that path being the only way out.

## Files Touched In This Iteration

- `src/main.cpp`
- `src/overview_controller.cpp`
- `src/overview_controller.hpp`
- `src/overview_logic.cpp`
- `src/overview_logic.hpp`
- `tools/overview_logic_test.cpp`
- `CMakeLists.txt`
- `meson.build`
- `README.md`
- `docs/spec.md`
- `docs/architecture.md`

## Validation

Validation performed during this iteration:

- `cmake --build build-cmake --target hymission hymission-overview-logic-test`
- `ctest --test-dir build-cmake --output-on-failure`
- repeated live plugin reloads with `hyprctl plugin unload ...` and `hyprctl plugin load ...`
- `ydotool` reproduction of `Super down -> Tab -> Tab -> ... -> Super up`
- targeted debug-log capture for switch-session arm, cycle, release close, and beginClose paths
- live manual validation that repeated `Tab` cycles correctly and release-exit no longer fails on the previously problematic targets

## Remaining Follow-Up

- toggle switch mode currently follows Hymission's existing overview ordering rather than a separate standalone task-switch order
- hover can still retarget the final committed selection during a switch session when `overview_focus_follows_mouse = 1`
- if a future refactor adds a dedicated standalone task-switcher, it should remain a separate feature from this toggle-scoped switch mode
