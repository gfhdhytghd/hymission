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

- `master` follows the latest official Hyprland release, `hyprwm/Hyprland` v0.57.0 = `0aa6745f705f2c4b0a5004eb019ef76e5bbdb426` (aquamarine `hyprwm` `52ea11f`).
- `hyprland-master` targets official `hyprwm/Hyprland` Git `main` (development HEAD); keep development API adaptations there. Do not substitute a stable release or the locally installed compositor as compatibility evidence.
- `my-hyprland-master` is opened only when the project's fork `gfhdhytghd/Hyprland` needs plugin changes the official tree does not; it holds the fork-specific version. Same-named fork and official tags are different commits (fork v0.57.0 is `f6725e43`) and their headers or binaries are not ABI evidence for each other; neither is `/usr/include/hyprland`.
- Omarchy branches (`omarchy-plugin`, `omarchy-marketplace`) stay Omarchy-specific; merge new `master` into them and validate against their target Hyprland version. Do not propagate branch-specific API changes to them without that validation.
- Before reporting development compatibility, update the `hyprland` flake input, build `nix build -L .#hymission`, and commit the resulting lock file with required fixes. Report the tested upstream revision and any skipped tests.
- Hooks resolve native functions by name or signature string, so a native signature change still compiles. Run `tools/verify_dev_hooks.py` against the exact Hyprland binary (CTest `hymission-hook-abi-test` covers the type half) and keep `HYMISSION_NATIVE_HOOK_ABI` assertions on thunks whose signatures changed.
- The GitHub compatibility workflow resolves fresh official `main` HEAD rather than only testing the committed lock. Daily scheduling requires the workflow to be present on the default branch as well.
- Offline build/test success is not live session acceptance. The reload rules above still apply.
