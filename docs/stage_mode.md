# Persistent workspace sidebar

Stage mode lives in its own controller, with pure geometry in `stage_logic`.
It does not run overview layouts, scale real windows, warp the pointer or change
the physical output mode. It is disabled by default.

## Geometry and layout ownership

Start with the monitor's logical box and apply its existing reserved area once.
For available dimensions `W × H`, directional padding `left/right/top/bottom`,
desktop gap `g`, card gap `q`, and `N` visible inactive workspaces,
define `A = W − left − right − g` and
`L = H − top − bottom − (N−1)q`. The largest fitting card width is
`c = L*A / (N*H + L)` when `L > 0`. Clamp to the configured minimum/maximum,
then derive desktop width `D = A − c` and card height `h = c*H/D`.
The default maximum (`stage_card_max_width = 0`) is one fifth of the output's
logical width, independently on each monitor. Positive values are pixel overrides.
At the minimum width, any excess height becomes scrollable content.

Sidebar spacing defaults to Hyprland's global gaps and updates on configuration
reload. Outer left/top/bottom margins follow `general:gaps_out`, including
asymmetric and zero values. Between cards, both adjacent `general:gaps_in`
edges are added. The desktop-facing margin adds only the remainder of the
left/right inner gap after the native desktop's left outer gap, clamped at zero;
this avoids adding the native outer gap twice. Workspace-specific gap overrides
remain owned by the native layout. The three `stage_*` spacing options default
to `-1` for automatic spacing and accept nonnegative manual overrides.

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
Resolved negative gaps become zero, minimum widths below 8 become 8, and maximum width
is raised to the minimum when positive limits are configured in reverse order.
Non-positive maxima select the automatic output-relative cap. Normalization is
logged on activation/config reload.

## Rendering and input

### Smartisan mode

`stage_smartisan_mode = 1` enables an optional per-monitor side transition.
The sidebar starts on the left. On the first ordinary workspace switch, the
selected workspace expands into the left desktop area; the old desktop shrinks
into its matching right-hand card. The remaining left sidebar translates out
through the left edge while its new contents enter from the right edge, both
moving left on the same transition timeline. Every subsequent ordinary workspace
switch exchanges the sides again: right-to-left mirrors the complete animation,
moving right. Each monitor alternates independently. Disabling this mode returns
the sidebar to the left.

Both copies are clipped to the owning monitor's logical rectangle, converted
to render pixels; live window flights additionally inset this limit by each side's
`general:gaps_out`. Neither copy is
submitted to an adjacent monitor. The native work-area reservation, sidebar
hit tests, preview coordinates, floating-window correction and drag-drop focal
points all follow the new side. Input to the sidebar is paused during the
edge transition. Zero duration or disabled animations changes sides immediately.
Fullscreen/overview/session lock and output reconfiguration cancel the visual
transition safely. Mode/side/transition state is exposed in `hymission-stage-state`.

### Window flights

Workspace switches animate outgoing windows into their sidebar card and incoming
windows from their previous card into the desktop. Both directions share one
linear timeline `t`. Growing windows move each corner along a straight segment
from its starting corner to its destination corner, with shared cubic ease-out
`1-(1-t)^3` for position and size. Shrinking windows retain smoothstep center
translation `t*t*(3-2*t)` and cubic ease-out size. Rounding uses cubic ease-out.
Incoming desktop windows are drawn last, above departing windows.
Growing endpoints are fitted inside the output inset by each side's
`general:gaps_out` before interpolation, keeping the entire straight path inside
that boundary. Shrinking frames are constrained individually. Oversized windows
are fitted uniformly. Decorations use the same inset clip. Swipe release and
rapid retargeting share these rules.
`stage_transition_ms` defaults to 300 ms (0 disables, maximum
2000 ms), and disabling Hyprland animations also disables these flights.
Rapid switches retarget from the currently displayed boxes and rounding.

Flights render current native window surfaces, with live blur against the
backdrop. Native window rendering is suppressed only for flight participants.
The workspace animation hook resolves the native slide/fade immediately, and
the workspace-change hook prepares Stage's flight before returning to the
dispatcher. This applies to sidebar clicks and ordinary keyboard dispatchers.
Native layout supplies final positions and sizes; its animation values are
resolved once after saving source boxes, so there is no second layout animation
after the Stage flight. Pinned windows never fly.
Session lock, overview, fullscreen, output geometry changes and config reload
cancel flights. Flight metadata is released when the motion timer finishes or cancels.
The real client geometry and pointer coordinate system remain native throughout.

Workspace swipes use the same window flights and sidebar transition.
The registered workspace trackpad gesture sends begin/update/end directly to
Stage, including inversion, scaling and cancellation. Begin only initializes;
Hyprland immediately sends the first displacement through update, where it is
consumed once with the configured axis sign. It does
not depend on optional native function hooks to enter the follow-finger state.
Overview owns the native unified-swipe hooks for other entry points and forwards
desktop gestures to Stage; Stage never registers a second hook for those functions. Surface box,
visible-region and UV hooks transfer to Overview before it attaches, and return
to Stage only after Overview detaches (including failed activation rollback).

While the finger moves, a provisional view advances with the gesture; the actual active
workspace changes only after release passes the native distance/speed threshold.
Cancellation reverses that view back to its origin without changing workspace.
Direction locking, inversion and monitor-local workspace selection remain native
policies. A previously nonexistent destination is previewed as an empty desktop
during the swipe, including the outgoing windows' flight into the sidebar.
Its native workspace object is created only on commit; cancellation leaves no
new workspace behind. Release continues from the current preview. Native swipe
rendering is not run alongside the Stage transition. State output includes
`swipe_active` and `swipe_progress`. Cumulative `swipe_begin_count`,
`swipe_update_count` and `swipe_end_count` expose Stage delivery;
`raw_swipe_update_count` independently counts compositor swipe events.

Only inactive workspaces appear; after a switch the new active workspace is
removed and the old one becomes eligible. There are no numeric/name labels.
Empty-workspace slots are transparent. Hovering does not draw a frame.
Retained cards animate vertical position changes using the transition duration;
hit testing and drop mapping follow their displayed positions throughout.

Mapped, non-hidden
non-pinned windows belonging to each target workspace
retain their original positions, relative sizes, overlap and stacking order.
A single transform maps the reduced desktop into the card; windows extending
outside that desktop are clipped, not rearranged. Live client geometry is unchanged.

Each window's current surface tree is rendered through `CSurfacePassElement`
during the sidebar's live render pass. There are no cached miniature framebuffers
or fake render begin/end calls. Surface boxes and visible regions use the same
uniform transform, and UV calculation uses the committed viewport size so preview
scaling cannot be mistaken for client resizing and crop the texture. Native
surface drawing retains buffer synchronization and presentation/frame feedback.
Windows that Hyprland considers
blur-enabled blur the wallpaper and lower previews behind them at their displayed
size, respecting global blur and window no-blur rules. The sidebar and
cards have no background fill, leaving the actual wallpaper visible between
windows. Window main surfaces are previewed; transient popups are not separate
miniatures. Compositor decorations default off (`stage_window_decorations = 0`);
application-drawn titlebars remain client content. Enabling decorations includes
their extents in each window's preview footprint. Native decoration passes draw
inside the current pass with the preview transform and output/card clip.

On plugin exit the pending render pass is cleared before controller destruction
and again before returning to the loader. Hyprland retains pass elements until
the next render begins, and even native elements created here can carry a
smart-pointer deleter compiled into the plugin. Leaving them queued across
`dlclose` can crash the next render (including another plugin's capture).
For the first upgrade from a version without this cleanup, restart the compositor
from a safe context: the new exit code cannot repair the old loaded version's exit.

`stage_window_rounding` sets the radius at the miniature's displayed logical
size, independently of the real window. Its default `-1` follows half the system
`decoration:rounding`; `0` disables rounding. Surface render data receives the
displayed radius directly. Decoration radius overrides and all preview render
state are scoped to each draw. Preview refresh never changes workspace visibility,
opacity, offsets, renderer framebuffer size or projection.

Visible cards schedule repaint every 16 ms by default (`stage_refresh_ms`, clamped
to 1–16 ms). Their clients are temporarily unsuspended so frame callbacks can
produce fresh content; windows leaving the visible preview set regain native
suspension policy. Hidden/covered sidebars stop repainting and sending preview
frame callbacks. Output refresh, client update rate and GPU load determine the
actual frame rate. `preview_render_fps` reports the measured composition rate over
one-second samples; this counts rendered frames, not unique client buffers.
`preview_renderer` reports `native_surfaces`, and cards expose `previews_ready`.

The sidebar is a render-pass element above windows. It intercepts input only in
its own band; native window hit testing excludes windows behind that band. Mouse
press/release ownership is paired across mode transitions. A press begun outside
the sidebar keeps its release, so application file/text drags are not consumed.
Native move drags are observed through the compositor drag controller; the
zero `binds:drag_threshold` setting accepts immediate drags without requiring
the native threshold flag, while positive thresholds must be reached. The
normal drag end restores floating/tiling and ends the grab before the native
workspace-move path commits the drop. This path also preserves native group
movement and cross-monitor behavior. The release point is mapped from the animated
card's rectangle into the destination desktop's global logical coordinates.
Tiled targets are reinserted using the native algorithm's focal-point API;
floating targets retain their grab offset at the mapped point. Dropping on the current workspace does not
issue a second move. Escape cancels sidebar delivery; no resize or application
data drag is interpreted as a window move.

With the default `stage_drop_follow = 0`, a successful drop shrinks the live
window from its release position into its destination preview, using
`stage_transition_ms` and the normal flight boundary/rounding rules. The card
copy is suppressed until the flight completes. The destination follows card
motion during the animation. Disabled animations apply the drop immediately;
follow mode uses the normal workspace-switch animation instead.

Overview/raw capture/input suppression, special workspaces and session locking
suspend stage drawing/input. They do not remove the native reservation.
Fullscreen coverage is per output; composing an in-progress slide clears only
that output's solitary-client shortcut, without taking ownership of the global
direct-scanout flag used by overview.

## Compatibility and verification

The controller requires the current Hyprland work-area, native drag-end,
window-hit-test, rounding and window-render symbols, and the OpenGL renderer. It refuses
to enable if a required hook is unavailable, leaving the native layout intact
and reporting the failure via notification and `hyprctl hymission-stage-state`.
It adds no extra process or layer-shell client.

Both CMake and Meson include the stage controller and `hymission-stage-logic-test`.
The automated tests cover coupled card/desktop aspect, width limits, portrait
and fractional-scale logical geometry, overflow, scroll hit testing/reveal,
drag-width freezing, empty outputs, invalid settings, coverage policy,
output-relative width caps, miniature aspect/non-overlap and independent rounding.
They do not substitute for the following compositor acceptance checks:

1. Enable with multiple tiled windows. Verify their actual sizes change and
   typing, scrolling, popups and clicks remain aligned at 100% and fractional
   output scales. Repeat with dwindle, master and scrolling layouts.
2. Verify all eligible windows are laid out as non-overlapping miniatures with
   their original aspect ratios. The current workspace and all number/name
   labels must be absent. Wallpaper should remain visible between miniatures.
   Check default/zero/custom rounding and decoration toggling, including a
   rotated output and a bar with a changing exclusive zone.
3. Add/remove workspaces; test both empty-workspace policies, few cards at maximum
   width and many cards at minimum width. Check scrollbar-free wheel navigation,
   top/bottom edge scrolling during a drag, and switching a card out of the list.
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
   desktop does not repeatedly relayout and hidden sidebars stop repainting.
7. Run animated content in visible sidebar windows and verify at least 30 fps
   with `preview_render_fps`, as well as fresh client content. Inspect Discord/QQ/
   WeChat/Telegram main surfaces and subsurfaces for clipping or distortion.
8. Switch with a key binding and with short/long/reversed/cancelled swipes. Check
   follow-through, cancellation, optional new workspaces and both sidebar sides.
   Sample early/middle/late flight frames: window edges, borders and shadows must
   stay within their owning output, with no second native animation at handoff.

Use `hyprctl hymission-stage-state` for computed card/desktop geometry and state,
`hyprctl -j clients` for real application geometry, and `hyprctl configerrors`
for configuration errors. Build and logic-test success are source verification;
the checks above require the updated plugin running in a compositor session.

On this repository's hyprpm setup, commit before updating. The user runs
`hyprpm update` from a safe context; do not mix manual plugin loading with that
managed instance.
