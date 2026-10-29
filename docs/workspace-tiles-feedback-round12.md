# Stage flights across workspace tiles (FB12)

Baseline: `f080e8b9679c632c0255457c0d9c5ba1eda26f8b`, branch `fix/stage-flight-clip`. Only this worktree is changed. The other FB11 and development worktrees are untouched.

## Cause and rendering contract

FB10 split each ordinary flight into two draw calls, intersected with the owning desktop and sidebar. The gap between these regions belongs to other Stage columns. A moving client crossing it therefore disappeared or became disconnected fragments. In addition, Stage drew each column's stationary pane and flights together; a later column's pane could overwrite an earlier column's flight even without the explicit scissors.

All stationary panes now finish before any flights are drawn. Flights use the existing output flight bounds and retain native Stage's departing-below-incoming order. The column's prepared swipe scene supplies held and settling four-finger flights. Drag hover and transfer ghosts remain after flights, and the existing deferred native floats/drag handling is unchanged. Settled desktops and cards continue using their existing per-column clips.

No endpoints, duration, easing, `flightFrame`, scrolling interpolation, window geometry or workspace-animation ownership code changes. Scrolling's intrinsic moving tape aperture remains: removing stationary tile scissors must not expose hidden scrolling content. The diagnostic flight JSON now exports that aperture as `viewport`, or null for ordinary flights.

## Reproduction and pixel oracle

`tools/tiles_stage_flight_clip_regression.py`, invoked through `tools/tiles_isolated_smoke.py`, records every rendered frame from real private Wayland clients. Each workspace has a distinct solid client body and a red CSD header. Real keyboard input and four-finger trackpad events exercise both workspace directions in every tile. Every transition contains both an incoming rightward flight and a departing leftward flight; swipe captures include intermediate held states.

The independent raster check samples the solid body inside the reported flight rectangle, excluding the scaled red header, native front-flight occlusion and, for scrolling, the native moving aperture. It never intersects with stationary tile or sidebar regions. It separately requires sampled incoming and outgoing bodies in foreign regions, unchanged neighbor geometry, held swipe coverage, and no own-client pixels leaking into foreign desktops/columns after flights settle. PNGs, raw frame/state indices, per-window pixel counts and coverage audits are retained. The earlier FB10 neighbor raster check now excludes actual moving flight footprints; it retains the strict pixel check elsewhere. The new test verifies the pixels inside those footprints.

Example invocation (matching release headers and compositor required):

```sh
python -B tools/tiles_isolated_smoke.py \
  --plugin build-tiles/fb12/build/libhymission.so \
  --fixture build-tiles/fb12/build/libhymission-tiles-test.so \
  --stage-flight-clip-regression three \
  --stage-flight-clip-layout scrolling \
  --output build-tiles/fb12/after-three-scrolling-v2
```

Use `--stage-flight-clip-observe` with the immutable baseline `build-tiles/fb12/before.so` to retain failing frames. `--stage-flight-clip-route key` or `swipe` restricts a diagnostic run. `python -B tools/tiles_stage_flight_clip_regression.py --verify-existing DIR` rechecks coverage and endpoint clips from retained recordings. Each new smoke run writes SHA256 identities for its plugin, fixture and compositor to `input-artifacts.json`.

## Evidence and limits

The full matrix contains 40 transitions per binary: two and three tiles, both workspace directions in every tile, keyboard and four-finger swipe, dwindle and scrolling. Each transition checks both incoming and outgoing windows. Baseline recordings reproduce missing bodies and foreign-column scissors; all final recordings have zero missing sampled body pixels, including foreign-region witnesses. All endpoint clip and held-swipe audits pass.

| Layout | Tiles | Transitions per binary | Baseline frames | Final frames | Final missing body pixels |
| --- | --- | --- | --- | --- | --- |
| dwindle | 2 | 8 | 491 | 493 | 0 |
| dwindle | 3 | 12 | 738 | 735 | 0 |
| scrolling | 2 | 8 | 491 | 491 | 0 |
| scrolling | 3 | 12 | 737 | 738 | 0 |

Primary artifacts are `build-tiles/fb12/before-{two,three}`, `before-{two,three}-scrolling`, `after-{two,three}-final` and `after-{two,three}-scrolling-v2`. Every frame is retained as a PNG with native and Stage state in the corresponding JSON index. `frame-pairs.json` selects phase-aligned before/after witnesses; `evidence-identity.json` records binary/source hashes, frame counts, validations and excluded pilot runs. Scrolling's baseline predates the viewport diagnostic: its coarse missing-body counts also include native aperture cropping. The visible stationary-column cuts and dwindle control independently reproduce the bug; the final scrolling oracle explicitly preserves that aperture. These coarse baseline counts are not claimed as an exact aperture-normalized percentage comparison.

- Full final CTest: 16/16 pass, no skips (`ctest-final.log`).
- Final `tiles_disabled_regression` passes; `disabled-final/default-off-observations.json` exactly equals the archived pretiles reference replay in `disabled-reference`.
- Full final release smoke passes (`release-smoke-final.log`).
- FB8 two- and three-tile presentation/drag regressions each pass 88 checks (`fb8-{two,three}`). They cover physical fullscreen, floating overlap and cross-tile native/overview/Stage drag.
- FB10 single, two-tile dwindle and three-tile scrolling pass 25, 82 and 123 checks respectively (`fb10-after-{single,two,three-scrolling}`). Key, relative, card, swipe/cancel and strip routes retain native ownership and neighbor geometry/pixels outside flight footprints.
- FB10 baseline/off/single rendered endpoints and per-frame trajectories pass (`fb10-frame-equivalence.json`). Maximum cubic phase residual is 0.003283; maximum raster interpolation residual is 2.406 pixels, within the unchanged 0.04/4-pixel bounds. Held swipe phases are included.
- Native tiles-off scrolling control passes both directions and both routes with zero missing sampled body pixels (`control-off-scrolling`); the dwindle keyboard control also passes (`control-off-key-v2`).

Immutable baseline plugin SHA256: `d295e35cc5cbb654763d9282a18735512d7b1d3d5b78d31a2a8cbd52b09343d4`. Final plugin SHA256: `af7c3a4b722a7aec652c0a90733c4f82bbce587f7d6481a948e9a14475c3d661`. The earlier fixed binary used for FB8 and the first off-mode trajectory control differs only by the later viewport JSON diagnostic. All primary final matrix, CTest, disabled and release runs use the final binary.

All evidence is from private nested compositors using release Hyprland `e5a03b6a5ce64ec4c85954649bad4df083627574`; it is not host-session or physical-device acceptance. No push, hyprpm update, or host plugin reload is performed.

Refreshing the development flake input was attempted, but Nix could not create its default `/nix/store` (permission denied). The lock file is unchanged. This task makes no fresh upstream-main compatibility claim.
