# Merge master defaults into the development tiles branch

Merge parents: development `08f247a` and master `d0e847d` (including opaque search
styling `5436e4f`). Work is confined to `hymission-merge-defaults`, branch
`merge/defaults-into-hm`. No stable or Omarchy branch is modified.

## Resulting layout contract

- Retain the new defaults: scrolling preview gap 24, selected outline 3,
  backdrop ARGB 0x26000000, blur 0. The search CSS is identical to master.
- Keep both overview JSON fields: `relayout_active` and `visualProgress`.
- A strip band uses **global** coordinates. Monitor and per-tile content
  rectangles use **monitor-local** coordinates. The merged
  `contentAfterGlobalStripColumns` helper subtracts the output x origin exactly
  once, then clips to each tile. Unit tests cover an output at x=1920, multiple
  tiles, reserved offsets and a narrow remaining viewport.
- Without split tiles, top/side strip positioning follows master's reserved-safe
  monitor rectangle. Split tiles keep their existing vertical strip columns,
  one per tile, including the continuous glass panel and its reserved header.
- All overview keeps one combined content layout. Workspace overview keeps each
  preview within its tile's allocation, with a 64 logical pixel search header
  reserved in **every** tile whether search is shown or hidden.
- Both scopes use **one search box centered over the combined content area**.
  Existing workspace overview collects all visible tiles' workspaces, and
  `windowMatchesActiveSearch` filters that entire collection. This merge preserves
  that shared query; it does not introduce a focused-tile-only search scope or
  move the header when typing. Explicit output-relative layer-shell positioning
  avoids adding exclusive reservations twice.

## Local build and native gates

System `hyprland.pc` headers identify
`e5a03b6a5ce64ec4c85954649bad4df083627574`, matching the private release compositor
`~/.local/lib/hyprland-hdr/Hyprland`. The installed stable compositor is not used
as development compatibility evidence. Configure and full build:

```sh
cmake -S . -B build-tiles/merge-defaults/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DHYPRLAND_TEST_EXECUTABLE=/home/wilf/.local/lib/hyprland-hdr/Hyprland
cmake --build build-tiles/merge-defaults/build -j4
ctest --test-dir build-tiles/merge-defaults/build --output-on-failure
```

**16/16 CTest tests pass, no skips.** The workspace-key header-only preload test
now links only its actual hyprutils dependency. Linking the entire dependency
list loaded system Aquamarine .14 alongside the private compositor's .15, causing
an invalid free on process exit; the minimal link removes that duplicate from
this test without changing plugin or compositor runtime behavior.

All native tests below use separate Weston headless parents, private runtime /
configuration / IPC / Wayland sockets and the matching release binary. No host
IPC is discovered or contacted. Common arguments:

```sh
--plugin build-tiles/merge-defaults/build/libhymission.so \
--fixture build-tiles/merge-defaults/build/libhymission-tiles-test.so
```

Run `python -B tools/tiles_isolated_smoke.py` with these arguments and the option
below, plus `--output build-tiles/merge-defaults/<directory>`:

| Gate | Options | Directory | Result |
| --- | --- | --- | --- |
| Tiles disabled | `--default-off-regression` (also callable as `tools/tiles_disabled_regression.py`) | `disabled` | PASS |
| Full release integration | `--integration` | `release` | PASS |
| Continuous per-tile strip | `--feedback2-regression panel` | `strip` | PASS |
| Ws overview switch/teardown, both tiles, Stage off/on, Escape/close | `--feedback3-regression workspace` | `workspace` | PASS |
| Multi-window Stage / resize / right-column origin | `--feedback3-regression stage-multi` | `stage` | PASS |
| Stage viewport/camera anchors | `--stage-viewport-regression three` | `stage-viewport` | PASS |
| Stage flight/order/handoff | `--stage-flight-clip-regression three --stage-flight-clip-route key --stage-flight-clip-layout scrolling --stage-flight-handoff --stage-order-regression` | `stage-flight-order` | PASS |
| Overview moving edge band | `--overview-reveal-regression three` | `overview-reveal` | PASS |
| Search / strip / split geometry | `--search-geometry-regression` | `search` | PASS, 32 scenes |

Each directory has a matching top-level `<directory>.log`. Search scenes cover
0/1/2/3 tiles, no bar / a real 48px exclusive top bar, strip force-show off/on,
all/workspace overview, helper hidden/shown and an actual matching typed query.
Workspace scope intentionally enables its strip even with force-show off.
Bar cases also move the output to **(1920,300)**. Assertions check exact header
y, no strip/preview overlap, exactly one search surface, unchanged matching-window
rectangles while typing, per-tile workspace preview bounds and both JSON fields.
A single native tile retains its existing non-split strip behavior.

Screenshot index: `build-tiles/merge-defaults/screenshots.html`. PNGs and state:

```text
build-tiles/merge-defaults/search/
  tiles-{0,1,2,3}-bar-{0,1}-strip-{0,1}-{forceall,onlycurrentworkspace}-{hidden,empty-search,shown}.png
  <same-name>.json
  search-checks.json
```

The additional empty-search captures distinguish helper appearance from filtering.
Three-tile screenshots with a bar demonstrate the per-column strip and shared
header clearly. The box styling remains master's opaque, shadowless surface.

## Fresh upstream build

The Hyprland flake input is refreshed to upstream main
`c7216a9807a2683ce243f6ed14ae01e7e884c426` and the updated lock is included in this
merge. Nix is run with a **worktree-local store**, because the machine has no
writable system `/nix/store`:

```sh
nix --extra-experimental-features 'nix-command flakes' \
  --store "$PWD/build-tiles/merge-defaults/nix-root" \
  --option sandbox false build -L .#hymission \
  --out-link build-tiles/merge-defaults/nix-result
```

The fresh upstream build passes. Its CTest run has **14 passed, 2 skipped**:
`hymission-stage-edge-render-test` and `hymission-snapshot-color-test` skip when
the Nix builder cannot create the required software EGL context. Both tests pass
in the system-pc run above; no fresh-upstream EGL/pixel acceptance is claimed.
See `nix-update.log`, `nix-build.log` and `nix-final.log` under the same artifact
root for the resolved input and build/check results. Fresh upstream build evidence
is separate from the nested runtime gates above, which use e5a03b6. No host install,
push, hyprpm update, manual host plugin load/unload or live reload is performed.
