# Hyprland compatibility baseline

- `hyprland-master`: official `hyprwm/Hyprland` `main` =
  `ab6141897d57e609e24c571a762c30c963e0885a` (`version.h` 0.57.0). The flake
  pins and locks this revision.
- `master`: official release `v0.57.2` = `11cbc6c5` (aquamarine `52ea11f`); see
  that branch's copy of this record.
- Fork: the project's `gfhdhytghd/Hyprland` v0.57.0 (`f6725e43`, aquamarine
  `4abfcab`) is a different commit with a different ABI. The same plugin source
  builds for it, so there is no `my-hyprland-master` branch; open one only when
  the fork needs plugin source changes the official tree does not.
- Date: 2026-11-07.
- Status: **PASS (Release build, 20/20 CTest, hook audit against the binary,
  headless smoke, Nix evaluation) on official `main` `ab614189`. No live
  session.**

## API and ABI changes handled

| Change in the baseline | Plugin adaptation |
| --- | --- |
| `Config::Actions::changeWorkspace(PHLWORKSPACE, bool noFocus)` and `changeWorkspace(const std::string&, bool noFocus)` (`f9f9157a6`, PR #16331) | Overview and tiles thunks forward `noFocus=true` directly to the original, bypassing their focus-change interception. Existing routing passes `false`. `changeWorkspaceOnCurrentMonitor` keeps its separate, unchanged type. Exact symbol strings and free-function ABI assertions use the new signatures. |
| `Event::bus()->m_events.workspace.created` split into `createdEarly` (old emit point, end of `init()`) and `createdLate` (new `ready()`) | Stage and tiles listen to `createdEarly`, keeping the previous timing; tiles still claim a new workspace for the tile focused at creation. |
| New workspaces must be marked `ready()` by their creator after setup; `CState::create()` does not | Every plugin creation goes through `createWorkspace()` / `createWorkspaceForKey()` (`src/workspace_key.hpp`), which marks it ready on the next event-loop turn through an owned `doLaterLock`; `PLUGIN_EXIT` makes pending ones ready and cancels the queued callbacks. |
| `IHyprRenderer::renderLayer(..., popups, lockscreen, workspaceScene)` | Overview and tiles thunks take and forward `workspaceScene`; the two background/bottom-layer snapshot calls pass `false`. Pre-existing mismatch: upstream `a1c2428` already had this signature. |
| `CPlacementController::moveWorkspaceToMonitor(..., noWarpCursor, carryFocus, replace)` | The tiles thunk forwards `replace`; its own old-tile replacement only runs when `replace` is true. The exact tiles symbol string was updated (it would otherwise disable tiles). |
| Legacy GLES2 renderer (`m_eglContextVersion == EGL_CONTEXT_GLES_2_0`) | Stage soft edges fall back to the hard viewport clip, the overview Gaussian blur and framebuffer blits are skipped, before any GLES3 state is touched. Not run on a GLES2 compositor. |

Guards added so the next native signature change fails at build or load time:

- `HYMISSION_NATIVE_HOOK_ABI` (`src/hook_abi.hpp`) compiles each changed thunk's
  member type against the native member (overview `renderLayer`, tiles
  `renderLayer` and `moveWorkspaceToMonitor`). The three changed free-function
  workspace thunks use equivalent `static_assert` checks against the selected
  `Config::Actions::changeWorkspace` overloads.
- CTest `hymission-hook-abi-test` runs `tools/verify_dev_hooks.py --types-only`
  (49 thunk/call types). With `--hyprland <binary>` the script also audits 46
  name lookups and every exact tiles symbol (41) against the binary.
- `PLUGIN_INIT` refuses to load when `__hyprland_api_get_client_hash()` differs
  from the running compositor's `__hyprland_api_get_hash()`.
- CMake adds only the selected `hyprland.pc` prefix's `hyprland/src`; it used to
  add `<dir>/hyprland/src` for every dependency include dir, including
  `/usr/include` (a different installed Hyprland).

## Validation

Exact headers and binary: official `main`
`ab6141897d57e609e24c571a762c30c963e0885a`, from
`/home/wilf/.local/state/hyprland-plugins-maintenance/hyprland/ab6141897d57e609e24c571a762c30c963e0885a`.
The generated hook compile flags select this prefix, not `/usr/include/hyprland`.
Commands below run from the task worktree (`$REPO`); `$PREFIX` is this prefix.

- `/home/wilf/.local/share/hyprland-plugins-maintenance/bin/plugin-build.sh --trusted "$REPO" "$PREFIX" nofocus`:
  exit 0, Release build with no warnings, `100% tests passed out of 20`,
  `== OK`. Log: `build-nofocus.log`. Includes `hymission-hook-abi-test`
  running `--types-only` and native preload tests against the exact binary.
- `python3 tools/verify_dev_hooks.py --build-dir build-nofocus --flags-file build-nofocus/hook-abi-flags.rsp --hyprland "$PREFIX/bin/Hyprland"`:
  exit 0; `PASS: 49 upstream hook/call signature checks`,
  `PASS: 46 lookups, 42 unique exported targets; each resolves exactly once`,
  `PASS: 41 exact tiles symbols resolve exactly once`.
- `/home/wilf/.local/share/hyprland-plugins-maintenance/bin/plugin-smoke.sh "$PREFIX/bin/Hyprland" "$REPO/build-nofocus/libhymission.so" hymission:toggle hymission:toggle`
  (outside the Codex sandbox; nested bubblewrap cannot create the loopback
  netlink socket): `load: ok`, two `dispatch hl.plugin.hymission.toggle: ok`,
  `unload: ok`, `SMOKE PASS`.
- Nix 2.35.4: `flake.nix` pins `github:hyprwm/Hyprland/ab6141897d57e609e24c571a762c30c963e0885a`;
  `nix flake lock` with a local store (`--store`, experimental features
  `nix-command flakes`) locked that revision with Hyprland's own inputs, and
  `nix eval .#packages.x86_64-linux.hymission.drvPath` resolved
  `hymission-0.12.1`. `nix build` was not run.
- No `.clang-format` exists, so clang-format was skipped.

Not verified for this revision: runtime focus retention with
`hl.dsp.workspace.show`, a Debug `-Werror` build, `nix build`, and a live
session.

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
