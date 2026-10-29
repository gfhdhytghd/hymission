# Hyprland compatibility baseline

- `hyprland-master`: official `hyprwm/Hyprland` `main` =
  `7a5e469c780d9c54ffd62c8ef345ec55131c08d7` (`version.h` 0.57.0), aquamarine
  `hyprwm` `163b138cfb22873de1e1bf253d57bf03154d1acd`, hyprutils `7e1f692`,
  hyprland-protocols `cd1aba6`. The flake locks this revision.
- `master`: official release `v0.57.0` = `0aa6745f` (aquamarine `52ea11f`); see
  that branch's copy of this record.
- Fork: the project's `gfhdhytghd/Hyprland` v0.57.0 (`f6725e43`, aquamarine
  `4abfcab`) is a different commit with a different ABI. The same plugin source
  builds for it, so there is no `my-hyprland-master` branch; open one only when
  the fork needs plugin source changes the official tree does not.
- Date: 2026-10-29.
- Status: **PASS (compile and offline tests) on official `main` `7a5e469c` —
  exact-header build and CTest without a compositor binary; hook audit against a
  binary, Nix build and runtime checks not run for this revision.**

## API and ABI changes handled

| Change in the baseline | Plugin adaptation |
| --- | --- |
| `Event::bus()->m_events.workspace.created` split into `createdEarly` (old emit point, end of `init()`) and `createdLate` (new `ready()`) | Stage and tiles listen to `createdEarly`, keeping the previous timing; tiles still claim a new workspace for the tile focused at creation. |
| New workspaces must be marked `ready()` by their creator after setup; `CState::create()` does not | Every plugin creation goes through `createWorkspace()` / `createWorkspaceForKey()` (`src/workspace_key.hpp`), which marks it ready on the next event-loop turn through an owned `doLaterLock`; `PLUGIN_EXIT` makes pending ones ready and cancels the queued callbacks. |
| `IHyprRenderer::renderLayer(..., popups, lockscreen, workspaceScene)` | Overview and tiles thunks take and forward `workspaceScene`; the two background/bottom-layer snapshot calls pass `false`. Pre-existing mismatch: upstream `a1c2428` already had this signature. |
| `CPlacementController::moveWorkspaceToMonitor(..., noWarpCursor, carryFocus, replace)` | The tiles thunk forwards `replace`; its own old-tile replacement only runs when `replace` is true. The exact tiles symbol string was updated (it would otherwise disable tiles). |
| Legacy GLES2 renderer (`m_eglContextVersion == EGL_CONTEXT_GLES_2_0`) | Stage soft edges fall back to the hard viewport clip, the overview Gaussian blur and framebuffer blits are skipped, before any GLES3 state is touched. Not run on a GLES2 compositor. |

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

Exact headers: official `main` `7a5e469c`, aquamarine `163b138` (0.15.1),
hyprutils 0.14.3, from an isolated prefix; never `/usr/include/hyprland` (no
compiler dependency file names an `/usr/include/hyprland`, `aquamarine` or
`hyprutils` header).

- `-Werror` Debug build of the plugin and every target: no warning.
- CTest without a compositor binary: 15/15 passed, including
  `hymission-hook-abi-test` (49 hook/call types against these headers). The
  native preload probes need a matching binary and are not registered.
- Nix: `flake.lock` pins `github:hyprwm/Hyprland` `main` `7a5e469c` with
  Hyprland's own lock for its dependencies; `nix eval
  .#packages.x86_64-linux.hymission.drvPath` with a local store resolves
  `hymission-0.12.0`. `nix build` was not run.

Not verified for this revision: `verify_dev_hooks.py` name lookups and exact
tiles symbols against a `7a5e469c` binary (none was built), any running
compositor, and a live session. The official v0.57.0 release on `master` was
checked with its binary and a nested compositor.

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
