# Hyprland development compatibility baseline

- Development line: `hyprland-master`, adapted on `port/hyprland-0.57`.
- Baseline: the project's fork `gfhdhytghd/Hyprland`, branch `main` = tag
  `v0.57.0` = `f6725e43eed50eaa061aa25fe7c9f8cdc9d355e2` (merges upstream
  `hyprwm/Hyprland` `main` through `7a5e469c7` plus fork changes). Upstream's
  own `v0.57.0` is a different commit (`0aa6745f…`); its headers and binaries
  are not ABI evidence for the fork (for example `SCMSettings` differs in size).
- Aquamarine: fork `gfhdhytghd/aquamarine` `master` = `4abfcabbf3908cabccead39e5b5b8361dbf2546a`.
- Date: 2026-10-28.
- Status: **PASS (offline) — exact-header build, hook audit against the exact
  binary, Nix build; no runtime acceptance.**

## API and ABI changes handled

| Change in the baseline | Plugin adaptation |
| --- | --- |
| `Event::bus()->m_events.workspace.created` split into `createdEarly` (old emit point, end of `init()`) and `createdLate` (new `ready()`) | Stage and tiles listen to `createdEarly`, keeping the previous timing; tiles still claim a new workspace for the tile focused at creation. |
| New workspaces must be marked `ready()` by their creator after setup; `CState::create()` does not | Every plugin creation goes through `createWorkspace()` / `createWorkspaceForKey()` (`src/workspace_key.hpp`), which marks it ready on the next event-loop turn through an owned `doLaterLock`; `PLUGIN_EXIT` makes pending ones ready and cancels the queued callbacks. |
| `IHyprRenderer::renderLayer(..., popups, lockscreen, workspaceScene)` | Overview and tiles thunks take and forward `workspaceScene`; the two background/bottom-layer snapshot calls pass `false`. Pre-existing mismatch: upstream `a1c2428` already had this signature. |
| `CPlacementController::moveWorkspaceToMonitor(..., noWarpCursor, carryFocus, replace)` | The tiles thunk forwards `replace`; its own old-tile replacement only runs when `replace` is true. The exact tiles symbol string was updated (it would otherwise disable tiles). |
| Fork legacy GLES2 renderer (`m_eglContextVersion == EGL_CONTEXT_GLES_2_0`) | Stage soft edges fall back to the hard viewport clip, the overview Gaussian blur and framebuffer blits are skipped, before any GLES3 state is touched. Not run on a GLES2 compositor. |

Guards added so the next native signature change fails at build or load time:

- `HYMISSION_NATIVE_HOOK_ABI` (`src/hook_abi.hpp`) compiles each changed thunk's
  member type against the native member (overview `renderLayer`, tiles
  `renderLayer` and `moveWorkspaceToMonitor`).
- CTest `hymission-hook-abi-test` runs `tools/verify_dev_hooks.py --types-only`
  (49 thunk/call types). With `--hyprland <binary>` the script also audits 46
  name lookups and every exact tiles symbol (41) against the binary.
- `PLUGIN_INIT` refuses to load when `__hyprland_api_get_client_hash()` differs
  from the running compositor's `__hyprland_api_get_hash()`.
- CMake adds only the selected `hyprland.pc` prefix's `hyprland/src`; it used to
  add `<dir>/hyprland/src` for every dependency include dir, including
  `/usr/include` (a different installed Hyprland).

## Validation

Exact headers: fork `f6725e43` (`version.h` `v0.57.0`), aquamarine `4abfcab`,
hyprutils `0d13d5a` (0.14.3), hyprland-protocols `044a434` (0.8.0), from an
isolated prefix (`pkg-config hyprland` = 0.57.0). Compiler dependency files of
every object contain no `/usr/include/hyprland`, `/usr/include/aquamarine` or
`/usr/include/hyprutils` header. The Nix lock follows the fork's own lock
(hyprutils `7e1f692`, hyprland-protocols `cd1aba6`): those only add API
(`getDataDir()`, `state/KVStateFile.hpp`; a `.gitignore`), the existing types and
`SP`/`WP` layouts are unchanged and both report `hu_0.14`.

- `-Werror` Debug build of the plugin and every target: no warning.
- CTest against the prefix: 14/14 without a compositor binary; 17/17 with
  `HYPRLAND_TEST_EXECUTABLE` set to the source-built `f6725e43` binary (native
  preload probes) and Mesa software GLES for the two GL tests. An independent
  review reproduced 17/17.
- `verify_dev_hooks.py` against a Release `f6725e43` Hyprland built from source:
  49 types, 46 lookups (42 unique targets), 41 exact tiles symbols.
- Negative checks: the old two-`bool` `renderLayer` fails the CTest ABI check;
  the old tiles `renderLayer` / `moveWorkspaceToMonitor` thunks fail to compile.
- Nix: `nix flake lock` to fork `main` `f6725e43`, then `nix build -L
  .#hymission` with a local store (`--store /home/wilf/.cache/hymission-nix`):
  17 registered tests, 15 passed (including `hymission-hook-abi-test` and the
  native preload probes against the Nix `hyprland-0.57.0+date=2026-10-28_f6725e4`),
  `hymission-snapshot-color-test` and `hymission-stage-edge-render-test`
  skipped (no EGL/GLES in the sandbox; not counted as passing). The flake now
  includes `python3`, which registers the hook ABI test there.

Not verified: any running compositor. Workspace ready/created events from the
plugin's creation paths, `replace = false` monitor lifecycle moves, overview /
Stage / tiles scenes, layer popups, snapshots and HDR on the fork, and the
GLES2 fallbacks were not run. Host plugins were never loaded, unloaded or
reloaded, `hyprpm update` was not run, nothing was installed.

## Historical initial failure (2026-10-11)

The initial check at upstream `663b29b846e4bad17cfa29941f6d2fbb6b61f139`
failed during Hymission compilation. The migration subsequently addressed:

- `managers/KeybindManager.hpp` was replaced by the `keybinds` subsystem.
- Window headers moved from `desktop/view/Window.hpp` to `desktop/view/window/Window.hpp`.
- Legacy `WORKSPACEID` is unavailable; upstream now uses typed workspace IDs and a new workspace subsystem.
- `SHyprCtlCommand` / `eHyprCtlOutputFormat` were replaced by `IPC::Socket1::SCommand` / `eOutputFormat`, with request/response handlers.
- Renderer state and passes moved from `m_renderData` / `m_renderPass` to explicit render contexts. GL scissor and render-window entry points changed signatures.

Those errors describe the historical check, not the current build result.

## Continuous checks

The branch workflow updates the upstream input before each build and uploads
resolved input metadata even when compilation fails. Push/PR checks become active
after publication. The daily schedule also requires the workflow on GitHub's
default branch. No workflow execution or remote publication occurred in this run.

The latest [Release symbol lookup follow-up](../workspace-tiles-symbol-lookup.md)
validates canonical mangled-name decoding with both normal and deliberately
mispaired API metadata in the same e5a03b6 Release compositor.
