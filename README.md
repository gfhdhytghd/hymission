# hymission

`hymission` is a Hyprland plugin that provides a Mission Control-style overview with live compositor-side previews, scope-aware collection, trackpad gestures, and a workspace strip for active-workspace overview mode.

> [!IMPORTANT]
> This README focuses on installation, public usage, and user-facing configuration. The behavioral contract lives in [`docs/spec.md`](docs/spec.md).

> [!WARNING]
> Hyprland plugins run inside the compositor process. Install plugins only from sources you trust.
> `hymission` may not work correctly on NVIDIA GPUs/drivers.

> [!WARNING]
> This software is 99% vibe coded with OpenAI CodeX, but have been manual audited, warn in case you mind it.

**Inspired By Apple Mission Control**

**Referenced [hyprexpo](https://github.com/hyprwm/hyprland-plugins/tree/main/hyprexpo), [hycov](https://github.com/ernestoCruz05/hycov), and [Hyprspace](https://github.com/KZDKM/Hyprspace).**
## Features

- Mission Control-style overview with animated window previews
- Scope control with default config scope, `onlycurrentworkspace`, and `forceall`
- Mouse, keyboard, and trackpad-driven overview interaction
- Optional selected-preview expansion with local push-away animation
- Gesture-only `recommand` mode for two-sided `toggle` gestures
- Workspace strip when the current overview scope shows only the active workspace
- Multi-monitor support
- Pinned-window, special-workspace, and scrolling-layout aware behavior
- Workspace-to-workspace overview transitions without showing the native workspace animation in the middle



https://github.com/user-attachments/assets/d3e7625f-a831-474a-ac85-02dca635beda




## Installation

### Install with `hyprpm`

`hyprpm` is the preferred user-facing install path in the Hyprland ecosystem.

```sh
hyprpm update
hyprpm add https://github.com/gfhdhytghd/hymission
hyprpm enable hymission
hyprpm reload
```

If you use Hyprland's permission system, you may need to allow `hyprpm` in your config:

```lua
hl.permission("/usr/(bin|local/bin)/hyprpm", "plugin", "allow")
```

Do not also manually `hyprctl plugin load` the same plugin if you manage it through `hyprpm`.

### Manual build and reload

For local development, `hymission` uses CMake and outputs `build-cmake/libhymission.so`.

Requirements:

- Hyprland development headers for the exact Hyprland build you are running
- `cmake`
- `pkg-config`
- a C++23-capable compiler

`nlohmann/json` is bundled as a single header under `src/vendor/` (v3.12.0),
so no system package is required. Do not re-add `find_package(nlohmann_json)`.

Build:

```sh
cmake -DCMAKE_BUILD_TYPE=Release -B build-cmake
cmake --build build-cmake -j"$(nproc)"
ctest --test-dir build-cmake --output-on-failure
```

Unload (optional): only needed if a previous copy is already loaded, so you
start from a clean state. `plugin not loaded` is expected and harmless when a
path was not the active copy.

```sh
hyprctl plugin unload "$(pwd)/build-cmake/libhymission.so"
```

> If you previously built into a different directory, unload that path too,
> e.g. `build/` or `build-meson/`.

Load the freshly built copy and confirm it is active:

```sh
hyprctl plugin load "$(pwd)/build-cmake/libhymission.so"
hyprctl plugin list
```

Build outputs:

- Plugin: `build-cmake/libhymission.so`
- Layout demo: `build-cmake/hymission-layout-demo`
- Layout test: `build-cmake/hymission-mission-layout-test`
- Logic test: `build-cmake/hymission-overview-logic-test`

## Usage

### Dispatchers

```lua
hl.bind("SUPER + TAB", hl.plugin.hymission.toggle)
hl.bind("SUPER + SHIFT + TAB", function()
    hl.plugin.hymission.toggle("reverse")
end)
hl.bind("SUPER + CTRL + TAB", hl.plugin.hymission.close)
hl.bind("SUPER + C", function()
    hl.plugin.hymission.toggle("onlycurrentworkspace")
end)
hl.bind("SUPER + A", function()
    hl.plugin.hymission.toggle("forceall")
end)
hl.bind("SUPER + M", hl.plugin.hymission.debug_current_layout)
```

Lua configuration should normally call the native plugin functions shown below. The
colon-form names are the corresponding legacy dispatchers for non-Lua
configuration.

| Lua function | Legacy dispatcher | Arguments | Behavior | Why it exists |
| --- | --- | --- | --- | --- |
| `hl.plugin.hymission.toggle(args?)` | `hymission:toggle` | Optional scope; optional `reverse` | Opens overview while it is hidden and closes it while it is visible. In toggle switch mode, repeated calls cycle the selection instead of closing it. | Provides the normal one-key overview entry point. Its state-aware behavior also lets the same modifier-backed binding act as an Alt-Tab-style switcher. |
| `hl.plugin.hymission.open(args?)` | `hymission:open` | Optional scope | Ensures overview is open. Calling it again with the same scope is a no-op; a different scope rebuilds the visible overview for that scope. | Gives gestures, scripts, and other stateful integrations a deterministic “show overview” operation without the risk that `toggle` closes an already-visible overview. |
| `hl.plugin.hymission.close()` | `hymission:close` | None | Ensures overview is closed. Calling it while overview is already inactive or closing is a no-op. | Gives cancellation paths, scripts, and integrations a deterministic “leave overview” operation without the risk that `toggle` opens it. |
| `hl.plugin.hymission.debug_current_layout()` | `hymission:debug_current_layout` | None | Computes the default-scope layout and shows a notification containing the preview count and up to three preview rectangles, without entering overview. | Started as the layout-prototype entry point and remains a low-risk way to verify window collection and layout geometry without taking over input or rendering overview. |

#### Scope arguments

| Argument | Accepted by | Meaning |
| --- | --- | --- |
| No argument | `toggle`, `open` | Use the collection scope selected by the plugin configuration. |
| `onlycurrentworkspace` | `toggle`, `open` | Show only the current regular workspace on the anchor monitor. |
| `forceall` | `toggle`, `open` | Show all regular workspaces across participating monitors and include currently visible special workspaces. |
| `reverse` | `toggle` only | Move backward in toggle switch mode. It can be combined with one scope, for example `forceall,reverse`. Outside an active switch session it only affects the initial switch-mode selection. |

### Toggle Switch Mode

Toggle switch mode only changes `hymission:toggle`; `open`, `close`, and gesture
paths keep their normal behavior.

| Option | Type | Default | Meaning | Why it exists |
| --- | --- | --- | --- | --- |
| `toggle_switch_mode` | bool | `0` | Turns a hidden-state `toggle` into the start of a switch session. While that session is visible, later `toggle` calls cycle through the existing overview order instead of closing overview. | A normal toggle can only alternate between open and closed. This mode adds a transient Alt-Tab / Super-Tab workflow without creating a separate task-switch list or changing explicit `open` and `close`. |
| `switch_toggle_auto_next` | bool | `1` | Immediately advances one target when the first `toggle` opens a switch session. `reverse` changes that first step to the previous target. | Makes the first modifier-plus-Tab press select a different window, matching conventional task switchers; disable it when the session should initially stay on the current selection. |
| `switch_release_key` | string | `Super_L` | Commits the current selection and closes the switch session when this keysym or `code:N` key is released. Release state is checked across active keyboards and by a polling fallback. | Lets the overview remain visible only while the modifier is held, and keeps release-to-commit reliable across focus changes, multiple keyboards, and missed per-window release events. |

With a binding such as `hl.bind("SUPER + TAB", hl.plugin.hymission.toggle)` and:

```lua
hl.config({
    plugin = {
        hymission = {
            toggle_switch_mode = 1,
            switch_toggle_auto_next = 1,
            switch_release_key = "Super_L",
        },
    },
})
```

| Input | Result |
| --- | --- |
| First `SUPER+TAB` | Opens overview as a switch session and, with `switch_toggle_auto_next = 1`, selects the next target. |
| Repeated `TAB` while `SUPER` stays held | Cycles forward through the current overview order with wraparound. |
| `SUPER+SHIFT+TAB` bound to `toggle("reverse")` | Opens and cycles the switch session backward. |
| Release `SUPER` | Commits the current selection and exits overview. |

This mode is intended for modifier-backed bindings such as `ALT+TAB` and
`SUPER+TAB`.

Hymission exposes native plugin functions under `hl.plugin.hymission`:

```lua
hl.bind("SUPER + TAB", hl.plugin.hymission.toggle)
hl.bind("SUPER + A", function()
    hl.plugin.hymission.toggle("forceall")
end)
hl.bind("SUPER + S", function()
    hl.plugin.hymission.open("onlycurrentworkspace")
end)
hl.bind("SUPER + Escape", hl.plugin.hymission.close)
hl.bind("SUPER + O", function()
    hl.plugin.hymission.fullscreen({ mode = "maximized", action = "toggle" })
end)
```

Available functions:

- `hl.plugin.hymission.toggle(args?)`
- `hl.plugin.hymission.open(args?)`
- `hl.plugin.hymission.close()`
- `hl.plugin.hymission.fullscreen({ mode = "fullscreen"|"maximized", action = "toggle"|"set"|"unset" })`
- `hl.plugin.hymission.debug_current_layout()`
- `hl.plugin.hymission.dispatch(name, args?)`
- `hl.plugin.hymission.gesture(table|string, disable_inhibit?)`

`toggle` and `open` accept the optional scope arguments `forceall` and `onlycurrentworkspace`. Only `toggle` additionally accepts `reverse` as a switch-session direction modifier.

### Gestures

Register Hymission gestures through `hl.plugin.hymission.gesture(...)` instead of `hl.gesture({ action = function() ... end })` when you want continuous overview progress:

```lua
hl.plugin.hymission.gesture({
    fingers = 4,
    direction = "vertical",
    action = "toggle",
    args = "forceall",
})

hl.plugin.hymission.gesture({
    fingers = 4,
    direction = "vertical",
    action = "toggle",
    recommand = true,
})

hl.plugin.hymission.gesture({
    fingers = 4,
    direction = "vertical",
    action = "open",
    scope = "onlycurrentworkspace",
})

hl.plugin.hymission.gesture({
    fingers = 3,
    direction = "horizontal",
    action = "scroll",
    mode = "layout",
})

-- Native alternative:
-- hl.gesture({ fingers = 3, direction = "horizontal", action = "scroll_move" })

hl.plugin.hymission.gesture({
    fingers = 3,
    direction = "vertical",
    action = "workspace",
})
```

Optional gesture fields are `mods`, `scale`, and `disable_inhibit`.

Gesture notes:

- `vertical` and `horizontal` are supported for plugin-managed overview gestures; `hymission:scroll,layout` also supports `swipe`
- default gesture semantics are state-aware: hidden overview opens in the configured direction, and visible `hymission:toggle,*` overview can close in either swipe direction
- `recommand` is gesture-only and is only valid with `hymission:toggle`
- scrolling layout movement supports both `hymission:scroll,layout` and Hyprland's official `scrollMove` / Lua `scroll_move`
- workspace swipes should use `hl.plugin.hymission.gesture({ ..., action = "workspace" })`; Hymission already intercepts that path while overview is visible
- in `recommand` mode, one side opens `forceall` and the other side opens `onlycurrentworkspace`
- switching from one visible `recommand` side to the other only works in the side-changing direction; it must pass through hidden state and then cross a small transfer gap before the opposite side starts opening
- swiping the other visible `recommand` direction only exits overview back to hidden and does not continue into the opposite side
- a gesture that started from hidden can still be pulled back to cancel, but it cannot become a new visible-start close/transfer gesture until you lift and swipe again
- release still uses a `50% + velocity` commit rule

## Configuration

All user-facing settings live under `plugin.hymission` in `hl.config`.

Example:

```lua
hl.config({
    plugin = {
        hymission = {
            -- Layout: common geometry and sizing
            outer_padding_top = 92,
            outer_padding_right = 32,
            outer_padding_bottom = 32,
            outer_padding_left = 32,
            row_spacing = 32,
            column_spacing = 32,
            min_window_length = 120,
            min_preview_short_edge = 32,
            small_window_boost = 1.35,
            max_preview_scale = 0.95,
            workspace_overview_max_preview_scale = 0.95,
            min_slot_scale = 0.10,
            one_workspace_per_row = 0,

            -- Layout: engine selection and per-engine settings
            layout_engine = "grid",
            layout_engine_forceall = "",
            layout_engine_all = "",
            layout_engine_onlycurrentworkspace = "",
            layout_scale_weight = 1.0,
            layout_space_weight = 0.10,
            natural_scale_flex = 0.22,

            -- Behavior: workspace scope and transitions
            multi_workspace_sort_recent_first = 1,
            only_active_workspace = 0,
            only_active_monitor = 0,
            show_special = 0,
            workspace_change_keeps_overview = 1,

            -- Behavior: hover and selection
            selected_expand_scale = 1.18,
            hover_expand_scale = 1.18,
            overview_focus_follows_mouse = 1,
            show_focus_indicator = 0,
            grouped_windows_policy = "expanded",
            grouped_windows_collapsed_labels = 1,
            grouped_windows_collapsed_scroll = 1,

            -- Animation: hover relayout
            hover_relayout_animation = "",
            hover_relayout_duration = 140,
            hover_relayout_curve = "ease_out_cubic",

            -- Behavior: toggle switch and gestures
            toggle_switch_mode = 0,
            switch_toggle_auto_next = 1,
            switch_release_key = "Super_L",
            gesture_invert_vertical = 0,

            -- Niri mode
            niri_mode = 0,
            niri_scroll_pixels_per_delta = 1.0,
            niri_workspace_scale = 1.0,
            niri_scrolling_preview_gap = 0,

            -- Workspace strip and bar
            workspace_strip_anchor = "left",
            workspace_strip_empty_mode = "existing",
            workspace_strip_thickness = 160,
            workspace_strip_gap = 24,
            hide_bar_when_strip = 1,
            hide_hyprbars_during_overview = 0,
            bar_single_mission_control = 0,
            hide_bar_animation = 1,
            hide_bar_animation_blur = 1,
            hide_bar_animation_move_multiplier = 0.8,
            hide_bar_animation_scale_divisor = 1.1,
            hide_bar_animation_alpha_end = 0,

            -- Label picking and window controls
            pick_labels_enabled = 0,
            pick_labels_show = 1,
            pick_labels_mode = "sequential",
            pick_labels_direct_activate = 0,
            window_decoration_enabled = 1,
            close_button_enabled = 0,
            close_button_size = 18,
            close_button_inset = 0,

            -- Appearance and color customization
            backdrop_blur = 0,
            backdrop_color = "rgba(00000000)",
            focus_hover_color = "rgba(f2f7ff8c)",
            focus_selected_color = "rgba(3dc7fff2)",
            focus_hover_thickness = 2,
            focus_selected_thickness = 4,
            workspace_strip_inactive_tint_color = "rgba(00000000)",

            -- Debug
            debug_logs = 0,
            debug_surface_logs = 0,
        },
    }
})
```

### Layout

#### Common geometry and sizing

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `outer_padding` | int | `32` | Legacy fallback for all four edge paddings. |
| `outer_padding_top` | int | `32` | Top padding for the overview content area. |
| `outer_padding_right` | int | `32` | Right padding for the overview content area. |
| `outer_padding_bottom` | int | `32` | Bottom padding for the overview content area. |
| `outer_padding_left` | int | `32` | Left padding for the overview content area. |
| `row_spacing` | int | `32` | Vertical spacing between preview rows. |
| `column_spacing` | int | `32` | Horizontal spacing between preview columns. |
| `min_window_length` | int | `120` | Minimum edge length used before layout scoring. |
| `min_preview_short_edge` | int | `32` | Minimum rendered short edge for previews, used to keep ultra-wide, ultra-tall, or very small windows recognizable. |
| `small_window_boost` | float | `1.35` | Weight boost applied to smaller windows during layout. |
| `max_preview_scale` | float | `0.95` | Maximum preview scale for all-workspace / multi-workspace overview. |
| `workspace_overview_max_preview_scale` | float | `0.95` | Maximum preview scale for active-workspace overview, including niri direct overview. |
| `min_slot_scale` | float | `0.10` | Minimum allowed slot scale. |
| `one_workspace_per_row` | bool | `0` | Keep each workspace on its own row instead of searching for the best row count. |

#### Engine selection and scope overrides

An empty scope override inherits `layout_engine`.

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `layout_engine` | string | `grid` | Default geometry solver. `grid` uses row search. `natural`, `apple`, `expose`, and `mission-control` are aliases for the Apple-like natural solver. |
| `layout_engine_forceall` | string | empty | Engine override for the explicit `forceall` dispatcher scope. Falls back to `layout_engine_all`, then `layout_engine`. |
| `layout_engine_all` | string | empty | Engine override for the default all-workspace / multi-workspace scope. |
| `layout_engine_onlycurrentworkspace` | string | empty | Engine override for `onlycurrentworkspace` and default active-workspace scope. |

#### Grid / row-search engine

The natural engine can also use the row-search path as an emergency fallback, so
these weights still affect that fallback.

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `layout_scale_weight` | float | `1.0` | Weight of preview scale in the layout scoring pass. |
| `layout_space_weight` | float | `0.10` | Weight of space utilization in the layout scoring pass. |

#### Natural engine

The natural engine tries to preserve original window positions while removing
overlap. It attempts every window count before falling back to row search.

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `natural_scale_flex` | float | `0.22` | Natural-engine-only free scale range. Values are clamped to `0.0` - `0.25`; recent-first multi-workspace ordering keeps earlier windows visibly larger, while natural layouts may use larger per-window scale differences to fill sparse space. |

### Behavior

#### Workspace scope, ordering, and transitions

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `multi_workspace_sort_recent_first` | bool | `1` | Multi-workspace overview only. When enabled, `forceall` and any default overview scope that spans multiple workspaces place more recently used windows earlier in the grid, filling left-to-right then top-to-bottom. |
| `only_active_workspace` | bool | `0` | Restrict the default scope to the active regular workspace per participating monitor. |
| `only_active_monitor` | bool | `0` | Restrict the default scope to the monitor under the cursor. |
| `show_special` | bool | `0` | Include currently visible special workspaces in the default scope. |
| `workspace_change_keeps_overview` | bool | `1` | Keep overview open when switching workspaces in active-workspace scope. |

In multi-workspace overview, hover-driven real focus may still cross workspaces,
but the overview grid stays anchored instead of rebuilding on every workspace
change. In active-workspace overview, workspace changes use the dedicated
overview-to-overview transition path.

#### Hover and selection behavior

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `selected_expand_scale` | float | `1.18` | Selected-preview scale multiplier. Values are clamped to `1.0` - `2.0`; `1.0` disables selected expansion, and layout bounds may cap the visible result. |
| `hover_expand_scale` | float | `1.18` | Independently hovered-preview scale multiplier when `overview_focus_follows_mouse = 0`. Values are clamped to `1.0` - `2.0`; `1.0` disables hover expansion. It is ignored when focus follows the mouse. |
| `overview_focus_follows_mouse` | bool | `1` | Keep the overview selection aligned with hover and sync real focus when allowed. When enabled, hover uses `selected_expand_scale` through the updated selection and `hover_expand_scale` is ignored. When disabled, different selected and hovered previews may be enlarged at the same time. |
| `expand_selected_window` | bool | `0` | Deprecated compatibility key. It is accepted to avoid a config error but ignored, and an on-screen migration notice asks the user to switch to `selected_expand_scale` / `hover_expand_scale`. |
| `show_focus_indicator` | bool | `0` | Render selected and hovered preview focus chrome. |

#### Toggle switch behavior

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `toggle_switch_mode` | bool | `0` | Turn `hymission:toggle` into a toggle-only switch session. Intended for modifier-backed bindings such as `ALT+TAB` / `SUPER+TAB`. |
| `switch_toggle_auto_next` | bool | `1` | Toggle switch mode only. When enabled, the first switch-mode `toggle` both opens overview and advances to the next target. |
| `switch_release_key` | string | `Super_L` | Toggle switch mode only. Release of this key commits the current selection and closes the switch session. Supports keysym names such as `Alt_L` / `Super_L` and `code:N`, and release tracking is resilient to missing per-window release events. |

Toggle switch mode keeps the normal hover semantics: with
`overview_focus_follows_mouse = 1`, moving the pointer can retarget the final
selection committed when the modifier is released.

#### Gesture behavior

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `gesture_invert_vertical` | bool | `0` | Invert the plugin-managed vertical overview gesture direction. |

### Animations

#### Hover relayout animation

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `hover_relayout_animation` | string | empty | Hyprland animation leaf used for selected-preview hover relayout, for example `windowsMove`. When set to a valid leaf, Hyprland's animation tree controls speed and supports both bezier and spring curves. Invalid or empty values fall back to `hover_relayout_duration` / `hover_relayout_curve`. |
| `hover_relayout_duration` | float | `140` | Fallback selected-preview hover relayout duration in milliseconds. Values are clamped to `0` - `2000`; `0` completes immediately. Ignored when `hover_relayout_animation` resolves to a valid Hyprland animation leaf. |
| `hover_relayout_curve` | string | `ease_out_cubic` | Fallback selected-preview hover relayout easing curve. First tries a Hyprland registered bezier name such as `default`, `linear`, or `easeOutQuint`; otherwise supports `ease_in_cubic`, `ease_out_cubic`, and `ease_in_out_cubic`, with invalid values falling back to `ease_out_cubic`. Ignored when `hover_relayout_animation` is active. |

### Niri mode

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `niri_mode` | bool | `0` | Enable niri-like overflow behavior for the edge workspace strip. This is opt-in and does not turn the strip into the main overview content. |
| `niri_scroll_pixels_per_delta` | float | `1.0` | Multiplier for `hymission:scroll,layout` movement outside overview. A value of `1.0` maps roughly one `gestures:workspace_swipe_distance` of finger travel to one viewport of scrolling-layout movement. Native `scrollMove` ignores this option. |
| `niri_workspace_scale` | float | `1.0` | Niri mode strip thumbnail scale inside the configured strip thickness. Values are clamped to `0.05` - `1.0`; `1.0` uses the full strip cross-axis size. |
| `niri_scrolling_preview_gap` | int | `0` | Extra gap in pixels between niri direct scrolling-layout preview cells along the scrolling axis. In horizontal scrolling layouts this is the horizontal preview gap. |

With `niri_mode = 1`, the strip stays in the configured edge band and the main
overview remains the scaled window overview. The strip uses monitor-aspect
workspace thumbnails, centers the active workspace on open, and allows the
thumbnail list to overflow instead of shrinking every workspace into view.
Tiled `scrolling` layout previews use `workspace_overview_max_preview_scale` on
the non-scrolling axis and may overflow along the scrolling axis, so gesture
panning moves the centered row or column. Both `hymission:scroll,layout` and Lua
`scroll_move` can scroll the layout inside the niri overview; workspace switching
continues to use `hl.plugin.hymission.gesture({ ..., action = "workspace" })`.

### Grouped windows

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `grouped_windows_policy` | string | `expanded` | `expanded` shows every member of a Hyprland window group as a separate live preview. `collapsed` gives each group one stable overview slot bound to its current member. Unknown values fall back to `expanded`. |
| `grouped_windows_collapsed_labels` | bool | `1` | In `collapsed` mode, draw an equal-width title tab for every group member. Clicking a tab changes the current member immediately without closing overview. |
| `grouped_windows_collapsed_scroll` | bool | `1` | In `collapsed` mode, vertical scrolling over a grouped preview selects the previous or next member. The selection is committed immediately and remains after closing with `Escape`. |

Hymission suppresses Hyprland's native groupbar only while overview is visible.
In `expanded` mode, inactive group members ignore Hyprland's group-layout alpha
inside overview while retaining their normal rule and fade opacity. Dragging any
group member moves the whole group: all member previews shrink into a bounded
stack under the pointer, then return together or animate into the target
workspace thumbnail. Group order, membership, and lock state are preserved.

### Label picking

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `pick_labels_enabled` | bool | `0` | Enable direct keyboard selection in the configured `pick_labels_mode`. When labels are shown, it reuses `close_button_color` / `close_button_glyph_color` / `close_button_size` for styling; previews too small for a legible chip skip drawing it but remain selectable. |
| `pick_labels_show` | bool | `1` | Controls whether label chips are drawn. Set to `0` to keep keyboard picking active without displaying labels; `pick_labels_enabled` must still be `1`. |
| `pick_labels_mode` | string | `sequential` | `sequential` keeps the numbered `1`-`9`, `A1`-`Z9` scheme. `spatial` maps the physical ANSI alphanumeric and punctuation area to preview centers across the participating monitors. Up to 47 windows receive distinct single-key labels; denser layouts share a primary key and show a two-key route such as `FF` or `FR`. |
| `pick_labels_direct_activate` | bool | `0` | Only applies when `pick_labels_enabled = 1`. `0` only moves the selection (still requires `Return` to confirm, same as arrow keys); `1` activates and closes overview immediately when a pick label is hit. |

- In `sequential` mode, past the 9th window a letter key (`A`-`Z`) arms a ~1.5s prefix waiting for its digit (e.g. `A` then `2` picks `A2`); any other key cancels the prefix without losing its own normal effect (e.g. `Esc` still closes overview).
- In `spatial` mode, key positions and activation use physical ANSI alphanumeric and punctuation keycodes (`` ` 1-0 - = Q-P [ ] \\ A-L ; ' Z-M , . / ``), while badge text follows the active keyboard's current XKB layout automatically (using its unshifted level, so Shift/Caps Lock do not change the badge). A shared primary waits up to ~1.5s for the same key (center) or an adjacent key in the labelled direction; equivalent adjacent keys in that direction are also accepted.

### Window decorations and controls

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `window_decoration_enabled` | bool | `1` | Preserve eligible window borders and shadows on overview previews. This does not control Hymission's label chips or close buttons. |
| `close_button_enabled` | bool | `0` | Show a clickable close button on eligible window previews. |
| `close_button_size` | int | `18` | Close-button size in logical pixels. Also supplies the label-chip font-size scale. Values below `8` are clamped. |
| `close_button_inset` | int | `0` | Additional inset applied to the close-button position. |

### Appearance

#### General appearance

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `backdrop_blur` | bool | `0` | Blur the full-monitor overview backdrop. |
| `focus_hover_thickness` | float | `2` | Hover focus outline thickness. |
| `focus_selected_thickness` | float | `4` | Selected focus outline thickness. |

### Workspace strip and bar

#### Workspace strip behavior and geometry

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `workspace_strip_anchor` | string | `left` | Strip anchor. Supports `top`, `left`, and `right`. |
| `workspace_strip_empty_mode` | string | `existing` | Empty-workspace strip policy. `existing` only shows real workspaces; `continuous` inserts the next missing numbered workspace in each positive-id gap without expanding named-workspace spans. |
| `workspace_strip_thickness` | int | `160` | Strip thickness. |
| `workspace_strip_gap` | int | `24` | Gap between the strip and the main overview content. |

The workspace strip is shown when the current overview scope displays only the
active workspace. By default it only shows real workspaces plus the trailing
new-workspace card. In `continuous` mode, synthetic empty workspaces progressively
expose numbered gaps one slot at a time and render the monitor
background/wallpaper when available.

#### Bar integration

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `hide_bar_when_strip` | bool | `1` | Replace matching exclusive bars with a short self-blur / slide / scale proxy handoff while the strip is shown. |
| `hide_hyprbars_during_overview` | bool | `0` | Suppress drawing of official `hyprbars` title bars while overview renders, without changing their reserved decoration space. This is a no-op unless `hyprbars` is loaded. |
| `bar_single_mission_control` | bool | `0` | Multi-workspace overview only. Keep this at `0` to preserve the bar's normal numbered workspace display. When enabled, the bar workspace list collapses to a single `Mission Control` entry and the other regular overview workspaces are renamed to an internal hidden prefix so bars can filter them out. Intended for Waybar `ignore-workspaces`. |

#### Bar handoff animation

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `hide_bar_animation` | bool | `1` | Enable the bar handoff animation. When disabled, matching bars hide/show instantly with the strip. |
| `hide_bar_animation_blur` | bool | `1` | Enable blur during the bar handoff. When disabled, the handoff keeps alpha / move / scale only. |
| `hide_bar_animation_move_multiplier` | float | `0.8` | Multiplier for how much the bar follows strip movement. Clamped to `0.0` - `2.0`. `1.0` matches full strip travel and `2.0` doubles it. |
| `hide_bar_animation_scale_divisor` | float | `1.1` | Bar scale divisor at full strip reveal. A value of `n` means the proxy scales to `1 / n` of its original size at maximum. `1.0` disables scaling. |
| `hide_bar_animation_alpha_end` | float | `0.0` | Final bar proxy alpha when the strip is fully revealed. Clamped to `0.0` - `1.0`. `0.0` fully fades out; higher values keep part of the bar visible. |

### Color customization

Color values below use Hyprland `rgba(rrggbbaa)` syntax.

#### Overview, focus, and title

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `backdrop_color` | color | `rgba(00000000)` | Optional full-monitor overview backdrop tint. Keep transparent for blur without dimming. |
| `focus_hover_color` | color | `rgba(f2f7ff8c)` | Hover focus outline color. |
| `focus_selected_color` | color | `rgba(3dc7fff2)` | Selected focus outline color. |
| `focus_title_color` | color | `rgba(ffffffff)` | Selected window title text color. |

#### Window controls and label chips

Label chips reuse the close-button background, glyph, and size settings.

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `close_button_color` | color | `rgba(29292eeb)` | Close button idle fill and label-chip background color. |
| `close_button_hover_color` | color | `rgba(f24d47f2)` | Close button hover fill color. |
| `close_button_glyph_color` | color | `rgba(fffffffa)` | Close button glyph and label-chip text color. |

#### Workspace strip colors

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `workspace_strip_background_color` | color | `rgba(0812243d)` | Strip band background color. |
| `workspace_strip_inactive_color` | color | `rgba(0d17262e)` | Inactive workspace card fill. |
| `workspace_strip_active_color` | color | `rgba(1a2e523d)` | Active workspace card fill. |
| `workspace_strip_empty_color` | color | `rgba(0f1a292e)` | Synthetic empty workspace card fill. |
| `workspace_strip_new_color` | color | `rgba(1c293b42)` | New-workspace card fill. |
| `workspace_strip_hover_tint_color` | color | `rgba(ffffff0f)` | Tint drawn over the hovered workspace thumbnail. |
| `workspace_strip_active_tint_color` | color | `rgba(5794f21a)` | Tint drawn over the active workspace thumbnail. |
| `workspace_strip_inactive_tint_color` | color | `rgba(00000000)` | Tint drawn over inactive workspace thumbnails. Defaults to transparent. |
| `workspace_strip_plus_color` | color | `rgba(f7fbffe0)` | Plus glyph color for the new-workspace card. |

### Optional Waybar Single-Entry Setup

Leave `bar_single_mission_control = 0` if you want `hyprland/workspaces` to keep showing the usual numbered workspaces.

If you explicitly want `hyprland/workspaces` to collapse to a single `Mission Control` button while multi-workspace overview is visible:

1. Set `bar_single_mission_control = 1` in `hl.config({ plugin = { hymission = { ... } } })`.
2. Add an `ignore-workspaces` rule that hides the plugin's temporary names:

```jsonc
"hyprland/workspaces": {
  "all-outputs": true,
  "disable-scroll": true,
  "on-click": "activate",
  "persistent_workspaces": {},
  "ignore-workspaces": ["^__hymission_hidden__:"]
}
```

This keeps normal workspace names untouched outside overview. While overview is open, the anchor workspace remains `Mission Control` and the other regular overview workspaces are renamed to the hidden prefix so Waybar drops them from the module.

### Debug options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `debug_logs` | bool | `0` | Enable overview debug logging. |
| `debug_surface_logs` | bool | `0` | Enable more verbose surface-level debug logging. |

## Development

Useful commands:

```sh
./build-cmake/hymission-layout-demo
./build-cmake/hymission-layout-demo --list-scenes
./build-cmake/hymission-layout-demo --scene forceall --engine natural --output /tmp/hymission-forceall-natural.svg
./build-cmake/hymission-layout-demo --scene forceall --engine grid --output /tmp/hymission-forceall-grid.svg
./build-cmake/hymission-layout-demo --stress 5000 --seed 1 --output /tmp/hymission-stress-worst.svg
./build-cmake/hymission-mission-layout-test
./build-cmake/hymission-overview-logic-test
hyprctl dispatch hymission:debug_current_layout
```

`hymission-layout-demo` runs the geometry solver without loading the Hyprland plugin. In SVG output, dashed rectangles are source window geometry and solid rectangles are overview targets. Built-in scenes include `forceall`, `default`, `stacked`, `right-biased`, and `workspace-rows`. It also reports gravity, heatmap balance, motion, and x/y inversion metrics; SVG output draws heat cells, the screen center, and the target-area centroid. `--stress` generates random pathological scenes and writes the worst-scoring case for solver tuning.

Project docs:

- [`docs/spec.md`](docs/spec.md): behavior and user-facing semantics
- [`docs/architecture.md`](docs/architecture.md): controller, hooks, and state-machine structure
- [`docs/research.md`](docs/research.md): layout tradeoffs and prior-art notes
- [`docs/workspace_strip_plan.md`](docs/workspace_strip_plan.md): strip-specific implementation planning
- [`docs/todo.md`](docs/todo.md): current gaps and next steps
- [`devlog/`](devlog): implementation notes for recent iterations

## Notes

- The repository includes a root [`hyprpm.toml`](hyprpm.toml) manifest, which is expected by `hyprpm`.
- For inclusion in the official `hyprland-plugins` repository, Hyprland asks plugin authors to coordinate with the repository maintainer first.

## Lua Config Mode (Hyprland 0.55+)

Since Hyprland 0.55, the default configuration format is Lua. If your setup uses `configProvider: lua` (check with `hyprctl systeminfo`), follow these notes.

### Loading order

In Lua config mode, `hyprland.conf` is **not loaded**. Your entry point is `~/.config/hypr/hyprland.lua`. The `source` directive only works for `.conf` (hyprlang) files — it will silently ignore `.lua` files.

Load hymission via `require` in your `hyprland.lua`:

```lua
-- ~/.config/hypr/hyprland.lua
require("default.hypr.omarchy")  -- or your framework's defaults
require("hypr.hymission")
```

This expects `hymission.lua` at `~/.config/hypr/hymission.lua` (create the `hypr/` directory if needed).

### Plugin must be loaded before bindings

`hl.plugin.hymission` is only available **after** the plugin binary is loaded. In Lua config mode, `hl.exec_cmd` is asynchronous, so the plugin is not yet loaded when your config file first runs.

Guard your bindings to avoid a nil error:

```lua
-- hymission.lua
hl.exec_cmd("hyprctl plugin load " .. os.getenv("HOME") .. "/.local/lib/hymission.so")

if hl.plugin and hl.plugin.hymission then
  hl.bind("SUPER + TAB", hl.plugin.hymission.toggle)
  -- ... other bindings
end
```

The plugin calls `HyprlandAPI::reloadConfig()` after loading, which re-runs all Lua files. On that second pass `hl.plugin.hymission` is available and your bindings register.

For belt-and-suspenders, also load the plugin at session start:

```lua
-- In your autostart.lua
hl.on("hyprland.start", function()
  hl.exec_cmd("hyprctl plugin load ~/.local/lib/hymission.so")
end)
```

### hl.config error handling

Invalid keys inside `hl.config({ plugin = { hymission = { ... } } })` cause a runtime error. In Lua config mode this can abort the rest of the file before bindings and gestures are registered.

The full list of valid config keys is documented in [Configuration](#configuration) above. If you encounter silent failures, check `hyprctl configerrors` and verify your config block uses the correct key names.

### Persisting binding overrides across updates

If you override default Hyprland bindings (e.g. rebinding `SUPER+TAB` to `hymission:toggle`), be aware that plugin or framework updates may overwrite the original binding file. To persist your overrides:

- In Omarchy 4: the default bindings live in Lua files under `~/.local/share/omarchy/default/hypr/bindings/`. Add a `post-update` hook in `~/.config/omarchy/hooks/post-update` to re-apply your changes after `omarchy update`.
- In other setups: prefer overriding bindings in your own Lua config files (loaded after defaults) rather than editing default files directly.

### Omarchy 4 integration example

Omarchy 4 uses a pure Lua config (`configProvider: lua`). Here is a tested integration pattern:

```lua
-- ~/.config/hypr/hyprland.lua (add after other requires)
require("hypr.hymission")
```

```lua
-- ~/.config/hypr/hymission.lua
-- Load plugin (async; will trigger config reload after init)
hl.exec_cmd("hyprctl plugin load ~/.local/lib/hymission.so")

-- macOS-like config
hl.config({
  plugin = {
    hymission = {
      toggle_switch_mode = 1,
      switch_toggle_auto_next = 1,
      switch_release_key = "Super_L",
      gesture_invert_vertical = 1,
    },
  },
})

-- Bindings (only register when plugin is available)
if hl.plugin and hl.plugin.hymission then
  hl.bind("SUPER + TAB", hl.plugin.hymission.toggle, { description = "Mission Control" })
  hl.bind("SUPER + SHIFT + TAB", function()
    hl.plugin.hymission.toggle("reverse")
  end, { description = "Mission Control (reverse)" })
  hl.bind("SUPER + CTRL + TAB", hl.plugin.hymission.close, { description = "Close Mission Control" })
  hl.bind("SUPER + A", function()
    hl.plugin.hymission.toggle("forceall")
  end, { description = "Mission Control (all)" })
end

-- Gesture: 3-finger swipe up opens overview
hl.gesture({
  fingers = 3,
  direction = "up",
  action = function()
    if hl.plugin and hl.plugin.hymission then
      hl.plugin.hymission.toggle("forceall")
    end
  end,
})
```

```lua
-- ~/.config/hypr/autostart.lua
hl.on("hyprland.start", function()
  hl.exec_cmd("hyprctl plugin load ~/.local/lib/hymission.so")
end)
```

The default `SUPER+TAB` binding in Omarchy's `tiling.lua` is bound to workspace switching. To override it, you can either:

1. Comment out the binding in `~/.local/share/omarchy/default/hypr/bindings/tiling.lua` and add a `post-update` hook to re-apply:

   ```bash
   #!/bin/bash
   # ~/.config/omarchy/hooks/post-update
   TILING="$HOME/.local/share/omarchy/default/hypr/bindings/tiling.lua"
   if [ -f "$TILING" ] && ! grep -q 'omarchy-hymission-override' "$TILING" 2>/dev/null; then
     sed -i '30,32s/^/-- [omarchy-hymission-override] /' "$TILING" 2>/dev/null
   fi
   ```

2. Or accept both bindings: the default workspace-switch `SUPER+TAB` stays, and you add `SUPER+A` or another key for Mission Control.

> [!NOTE]
> `hl.unbind` does not reliably remove bindings created by Omarchy's `o.bind()` wrapper during config load. The override-hook pattern above is the most reliable approach.

## Troubleshooting

### Bindings don't register after plugin load

**Symptom**: `SUPER+TAB` or other bindings don't work despite the plugin being loaded.

**Cause**: In Lua config mode, `hl.plugin.hymission` is nil when the config first runs because `hl.exec_cmd("hyprctl plugin load ...")` is asynchronous. If your binding code is not guarded, it silently fails.

**Fix**: Guard all `hl.plugin.hymission.*` calls:

```lua
if hl.plugin and hl.plugin.hymission then
  hl.bind("SUPER + TAB", hl.plugin.hymission.toggle)
end
```

The plugin triggers `reloadConfig()` after loading, which re-runs your Lua config with the plugin available.

### Gestures don't work

**Symptom**: Trackpad gestures (swipe, pinch) have no effect.

**Check**: Verify gesture registration with `hyprctl configerrors`. Also ensure the gesture is not conflicting with another gesture using the same finger count and direction (e.g. 3-finger horizontal workspace swipe and 3-finger drag).

**Fix**: If gestures conflict, disable competing features (e.g. `drag_3fg = 0` in touchpad config to free 3-finger gestures for overview).

### Plugin loads but overview shows empty

**Symptom**: Overview opens but shows no windows.

**Check**: Run `hyprctl dispatch hymission:debug_current_layout` to see a notification with the layout count and preview rectangles.

**Fix**: Verify `show_special` and `only_active_monitor` settings match your expected scope.
