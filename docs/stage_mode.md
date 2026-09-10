# Persistent workspace sidebar

Stage mode lives in its own controller, with pure geometry in `stage_logic`.
It does not run overview layouts, scale real windows, warp the pointer or change
the physical output mode. It is disabled by default.

## Geometry and layout ownership

Start with the monitor's logical box and apply its existing reserved area once.
For available dimensions `W × H`, padding `p`, desktop gap `g`, card gap `q`,
and `N` visible workspaces, define `A = W − 2p − g` and
`L = H − 2p − (N−1)q`. The largest fitting card width is
`c = L*A / (N*H + L)` when `L > 0`. Clamp to the configured minimum/maximum,
then derive desktop width `D = A − c` and card height `h = c*H/D`.
At the minimum width, any excess height becomes scrollable content.

The work-area hook calls the original `CSpace::recheckWorkArea` first, then
insets both fresh tiled and floating work areas. This preserves per-workspace
gaps and never writes `CMonitor::m_reservedArea`, layer-shell reservations, or
monitor dimensions. Geometry changes invalidate all of the monitor's workspaces,
including inactive ones. Ordinary floating windows are not resized; only
completely unreachable placements are brought back into view, outside a drag.

Maximized workspaces bypass this inset only when configured to cover the strip.
True fullscreen keeps its normal full-output geometry. The sidebar's visual
slide does not resize the native desktop on every animation frame.

No cards means no reservation. On unusually small outputs the controller may
go below the configured minimum card width, retaining at least 64 logical pixels
of desktop; if even an 8-pixel card cannot fit, that output's sidebar is suspended.
Negative gaps become zero, minimum widths below 8 become 8, and maximum width
is raised to the minimum when configured in reverse order. Normalization is
logged on activation/config reload.

## Rendering and input

Offscreen capture runs outside a compositor render pass. It clears a per-monitor
scratch framebuffer to transparent and renders only mapped, non-hidden windows
belonging to the target workspace (plus its monitor's pinned windows), including
decorations and popups. It never calls overview's workspace re-layout or renders
wallpaper/layer surfaces. Temporary workspace visibility, opacity and render
offset overrides are restored before returning to the event loop; the real
active workspace is never switched for capture.

The reduced desktop rectangle is cropped from the logical-orientation export
framebuffer into each card. Display rendering uses physical pixels only at the
final composition boundary. Only visible cards are captured; dirty offscreen
cards refresh when scrolled into view. Each output owns one reusable full-size
scratch buffer, plus its small card textures. Removing outputs/cards releases
their resources. Suspended/covered sidebars skip capture.

The sidebar is a render-pass element above windows. It intercepts input only in
its own band; native window hit testing excludes windows behind that band. Mouse
press/release ownership is paired across mode transitions. A press begun outside
the sidebar keeps its release, so application file/text drags are not consumed.
Native move drags are observed through the compositor drag controller; the
normal drag end restores floating/tiling and ends the grab before the native
workspace-move path commits the drop. This path also preserves native group
movement and cross-monitor behavior. Dropping on the current workspace does not
issue a second move. Escape cancels sidebar delivery; no resize or application
data drag is interpreted as a window move.

Overview/raw capture/input suppression, special workspaces and session locking
suspend stage drawing/input. They do not remove the native reservation.
Fullscreen coverage is per output; composing an in-progress slide clears only
that output's solitary-client shortcut, without taking ownership of the global
direct-scanout flag used by overview.

## Compatibility and verification

The controller requires the current Hyprland work-area, native drag-end,
window-hit-test and window-render symbols, and the OpenGL renderer. It refuses
to enable if a required hook is unavailable, leaving the native layout intact
and reporting the failure via notification and `hyprctl hymission-stage-state`.
It adds no extra process or layer-shell client.

Both CMake and Meson include the stage controller and `hymission-stage-logic-test`.
The automated tests cover coupled card/desktop aspect, width limits, portrait
and fractional-scale logical geometry, overflow, scroll hit testing/reveal,
drag-width freezing, empty outputs, invalid settings and coverage policy.
They do not substitute for the following compositor acceptance checks:

1. Enable with multiple tiled windows. Verify their actual sizes change and
   typing, scrolling, popups and clicks remain aligned at 100% and fractional
   output scales. Repeat with dwindle, master and scrolling layouts.
2. Compare each card to its workspace: relative window positions, overlap,
   decorations and transparency should agree; wallpaper and bar must be absent.
   Check a rotated output and a bar with a changing exclusive zone.
3. Add/remove workspaces; test both empty-workspace policies, few cards at maximum
   width and many cards at minimum width. Check scrollbar-free wheel navigation,
   current-card reveal and top/bottom edge scrolling during a drag.
4. Click a card, then drag tiled, floating and grouped windows onto other cards.
   Test same-workspace and cross-monitor drops. Default drops must not switch
   workspaces; enabling follow must switch and focus the moved window. Test
   Escape, disappearing destinations and release outside the strip. Application
   text/file drags and resizing must keep their ordinary behavior.
5. Exercise true fullscreen, both maximization policies, rapid entry/exit,
   overview and special workspaces. Other monitors must remain independent.
   Check that focus does not remain attached to a moved, hidden window.
6. Check session lock/unlock, output hotplug, config reload and disabling the
   feature. Work areas and native hit tests must restore. Confirm a static
   desktop does not repeatedly relayout and hidden sidebars stop capturing.

Use `hyprctl hymission-stage-state` for computed card/desktop geometry and state,
`hyprctl -j clients` for real application geometry, and `hyprctl configerrors`
for configuration errors. Build and logic-test success are source verification;
the checks above require the updated plugin running in a compositor session.

On this repository's hyprpm setup, commit before updating. The user runs
`hyprpm update` from a safe context; do not mix manual plugin loading with that
managed instance.
