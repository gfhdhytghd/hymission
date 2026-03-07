# AGENTS.md

## Hymission Plugin Reload

- Build output to prefer: `/home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
- `hyprctl plugin load` and `hyprctl plugin unload` both require an absolute path.
- Before loading a new build, check the live session with `hyprctl plugin list`.
- In this repo, old local copies may still be loaded from one of these paths:
  - `/home/wilf/data/hyprland_plugins/hymission/build/libhymission.so`
  - `/home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
  - `/home/wilf/data/hyprland_plugins/hymission/build-meson/libhymission.so`
- Safe reload sequence for the current machine:
  1. `hyprctl plugin unload /home/wilf/data/hyprland_plugins/hymission/build/libhymission.so`
  2. `hyprctl plugin unload /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
  3. `hyprctl plugin unload /home/wilf/data/hyprland_plugins/hymission/build-meson/libhymission.so`
  4. `hyprctl plugin load /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so`
  5. `hyprctl plugin list`
- If an unload target is not the active copy, Hyprland returns `plugin not loaded`; that is expected and safe.
- The active Hyprland config for this setup is in `~/.config/HyprV/hypr/hyprland-plugins.conf`, and it already contains the `plugin { hymission { ... } }` runtime config block.
