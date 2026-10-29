# Stage column drops through native input (FB11)

> FB18 supersedes the ordering policy in this historical report: every Stage
> column uses numeric/name order; sidebar drag height never inserts/reorders cards
> or swaps workspace contents, including with tiles disabled. Ownership transfer
> and FB17's shared preview/static/flight authority remain. See
> [the current contract](workspace-tiles-feedback-round18.md).

## Reproduction and the FB6 gap

Baseline: `f080e8b9679c632c0255457c0d9c5ba1eda26f8b`. The supplied screenshot has four cards in the first column, one in the second, and a large unused region below the second column. No screenshot is committed.

The actual configuration uses `general.layout = "scrolling"`, `stage_show_active = 0`, and `main = "SUPER"`. `hyprland-bind.lua:72` binds `SUPER + mouse:272` to `hl.dsp.window.drag()` with `{ mouse = true }`; this is the native move-window mouse binding, not a Stage thumbnail dispatcher. A plain Stage card press follows Stage's swallowed-press/release path; Meta over a preview instead starts `thumbnailDrag`.

FB6 missed two distinct routes:

1. Native `endDrag()` required both a screen and a card index. A column background was ignored, although `tileColumn()` already covered its full height for Stage card transfers. FB6 tests called the event bus directly and did not run the pointer device → InputManager → mouse binding → native drag-controller lifecycle.
2. A plain card press over a visible window set `tileDragClicked`. On a cross-column release, that flag selected `moveWindows()` into the hovered card or column's active workspace. The workspace-transfer test deliberately pressed the card border, missing the usual preview-body press. The old test actually asserted the unwanted merge behavior for body presses.

A strengthened two-column existing-card case also exposed stale insertion geometry (`fb11-two-final`, before the final correction): `moveWorkspace()` publishes synchronously and Stage rebuilds `screens`; the caller then used its old column pointer to calculate ordering. Shared adaptive card dimensions can change at that point as well. Insertion is now computed before publishing, against the visible drop geometry. The target monitor/tile is saved by value; failed publication restores the old order.

The new fixture emits private `IPointer` motion/button signals, which run the compositor's registered InputManager listeners. The test sets Meta through the private keyboard modifier state and `onKeyboardMod`, installs the same mouse binding as the machine, and asserts a native drag target exists after the Super+press. It never invokes the fixture's internal drag/drop API. Esc uses the private keyboard device key signal. These are real compositor input routes with synthetic private-device events; no physical mouse or host session is driven.

## Confirmed tiles-disabled semantics

The requested native analogy needs a distinction. With tiles disabled and `stage_empty_slots = 0`, Super+dragging onto arbitrary sidebar background does **not** create a workspace. Dragging onto an explicitly configured synthetic empty card does create one. Both were reproduced on the baseline and repaired plugin (`fb11-off-final`, `fb11-final-off`). The baseline arbitrary-background case is not described as a pre-existing feature.

FB11 deliberately extends the tile-column background into a create-workspace target while retaining the old tiles-disabled behavior. The destination id is the next positive id after existing workspaces; it is allocated only on a valid, uncancelled native release. It is hidden unless `stage_drop_follow` requests activation. A native drag into an existing card still moves the individual window into that card's workspace.

## Repaired behavior

- Native Super+move release resolves the complete tile column, including its bottom background, padding and inter-card gaps. An empty-background drop creates a distinct workspace. `finishDrop()` moves the window through `TilesController::moveWindows()` so destination ownership and window movement commit through the existing activation transaction. It no longer inherits the currently focused tile's home.
- A plain card drag between columns always calls `moveWorkspace()`, regardless of whether the initial press hit a window preview. Window workspace identities are preserved. A visible workspace retains the existing display-slot exchange policy; a hidden workspace changes its column ownership.
- Plain card drops onto existing cards insert before/after according to the pointer's position relative to card centers; they do not merge window contents. Bottom background inserts at the end. Same-column ordering retains the existing policy.
- Meta+preview remains the explicit individual-window gesture. It is not upgraded into a split transfer. Native desktop drops, split previews and cancellation retain their prior owners.

## Evidence and validation

Run the new private regression with:

```sh
python -B tools/tiles_isolated_smoke.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --stage-device-drop-regression two \
  --output build-tiles/fb11-current-two
```

Use `three`, `single` or `off` for controls. `--stage-drop-observe` records failed checks without stopping baseline playback. `--stage-device-layout scrolling --stage-device-hide-active` reproduces the native blank-column route with the machine's layout and hidden active cards; active cards are then enabled to expose the explicit destination-card controls.

Each drag records every rendered framebuffer, native window/workspace state and Stage state, plus held/released screenshots and membership snapshots. PNGs and JSON frame indices remain ignored under `build-tiles`; `frame-manifest.json` contains per-sequence frame counts and pixel hashes. The test uses two windows in the transferred workspace and checks every window's workspace identity, not merely the selected preview.

| Private run | Result | Recorded frames |
| --- | --- | --- |
| Baseline two tiles, `fb11-baseline-two-final` | 7/15; reproduces blank-create and card-identity failures | 95 |
| Baseline three tiles, `fb11-baseline-three` | 11/15; reproduces blank-create and card-identity failures | 96 |
| Repaired two tiles, `fb11-current-two` | 15/15 | 99 |
| Repaired three tiles, `fb11-current-three` | 15/15 | 91 |

Additional repaired runs pass: single tile, tiles disabled, and scrolling with two/three tiles (`fb11-current-single`, `fb11-current-off`, `fb11-current-scrolling-{two,three}`). Assertions include no membership mutation while the native drag is held, Esc cancellation before workspace creation, destination-column ownership, insertion order, explicit Meta movement and release of the shared interaction owner. These establish isolated compositor behavior, not user acceptance on the physical session.

Required and adjacent regressions:

- Full local CTest: **16/16**, no skips (`fb11-current-ctest.log`).
- `tiles_disabled_regression`: pass, with exact JSON equality to `feedback3-disabled-reference/default-off-observations.json` (`fb11-current-disabled`).
- Complete release integration smoke: pass (`fb11-current-release`). Its individual-window column test now explicitly holds Meta; the old plain-press merge assertion is replaced according to FB11, rather than weakening the membership check.
- Existing three-column FB6 regression: pass (`fb11-current-legacy`), including ordering, desktop transfer, cancellation and owner release. Its individual-window case likewise uses explicit Meta.
- Three-column four-finger Stage swipe regression: **36/36** (`fb11-current-swipe`).

Release runtime: `~/.local/lib/hyprland-hdr/Hyprland`, commit `e5a03b6a5ce64ec4c85954649bad4df083627574`, SHA256 `b08dd5a78a0f8f04ae3936d77a4b99da105c97b53e5d975cdd4fdbd88dc0ced6`. Binary/source hashes and frame manifests are indexed in `build-tiles/fb11-evidence-identity.json`.

The refreshed flake still resolves upstream main `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`, independently checked with `git ls-remote`. Development `nix build -L .#hymission --no-link` passes using the writable local store. Its two EGL tests are skipped in Nix; both pass in the local complete CTest run. The lock file is unchanged.

Fresh fetch confirms `origin/hyprland-master` is the baseline `f080e8b`, already HEAD's ancestor before this commit; no rebase is needed. The FB7 worktree is untouched. No push, host reload, hyprpm update, or manual host plugin load/unload is performed.
