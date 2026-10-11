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

- `hyprland-master` targets current Hyprland Git development HEAD. The upstream branch is now named `main`; do not substitute a stable release or the locally installed compositor as compatibility evidence.
- Keep development API adaptations on `hyprland-master`. Do not propagate branch-specific API changes to stable `master` or Omarchy branches without validating their target Hyprland versions.
- Before reporting development compatibility, update the `hyprland` flake input, build `nix build -L .#hymission`, and commit the resulting lock file with required fixes. Report the tested upstream revision and any skipped tests.
- The GitHub compatibility workflow resolves fresh upstream HEAD rather than only testing the committed lock. Daily scheduling requires the workflow to be present on the default branch as well.
- Offline build/test success is not live session acceptance. The reload rules above still apply.
