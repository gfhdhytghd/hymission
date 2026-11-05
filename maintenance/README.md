# Plugin maintenance automation

Unattended issue/PR handling and branch synchronization for the maintainer's Hyprland plugin repositories
(`repos.json`), run on the maintainer's machine through systemd user timers (`hyprland-plugins-triage`,
`hyprland-plugins-notify`, `hyprland-plugins-sync`). They point at `~/.local/share/hyprland-plugins-maintenance`,
a symlink to this directory on the maintainer's hymission checkout.

- `run.py triage|notifications|sync [--check] [--repo NAME]` — the scheduler; `--check` only lists what would run.
- `instructions.txt` — the rules and change-tier gates every agent run receives before its task.
- `bin/hyprland-prefix.sh` — builds the exact Hyprland revision a branch targets into a cached prefix.
- `bin/plugin-build.sh` — configure, build and ctest a checkout against that prefix inside a bubblewrap sandbox.
- `bin/plugin-smoke.sh` — private headless compositor (Weston headless parent + nested Hyprland) that loads the
  plugin and runs its dispatchers; `wayland-parent-shim.c` and `weston-test-seat.c` support it.
- `bin/sandbox.sh` — the sandbox wrapper (read-only system, no network, no home, render node only).

State, logs, prefixes and per-run workspaces live outside the repository in `~/.local/state/hyprland-plugins-maintenance/`.
