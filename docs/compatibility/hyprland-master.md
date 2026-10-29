# Hyprland compatibility baseline

- `master`: official `hyprwm/Hyprland` release `v0.57.0` =
  `0aa6745f705f2c4b0a5004eb019ef76e5bbdb426`, aquamarine `hyprwm`
  `52ea11f06dd492f34b3cc0b39a9c805beecafcff`, hyprutils `0d13d5a` (0.14.3),
  hyprland-protocols `044a434`. The flake pins this tag.
- `hyprland-master`: official `hyprwm/Hyprland` `main`; see that branch's copy of
  this record for its tested revision.
- Fork: the project's `gfhdhytghd/Hyprland` v0.57.0 (`f6725e43`, aquamarine
  `4abfcab`) is a different commit with a different ABI (for example
  `SCMSettings` differs in size). The same plugin source builds and passes the
  checks below against it too, so there is no `my-hyprland-master` branch; open
  one only when the fork needs plugin source changes the official tree does not.
- Date: 2026-10-29.
- Status: **PASS (offline) on official v0.57.0 — exact-header build, CTest with
  the matching binary, hook audit, Nix lock evaluation; nested compositor checks
  only, no live-session acceptance.**

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

Exact headers: official `0aa6745f` (`version.h` `v0.57.0`), aquamarine `52ea11f`
(0.15.1), hyprutils `0d13d5a` (0.14.3), from an isolated prefix
(`pkg-config hyprland` = 0.57.0); never `/usr/include/hyprland`.

- `-Werror` Debug build of the plugin and every target: no warning.
- CTest with `HYPRLAND_TEST_EXECUTABLE` set to the source-built `0aa6745f`
  binary (native preload probes) and Mesa software GLES for the two GL tests:
  18/18 passed, none skipped.
- `verify_dev_hooks.py` against that binary: 49 types, 46 lookups (42 unique
  targets), 41 exact tiles symbols.
- Negative checks (earlier, on the fork): the old two-`bool` `renderLayer` fails
  the CTest ABI check; the old tiles `renderLayer` / `moveWorkspaceToMonitor`
  thunks fail to compile.
- Nix: `flake.lock` pins `github:hyprwm/Hyprland/v0.57.0` with Hyprland's own
  lock for its dependencies; `nix eval .#packages.x86_64-linux.hymission.drvPath`
  with a local store resolves `hymission-0.12.0`. `nix build` was not run for
  this baseline.
- Nested official v0.57.0 compositor (private runtime directory): load, two
  `reload full-reset`s and unload with the compositor alive; after a full reset
  `hl.plugin.hymission.*` and the plugin's config values work; with overview
  open, `reload; plugin unload` in one batch leaves the compositor alive; Stage
  push, maximize fit, no-card restore and vertical scrolling give the same
  geometry as on the fork.

Not verified: a live session. Workspace ready/created events from the plugin's
creation paths, `replace = false` monitor lifecycle moves, layer popups,
snapshots and HDR, and the GLES2 fallbacks were not run. Host plugins were never
loaded, unloaded or reloaded, `hyprpm update` was not run, nothing was installed.

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
