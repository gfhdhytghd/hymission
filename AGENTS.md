# AGENTS.md

## Hymission Plugin Reload

- Build output to prefer: `/home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
- `hyprctl plugin load` and `hyprctl plugin unload` both require an absolute path.
- Before loading a new build, check the live session with `hyprctl plugin list`.
- Preferred safe reload sequence for the current machine:
  1. `hyprpm disable hymission`
  2. `hyprctl plugin unload /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
  3. `hyprctl plugin load /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
- One-line form:
  `hyprpm disable hymission && hyprctl plugin unload /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so && hyprctl plugin load /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
- If the unload step returns `plugin not loaded`, that is expected and safe.
- The older unload-every-possible-local-copy sequence is no longer needed on this machine when `hyprpm disable hymission` is used first.
- The active Hyprland config for this setup is in `~/.config/HyprV/hypr/hyprland-plugins.conf`, and it already contains the `plugin { hymission { ... } }` runtime config block.
