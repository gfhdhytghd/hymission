# Overview focus/resize layout regression

Baseline: 9c92cb5. Reference: 552bd339fe4809e0a031d277ca97f4dac6d0a5da.
All runtime experiments use private Weston/Hyprland sessions with compositor
revision e5a03b6a5ce64ec4c85954649bad4df083627574 and matching cached headers.
The host session was not reloaded, configured, or sent input. No push is made.

## Cause and repair

Tile notifications include focus changes, as well as changes to allocations and
workspace bindings. `OverviewController::handleTilesChange()` always passed
`forceRelayout=true`, bypassing the pre-existing same-scene layout freeze. If a
layout resized native windows during focus handling, the forced rebuild packed
those new dimensions and moved the overview previews.

Use an ordinary rebuild for tile notifications. Snapshot each participating
tile's output, ID, workspace binding, and overview content bounds in the state.
Compare these snapshots when deciding whether the scene layout can be retained;
exclude the focused flag. Window membership, row groups, output membership,
tile content bounds, and workspace bindings still invalidate the frozen layout.
Transition target states snapshot their overridden destination binding.

The existing selection expansion remains enabled when configured. The repair
preserves its base slots instead of repacking from focus-dependent native sizes.
It does not change Stage geometry or tile-controller focus/routing behavior.

## Reproduction and evidence

`tools/overview_focus_reflow_regression.py` uses real Wayland clients and actual
overview hover, Tab, and tile focus inputs. Its optional private fixture listener
invokes native scrolling `colresize` messages on window focus. This is a
controlled focus-dependent layout response, not a claim that unmodified
scrolling always resizes windows on focus. Every resize trial checks the real
client sizes and real focus change. No overview rebuild method or internal
layout rectangle is called or overwritten by the fixture.

Two complementary reproductions cover the notification paths:

- Workspace overview: keyboard tile focus with unchanged tile allocations and
  window membership. Before repair, three previews move in 23 recorded frames;
  after repair, no preview moves, although three native client sizes change.
- Cross-output overview focus: a managed single-tile output on each of two
  nested compositor outputs. Hovering a preview on the other output exercises the real
  monitor focus event. This also provides a comparable scene for historical
  commits before tile-scoped overview existed.

Selected tile-focus frames and their complete sidecars are retained locally:

| Evidence | Before | After |
| --- | --- | --- |
| Focus resize, workspace overview | [frame 10](../build-tiles/before-confirm/onlycurrentworkspace-tile-keyboard-resize1-10.png) | [frame 10](../build-tiles/after-confirm/onlycurrentworkspace-tile-keyboard-resize1-10.png) |
| Every frame and native sizes | [JSON](../build-tiles/before-confirm/onlycurrentworkspace-tile-keyboard-resize1.json) | [JSON](../build-tiles/after-confirm/onlycurrentworkspace-tile-keyboard-resize1.json) |

The fixture reads native RGBA8 pixels from the current framebuffer and saves
native damage rectangles, output identity, overview state, tile state, and
native sizes. PNGs reconstruct presented output from the prior screencopy plus
the damaged regions. Cross-output tests capture the destination output only,
because the Weston parent can occlude the first child output. Readback and PNG encoding are
validation overhead; frame counts are not performance measurements. Source
reference pixels are not compared for equality because the client animates.

Without selection expansion, acceptance requires invariant preview geometry in
every recorded frame. With the normal 1.18 expansion, paired trials reset native
column widths and focus to the same opening scene; resize and no-resize trials
must end at the same preview geometry. Each full run also checks real client
open/close, and multi-tile runs check that changing tile bounds still relayouts.

Explicit tile-focus dispatch in force-all is intentionally not a focus-only
control: existing force-all workspace-switch policy closes overview. Its
focus-only controls use overview hover/Tab; cross-output hover covers the
monitor notification without changing workspace membership.

## Bisect

A source-predicate `git bisect run` over 552bd339..9c92cb5 identifies
**2ee664b98f6da007297106bd0c1c49e62e14fdff** as the first commit adding the
forced rebuild in `handleTilesChange()`. Its parent fd135642 lacks that path.
The full predicate, sampled revisions, and result are retained in
`build-tiles/bisect-predicate.py`, `bisect-source.log`, and
`bisect-source-result.log`.

This is a source bisect of the causal notification path, not a complete
historical runtime bisect. Unmodified runtime candidates cfbed82 and 3b3988f
reproduce the failure. Earlier candidate 845d627 cannot open overview on the
release compositor: Config::Actions fullscreen/workspace hooks fail to
initialize. Replaying only the later 3b3988f symbol matching adapter in the
isolated historical checkout also fails that initialization; no adapted run is
accepted as a pass. Runtime attempts and the compatibility-only diff remain in
`bisect-runtime.log`, `bisect-driver-v2.log`, `bisect-driver-adapted.log`, and
`bisect-845d627a/`. Consequently there is no claim of an adjacent unmodified
parent/child runtime comparison.
The isolated clone and all build directories live under this worktree's
`build-tiles/`; other development worktrees are not used or modified.

## Verification

The repaired primary-output matrix covers both workspace and force-all scopes:

| Tiles | Trials | Recorded frames | Moved preview frames |
| --- | ---: | ---: | ---: |
| Disabled | 8 | 190 | 0 |
| Enabled, single | 8 | 191 | 0 |
| Two | 10 | 239 | 0 |
| Three | 10 | 239 | 0 |

The extra two-/three-tile trials exercise keyboard tile focus in workspace
scope. Every topology also tests hover and Tab, with and without real native
resize. `verified-two-expand` and `verified-three-expand` contain another 20
paired trials and 475 frames at 1.18 expansion: every resized trial ends at
exactly its no-resize control geometry. Intentional expansion movement is
retained.

Both 552bd339 and 9c92cb5 with tiles disabled pass all eight invariance trials;
their opening preview rectangles match exactly. Single-tile cross-output
force-all in `monitor-before-v2` moves six previews in each 24-frame trial;
`monitor-after-v2` and `monitor-reference-v2` have zero moved frames. The native
resize trial changes two real client sizes in all three builds. Paired output
frames are [before](../build-tiles/monitor-before-v2/forceall-monitor-hover-resize1-10.png)
and [after](../build-tiles/monitor-after-v2/forceall-monitor-hover-resize1-10.png).
The adjacent JSONs include monitor identity and each frame's native sizes.
An exploratory combined multi-tile/multi-output fixture did not settle its
opening scene and is excluded from acceptance (`verified-two-outputs-v2.log`).
The supported cross-output control uses single-tile outputs; multi-tile
acceptance uses the complete one-output matrix above.

`build-tiles/focus-summary.json` summarizes the observations; hashes and source
identities are retained in `build-tiles/fb7-manifest.json`.

Full CTest: 16/16 pass, no skips (`build-tiles/ctest-final.log`).
`tiles_disabled_regression` passes for repaired and reference builds, with
exactly equal `default-off-observations.json`. Full release integration smoke
passes (`build-tiles/release-smoke-final.log`). The final workspace-transition
regression also passes all six dispatcher/held-trackpad/release × Stage-off/on
checks with zero moved or missing neighbor previews and zero changed neighbor
pixels (`build-tiles/switch-final.log`).

Fresh upstream Nix compatibility is unavailable in this environment:
`nix --extra-experimental-features 'nix-command flakes' flake update hyprland`
fails creating `/nix/store` with permission denied (`build-tiles/flake-update.log`).
The lock file is unchanged. The release-compositor evidence above does not
establish compatibility with fresh upstream main. No live user-session or HDR
appearance acceptance is claimed.

## Run

```sh
python -B tools/tiles_isolated_smoke.py --plugin build-tiles/current/libhymission.so --fixture build-tiles/current/libhymission-tiles-test.so --focus-reflow-regression two --output build-tiles/reproduce-focus
```

Use `off`, `single`, or `three` for the other primary-output topologies.
Add `--focus-reflow-expand` for normal selection expansion.
With topology `off` or `single`, `--focus-reflow-bisect` adds a second nested
output and runs only paired cross-output focus trials in force-all.
Use `--focus-reflow-observe` only for known failing builds; it records the
failure metrics without claiming acceptance.
