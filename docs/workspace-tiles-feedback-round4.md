# Tile overview feedback round 4

Baseline: c2f0b40. Release compositor e5a03b6a5ce64ec4c85954649bad4df083627574,
matching cached hyprpm headers. Experiments use private Weston/Hyprland sessions;
no host reload, input, hyprpm update or push.

## 1. Shadow rectangle during opening

The ordinary tile overview shadow pass used preview geometry for the shadow but
left native shadow exclusion in desktop coordinates. An opaque window covers
most of this normally, but the portion outside the revealed viewport exposes a
filled rectangular shadow. This is independent of the soft-edge shader.

Map the native window cutout and rounding along with the preview for enabled
multi-tile outputs, as the existing Stage transition path already does. Preserve
the original tiles-disabled path and the real outer shadow/soft-edge parameters.

A private fixture records every rendered frame with an undiscardable final pass,
using native CGLFramebuffer::readPixelsRGBA8 on the main workbuffer (including
native binding/pack-state restoration). It records overview, tile, Stage and
native alpha/offset state beside each PNG. Readback adds stalls; these are visual
regression frames, not performance measurements. Earlier raw glReadPixels trials
read the wrong framebuffer or used unsupported RGB readback and were discarded;
only `*-native`/`*-verified` results below are evidence.

1920x1080, two tiles, scrolling width .85, three real CSD marker clients per
workspace, Stage off/on, ws-only and forceall: baseline ws-only Stage-off frame 2
has a 131-pixel continuous dark interior stripe. The fixed regression observes
7/1/5/1 pixels across the four cases, below the 16-pixel allowance for the actual
20-pixel soft shadow near a clipped boundary. Every case includes intermediate
opening frames and retains normal borders/shadows.

Evidence: `build-tiles/fb4-baseline-native/switch-onlycurrentworkspace-stage0-open-02.png`,
its paired JSON, and `build-tiles/fb4-shadow-verified/` (all frames and observations).
Run with `--feedback4-regression shadow`; `--feedback4-observe` preserves failing
baseline observations without accepting them as passing regressions.

## 3. Workspace transitions stay in the selected tile

The target-state override previously replaced the entire monitor's workspace
collection with the new workspace. The transition then applied its monitor-wide
pan to all source items, including other tiles. Add a tile identity to the
transition and its override: replace just that tile's workspace, preserve the
other views, leave their preview rectangles stationary, and use the tile width
for a horizontal transition. Commit to the stored tile through the activation
transaction, including the synthetic-workspace rebuild path; pinned-window
migration uses that tile's old workspace. The non-tile path is unchanged.

Native workspace-dispatch reproduction while ws overview is active records 11
moving frames on the non-focused tile in each Stage-off/on baseline. The fixed
run records zero moved/missing frames in both cases, including the committed
state. Evidence: `fb4-baseline-native/switch-stage{0,1}.json` versus
`fb4-switch-fixed/switch-stage{0,1}.json`, with every-frame PNGs in each directory.

## 2. Non-focused tile flashes during close handoff

`shouldRenderWindowHook` used the physical monitor active workspace to classify
windows during `m_deactivatePending`. Its exit filter therefore hid the other
tile's still-presented workspace for the handoff frame. Include presented tile
workspaces in that existing visibility decision; hidden workspaces remain
filtered, and tiles-disabled behavior is unchanged.

The final recorder also saves native damage rectangles and raw workbuffer PNGs.
Undamaged workbuffer contents are not guaranteed to represent the displayed
output: compose each frame from the prior frame plus the current damaged
rectangles. Each capture starts on the full-output damage from open/close/pan.
A separate native screencopy at the settled endpoint is retained for comparison.
The debug fixture now records actual current framebuffer identity too (equal to
the main framebuffer in these runs). Raw partial-damage black regions are not
counted as flashes.

Baseline `fb4-close-before-final` has exactly one fully-damaged blank-neighbor
frame (index 8) in ws-only close with Stage off, and another in ws-only close
with Stage on. `fb4-close-after` has zero blank-neighbor frames in all four
ws-only/forceall × Stage-off/on cases. Native workspace/window alpha stayed one;
this was an admission/filtering error, not an alpha animation. Every-frame
native damage/state, reconstructed PNGs, raw PNGs and endpoint screencopies are
retained in these directories. The regression fails if an occupied non-focused
tile loses its marker pixels during the close or handoff.
