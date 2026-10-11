# Hyprland development compatibility baseline

- Branch: `hyprland-master`, based on Hymission `cc541a7`.
- Upstream: `hyprwm/Hyprland`, development branch `main` (formerly `master`).
- Tested upstream revision: `663b29b846e4bad17cfa29941f6d2fbb6b61f139`.
- Date: 2026-10-11.
- Status: **FAIL — API migration required; this branch is not yet compatible.**

## Validation

The Nix input lock was resolved successfully and the Hyprland dependency built.
The Hymission build then failed. Tests did not run because compilation failed.
No plugin was loaded or reloaded and no compositor runtime acceptance was performed.

The local machine has no writable system Nix store, so validation used:

```sh
nix --extra-experimental-features 'nix-command flakes' \
  --store /home/wilf/.cache/hymission-nix build -L .#hymission --no-link
```

## Observed incompatibilities

- `managers/KeybindManager.hpp` was replaced by the `keybinds` subsystem.
- Window headers moved from `desktop/view/Window.hpp` to `desktop/view/window/Window.hpp`.
- Legacy `WORKSPACEID` is unavailable; upstream now uses typed workspace IDs and a new workspace subsystem.
- `SHyprCtlCommand` / `eHyprCtlOutputFormat` were replaced by `IPC::Socket1::SCommand` / `eOutputFormat`, with request/response handlers.
- Renderer state and passes moved from `m_renderData` / `m_renderPass` to explicit render contexts. GL scissor and render-window entry points changed signatures.

These are the first compilation failures, not an exhaustive migration inventory.
Do not mark this baseline as compatible until the complete plugin builds and tests
pass against an updated lock. Preserve overview, Stage, gesture, workspace, and
capture behavior when migrating; compiling by removing functionality is not a
compatibility fix.

## Continuous checks

The branch workflow updates the upstream input before each build and uploads
resolved input metadata even when compilation fails. Push/PR checks become active
after publication. The daily schedule also requires the workflow on GitHub's
default branch. No workflow execution or remote publication occurred in this run.
