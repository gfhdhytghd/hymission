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
When the sidebar returns, visible cards enter in sequence following the incoming
workspace's vertical motion: downward motion reveals top to bottom, upward motion
bottom to top. The stagger uses the same `stage_transition_ms` timeline, so every
card arrives by the end of the transition. Returning from fullscreen during a
workspace swipe drives this reveal directly from finger displacement, including
holds and reversal. Release settles from that same progress without restarting
the reveal. Offscreen cards add no delay, and card
input resumes when the reveal completes.

No cards means no reservation. On unusually small outputs the controller may
go below the configured minimum card width, retaining at least 64 logical pixels
of desktop; if even an 8-pixel card cannot fit, that output's sidebar is suspended.
Resolved negative gaps become zero, minimum widths below 8 become 8, and maximum width
is raised to the minimum when positive limits are configured in reverse order.
Non-positive maxima select the automatic output-relative cap. Normalization is
logged on activation/config reload.

## Rendering and input

### Scrolling workspaces

Native `scrolling` tiled windows always clip to the reduced desktop viewport,
including their decorations, popups and native window close animations. They
cannot scroll into the sidebar or another output. In cards, the same viewport
maps to the card rectangle. Offscreen columns keep their native geometry and
scroll position; they are never moved or fitted back into view.

Inside these clips, a fixed band progressively blurs the window content and
fades it to transparency. The desktop has one band facing the sidebar (left by
default, mirrored when Smartisan changes sides); each scrolling card has both
left and right bands. Only windows crossing a viewport edge receive that edge
effect; a fully visible window keeps its own edge sharp. Opacity rises from zero
at the clip to full opacity halfway through the band,
independently of the smooth blur-strength ramp. Compressing this fade instead of
translating it keeps the clip boundary continuous.
The revealed background remains unchanged. Unpinned floating windows use the
same desktop boundary and edge effect in every layout. Pinned floating windows
render above Stage and retain native pointer input over it, without clipping.
Floating windows inside Stage cards also use the card's four-sided gradient
blur bands when their content crosses those edges, in every layout. They share
`stage_scrolling_preview_edge_width`; fully visible edges remain sharp. Card
endpoints handed to Overview preserve the same gradient and hard clip.
Special/fullscreen windows and non-scrolling tiled windows retain native policy.
Stage cards fade and blur each crossed edge independently, including top/bottom
for vertical scrolling and floating windows. Corners combine the strongest blur
and lowest opacity of their adjacent bands. The desktop seam remains horizontal.
Native desktop clipping uses the output height so shadows may extend above or
below the work area. Card clips and flight coordinate mapping stay unchanged.

`stage_scrolling_desktop_edge_width = 32` and
`stage_scrolling_preview_edge_width = 16` set independent widths in displayed
logical pixels. Zero or negative values mean **hard clipping**, never unclipped
rendering. Excessive widths are clamped to the viewport and bilateral bands
cannot overlap. Shader/resource failures also retain the hard clip. The state
command reports whether the blur hook is available and any effect error.

The effect renders native window passes into a reusable transparent RGBA layer
in the current pass, then composites it with premultiplied alpha. Native window
background blur samples the original backdrop. It does not start a nested
compositor render, capture static client snapshots or draw an opaque blur panel.
Full-resolution, horizontal-then-vertical Gaussian filtering uses paired bilinear
taps rather than a stretched sparse grid. Gaussian sampling stays inside the
clip; its support is bounded by the native
live-blur damage expansion (and at most 12 logical pixels). Width zero requires
no effect framebuffer. Framebuffers retain output precision and color space.

Workspace flights interpolate the viewport and its effect widths together;
window positions stay relative to that viewport. Completely offscreen columns
stay invisible, partially visible columns stay clipped, and interruption,
swipe cancellation and retargeting preserve the displayed crop. Existing
fullscreen, Overview and session-lock suspension rules still apply.

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
Floating windows returning from a card are an exception: only their initial
sidebar frame is fitted. They then interpolate directly to the native desktop
position and size, including partially offscreen targets. A sampled interrupted
restore is not fitted again, so neither completion nor retargeting jumps to or
from the output edge. Sidebar destinations retain their existing constraints.
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

With Stage interactive, scrolling-layout automatic focus intersects its work area
with the visible desktop for `scrolling:follow_min_visible`: both the visible intersection and
the percentage denominator exclude the sidebar. Horizontal layouts use desktop
width; vertical layouts use desktop height. Offscreen targets have zero overlap.
The native fit/center setting and already-visible-column behavior are retained;
clicks, keyboard focus and non-Stage input keep native handling.

The `hymission:scroll, layout` trackpad gesture targets the Stage card under the
pointer when it begins. It scrolls that workspace's scrolling-layout canvas
without activating the workspace or changing focus, using its layout direction
and monitor size. The target stays fixed until the gesture ends, even if the
pointer leaves the card. The gesture writes the real scrolling controller's
camera offset. On subsequent activation, Stage preserves that offset through
native focus/layout recalculation and restores focus to a window in the scrolled
viewport. This protection ends after activation; explicit window navigation can
move the camera normally. Clicking a specific window preview overrides the saved
camera position and scrolls that window into view, even if it was already focused.
Clicking card background or switching by workspace gesture retains the position.
Empty cards, sidebar gaps and non-scrolling layouts do
not pass the gesture through to the active desktop. Outside Stage, desktop and
Overview canvas scrolling retain their existing behavior. Two-finger/wheel axis
input still scrolls the sidebar's card list.

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
When a prepared swipe commits to a fullscreen workspace, Stage retains the
remaining flight until it finishes. Fullscreen coverage hides the resting
sidebar but does not cancel that flight or restart a native workspace slide.
The departing sidebar uses the same swipe progress and cubic easing as the
switch, including held gestures, release and cancellation. It does not start
an independent hide animation on commit. Side-exchange panes use that same
easing; ordinary sidebar show/hide uses `stage_transition_ms` instead of a
separate fixed duration.
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
removed and the old one becomes eligible. With `stage_show_active` enabled, the
active workspace stays in the sidebar alongside every other workspace and its
card keeps live previews; clicking one of its windows focuses that window. Set `stage_active_glow = 1`
to add a soft glow outline in `focus_selected_color`. Both `stage_show_active`
and `stage_active_glow` default to `0`. With `stage_empty_slots` set, missing numbered
workspaces up to that id are listed as synthetic empty slots that
create the workspace on click or drop; when `stage_backdrop` is enabled,
every card draws the monitor's background and bottom layer surfaces (the
desktop region) as a backdrop (cards are transparent by default), and
`stage_even_spacing` stretches
the vertical gaps so the stack spans the sidebar evenly. There are no numeric/name labels.
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
floating targets map the pre-release preview center back into the destination
desktop and place the window around that center, avoiding a jump from the
original mouse grab offset. Dropping on the current workspace does not
issue a second move. Escape cancels sidebar delivery; no resize or application
data drag is interpreted as a window move.

Dragging a floating window from the desktop into a Stage card slides its live visual out through
the source region's facing edge, then in through the destination region's facing
edge. Each half keeps that region's clip and window size fixed; the size changes
while hidden between the halves. The whole transfer uses `stage_transition_ms`.
Leaving that card uses the same two-phase return to the native dragged window.
Drags picked up inside Stage retain their original direct animation throughout,
including cross-card movement and a temporary excursion over the desktop.
Moving within Stage, including between cards, retains the original direct
preview movement and drop-settling animation. Strip padding and inter-card gaps
also count as Stage during native desktop drags: desktop -> card A -> gap ->
card B does not restart a desktop exit/entry. Reversing a desktop/card transfer
starts from the current visual and clip. The native drag and
workspace remain unchanged until release. Escape cancels the preview/drop.
Ordinary workspace-switch animations are unchanged.
Originally tiled windows retain direct drag transitions above the Stage strip;
the compositor's temporary floating state during a move does not enable edge
transfers. When a dragged preview returns to the desktop, native decorations
are drawn immediately, independently of `stage_window_decorations` (which still
controls resting card previews). Release continuation retains these decorations.
The window's native rounding also takes effect immediately on the desktop.
Desktop drag targets keep their native size and pointer-relative position even
when extending beyond the output; hover and release animation endpoints are
not fitted back inside the desktop. Card targets remain fitted to their cards.
With the default `stage_drop_follow = 0`, release continues from that preview
into the window's actual destination slot, suppressing the card copy until the
flight completes. Disabled animations update immediately; follow mode uses the
normal workspace-switch animation after release. `drag_hover_active` exposes
the hover-preview state.
For a silent drop into a hidden workspace, automatic focus requests for that
specific window are ignored for up to two seconds while the original workspace
remains visible. A new key press, pointer press, scroll, or explicit window/
workspace selection ends or bypasses this protection; `stage_drop_follow = 1`
also bypasses it. This covers delayed focus requests after the drop handler ends.
`last_drop_trace` records the latest release's native drag-end, move, placement
and focus phases, along with workspace-change requests during the release and
the following two seconds. It contains workspace IDs and policy flags, not
window titles, and can be read through `hymission-stage-state` for unexpected
follow behavior.
Suppressed requests appear as `suppressed_drop_focus` with the compositor's
numeric focus reason.

Win/Super + left-drag on a window inside a sidebar card picks the topmost
preview under the pointer. Dragging into the current desktop enlarges it;
dragging onto another workspace card keeps it miniature. The original window
stays in its workspace until release. Dropping on the desktop or a different
card commits through the normal workspace move/layout path. Dropping in empty
sidebar space, on the original card, or pressing Escape cancels the move.
Plain clicks on a window preview switch workspace and focus that window;
overlapping previews select the topmost visible window. Clicking empty card
space keeps the usual workspace activation behavior. `thumbnail_drag_active`
reports this drag separately from a native desktop-window drag.

Overview/raw capture/input suppression, special workspaces and session locking
suspend stage drawing/input. They do not remove the native reservation.
In `onlycurrentworkspace` Overview, the sidebar slides toward the outside edge
(left in the default layout), following timed or gesture progress and reversing
on cancellation. It is composited below overview windows. `forceall` keeps the
sidebar suppressed because overview already animates those same windows.

Opening Overview captures Stage's displayed window rectangles before suspension.
Sidebar windows expand from their current card previews (including scroll and
flight offsets), while active desktop windows retain their desktop origins.
Timed and gesture opening share these origins; refreshing the Overview layout
during opening preserves them. Native geometry remains separate from these
animation-only origins. Partially visible windows carry their actual card/strip
clip and edge gradient into Overview; these unfold continuously as the window
expands and return to the prepared destination clip on exit, including when a
new workspace is selected. On the card's outside edge (left for a left sidebar,
right for a right sidebar), the fade and clip retain the card edge's x coordinate
through reveal and reversal; their y coordinates may move. The desktop-facing
seam retains its existing transition. Fully clipped windows use offscreen endpoints.
Gesture rollback prepares the original workspace's Stage destination before
timed closing begins, including the `recommand` return-to-desktop branch.
Cancelling a close reuses its prepared scene when reopening.
Interrupted transitions retain the sampled position, clip and gradient before
heading toward the new endpoint. `hyprctl hymission-overview-state` window diagnostics include
`stageOpeningReveal`, `stageClosingReveal` and the current Stage transition
clip, viewport and edge widths.

Overview window surfaces retain native transparency and use live backdrop blur
during entry, exit and the settled overview. Blur planning uses the same
transformed surface state as drawing. Deferred Stage shadow passes retain their
window owner and transform the native interior cutout with the preview, so the
shadow does not fill the transparent window layer. Offscreen workspace-strip snapshots still
disable window blur because their export framebuffer may differ from the output.

On the normal desktop, native closing snapshots are classified individually by
their animated window bodies. A fully visible closing window keeps sharp edges;
a window crossing the scrolling boundary retains that edge effect. Native
close geometry, opacity and draw order remain in use.

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

Workspace swipes involving fullscreen retain the desktop-to-card scaling path.
When leaving fullscreen, incoming windows grow from their thumbnail positions in
the hidden sidebar just outside the output. When entering fullscreen, outgoing
windows shrink to those hidden sidebar positions. Only the thumbnail endpoint
is translated beyond the sidebar edge; rendering stays clipped to this output.

## Window capture integration

`hyprctl hymission-stage-state` exposes `captureVersion: 1` and a bottom-to-top
`captureWindows` array. Each entry contains the native window `address`, its
`selectionGeometry` in global logical coordinates, and `selectionClipGeometry`.
These rectangles come from the last rendered card previews, including card
scroll, slide offsets, and output clipping. Hidden/fullscreen-covered windows,
empty cards and clipped-out previews are omitted. Capture clients must retain
clipping and stacking, and resolve the address again before rendering a window.

HyprCapture consumes this optional capability for Stage window selection. It
captures the original window without activating its workspace; editor captures
are centered at a usable display scale without resampling the exported image.
The existing token-based `hymission-capture-input begin|end` handshake suspends
Stage interaction while the screenshot UI owns input, preventing a click from
also activating a workspace.

After updating both plugins, verify window/fusion selection on both sidebar
sides, overlapping tiled/floating previews, a partially scrolled card, inactive
workspaces, a fullscreen preview, and a pinned floating window over Stage.
Confirm editor centering, native output resolution, unchanged workspace/focus,
and cancellation restoring normal Stage interaction. Repeat with a target
closing before its deferred capture. Build and offscreen UI tests do not replace
these checks in the compositor.
