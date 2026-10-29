# FB21: stale desktop behind a continuous overview swipe

Baseline: local `hyprland-master` `669c6243d57986c2cc55c76139b401424c4076a1`.
Work is confined to `hymission-tiles-fb7`, branch `fix/continuous-swipe-ghost`.
All runtime evidence uses a private nested compositor and synthetic four-finger
device events through PointerManager/InputManager/EventBus/gesture matching.
No host IPC or physical input is used.

## Reproduction and cause

With tiles enabled but only one logical tile, open all-workspace overview from
workspace 1, then hover a workspace-2 window with focus follows mouse and 1.18
expansion. The real compositor switches focus and its active workspace to 2.
The single-tile model still caches workspace 1 until reconciliation. That is
normally harmless: one tile deliberately delegates desktop rendering to native
Hyprland rather than running the multi-tile presentation hooks.

A continuous downward four-finger recommendation gesture crosses all overview,
the desktop gap, and workspace overview without deactivating overview between
the two scopes. `restoreOverviewRenderState()` treated the cached tile binding
as another physically presented workspace. It restored workspace 1 to
`visible=true` and workspace alpha 1 even though it was no longer active or in
the compact overview's managed list. Native rendering then drew its windows
at desktop coordinates behind the real previews. Their normal inactive opacity
and dim made them look like a retained dark overlay.

This is live native rendering, not stale framebuffer damage: the captured
damage covers the complete output, and native state explicitly reports the
extra workspace as visible. It is not a retained managed/relayout item, stuck
`forceRendering`, nonzero renderOffset, or a second FB20 alpha multiplication.

Representative state after crossing into workspace overview:

| State | Baseline | Fixed |
| --- | --- | --- |
| Native active workspace / hovered focus | 2 | 2 |
| Cached single-tile binding | 1 | 1 |
| Main managed windows | workspace 2 | workspace 2 |
| Old workspace-1 visibility / alpha | true / 1 | false / 0 |
| Old workspace-1 forceRendering | false | false |
| Old workspace-1 renderOffset and goal | 0,0 | 0,0 |
| Native inactive window alpha / dim in opacity control | 0.4 / 0.15 | 0.4 / 0.15 |

The opacity/dim values correctly remain native properties; the hidden old
workspace must not be rendered at all. Active target windows retain 0.85 alpha
and zero dim in that control. Gesture, transient-window and relayout diagnostics
verify cleanup after commit/cancel and final close.

## Repair

`TilesController::presented()` now admits extra tile presentation only when
the real multi-tile path is active. With a single logical tile, overview's
existing native-active-workspace checks are authoritative. This also corrects
the shared offscreen endpoint and exit visibility predicates without changing
the cached model during hover or forcing a topology/layout rebuild.

FB7's frozen main slots, FB19's shadow and live strip geometry, and FB20's live
active opacity remain in place. Additional read-only overview diagnostics and
private frame metadata expose gesture/relayout progress, transient count,
native focus, dim, active-workspace identity and renderOffset goal.

## History controls

Exact archived sources were built under `build-tiles/fb21/candidates/`, without
checking out or editing other worktrees. The same device-level scenario and
final pixel oracle reject all of these controls with the same visible inactive
desktop in workspace overview:

- `669c624`: current baseline, including FB20.
- `9c56a48`: FB19.
- `4c1f225`: frozen-slot/focus changes.
- `931b195`: natural replacement.
- `6e0b593`: parent of the natural replacement, using its original natural
  engine in this scene.

The defect therefore predates all four suspected recent changes. Source blame
and `git log -S` identify `2ee664b` as the introduction of the tile-presented
exception in overview restoration. That is source provenance, not a claim to
have executed an exact runtime bisect across `2ee664b` and its parent.
Raw history runs and failed pilot oracles are retained.

## Pixel oracle and evidence

`tools/overview_fb21_regression.py` creates five real colored Wayland clients
across three persistent workspaces and records every rendered frame. It uses
the user's natural layout, focus follows mouse, 1.18 hover/selected expansion,
workspace-change preservation, toggle-switch mode, and recommendation gesture.
Each configuration exercises:

1. One uninterrupted all → desktop → workspace gesture.
2. Two separate gestures, with a completed desktop handoff between them.
3. Crossing into partial workspace overview, cancelling, then closing.

With `gesture_close_restores_focus=1`, the separate desktop handoff restores
the opening focus, while the continuous transfer targets the hovered workspace.
The two controls intentionally need not have the same final workspace identity.

The oracle rejects solid marker-body pixels outside each window's owned scene.
It excludes the reserved left strip columns, including their slide-in path;
FB19 independently validates the strip itself. One-pixel body erosion removes
red-CSD interpolation hues that can imitate another marker color. The initial
pilot also incorrectly assumed the hovered workspace must own the zero-openness
desktop; it now respects the gesture's prepared exit geometry. These pilot
failures and corrections are preserved; the final **64-pixel** foreign-body
threshold is unchanged. A synthetic 24×24 unmanaged-window ghost must trigger
the same ownership mask; all accepted configurations detect **484** interior
foreign pixels in that explicitly labeled negative control.

The final fixed matrix has **11/11 configurations**, **33 gesture paths**, and
**3,824 recorded frames** passing, with **zero foreign main-scene body pixels**.
It covers tiles off/single/two in both dwindle and scrolling; active/inactive
opacity plus dim; FP16/MRT; hover interrupted mid-relayout; same-workspace hover;
and native compositor animations enabled. The mid-hover run verifies relayout
is actually active when the gesture starts. Stable expanded trials require
actual preview growth, and all trials require a real native focus change.

Before/after screenshots, native states and pixel reports are indexed by
`build-tiles/fb21/index.html` and `evidence-identity.json`. Baseline `before-final/`
and `before-opacity/` fail the final oracle when the compact scope first opens;
the retained failing samples contain roughly **934,555** and **804,016** foreign
pixels respectively. Fixed captures are under `final/`, with per-case
`fb21-pixel-checks.json`, settings, input artifact hashes and raw frames.

## Required gates

- Complete local Release build and CTest: **16/16 passed**, no skips.
- Required isolated regression matrix: **18/18 configurations passed**: FB7
  focus/expansion off/single/two/three; FB19 off/single/two/three plus off/three
  FP16; six FB20 opacity/blur/Kitty configurations; disabled regression; and
  full release integration smoke. Logs and raw results are under `required/`.
- The Hyprland flake input was refreshed and remains
  `a1c2428f79b3b05c3fd36c27378e3538c8c6a014`. Nix builds the production repair
  and private fixture successfully. Fourteen tests pass there; the Stage EGL
  and snapshot-color EGL tests skip and both pass locally (`nix.log`).
- Isolated runtime and matching cached headers remain private HDR compositor
  revision `e5a03b6a5ce64ec4c85954649bad4df083627574`. This is distinct from
  upstream compilation and from physical host-session acceptance.

Local plugin SHA256:

- Baseline: `daa868e03f0df9783ba937c71784598a7eda2f9b8732bc70a4c84b8c06a92220`.
- Fixed: `fb38211e4b8dabdae7e6a539c096a0158813d22d484bf2f95bd2c4e942879190`.

Example isolated reproduction (requires the existing nested harness dependencies,
Pillow and NumPy):

```sh
python3 tools/tiles_isolated_smoke.py \
  --plugin build-tiles/fb20/build/libhymission.so \
  --fixture build-tiles/fb20/build/libhymission-tiles-test.so \
  --output build-tiles/fb21/reproduce \
  --fb21-regression single --fb21-layout scrolling --fb21-opacity
```

No push, host reload, `hyprpm update`, host plugin load/unload, or other-worktree
edit was performed. The supplied WeChat screenshot was inspected, but actual
WeChat/host-session acceptance is not claimed from these isolated fixtures.
