# Tile-owned relative workspace navigation (FB9)

## Behavior and root cause

The baseline is `origin/hyprland-master` at `d1247dbabadf5628ae65da5e32cd093f3da4dc02`. The native resolver sees one physical monitor, so `m/e/r/+/-/next` previously included other tiles' workspaces. The separate tile swipe candidate loop excluded visible foreign workspaces but still included hidden foreign workspaces. Previous/back-and-forth consulted unfiltered tile history. Stage and the workspace strip used a separate `homes` table with a history fallback, so those paths could disagree.

Each output now has one `tiles::Ownership` table used by Stage/strip `preferredTile`, relative resolution, swipe and history. Presented workspaces belong to their current display slot. Hidden workspaces retain their assigned home. FB6 workspace-card transfers and explicit hidden workspace activation update that same table. History cannot reclaim a transferred workspace. Unknown preexisting workspaces retain the first-column default. Workspace creation during multitile operation records the focused tile. Tile removal merges hidden homes into the adjacent surviving tile, preferring the left neighbor, matching the removed tile's width recipient. No synthetic monitors or compositor source changes are introduced.

Named workspaces retain native reverse creation order before sorted numbered workspaces. `m/e` wrap within the owned existing list; absolute `~` selectors index that list. `r`, bare signed offsets and `next` first traverse that list and create on positive overflow, skipping foreign occupied IDs and native monitor-rule reservations. Reverse overflow stops at the first owned entry. Invalid selectors and numeric overflow fail. Previous and back-and-forth select only still-existing entries still owned by the current tile. Explicit named/numbered requests retain focus/swap behavior for a displayed target and adopt a same-monitor hidden target into the caller tile.

Swipe reuses the same candidates and relative resolver, preserving `workspace_swipe_use_r`, right-edge create versus wrap, native left-edge stopping, direction locking and the existing cancellation/forever animation owner. Only multitile operation intercepts selectors and swipes. Off/single operation uses the native resolver and gesture implementation.

## Isolated evidence

`tools/tiles_relative_regression.py` uses six/four real Wayland clients in a private nested compositor, with homes `(1,7)`, `(3,8)`, `(5,9)`. Keyboard tests bind F8 and emit private `IKeyboard` press/release events through InputManager. Gesture tests emit four-finger vertical device swipe events through PointerManager/InputManager/trackpad matching; they do not call unified swipe or workspace dispatch directly. Both `workspace_swipe_use_r` settings and Stage off/on are exercised. Native animation reports, tile samples and middle-frame PNGs record the owned incoming workspace and stable neighboring tiles.

The archived prechange plugin `build-tiles/fb9-before.so` matches the FB8 artifact identity. In the three-tile observation run, the first `m+1` from workspace 1 selects foreign workspace 3; the first forward device swipes from 1/3/5 end on 3/5/7. Fixed runs select 7/8/9 respectively. Baseline observation mode continues after failures, so subsequent state may diverge; its failure count is diagnostic coverage, not independent causal findings.

Artifacts are ignored under `build-tiles/fb9-*`, including keyboard JSONL, Stage/strip membership snapshots, gesture native state/PNGs, before/after results and binary hashes. These are isolated compositor observations, not physical touchpad or host-session acceptance. No host reload, update or push is performed.

## Validation

- Logic tests cover ownership claims/transfers, adjacent removal in both directions, stale/deleted history, named/numbered order, signed/m/e/r selectors, absolute indexing, wrap, create, reserved foreign IDs, malformed input and overflow, plus the existing 40,000 randomized model operations.
- Fixed two/three-tile regressions pass 158/232 checks; the latest baseline three-tile observation records 74 failed checks out of 232 (later state can diverge after the first failure).
- Full local CTest passes 16/16, including both GL tests.
- `tiles_disabled_regression` observations exactly match `feedback3-disabled-reference/default-off-observations.json`.
- Release integration smoke passes on `~/.local/lib/hyprland-hdr/Hyprland` (e5a03b6), including default-off, focus/swap, Stage/overview drag transactions, fullscreen, second-output placement and disable cleanup.
- FB6 two/three-tile Stage drop, real four-finger animation, native animation and FB7 three-tile focus invariance regressions pass.

- Native off/single controls each compare seven real-keyboard selectors (`m±`, `e-`, `r+`, signed offset, next, previous) against the archived baseline; both JSON records match exactly (`fb9-{off,single}/control.json`, `fb9-baseline-{off,single}/control.json`).
- FB8 three-tile physical fullscreen, floating-center ownership and native/overview/Stage drag pixels pass (`fb9-presentation-three/results.json`).
- Complete local rerun is recorded in `fb9-complete.log` and `fb9-final-ctest.log`. Private compositor config-error files are empty. Per-artifact SHA-256 identities and check totals are recorded in `fb9-evidence-identity.json`.

- Refreshed Hyprland flake input remains `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`; `nix build -L .#hymission` passes (`fb9-flake.log`, `fb9-nix.log`). Nix runs 14 tests and skips two EGL tests in its environment; both pass in the local complete 16-test run. The writable local Nix store was used; no system install or host reload was performed.
