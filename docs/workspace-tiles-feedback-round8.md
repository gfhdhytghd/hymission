# Physical fullscreen, drag stacking and floating ownership (FB8)

## Policy

True fullscreen (`internal=fullscreen`, `client=fullscreen`, native monitor handler) covers the physical monitor, including all tile allocations and Stage/strip. Maximize, internal-only fullscreen and scrolling layout fullscreen columns retain their tile bounds. Requests are classified using the same `sync_fullscreen` rule as native Hyprland, including single-mode actions with an absent client argument. Effective internal/client fullscreen is routed to the native default monitor handler even when the caller requests layout-aware fullscreen. Internal-only requests keep their layout handler. This distinguishes a client fullscreen request from the layout's viewport sizing mechanism.

A true fullscreen keeps keyboard/pointer focus while its workspace remains displayed. Hover or `tile_focus` cannot send input behind it. Other tile bindings remain intact; switching away from the fullscreen workspace hides it normally, and returning preserves its native mode. Overview can temporarily present all windows and restores fullscreen on exit. Stage hides all columns together.

Normal floating windows may straddle tile boundaries. They render after tiled content in native global stacking order, without a source tile clip, and retain normal native top/overlay-layer rules. Their actual rectangle center selects the destination tile (inverse-mapped through the shared Stage desktop transform). Moving or resizing across that center boundary transfers to the destination's active workspace without changing global geometry. A focused float updates the tile/compatibility workspace in the same movement transaction while keeping the actual keyboard-focused window, so subsequent workspace/new-window actions use its destination tile. Hidden owner workspaces hide their floats. Fullscreen exit and other noninteractive geometry animations settle before automatic rehoming; native drag/resize still updates live. Geometry restoration uses the native floating algorithm API to keep its restore cache consistent, rather than only rewriting the target rectangle. Pinned/special windows and workspace-switch animations retain their native policies.

Native tiled drags still commit on release through the existing drag controller transaction. Real floating drags may update ownership live; Esc restores the original workspace and geometry. A physical fullscreen request cancels a held Stage transfer without committing its drop. Only the native drag controller moves the window. Its draw passes are deferred past tile scenes and plugin chrome, before native top/overlay layers and cursor. Stage draws one ghost after all columns, including sidebar reorder. Overview already draws its single ghost after the workspace strip; that implementation is retained.

## Root cause

`clampFullscreen()` unconditionally selected tile allocation for every internal fullscreen mode. The current allocation/Stage-presentation expression is blamed to `4a439a5`; earlier tile fullscreen clamping already existed. `e9d68cd` additionally made scrolling `logicalBox()` use Stage's tile presentation. Both geometry boundaries need a physical-fullscreen exception; changing only one lets a later native sync/layout pass shrink the window again. Fullscreen publication can set client mode after the internal geometry setter, so the deferred reconcile also synchronizes the final physical geometry. A strict smoke assertion additionally caught `hymission.fullscreen` using the scrolling layout handler: its native state was internal/client `2/2` while its rectangle remained tile-sized. The public setter hook normalizes this real fullscreen request to the monitor handler, retaining internal-only `2/0` semantics. Tests assert the actual mode pair, not just a small rectangle.

Tile fanout also made draw ordering local to each workspace: an unclipped drag was not guaranteed to remain above later tile/effect passes. Floating windows followed the same per-tile clip and hit-test path. Deferred native draw passes and global native floating stacking fix that without creating another motion/animation owner. Deferred floats are processed once, not redrawn in every tile. A visible physical fullscreen renders and sends frame callbacks only for its workspace; other tile windows and Stage previews are not submitted.

## Evidence

The new `--presentation-regression two|three` private compositor cases use six real Wayland clients: green source windows and opaque blue destination windows. Tests cover dwindle/scrolling, Stage off/on, full enter/exit frame sequences, physical-width/Y/H endpoints, maximized/internal-only bounds, whole-output hit tests, keyboard focus, overview open/close mode restoration, unchanged tile workspace bindings, floating visibility across edges, move/resize ownership, hiding/restoring owner workspace, and native/overview/Stage drag screenshots both over occupied desktops and sidebar/strip.

The observation-only archived `34908f1` binary is `build-tiles/fb8-before.so`. Results and screenshots are in `build-tiles/fb8-baseline-two/`; fixed records are in `build-tiles/fb8-final-two/` and `fb8-final-three/`. Each sample includes native clients and tile/Stage/overview state. Fullscreen animation `.json` records intermediate native size/position and framebuffer readbacks. The regression disables overview hover enlargement/focus-following and moves the pointer over the source before pressing; focus-dependent reflow belongs to the independent FB7 change.

Representative before/after images: `build-tiles/fb8-baseline-two/dwindle-stage1-fullscreen.png` versus `build-tiles/fb8-final-two/dwindle-stage1-fullscreen.png`; floating edge images `float-straddling-left-center.png`; held-drag images `native-drag-stage1.png`, `overview-drag.png`, `stage-drag-over-sidebar.png`.

Runtime evidence is from isolated Weston/Hyprland with the installed release compositor, not from the host desktop or a physical HDR acceptance run. No push or host reload is performed.

## Measured endpoints and validation

With a 1920×1080 output and Stage open, the source workspace window's native `(x,y,width,height)` was `(333,0,793,1080)` with two tiles and `(411,0,755,1080)` with three tiles before this change. It is now `(0,0,1920,1080)` in both cases, with native modes `2/2`. Internal-only tests explicitly assert `2/0` and tile bounds. Each fixed two/three-tile case passes 88 state/pixel/frame checks, including layout-aware fullscreen, floating fullscreen enter/exit and geometry restoration, focus following the floating center/resize without changing the focused window, a fullscreen request interrupting a held Stage drag, and pinned windows above ordinary floats.

The observation-only baseline continues after failed assertions, so later workspace bindings can diverge. Initial fullscreen and first floating-edge/center samples provide the targeted before/after comparisons; final held-drag screenshots establish the requested top ordering without treating every later baseline failure as an independent causal experiment.

The branch was rebased onto `origin/hyprland-master` at `cb1b5def81c2c7175e975ed939e3ca6b9da42219`. The independent FB6 commit is now `67e2785`. Only a shared test-fixture conflict required resolution: the FB7 native width-reset action, monitor-filtered readback and native frame fields were retained alongside the new FB8 actions. No changes were made in the FB7 worktree or to its overview implementation.

Validation after rebase:

- Full local CTest: 16/16 pass, no skips (`fb8-final-ctest.log`, also `fb8-ctest.log`).
- Disabled observations exactly match the pretiles reference (`fb8-final-disabled/default-off-observations.json`).
- Full release smoke passes, including strict exact physical fullscreen rectangles for typed/default and scrolling `hymission.fullscreen` actions (`fb8-final-release.log`).
- Final two/three-tile presentation regressions: 88/88 each (`fb8-final-{two,three}/results.json`). Real native move drags retain tiled/floating state, cross occupied and empty targets in both directions, and restore cancellation (`fb8-native-drag.log`).
- FB6 two/three column drops, CSD/reserved top, native animation curves/speed/swipe, three-tile Stage desktop height, bounded Stage preview workload and all-overview endpoints pass (`fb8-complete-core.log`).
- Rebased off/single/two/three focus reflow controls, two/three hover-enlargement controls, panel, device gesture, shared Stage scene, hover routing, workspace overview exit, HiDPI Stage, scrolling camera/crop, adaptive card count and three-tile shadow/close/device-swipe regressions pass (`fb8-rebased-visual.log` and `fb8-rebased-remaining.log`). The visual runner's old `stage` assertion still reused horizontal compression for vertical geometry; it now checks the separate vertical scale required by FB5, retaining its original CSD/pixel checks.
- The Hyprland flake input refresh resolves unchanged to `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`; Nix builds pass (`fb8-complete-nix.log`). Its 14 CPU/contract tests pass and two EGL tests are skipped in the build environment; both GL tests pass in the complete local 16-test suite.
- Final artifact replay of FB7 two/three-tile focus and hover-enlargement cases also passes (`fb8-final-fb7.log`). A final fetch confirms `origin/hyprland-master` remains `cb1b5de` and is an ancestor of this branch.
- Isolated config-error files are empty. Logs/screenshots/readback data are ignored build artifacts, not committed.

Final native release binary SHA-256: `b08dd5a78a0f8f04ae3936d77a4b99da105c97b53e5d975cdd4fdbd88dc0ced6`. The complete per-artifact identity record is `build-tiles/fb8-evidence-identity.json`.
