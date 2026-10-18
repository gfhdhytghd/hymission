# Stage desktop vertical extent (feedback round 5)

## Root cause and history

Baseline: 9c92cb5. The issue was introduced by **4a439a521d1ec463712889528f755a46009b8168**, not by the later camera or adaptive-sizing follow-ups. `git blame 9c92cb5 -L 31,38 src/stage_logic.hpp` attributes the entire old `scaleDesktopRect` body to 4a439a5. The same commit replaced `afterRecheck()`'s native `reservedWorkArea()` path with it for clustered tiles.

The old transform used `s = 1 - reservation / outputWidth` for **both axes**, and added `baseHeight * (1-s) / 2` to Y. Thus giving the sidebar horizontal room also shortened every tiled/floating work area and the desktop viewport, creating real resized windows and empty bands above/below. Before 4a439a5 the native Stage helper only changed X/width. e9d68cd and 1c344e6 changed preview/fullscreen/camera handling, while 54a7adf changed the shared reservation and adaptive card sizing; none removed this old Y/height formula.

An isolated run of the archived 4a439a5 binary also reproduces reduced height. Its older sidebar budget differs from 9c92cb5, so its amount of shrinkage is not an equal-reservation comparison. Source history establishes introduction; the runtime runs confirm the behavior in that revision and the requested baseline.

## Fix

- Keep one affine **horizontal** compression/translation for the whole physical output. Preserve each input work area's native Y, height and vertical gaps through the entire Stage animation. Neighbor tile boundaries stay contiguous; the shared sidebar reservation is still applied once.
- Keep cards at one common height/thumbnail scale, widths proportional to tile widths, and the maximum workspace count as the adaptive reference. Use original Stage's coupled width solve against the actual reserved-width/full-height desktop: `cardHeight = totalCardWidth * height / desktopWidth`. This avoids letterboxing now that the desktop is no longer uniformly scaled. The original min/max width limits, spacing and empty-column behavior remain.
- Invert pointer presentation separately on X/Y. Scale workspace animation and trackpad gap distances on their own axes; a horizontal reservation must not shorten vertical swipe travel.
- Update current design documentation and the former assertions that explicitly required uniform two-axis desktop shrinkage. Retain equal-height cards, shared horizontal scaling, proportional columns, camera/crop checks and real input checks.

## Isolated geometry and screenshots

Release compositor `~/.local/lib/hyprland-hdr/Hyprland`, e5a03b6a5ce64ec4c85954649bad4df083627574, with matching cached hyprpm headers. Private Weston/Hyprland only. Logical output 1920x1080, scale 1; reserved top 80/bottom 24 gives a native 976-high work area. Six real Wayland CSD marker clients, scrolling width .85 and native fullscreen-on-single-column behavior. Stage shows inactive workspace cards. Stage is opened and closed, with screencopies and native client/window geometry captured in each phase. Three tiles deliberately have unequal widths 960/480/480.

All values below are logical pixels. Per-tile heights are equal in each multi-tile row. Native gaps/borders account for the difference between the 976-high viewport and 934-high window; the fix does not remove reserved space or gaps.

| Build | Tiles | Desktop Y | Desktop height | Window Y | Window height |
|---|---|---:|---:|---:|---:|
| 552bd339 reference | off | 80.000 | 976.000 | 101 | 934 |
| 4a439a5 | two | 285.367 | 565.267 | 298 | 540 |
| 9c92cb5 | off | 80.000 | 976.000 | 101 | 934 |
| 9c92cb5 | single | 80.000 | 976.000 | 101 | 934 |
| 9c92cb5 | two | 187.767 | 760.467 | 204 | 728 |
| 9c92cb5 | three | 192.850 | 750.300 | 209 | 718 |
| fixed | off | 80.000 | 976.000 | 101 | 934 |
| fixed | single | 80.000 | 976.000 | 101 | 934 |
| fixed | two | 80.000 | 976.000 | 101 | 934 |
| fixed | three | 80.000 | 976.000 | 101 | 934 |

Tiles-off/single base/desktop rectangles, card dimensions and before/after native window positions/sizes **exactly match** the pre-tiles reference. The two-tile fixed desktops retain widths 748/748, the three-tile desktops 738/369/369. Three fixed cards retain widths 192/96/96 and a common height 253.919. No tile gets an independent desktop scale or card height.

| Case | Before | After |
|---|---|---|
| Two tiles | [Stage open](../build-tiles/fb5-baseline-two/desktop-stage.png) | [Stage open](../build-tiles/fb5-final-two/desktop-stage.png) |
| Three unequal tiles | [Stage open](../build-tiles/fb5-baseline-three/desktop-stage.png) | [Stage open](../build-tiles/fb5-final-three/desktop-stage.png) |
| Native reference | [Pre-tiles Stage](../build-tiles/fb5-reference-off/desktop-stage.png) | [Tiles off](../build-tiles/fb5-final-off/desktop-stage.png) |

Each directory also has `desktop-before.png`, `desktop-closed.png` and `geometry.json` (native before/after clients, full Stage state, desktop/card/column rectangles and raster samples). Upper/lower body probes remain green after the fix; they sampled wallpaper above/below the incorrectly shortened desktop in the baseline. The final regression also checks native Y/height after closing Stage. PNGs/build outputs are intentionally not committed.

Archived comparison binaries:

- 9c92cb5 pre-edit copy: SHA256 `00e563496a25bfb4057b547a6f64073f95c960d78e809b70dd2e2da74dd8c96d`.
- 4a439a5 archived optimized build: SHA256 `628758741028f2e01b04bd7b419b45ba73833b5943e38d51c9e7928d3a6b5047`.
- 552bd339 archived optimized build: SHA256 `50c72bb2fe94ce61cd6987c6c5cfca98747aa8fd3551ad615cfd1221679ca0c1`.

## Reproduction and regression gates

```sh
# A fresh private compositor per configuration; repeat for off/single/two/three.
python -B tools/tiles_isolated_smoke.py --plugin build-tiles/libhymission.so --fixture build-tiles/libhymission-tiles-test.so --stage-height-regression three --output build-tiles/fb5-check-three
```

`--stage-height-observe` records a known failing baseline without claiming PASS. A pre-tiles plugin supports the `off` case; its older IPC lacks `base`, so the helper derives that rectangle from native monitor position/scale/reserved areas and reads the actual window geometry independently. No version-specific native geometry threshold is relaxed.

Before changing the transform, the new Stage vertical-extent assertions fail on the original helper (`fb5-logic-negative.log`). Final logic checks cover intermediate progress, unequal widths, negative monitor origin, nonzero top reservation, preserved work-area gaps, pointer round trips, and comparison of multi-tile thumbnail sizing against original Stage's public layout primitive. A tiny floating-point residual in maxScroll uses the existing 1e-7 geometry tolerance instead of exact equality.

Validation results are recorded below. Physical HDR appearance and live session acceptance remain untested; no host reload/input/hyprpm update or push is performed.

## Final checks

- Full local CTest **16/16 PASS**, no skips (`fb5-ctest.log`).
- Final height regression PASS for off/single/two/three, including body-row pixels
  and native Y/height after closing; off/single geometry exactly matches 552bd339.
- `tiles_disabled_regression` PASS; its observation JSON is identical to the
  established 552bd339 reference. Release-compositor full integration smoke,
  reserved-top/CSD and native/tile workspace animation curve/speed/style/swipe
  regressions PASS (`fb5-core-regressions.log`).
- Maximum-workspace-count adaptive sizing, three-window scrolling camera/crop,
  shared horizontal animation and real native hit testing, bounded rendering and
  both hook installation orders, all overview endpoints, and FB4 shadow/close/
  three-tile dispatcher/four-finger raster isolation PASS
  (`fb5-visual-regressions.log`). The strict non-focused-tile raster check again
  reports zero changed pixels across dispatcher, held-finger and release phases.
- Refreshed flake Hyprland input; upstream main remains
  8010b4e5c66f97aa82cd5b3332b4ec613779b9a7 and the lock is unchanged.
  `nix build -L .#hymission` PASS (`fb5-nix-build.log`): 14 tests pass and two GL
  tests skip because EGL is unavailable in the Nix check. Both skipped tests pass
  in the local complete suite. Build compatibility does not imply live acceptance.
- Final isolated configuration errors are empty; `git diff --check` passes.
