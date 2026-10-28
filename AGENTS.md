# AGENTS.md

## Hymission Plugin Reload

- Hyprpm uses this repository as a local source. Commit changes before updating.
- Preferred update path for the current machine:
  1. Commit the intended changes in this repo.
  2. Let the user run `hyprpm update` from a safe context. Do not run it automatically from Codex while the active Hyprland session is in use.
- Treat `hyprpm update` as a live plugin reload, not just a build step. It can unload/load the enabled plugin and may crash Hyprland if the plugin is active or currently rendering overview.
- Do not mix a hyprpm-managed hymission instance with manual `hyprctl plugin load` / `hyprctl plugin unload` for local build outputs in the same live session. Duplicate plugin instances can fight over hooks and destabilize Hyprland.
- Manual `hyprctl plugin load` / `hyprctl plugin unload` should only be used when explicitly requested for a non-hyprpm debugging session, and both commands require an absolute path.
- The active Hyprland config for this setup is in `~/.config/HyprV/hypr/hyprland-plugins.conf`, and it already contains the `plugin { hymission { ... } }` runtime config block.

## Hyprland Development Branch

- `hyprland-master` targets the project's Hyprland fork, `gfhdhytghd/Hyprland` branch `main` (baseline v0.57.0 = `f6725e43eed50eaa061aa25fe7c9f8cdc9d355e2`), which merges upstream `hyprwm/Hyprland` `main` and adds fork changes. Aquamarine comes from the fork's own lock (`gfhdhytghd/aquamarine` `master`). Same-named upstream tags are different commits (upstream v0.57.0 is `0aa6745f`) and their headers or binaries are not ABI evidence for the fork; neither is the locally installed compositor or `/usr/include/hyprland`.
- `master` follows the latest fork release and is currently the same commit as `hyprland-master`; adapt both to a new release together. Do not propagate branch-specific API changes to Omarchy branches without validating their target Hyprland versions.
- Before reporting development compatibility, update the `hyprland` flake input, build `nix build -L .#hymission`, and commit the resulting lock file with required fixes. Report the tested fork revision and any skipped tests.
- Hooks resolve native functions by name or signature string, so a native signature change still compiles. Run `tools/verify_dev_hooks.py` against the exact Hyprland binary (CTest `hymission-hook-abi-test` covers the type half) and keep `HYMISSION_NATIVE_HOOK_ABI` assertions on thunks whose signatures changed.
- The GitHub compatibility workflow resolves fresh fork `main` HEAD rather than only testing the committed lock. Daily scheduling requires the workflow to be present on the default branch as well.
- Offline build/test success is not live session acceptance. The reload rules above still apply.
