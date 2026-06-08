# Dev Log: 2026-03-08 Strip Bar Handoff And Crop Fix

## Summary

March 8, 2026 added the animated workspace-strip bar handoff for exclusive layer surfaces such as Waybar, then followed through on a live regression where the proxy handoff could lose its right edge when the source snapshot came back as a monitor-sized framebuffer.

The main outcomes were:

- exclusive bars can now hand off to a captured proxy instead of disappearing instantly when the strip is shown
- the handoff gained dedicated blur / move / scale / alpha controls
- monitor-sized layer snapshots are now cropped in two stages before entering the padded proxy framebuffer, so the right edge no longer gets clipped during animation
- the final live path keeps the bar visible while preserving the blur-padding proxy used by the strip animation

## User-Visible Symptoms

- Before this work, `hide_bar_when_strip = 1` simply hid matching exclusive bars as soon as the strip appeared.
- After the first proxy pass landed, `hide_bar_animation = 1` could crop tens of pixels off the right side of the bar during the animation.
- A first attempt to switch the whole capture path to raw framebuffer blits fixed the crop but temporarily made the proxy disappear, because it bypassed Hyprland's normal texture render semantics for the monitor-sized layer snapshot.

## Work Completed

### 1. Animated bar proxy handoff

Exclusive layer surfaces now get a captured proxy when the active-workspace strip is shown.

That proxy:

- captures the live bar into its own framebuffer
- renders in the overlay pass while the real layer is suppressed
- blends sharp and blurred versions of the same snapshot during the open / close motion
- follows strip progress with configurable move / scale / alpha behavior

### 2. Config surface for the handoff

The bar handoff now exposes explicit config knobs:

- `hide_bar_animation`
- `hide_bar_animation_blur`
- `hide_bar_animation_move_multiplier`
- `hide_bar_animation_scale_divisor`
- `hide_bar_animation_alpha_end`

Defaults were registered in plugin init and documented in the README so the runtime behavior and user-facing config stay aligned.

### 3. Right-edge crop fix for monitor-sized snapshots

Live logging showed that Waybar snapshots were coming back as monitor-sized framebuffers instead of bar-sized framebuffers.

The failing path looked like this:

- the source framebuffer was the full monitor render size
- the destination proxy framebuffer was slightly wider because it included blur padding
- Hyprland's fake-render path still interpreted draw boxes in monitor space
- the extra width on the proxy side caused the right edge of the animated bar to fall outside the effective monitor-space copy area

The final fix keeps Hyprland's texture rendering for the first crop step and only uses framebuffer-local blits after that:

- first crop the monitor-sized layer snapshot into a bar-sized temporary framebuffer with `renderTexture(...)`
- then blit that cropped result into the padded proxy framebuffer at the desired offset

This preserves the correct texture/projection behavior while removing the right-edge crop.

## Files Touched In This Iteration

- `src/overview_controller.cpp`
- `src/overview_controller.hpp`
- `src/main.cpp`
- `README.md`

## Validation

Validation performed during this iteration:

- `cmake --build build-cmake -j"$(nproc)"`
- live `hyprctl plugin unload ...` / `hyprctl plugin load ...` using `build-cmake/libhymission.so`
- `hyprctl plugin list`
- live `hymission:open onlycurrentworkspace` / `hymission:close`
- temporary `debug_logs` capture to confirm the monitor-sized snapshot path and the final cropped handoff path
- a live screenshot check to confirm the proxy bar was visible again after the crop fix

## Remaining Follow-Up

- the handoff is currently tuned for the observed Waybar path; other exclusive layer surfaces should still be spot-checked if they return different snapshot sizes
- if Hyprland later exposes a smaller-FBO-safe layer copy path directly, the current two-stage workaround could be simplified
