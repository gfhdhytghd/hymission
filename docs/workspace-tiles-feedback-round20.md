# FB20: live opacity in main overview previews

Worktree: `hymission-tiles-fb7`, branch `fix/onlycurrent-transparency`.
Baseline: `4c1f22545d032e57211434d446f5f4b55bf802a1`.
The user confirmed the symptom concerns main previews and supplied no application
name. All runtime work below uses private nested compositors.

## Reproduced defect and repair

Main previews cached the complete window opacity when overview opened. Native
focus changes subsequently updated `WINDOW_ALPHA_ACTIVE`, including windowrule
active/inactive opacity, while preview drawing continued using the cached value.
Opening with the test window active and hovering another window left its preview
at 0.85 instead of 0.4; opening inactive and hovering it left 0.4 instead of 0.85.
Native presentation alpha and frame pixels establish both directions.

`ManagedWindow` now caches opacity without the active channel. Preview drawing,
shadow opacity and render visibility planning combine that cache with the live
native active channel. Fade is not sampled a second time; fullscreen occlusion
remains excluded, and expanded group members still exclude layout occlusion.
There is no division by an initial active alpha, so an initial zero is safe.
No Stage production code or strip blur policy changed.

The focus-opacity defect reproduces in both `forceall` and
`onlycurrentworkspace`. Constant windowrule opacity and client alpha plus blur
did **not** reproduce a separate workspace-only loss in the tested scenes.
Consequently this report does not establish that every aspect of the original
workspace-only report has the same cause. Host-session acceptance remains
unverified.

## Before/after pixel evidence

Artifacts are under the ignored `build-tiles/fb20/` directory. `index.html` shows
paired raw screenshots. Each run retains input artifact hashes, private config,
native frame metadata and pixels, observations, and config errors.

The final session scene uses scrolling, occupied workspaces 1/2/3, two tiles,
Stage disabled, natural workspace layout, focus follows mouse, selected/hover
scale 1.2, maximum preview scale 0.5, and a real 48 px exclusive layer bar.
Kitty runs with an isolated empty config, background `#f0a040`, and
`background_opacity=0.5`. Three other real Wayland clients provide constant
windowrule opacity 0.8, active/inactive opacity 0.85/0.4, and an opaque control.
A 16 px checkerboard behind them makes alpha and blur separately measurable.
Blur off/on uses size 8, two passes and `ignore_opacity=true` in this session
scene. Earlier controls also cover `ignore_opacity=false`, floating clients,
wide color mode, and the thumbnail/niri path as observations.

Representative **main-preview** 32×32 ROI means, blur enabled, workspace scope:

| Opening focus → hovered window | Expected active/inactive opacity | Baseline RGB | Fixed RGBA8 RGB | Fixed FP16 RGB |
| --- | --- | --- | --- | --- |
| active-opacity client → Kitty | 0.4 | 223, 155, 73 | 172.39, 140.39, 102.39 | 172.34, 140.34, 102.34 |
| opaque control → active-opacity client | 0.85 | 172.39, 140.39, 102.39 | 223, 155, 73 | 222.97, 154.97, 72.97 |

These are measurements of the active/inactive client's preview, not Kitty's
background. The baseline fails the same automated oracle used for the candidate.
Raw comparisons: `session/before-final/`, `session/after-final/`, and
`session/after-fp16/`; logs are `session-*-final.log` and
`session-after-fp16.log`.

Kitty itself measures approximately RGB **184,144,96** without blur and
**183.9,143.9,95.9** with blur on desktop and in main previews. Checkerboard
standard deviation falls from **38** to about **1.5** with blur. Constant rule
opacity measures **217.5,153.5,76.5**; the opaque control is **240,160,64**.
The mean checks detect missing or doubled alpha; the variance checks separately
detect missing blur. The terminal has real rendered text above the sampled ROI.

Final `pixel-checks.json` reports:

| Session scene | Desktop ROIs | Stable main ROIs | Entry/exit captures | Measured entry/exit frames (intermediate) | Hover captures / frames |
| --- | ---: | ---: | ---: | ---: | ---: |
| RGBA8 | 10 | 220 | 16 | 249 (104) | 32 / 702 |
| FP16 with MRT mirror | 10 | 220 | 16 | 245 (104) | 32 / 695 |

Hover records cover movement onto the rule client, Kitty, active-opacity client
and opaque control. They verify all three transparent clients against native
alpha during relayout. Geometry area ranges are retained: expansion is bounded
by available layout space, so enabling scale 1.2 does not imply every preview
can grow by exactly 20%. Opening/closing and hover checks require a clear ROI
away from other windows and strip rectangles; blurred terminal ROIs must also
have standard deviation below 4. Opening and closing each require measured
intermediate-progress frames. Hover frame checks total 2,106 transparent-window
samples in RGBA8 and 2,085 in FP16.

Additional strict controls under `proof/after-{off,tiles,stage,tiles-stage-fp16}`
pass with client-alpha fixtures, blur off/on and both scopes. Each has 96 stable
ROI samples and 16 entry/exit captures. These cover tiles and Stage toggles
independently. The actual Kitty session adds the requested realistic scrolling
and hover setup with Stage off.

FP16 metadata verifies work format `DRM_FORMAT_ABGR16161616F` and a nonzero MRT
mirror format. The private frame reader resolves a normalized RGBA8 copy for
recording and restores GL bindings and pixel-pack state. This is readback code
in the test fixture, not a change to production color rendering. The saved PNGs
are normalized evidence; they are not measurements of physical PQ/HDR output.
The local color regression separately exercises the extended float path.

## Regression results and preserved failures

- Release build and complete local CTest: **16/16 passed**, including both EGL
  rendering tests, with no skips (`build-final.log`, `ctest-final.log`).
- `tiles_disabled_regression`, strip panel, workspace switch with Stage off/on,
  multi-window Stage, scrolling overview reveal, and three-tile Stage flight /
  handoff / numeric order gates pass (`after-gate-*.log`). The flight gate covers
  key/swipe in both directions and reports zero missing foreign pixels.
- The additional **23-case** historical matrix has **21 passes and 2 failures**
  (`legacy-results.json`). Passing cases cover FB2 gestures/overview/Stage,
  FB3 hover/drag/performance/adaptive columns, FB4 shadow/close/switch, entry/exit
  animation, top clipping, gesture lifecycle, two/three-tile presentation,
  relative switching, scrolling Stage viewport/switch, Stage drop/height/edge.
- Focus/resize with expansion enabled passes in both scopes, including hover,
  keyboard and cross-tile focus (`after-gate-focus.log`). The legacy no-expansion
  case fails `tile bounds did not reflow layout` on both the original baseline
  and candidate (`gate-focus.log`, `legacy-focus-noexpand.log`).
- Full release integration smoke was run and **fails on both baseline and
  candidate** at `same-column-reorder-preserves-identity`; it expects workspace
  IDs to remain fixed although the current default permits drag renumbering.
  A second baseline replay reproduces it. Raw results are `gate-release.log`,
  `release-before-replay.log`, and `release-after-replay.log`. The existing oracle
  and production Stage behavior were not altered to obtain a pass.
- The native-device Stage drag gate fails
  `ordinary card onto existing card never merges contents` on both baseline and
  candidate (`baseline-device-drop.log`, `legacy-stage-device-drop.log`). This
  is an assertion about its workspace-membership map; it does not by itself
  establish that client contents actually merged. It is outside the opacity
  repair. Raw observations remain available.

These failures are disclosed as failures, not green acceptance. This run uses
representative cases from the historical suites, not a claim to rerun every
off/single/two/three permutation from all earlier rounds.

## Development build and reproduction

The updated flake pins upstream Hyprland `main` revision
`a1c2428f79b3b05c3fd36c27378e3538c8c6a014` and its hyprutils update. The final
production fix passes `nix build -L .#hymission` against that revision. Nix runs
14 tests and skips the two EGL tests (`hymission-stage-edge-render-test` and
`hymission-snapshot-color-test`) because surfaceless EGL is unavailable there;
both pass in the local 16-test run. Logs: `nix-update.log`, `nix-final.log`.

Isolated runtime is the matching private HDR compositor and cached headers at
`e5a03b6a5ce64ec4c85954649bad4df083627574`. This runtime is distinct from the
fresh upstream development compilation.

```sh
python3 tools/tiles_isolated_smoke.py \
  --plugin build-tiles/fb20/build/libhymission.so \
  --fixture build-tiles/fb20/build/libhymission-tiles-test.so \
  --output build-tiles/fb20/session/reproduce \
  --opacity-regression --opacity-session --opacity-tiles
# Add --opacity-fp16 for the FP16/MRT session control.
```

The session control additionally requires Kitty. All opacity controls require
Pillow, NumPy, grim, Wayland development tools, GTK4/layer-shell, and the nested
compositor harness dependencies. `--opacity-observe` records exploratory cases
without asserting success.

Local runtime artifact SHA256:

- Baseline: `3fdfd9935a967e551f2b3d6f81e0fa3964563860763ee5d95c8cdf3a211eb2f5`.
- Fixed: `a82541f1853c804843c4467c630c7f5a3510f7281077aa704ef479da97770c1b`.

## Remaining boundaries

Strip window snapshots still suppress background blur. Its export path uses
monitor blur framebuffers whose viewport reset conflicts with rotated export
dimensions. Removing the guard is not a safe standalone flag change, and strip
repair is not needed for the confirmed main-preview focus-opacity defect.
Strip screenshots are observations, not a claim of desktop blur parity.

No other worktree, host configuration, host plugin loading, `hyprpm update`,
host reload, push, or physical input was used. Installation/reload remains a
user-controlled action after this local commit.

## Validation after rebasing onto FB19

The FB20 commit has now been rebased directly onto local `hyprland-master`
`9c56a48550e3c6a0e89aae111634d59af97a5f1c`. The earlier baseline, screenshots
and failures above remain historical evidence from before this rebase.

The conflict in `tiles_native_test_plugin.cpp` retains both suites' contracts:
FB19's `drm_format` and float-readback fallback, and FB20's `format`,
`mirror_format`, normalized FP16 readback and native active/presentation alpha
fields. The FP16 recorder first resolves an RGBA8 copy, then uses float readback
if that fails. The production merge retains FB19's shadow continuity and live
desktop strip geometry together with FB20's live active/inactive opacity.

Rebuilt combined-source validation, with raw artifacts and commands under
`build-tiles/fb20-rebase/`:

- Complete local CTest: **16/16 passed**, no skips (`ctest.log`).
- FB19: **six configurations, 200/200 checks passed**. These are tiles
  off/single/two/three, plus off/three with FP16; each covers Stage off/on and
  all/workspace scope, held and timed shadow transitions, live strip geometry,
  focus-induced native resizing, and frozen main-overview slots.
- FB20: **six configurations passed**. The four client-alpha controls cover
  tiles/Stage off, tiles on, Stage on, and tiles+Stage with FP16. The two real
  Kitty scrolling/hover session controls cover RGBA8 and FP16 with occupied
  workspaces and tiles on, Stage off. Together they check **824 stable main
  ROIs, 96 entry/exit captures and 128 hover captures**, including blur off/on
  and both scopes. Each case retains its `pixel-checks.json`.
- Supplementary disabled, full release integration, no-expansion focus reflow,
  and scrolling three-tile native-device Stage drop controls all pass.
  The three failures recorded before the rebase no longer fail with the
  inherited FB19 fixture/oracle corrections described in
  `overview-feedback-round19.md`. This rebase adds no further oracle changes.
- The combined-source Nix build passes against the unchanged upstream pin
  `a1c2428f79b3b05c3fd36c27378e3538c8c6a014`. Its 14 non-EGL tests pass; the two
  EGL tests skip in Nix and pass locally (`nix.log`).

`results.json` records all **16/16** isolated configurations passing, while
each case's `input-artifacts.json` records the loaded plugin, fixture and
compositor hashes. The combined local plugin SHA256 is
`daa868e03f0df9783ba937c71784598a7eda2f9b8732bc70a4c84b8c06a92220`.
No push or host reload was performed, and the other worktree was not edited.
