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
