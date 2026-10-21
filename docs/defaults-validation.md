# Defaults and search placement validation

Target: stable Hyprland **0.56.2**, revision
`efb50993780079460b0cbed1363e2166a2de1d9f`. This change is on
`feat/defaults-search`, based on master plus search styling commit `5436e4f`.

## Findings and behavior

- Defaults: niri scrolling preview gap **24**, selected outline **3**, backdrop
  ARGB **0x26000000** (`rgba(00000026)`), blur still **0**.
- Previously the helper's top margin was 28, with no corresponding reservation
  in overview layout. In the nested stable compositor layer-shell itself added
  the exclusive top zone: the observed helper y was 28 without a bar and 76 with
  a 48px bar. It did not account for the workspace strip. A top strip rendered
  relative to the full output while preview layout subtracted reserved areas.
  Search could therefore cover the strip, and top padding 92 was a workaround
  rather than a shared layout contract.
- Now reserved areas, strip, and an always-present 64 logical pixel search band
  are composed in that order. Search is centered in the band, including on side
  strips. Explicit layer-shell zone -1 makes these margins output-relative,
  preventing double subtraction of exclusive zones. Initial spawn placement and
  rebuild IPC updates use the same geometry. Actual surface-size changes recenter
  through GdkSurface's layout signal, without a continuous frame callback.
- Hiding exclusive bars changes rendering only; their reservation remains 48px
  in both visible/hidden strip scenes. The top strip now begins below that zone.
- Top padding remains additional: default 32 plus search band 64 gives 96 pixels
  after strip/reserved areas. Existing 92 remains usable; users who used it only
  to avoid search can migrate to 32, close to the former 92-pixel clearance.
- The helper CSS from `5436e4f` is byte-identical (SHA-256 of the CSS provider
  block: `6eb3a42d760fad3ec0a7ede7eebedc5950b5dab07b50a9b0b98fb96ac2220548`).

## Backdrop path audit

`OverviewController::renderBackdrop` returns before adding a render pass when
alpha is zero and blur is disabled. Nonzero alpha adds one blended, output-sized
`CRectPassElement`, with color alpha multiplied by `visualProgress()`. Blur is
still disabled; the tint adds no framebuffer, snapshot, texture allocation or
blur pass. These are source-path observations, not a GPU performance benchmark.
The existing full-output overview damage policy applies with either color.

`renderStage` excludes strip snapshot depth and native window capture, and
requires visible overview on an owned monitor. Stage-only rendering is unaffected;
strip snapshots and Stage native captures do not recursively acquire this tint.
No additional nonzero-color rendering defect was found. This master-based branch
has ordinary overview preview tiles, but no development workspace `TilesController`;
workspace-tiles runtime compatibility is outside this branch's evidence.

## Verification

Build used the task's `scratchpad/pc056` through `PKG_CONFIG_PATH`. Its pc version
label is 0.56.0, but `/usr/include/hyprland/src/version.h` and `/usr/bin/Hyprland`
both identify the exact 0.56.2 revision above. Full CMake build and **13/13 CTest**
tests passed, including new reserved-area/strip/search geometry cases.

All screenshots came from a private 1280x720 Weston headless parent and nested
`/usr/bin/Hyprland`, with isolated runtime, configuration, IPC and Wayland sockets.
The host compositor was not reloaded or used as the test target. Test-only parent
Wayland shims cap protocol bindings to Weston's advertised versions and defer
Aquamarine's first buffer commit until xdg_surface configure acknowledgement.
The installed compositor/plugin binaries and their render code are unmodified by
those transport shims. These captures establish nested behavior, not host-session
acceptance or an unmodified Aquamarine startup test.

Local artifacts, relative to the worktree:

- `build-defaults/comparison.html`: eight before/after screenshot pairs.
- `build-defaults/{before,after}/{no-bar,bar}-{no-strip,strip}-search-{hidden,shown}.png`:
  the full requested matrix, with matching JSON state and layer geometry.
- `build-defaults/after/{left,right}-strip-search-shown.png`: side-strip centering.
- `build-defaults/after/legacy-padding-92.png`: existing padding configuration.
- `build-defaults/after/automatic-search-default.png`: default automatic helper.
- `build-defaults/{before,after}/{transparent,tint}-{overview,stage}.png`: backdrop
  and Stage comparisons. A clear wallpaper pixel changes 17 to 14 in overview;
  Stage-only stays 17 with either setting.
- `build-defaults/after/{opening,closing}-*.png` and `fade-samples.json`: fade
  captures and surrounding overview progress observations. A known gray wallpaper
  pixel opens through 125, 117, 112, 110, 109 and closes through 109, 112, 118, 128.
- `build-defaults/verification.json`: artifact hashes and automated checks for
  unchanged matching-window geometry while typing, search/preview separation,
  exactly-once exclusive zone placement, monotonic fade and unchanged CSS.
- `build-defaults/{configure,build-before,build-final,ctest}.log`: build/check logs.
- `build-defaults/harness/{capture,verify}.py`: local capture/check scripts and
  companion parent/marker/bar sources. These generated artifacts are gitignored.

Observed search geometry (42px height; full-width centered x=408):

| Scene | Before y | After y |
| --- | ---: | ---: |
| No bar, no strip | 28 | 11 |
| No bar, top strip | 28 | 195 |
| Exclusive bar, no strip | 76 | 59 |
| Exclusive bar, top strip | 76 | 243 |

All four matching-window layouts were identical before/after starting search and
entering `marker`. Every revised preview content rectangle cleared search by at
least the configured 32px additional padding. Existing padding 92 and default
automatic helper startup were also captured successfully. No push, host install,
`hyprpm update`, or host plugin reload was performed.
