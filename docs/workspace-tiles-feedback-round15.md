# Stage workspace flight continuity (FB15)

Task baseline: `7cdc3ed711c803a1663c675bc0668411042e5eec`, branch `fix/stage-switch-jump`. Before committing, the branch was rebased onto newly published `origin/hyprland-master` `5650d1a66cd944c54f37b99e767e6da5dbc6ee7a` (FB14), without conflicts. The final build and gates below were replayed after that rebase. Work is confined to this worktree. The FB14 and development worktrees are untouched. Source fixes concern Stage flight endpoints and apertures; workspace ownership, shared Stage sizing and native scrolling width policy are unchanged.

## Recording audit

Source: `/home/wilf/Videos/Screenrecords/Recording-2026-10-18-015338.mp4`. `ffprobe` reports 1268x1868, nominal 60 fps, 298 original frames over 5.583338 seconds. Original timestamps are uneven (average about 53.37 fps). The requested extraction uses:

```sh
ffmpeg -i /home/wilf/Videos/Screenrecords/Recording-2026-10-18-015338.mp4 \
  -vf fps=60 build-tiles/fb15/video-frames/frame-%04d.png
```

This produces 335 numbered images. **All frame numbers below are one-based extraction indices**, with nominal time `(frame-1)/60`; duplicated images are not extra compositor presentations. Original timestamps are retained in `video-timestamps.json`. `video-overview.png` covers the whole recording; all 335 full-resolution images remain available.

The review distinguishes an adjacent-frame motion candidate from a discontinuity relative to an animation's phase. This cropped recording contains no Stage phase, flight lifetime, native window dimensions or loaded plugin identity. It cannot by itself prove which frame hands off from a flight to a static card. Pixel measurements have about 1-2 pixels of edge uncertainty from compression, rounding and thresholding; scale-template estimates are used only to find candidates. Occluded or off-crop edges are not treated as measured window boundaries.

| Extracted frames | Element and observation | Measured displacement/size | Assessment |
| --- | --- | --- | --- |
| 21 -> 22 (0.333 -> 0.350 s) | Discord first moving presentation | Visible dark-body bounds `(25,980,605,834)` -> `(67,966,666,888)`: delta `(+42,-14,+61,+54)` in x/y/w/h | Takeoff candidate. Subsequent frames grow and move continuously; from frame 23 the bottom/right become cropped, so full size is unavailable. Without phase, this delta is not proof of an erroneous extra resize. |
| 65/66 -> 69/71 -> 73 -> 74 -> 75 -> 76 (1.067-1.250 s) | Feishu arrives at the first left slot | Observed left/right edges: `68/733`, `52/696`, `30/642`, `26/633`, `25/630`, `25/629`; widths `665,644,612,607,605,604` | Continuous convergence. The last two visible steps change width by 2 and 1 px. The proposed large **post-landing** shrink is not established by these frames. Frames 76-118 retain the 604 px slot width. |
| 73 -> 74 -> 75 (1.200-1.233 s) | Terminal card finishes its downward reorder | Titlebar top `975 -> 978 -> 979`, fixed horizontal slot | Continuous final 3/1 px steps. Overlapping flight frames are excluded from template measurements. |
| 118 -> 119 (1.950 -> 1.967 s) | Feishu leaves its static card | Left edge `25 -> approximately 190`; static width 604, first moving width approximately 845 (right edge has low contrast); bright top scanline `118 -> 130` | Strong takeoff candidate, including a temporary loss/blur of the top decoration. Width/height are not claimed as exact native geometry. Frames 120 onward continue the same departure. The recording alone does not identify its phase or distinguish a fast first step from a bad source aperture. |
| 159 -> 160 -> 161 -> 162 (2.633-2.683 s) | Discord arrives at the lower left slot | Dark-body bounds `(30,975,613,846)` -> `(26,978,608,838)` -> `(25,980,605,834)` -> `(25,979,604,835)` | Final changes converge to the slot; the last difference is within the 1-2 px extraction/raster uncertainty. No large post-landing resize is visible. |
| 159 -> 160 -> 161 (2.633-2.667 s) | Terminal returns to its upper slot | Titlebar top `121 -> 118 -> 117`, fixed horizontal slot | Continuous final 3/1 px steps. |
| 163-335 (2.700-5.567 s) | Settled columns | Same visible slot positions; application/status content can still change | No additional workspace transition or large card geometry discontinuity is visible. |

The complete numerical candidate tracks are `video-track-v2.json`, `video-discord-track.json` and `video-terminal-track.json`. They include confidence/occlusion information. Repeated avatars and repeated terminal text can yield false template matches; the table uses reviewed borders and excludes those matches. The initial unrestrained avatar tracker is retained as a rejected pilot, not used as jump evidence. In particular, this audit does **not** turn the initial suggested landing cause into a confirmed observation.

## Reproduced causes and fixes

Two independent continuity errors reproduce in private compositors at the task baseline.

1. **Scrolling source aperture uses a window as a whole card.** In multitile `startFlights`, `miniature()` returns the cached *window* thumbnail even when the caller asks to map a workspace rectangle. `miniatureView()` then fits a desktop viewport inside that window rectangle a second time. Stationary cards instead use the complete card slot. A 0.85-width column therefore starts through a different aperture and its interpolated flight geometry follows a different path. The baseline two-/three-tile matrices show up to 35.201/45.042 px aperture errors and 32.059/44.927 px path errors against the static-slot transform. A held four-finger release also inherits the wrong sampled origin (up to 10.776 px against the correct finger-owned frame).

   The multitile aperture now comes from the complete slot, with the same centering, card shift, scroll, side and geometry as stationary drawing. Native off/single miniature mapping is preserved. Scrolling's own moving aperture remains; no hidden tape content is exposed through stationary tile scissors.

2. **Output gap fitting moves tile-native endpoints.** Tile-native layout can put the window four pixels beyond the output's `gaps_out` flight boundary. `transitionBoxWithin` fits it inward even though the raw endpoint matches native geometry. This reproduces a 4 px frame error and up to 3.744 px release-origin error in dwindle. Multitile workspace flights now retain native endpoints and use the physical output clip. The easing functions themselves are unchanged. Off/single bounds, Stage drag previews, float deferral and FB12's panes-before-flights ordering are preserved.

The source fixes do not claim to reconstruct the host's unrecorded phase/configuration or prove that either cause explains every perceived motion in the cropped video. They remove independently reproduced endpoint/aperture discontinuities in the same two-/three-tile switching structure. Fresh host Feishu/Discord acceptance remains a user-run check after a safe update.

## Per-frame acceptance

`--stage-flight-handoff` extends the FB12 private-client recorder with a persistent neighboring card and top/bottom output reservations. It adds `tools/tiles_stage_handoff_regression.py` to compare every flight frame against the **static** workspace/card transform. Real private keyboard and four-finger trackpad input exercise both directions in every tile. The test explicitly handles the new animation segment created when a held swipe commits: its source must equal the last finger-owned frame, not the original static card.

Gates are: source/slot and aperture error **<0.1 logical px**; every sampled path frame **<1 logical px** from the original easing with correct endpoints; **<=2 physical px** extra raster displacement at landing for both incoming and outgoing bodies. The last rendered frame need not have phase exactly one: the analytic motion still remaining is subtracted before checking its difference from the static image. This prevents normal final motion from being mislabeled as a jump. The ordinary FB12 body/foreign-region/held-swipe/settled-clip checks remain enabled.

| Layout | Tiles | Forward/reverse key/swipe transitions | Retained final frames | Maximum path error | Maximum extra landing raster error |
| --- | --- | --- | --- | --- | --- |
| scrolling | 2 | 8 | 489 | 0.030 px | 1.003 px |
| scrolling | 3 | 12 | 736 | 0.037 px | 1.001 px |
| dwindle | 2 | 8 | 493 | 0.027 px | 1.002 px |
| dwindle | 3 | 12 | 734 | 0.032 px | 1.003 px |

All 40 transitions pass, including native client bodies and foreign-region coverage with zero missing sampled pixels. Source/slot error is below 0.000001 px; scrolling aperture error is below 0.039 px (phase is sampled separately from individual JSON fields). Settled clips and neighbor geometry remain correct. Baseline/final native static positions and sizes are exactly equal across all 40 scenes, including after integrating FB14; the comparison does not conflate a new native layout with the flight repair. The thresholds are not relaxed to accommodate baseline failures.

Reproduction:

```sh
python -B tools/tiles_isolated_smoke.py \
  --plugin build-tiles/fb15/build/libhymission.so \
  --fixture build-tiles/fb15/build/libhymission-tiles-test.so \
  --stage-flight-clip-regression three --stage-flight-clip-layout scrolling \
  --stage-flight-handoff --output build-tiles/fb15/final-three-scrolling
python -B tools/tiles_stage_handoff_regression.py build-tiles/fb15/final-three-scrolling
```

`--stage-flight-clip-observe` retains baseline failures. Primary baseline directories are `handoff-before-two-v2`, `handoff-before-three`, `before-two-dwindle`, `before-three-dwindle`; primary fixed directories are `final-{two,three}-{scrolling,dwindle}`. Every rendered PNG, native/window/Stage JSON, held gesture snapshot and per-frame raster sample is retained under `build-tiles/fb15/`. `frame-pairs.json` indexes before/after witnesses by phase. `evidence-identity.json` hashes the artifacts and records rejected pilots.

## Regression gates and boundaries

- Full CTest: 16/16 pass, no skips (`ctest-final.log`).
- `tiles_disabled_regression`: passes; observations exactly equal the independently replayed archived pretiles reference (`build-tiles/fb12/disabled-reference/default-off-observations.json`).
- Full release smoke passes (`final-release.log`).
- FB10 off/single/two/three-scrolling pass 23/25/82/123 checks. Archived pretiles reference `552bd339fe4809e0a031d277ca97f4dac6d0a5da` passes its 19 available checks; its legacy overview export lacks the current workspace strip, which is exercised in current runs. Phase-aligned pretiles/off/single real pixel endpoints agree within the unchanged 2 px endpoint gate; all key/relative/card/swipe trajectories pass unchanged 0.04 phase/4 px interpolation gates. Measured maxima: 0.002829 phase, 2.559 px (`fb10-final-frame-equivalence.json`). This is per-frame geometric agreement by phase, not byte-identical frames at nondeterministic wall-clock times.
- The original FB12 matrix is separately replayed without the new neighboring-card/reservation scene: two/three tiles, both layouts, both directions and key/swipe (`final-fb12-{two,three}[-scrolling]`). All body and clip gates pass.
- Integrated FB14 three-tile scrolling viewport/camera-anchor regression also passes (`final-fb14-three`).
- FB8 three-tile physical-fullscreen/floating/native/overview/Stage-drag regression passes 88 checks (`final-fb8-three`).

Baseline plugin SHA256: `39a705464557a4477d3d336442c89ebd82f0595a8a8ae17a98870b23bf35d2f8`. Fixed plugin SHA256: `d6bb662c7ed9f340fcf47b17f74f65607b3960db3a8d184548f73fa1a9378dc9`. All new runtime validation uses the matching private release compositor `e5a03b6a5ce64ec4c85954649bad4df083627574`. This is not fresh upstream-main compatibility evidence; no new Nix/main compatibility result is claimed. No push, hyprpm update, manual host load/unload or host reload is performed.
