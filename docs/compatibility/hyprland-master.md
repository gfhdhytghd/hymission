# Hyprland development compatibility baseline

- Development line: `hyprland-master`; latest validation on its feature worktree
  `feat/workspace-tiles`.
- Upstream: `hyprwm/Hyprland`, development branch `main` (formerly `master`).
- Tested upstream revision: `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`.
- Date: 2026-10-17.
- Status: **PASS — fresh upstream build; 14 tests passed, 2 GL tests skipped.**

## Validation

The Hyprland flake input was updated from upstream `main`. Both the dependency
and complete Hymission plugin built, including tiles, overview, Stage, gestures
and capture. The Nix check phase ran 16 registered tests: 14 passed;
`hymission-stage-edge-render-test` and `hymission-snapshot-color-test` returned
their EGL/GLES-unavailable skip status. They are not counted as passing tests.
The same two tests ran and passed in the default pkg-config build (16/16 total).

The local machine has no writable system Nix store, so validation used:

```sh
nix --extra-experimental-features 'nix-command flakes' \
  --store /home/wilf/.cache/hymission-nix flake update hyprland
nix --extra-experimental-features 'nix-command flakes' \
  --store /home/wilf/.cache/hymission-nix build -L .#hymission --no-link
```

The tested plugin output is
`/nix/store/hz6kanifdqkmnhdw36cblrdk68fk9sba-hymission-0.10.2` in that local
store. Final build log: `build-tiles/nix-final-build.log` (ignored artifact).
The updated lock is committed with the implementation.

No upstream HEAD runtime acceptance is claimed. A separate private nested
compositor at local revision `e5a03b6a5ce64ec4c85954649bad4df083627574` passed
the tiles integration suite, including an ASan-instrumented plugin run; see
[the validation record](../workspace-tiles-validation.md). Host plugins were
never loaded/unloaded/reloaded and `hyprpm update` was not run.

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
