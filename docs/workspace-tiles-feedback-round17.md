# FB17: Stage workspace order and landing slots

## Scope

The appended user correction supersedes the size diagnosis in the original task.
Sidebar/card sizing, adaptive sizing, aspect ratio and gaps are unchanged. This
change fixes ordering and motion only. No host IPC, reload, plugin update or push
is performed.

## Recording evidence

Input: `/home/wilf/Videos/Screenrecords/Recording-2026-10-18-035209.mp4`.
The file contains 455 decoded VFR frames, not 972 uniformly spaced frames despite
its nominal 120 fps rate. Input SHA-256:
`9b933ecd93b4087fa447b9e98ba47d59e400fb44e6894157998cec86efb64a3d`. All frames were extracted with original PTS, reduced
uniformly to 321 px wide for contact-sheet inspection. Original-resolution
samples were also inspected. Frame numbers below are 1-based.

[Per-frame annotations](workspace-tiles-fb17-recording.csv) cover every decoded
frame, identifying the visible preview ordering and the settled ordering. Slot
numbers count from the top. These are visual annotations, not host event logs;
the sidebar-only recording cannot establish the full desktop trajectory or
subpixel endpoint errors.

| Switch | Moving preview frames | Handoff witnesses | Preview slots | Settled slots | Difference |
|---|---|---|---|---|---|
| 2 → 6 | 11–47 (0.267–0.867 s) | 48–49 (0.883–0.900 s) | 1, 2 | 2, 1 | Workspace 2 approaches slot 1, then appears in slot 0; passive 1 changes slots. |
| 6 → 7 | 101–163 (1.767–2.800 s) | 164–167 (2.817–2.867 s) | 1, 2, 6 | 2, 6, 1 | Workspace 6 approaches slot 2, then appears in slot 1; passive 1/2 change slots. |
| 7 → 8 | 223–269 (3.800–4.567 s) | 270–273 (4.584–4.633 s) | 1, 2, 6 | 2, 6, 1 | Empty workspace switch still reorders the three passive cards at handoff. |
| 8 → 9 | 316–356 (5.350–6.017 s) | 357–360 (6.033–6.083 s) | 1, 2, 6 | 2, 6, 1 | Same passive-card ordering discontinuity. |

The recording keeps its normal two/three visible workspace cards and normal
card size; it does not support a sidebar-width repair. The host snapshots show
one tile after earlier cross-column dragging (`last_drop_trace` references tile
3), matching the retained-order reproduction below.

## Root cause and repair

`cardsForWorkspace()` is the committed/static authority: monitor/type filters,
tile ownership, active/empty-workspace policy, synthetic slots, numeric/name
order, followed by the saved cross-column drag order when Tiles is available.
That saved order remains valid after collapsing multiple tiles to one.

`prepareSwipe()` independently rebuilt the preview list and applied the saved
order only for **multiple** tiles. A collapsed single tile therefore used
numeric preview order but saved drag order at commit. Source history attributes
the static `available()` ordering to `67e2785` and the gesture-only `clustered()`
condition to `f080e8b`; this is source-history attribution, not a binary bisect.
The preview now calls the
same authority as the static scene, preserving cached native previews by
workspace identity. A not-yet-created workspace uses its requested identity for
active-card exclusion and its source workspace for tile ownership.

`multi_workspace_sort_recent_first` currently belongs to overview's workspace
sorting; Stage does not read it. This repair preserves that behavior and tests
both values instead of introducing a separate Stage MRU policy. Tile histories
choose workspace targets/ownership; they are not a second Stage slot sorter.

The destination miniature and scrolling aperture now use the final unshifted
slot, including draw-time retargeting. Sampled animated positions remain valid
**source** endpoints. Passive cards retain their sampled gesture-owned positions
when native activation rebuilds the final scene, then continue toward that final
order using the existing card transition clock. There is no new animation owner.

`hymission-stage-state` additionally exposes `presentation_cards`: during a
prepared swipe this describes the scene actually being drawn, while `cards`
continues to describe the committed scene. The regression can inspect passive
movement without mistaking committed metadata for the gesture-owned drawing.

## Reproduction and regression

The private regression uses native marker clients, persistent neighboring
workspaces, real pointer events to transfer a card between sidebar columns, and
real four-finger trackpad events through the existing fixture. Single mode first
adds and removes a second tile, retaining the public drag order. No private
ordering-state injection is used.

Example:

```sh
python -B tools/tiles_isolated_smoke.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --stage-flight-clip-regression single \
  --stage-flight-clip-layout scrolling \
  --stage-order-regression --stage-order-recent 1 \
  --output build-tiles/fb17-order-single-1
```

Every captured outgoing flight goal is compared with its actual settled static
window slot (maximum coordinate/extent error ≤1 px); incoming source slots are
also checked. Passive card reordering must contain intermediate frames and end
within 1 px of its static slot. Both directions, keyboard and held four-finger
swipe, tiles off/single/two/three, recent-sort 0/1 are covered. Existing flight
pixel coverage and FB15 landing/aperture checks remain separate oracles.

The unmodified `23ad7d7` library reproduced a **178.2869 px** flight-goal/static-slot
error on the collapsed-single reverse swipe. The first repaired run measured
**0 px** for both directions/routes and retained 43–44 intermediate passive-card
samples during the swipes. Baseline and repaired captures, raw frame state,
PNG pixels, input library hashes and recording contact sheets remain under the
ignored `build-tiles/fb17-*` paths.

## Validation

The branch was fetched and rebased onto latest `origin/hyprland-master`
`23ad7d7` before the final build/replays; a final fetch still resolves to that
revision. The other agent's worktree is untouched.

- Local full CTest: **16/16**, no skips, including EGL rendering tests.
- Full legacy plus order matrix: **85/85 scenarios**. No scenario failures.
- Final rebuilt release artifact: disabled regression, full integration smoke,
  all eight order configurations, and both two/three-tile dwindle handoffs pass.
  Disabled observations exactly equal the retained pre-tiles reference.
- Final order replay: **56 switches / 3,438 rendered frames**, maximum source and
  final-slot error **0 px**, passive backwards movement **0 px**. Strict retained
  frame/pixel checks also pass; neighboring geometry does not change. The
  original matrix independently retained 3,444 frames for these 56 switches.
- Baseline/repaired single-tile samples have identical card width/height,
  sidebar, desktop, reservation and base geometry.
- Refreshed development flake remains at Hyprland
  `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`; no lock-file change results.
  `nix build -L .#hymission` passes. Two EGL tests skip inside Nix and pass locally.
- Eight final profile configurations cover off/single/two/three and both hook
  orders: **1,229 frames**, complete GPU timings, warm layout median ≤1/worst ≤2,
  no symbol lookups, and no warm GL allocations/links/uniform lookups.

The first profile pass has one preserved failure: stage-first/two, warm close
cycle 1, frame 8 recorded three layout calls in a 36.948 ms interval (the limit
is two). Both a predetermined unmodified-baseline control and a fixed targeted
replay subsequently pass the unchanged cache gates. The final eight-profile
summary uses that replay for this configuration; the original raw profile and
failed gate remain in `fb17-profile-observed.json` and `fb17-gate-cache.log`.
The outlier's cause is not established, and this is not a claim of universal
smoothness under host scheduling delays. No threshold was relaxed and no
additional layout/performance source change was made to obtain a pass.

The release runtime is `~/.local/lib/hyprland-hdr/Hyprland`, commit
`e5a03b6a5ce64ec4c85954649bad4df083627574`, with matching release headers. Raw
identities, matrix/gate results, failed and passing profile controls, recording
samples and PNG/state captures are indexed by
`build-tiles/fb17-evidence-identity.json`. These are isolated-compositor/build
results; physical-session acceptance remains with the user. No push, host
reload, hyprpm update or host plugin load/unload was performed.
