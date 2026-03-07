# hymission

`hymission` 是一个面向 Hyprland 的 Mission Control 风格 overview 插件项目。目标不是做“临时切到另一个 layout”，而是在 compositor 侧把当前工作区窗口投影成一组可交互的预览卡片，尽量接近 macOS Mission Control 的视觉和操作方式。

当前仓库还处于原型阶段，但已经有两块可复用基础能力：

- 一个独立的窗口预览布局引擎
- 一个最小 Hyprland 插件入口，用来读取配置并调试当前布局结果

详细设计见：

- [`docs/spec.md`](docs/spec.md)
- [`docs/architecture.md`](docs/architecture.md)
- [`docs/research.md`](docs/research.md)
- [`docs/workspace_strip_plan.md`](docs/workspace_strip_plan.md)
- [`docs/todo.md`](docs/todo.md)

## 当前状态

已经实现：

- `MissionControlLayout::compute(...)`，输入窗口自然几何，输出 overview 目标预览几何
- overview 状态机与最小 opening/closing 动画
- overview preview 渲染、backdrop、hover/selected 高亮
- preview 对屏幕外窗口、Wayland/Xwayland 缩放和 pinned 浮窗的基础支持
- scope-aware overview 收集：支持按当前 workspace、按配置默认范围或 `forceall` 跨 monitor / 跨 workspace 收集
- overview 打开后的同窗口集重建会尽量保持 preview slot 顺序稳定，避免 scrolling focus 波动把 preview 洗牌
- overview 打开期间窗口集变化会重建当前场景；如果 scope 内已无可参与窗口，则自动退出 overview
- 官方 trackpad gesture 接入：可用 `dispatcher, hymission:toggle,...` 做跟手、可打断的 overview 开关
- gesture-only `recommand` 模式：同一条 `toggle` 手势可双向映射成 `forceall` / `onlycurrentworkspace`
- 鼠标命中测试、点击激活、方向键导航、`Esc` / `Return`
- workspace strip 首版：当前 overview scope 只展示活动 workspace 时显示 workspace 条带，支持 `top` / `left` / `right` 锚点
- 主 overview 仍只显示当前活动 workspace；strip 负责补全全 workspace 缩略图、切换和拖拽目标
- strip click 切换 workspace 并保持 overview 打开；支持 empty gap / trailing new-workspace 槽位
- `forceall` 下其他 workspace 窗口会从各自屏幕外 workspace 位置飞入，并在退出时飞回
- strip 打开和关闭都走 slide 进出场，而不是只在 opening 时滑入
- overview 内基础跨 workspace 拖拽：按下窗口后拖到 strip 目标可 move window，不自动切换到目标 workspace
- dispatcher：`hymission:toggle`、`hymission:open`、`hymission:close`、`hymission:debug_current_layout`
- 一组 overview / 布局配置项
- 独立 demo 程序 `hymission-layout-demo`

尚未实现：

- 与 Hyprland 原生 `bindm ... movewindow` 的 drag controller 集成
- strip 内高保真 surface / layer-surface 缩略图
- 搜索、窗口分组

README 只描述仓库现状和最小使用方式；行为定义以 [`docs/spec.md`](docs/spec.md) 为准。

## 设计目标

- 预览保持窗口原始宽高比，不裁切内容
- 不改变客户端逻辑分辨率，不向客户端发送 resize 作为 overview 主路径
- 尽量保留窗口原有的上下、左右空间关系，而不是退化成死板网格
- 把布局算法和渲染路径拆开，便于单测和替换

## 当前公开接口

### Dispatcher

当前支持以下 dispatcher：

```conf
bind = SUPER, TAB, hymission:toggle
bind = SUPER SHIFT, TAB, hymission:open
bind = SUPER CTRL, TAB, hymission:close
bind = SUPER, C, hymission:toggle,onlycurrentworkspace
bind = SUPER, A, hymission:toggle,forceall
bind = SUPER, M, hymission:debug_current_layout

gesture = 4, vertical, dispatcher, hymission:toggle,forceall
gesture = 4, vertical, dispatcher, hymission:toggle,recommand
```

- `hymission:toggle`: 打开或关闭 overview；支持可选参数 `onlycurrentworkspace` 和 `forceall`
- `hymission:open`: 打开 overview；支持可选参数 `onlycurrentworkspace` 和 `forceall`
- `hymission:close`: 关闭 overview
- `hymission:debug_current_layout`: 按默认 scope 只计算当前 preview slots，并用通知显示摘要，不进入 overview

scope 参数语义：

- 无参数：走配置驱动的默认收集范围
- `onlycurrentworkspace`：只收集光标所在 monitor 的当前普通 workspace，不纳入 special workspace
- `forceall`：跨所有 monitor 收集所有普通 workspace，并额外纳入当前可见的 special workspace 窗口

Gesture 说明：

- 只接管 Hyprland 官方 gesture 语法里的 `dispatcher, hymission:toggle,...` / `dispatcher, hymission:open,...`
- 推荐写法：`gesture = 4, vertical, dispatcher, hymission:toggle,forceall`
- `recommand` 是 gesture-only 参数，仅支持 `gesture = ..., dispatcher, hymission:toggle,recommand`
- `vertical` 和 `horizontal` 都支持；`horizontal` 体感上等价于把左右映射成上下
- `up` / `down` / `left` / `right` / 非官方简写不走插件接管
- 默认语义是 state-aware：overview 关闭时上滑打开，overview 打开时下滑关闭
- `recommand` 语义是双段式：hidden 时上滑进 `forceall`，下滑进 `onlycurrentworkspace`，且这一 compact side 不受 `only_active_workspace` 默认配置影响
- `recommand` 从一侧切到另一侧时，必须先拉回 hidden；穿过 hidden 后还需要再滑一小段 transfer gap，另一侧才会开始打开
- `recommand` 松手仍然走 `50% + velocity`；如果释放瞬间速度反向，会优先回到 hidden，不直接硬跳到对侧
- 如果手势开始方向和当前状态不匹配，例如 overview 关闭时直接下滑，则整个手势应 no-op
- 松手采用 `50% + velocity` 提交规则；手指未抬起时可以直接反向拖回
- 如果当前 overview scope 只展示活动 workspace，且 `workspace_change_keeps_overview = 1`，则 Hyprland 原生 `gesture = ..., workspace` 会在 overview 内被接管为 monitor-local 的 overview-to-overview 滑动；中间帧只显示 overview 预览，不显示原生 workspace 切换动画

`debug_current_layout` 会：

- 取光标所在显示器作为 anchor monitor
- 按默认配置收集 overview 参与窗口，以及参与 monitor 上可见的 pinned 浮窗
- 计算 Mission Control 风格 preview slots
- 用通知显示摘要结果

### 当前配置项

以下配置项已经在插件入口注册，但目前仅用于布局计算，不代表最终用户配置面已经冻结：

```conf
plugin {
    hymission {
        outer_padding_top = 48
        outer_padding_right = 48
        outer_padding_bottom = 48
        outer_padding_left = 48
        row_spacing = 32
        column_spacing = 32
        min_window_length = 120
        small_window_boost = 1.35
        max_preview_scale = 0.95
        min_slot_scale = 0.10
        layout_scale_weight = 1.0
        layout_space_weight = 0.10
        overview_focus_follows_mouse = 0
        gesture_invert_vertical = 0
        only_active_workspace = 0
        only_active_monitor = 0
        show_special = 0
        workspace_change_keeps_overview = 0
        one_workspace_per_row = 0
        workspace_strip_anchor = top
        workspace_strip_thickness = 144
        workspace_strip_gap = 24
    }
}
```

语义摘要：

- `outer_padding_top`: overview 内容到显示器上边缘的内边距
- `outer_padding_right`: overview 内容到显示器右边缘的内边距
- `outer_padding_bottom`: overview 内容到显示器下边缘的内边距
- `outer_padding_left`: overview 内容到显示器左边缘的内边距
- `row_spacing`: preview 行间距
- `column_spacing`: preview 列间距
- `min_window_length`: 小窗口参与布局前的最小边长钳制
- `small_window_boost`: 对小窗口的放大权重
- `max_preview_scale`: 预览最大缩放上限
- `min_slot_scale`: 布局最小缩放下限
- `layout_scale_weight`: 行数候选评分中对缩放大小的权重
- `layout_space_weight`: 行数候选评分中对空间利用率的权重
- `overview_focus_follows_mouse`: 是否让 overview 内部当前选中项跟随鼠标 hover；开启后，若 overview 打开前全局 `input:follow_mouse != 0`，则鼠标 hover 和键盘方向选中的 preview 都会实时同步真实窗口 focus，退出 overview 时也会提交到当前选中的 preview。对 scrolling 工作区，退出动画会先等待真实布局收敛到目标 focus，再朝该最终位置收尾
- `gesture_invert_vertical`: 反转 vertical overview 手势方向；默认是 overview 关闭时上滑打开、overview 打开时下滑关闭
- `only_active_workspace`: 默认 scope 下是否只纳入参与 monitor 的当前活动普通 workspace
- `only_active_monitor`: 默认 scope 下是否只纳入光标所在 monitor
- `show_special`: 默认 scope 下是否额外纳入参与 monitor 上当前可见的 special workspace 窗口
- `workspace_change_keeps_overview`: 当前 overview scope 只展示活动 workspace 时，是否允许切 workspace 后继续留在 overview；开启后键盘、dispatcher 和原生 `gesture = ..., workspace` 都会走 overview-to-overview 过渡，关闭时切 workspace 会直接退出 overview
- `one_workspace_per_row`: 布局解算时是否按 workspace 分行；开启后同一 monitor 上每个 workspace 占一整行，行顺序按 workspace 从上到下排列，行内仍尽量跟随真实窗口左右排布
- `workspace_strip_anchor`: 当前 overview scope 只展示活动 workspace 时 strip 的锚点；支持 `top`、`left`、`right`
- `workspace_strip_thickness`: strip 厚度
- `workspace_strip_gap`: strip 和主 overview 内容之间的间距

兼容性说明：

- 旧配置 `outer_padding` 仍然保留，作为四个方向 padding 的统一回退值
- 如果设置了 `outer_padding_top/right/bottom/left`，则对应方向会覆盖 `outer_padding`
- `onlycurrentworkspace` 和 `forceall` dispatcher 参数会覆盖默认 scope 配置

当前 runtime 还会临时接管两项现有 Hyprland 行为：

- overview 激活时会暂时关闭全局 `input:follow_mouse`，避免真实窗口命中区域和 preview 命中区域混在一起；如果 overview 打开前该值非 0，插件会在 preview hover / selection 时手动同步真实 focus，overview 关闭后恢复原值
- scrolling 工作区下会暂时关闭 `scrolling:follow_focus`，避免 layout 自己跟着真实 focus 跳动；overview 关闭后恢复原值
- 如果退出 overview 时目标窗口已经在当前显示器上有可见区域，插件会把光标挪到该可见区域中心；如果目标窗口在 scrolling 下仍然不在屏内，则会临时保持该窗口为真实 focus，直到下一次真实鼠标事件
- 多 workspace overview 下，如果 preview hover / selection 导致真实 workspace 切换，overview 会重建并继续保持打开；但原生 `changeworkspace` / `focusWorkspaceOnCurrentMonitor` / workspace swipe 的拦截规则不变
- 如果当前 overview scope 只展示活动 workspace，且 `workspace_change_keeps_overview = 1`，则键盘 / dispatcher / 原生 workspace swipe 切 workspace 后会在 overview 内直接滑到新 workspace；这一过渡会复用 Hyprland 原生 workspace swipe 的距离、反向、锁方向、速度阈值等配置，但会屏蔽原生普通窗口动画
- 如果当前 overview scope 展示了多个 workspace，则 overview 内禁止切 workspace，并把参与 monitor 上活动 workspace 的名字临时改成 `Mission Control`；退出 overview 后恢复原名
- 如果当前 overview scope 只展示活动 workspace，则会显示 workspace strip；当前 strip 缩略图是轻量 window-box minimap，不是完整 surface 重绘
- 当前跨 workspace 拖拽是 overview 内的本地拖拽实现，不依赖 Hyprland 原生 `bindm ... movewindow`

示例：开启 overview 内部选中项跟随鼠标，并在退出 overview 时提交到当前选中窗口

```conf
plugin {
    hymission {
        overview_focus_follows_mouse = 1
    }
}
```

示例：让 overview 在每个 monitor 上按 workspace 分行显示，workspace 自上而下排列

```conf
plugin {
    hymission {
        one_workspace_per_row = 1
    }
}
```

示例：在只展示当前 workspace 的 overview 下把 strip 固定到左侧

```conf
plugin {
    hymission {
        only_active_workspace = 1
        workspace_strip_anchor = left
        workspace_strip_thickness = 160
        workspace_strip_gap = 24
    }
}
```

示例：只 overview 当前活动 workspace，并允许在 overview 内继续切 workspace

```conf
plugin {
    hymission {
        only_active_workspace = 1
        workspace_change_keeps_overview = 1
    }
}
```

## 布局引擎

布局引擎定义在：

- [`src/mission_layout.hpp`](src/mission_layout.hpp)
- [`src/mission_layout.cpp`](src/mission_layout.cpp)

输入：

- 一组窗口自然矩形 `WindowInput::natural`
- 一个 overview 可用区域 `Rect area`
- 一组布局参数 `LayoutConfig`

输出：

- 每个窗口对应一个 `WindowSlot`
- `WindowSlot::target` 是 overview 中的目标预览矩形
- `WindowSlot::scale` 是该窗口最终等比缩放值

当前算法不是固定栅格，而是 row-based layout：

- 先按窗口中心点的 `y` 值大致分行
- 再在每行按 `x` 值排序
- 评估不同 row count
- 选择总分最高的候选布局

如果开启 `one_workspace_per_row = 1`：

- 布局不再搜索最佳 row count
- 每个 workspace 在其所属 monitor 上固定占一行
- 行顺序按当前 overview scope 中的 workspace 顺序自上而下排布
- 行内仍优先跟随窗口当前的真实水平顺序

具体来源和取舍见 [`docs/research.md`](docs/research.md)。

## 构建

### CMake

```sh
cmake -DCMAKE_BUILD_TYPE=Release -B build-cmake
cmake --build build-cmake -j"$(nproc)"
ctest --test-dir build-cmake --output-on-failure
```

产物：

- 插件：`build-cmake/libhymission.so`
- demo：`build-cmake/hymission-layout-demo`
- 逻辑单测：`build-cmake/hymission-overview-logic-test`

### 插件 Reload

```sh
hyprctl plugin unload /home/wilf/data/hyprland_plugins/hymission/build/libhymission.so
hyprctl plugin unload /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so
hyprctl plugin unload /home/wilf/data/hyprland_plugins/hymission/build-meson/libhymission.so
hyprctl plugin load /home/wilf/data/hyprland_plugins/hymission/build-cmake/libhymission.so
hyprctl plugin list
```

- `plugin not loaded` 是安全且预期的返回
- 当前本地开发默认应以 `build-cmake/libhymission.so` 为准

## 最小调试流程

1. 编译插件
2. 通过 `hyprctl plugin unload ...` / `hyprctl plugin load ...` reload `build-cmake/libhymission.so`
3. 绑定 `hymission:toggle` 和 `hymission:debug_current_layout`
4. 先触发 `hymission:debug_current_layout`，确认通知里能看到 preview 数量和前几个目标矩形
5. 再触发 `hymission:toggle`，确认 overview 能正常打开、关闭和选窗

额外回归建议：

- 当前 monitor 上有 pinned 浮窗时，切换 workspace 后进入 overview，确认 pinned 浮窗仍在 overview 中
- 打开 overview 后用触控板 workspace swipe 切换工作区：
  - `workspace_change_keeps_overview = 1` 且当前 scope 只展示活动 workspace 时，确认 overview 会直接滑到相邻 workspace，且过程中不出现原生普通窗口动画
  - 其他 scope 下，确认 workspace 切换仍按既有规则被阻止或退出 overview
- `overview_focus_follows_mouse = 1` 且 `input:follow_mouse != 0` 时，确认鼠标 hover 和方向键选中的 preview 都会实时同步真实 focus；hover 到其他 workspace 的 preview 时 overview 不会退出

如果只想验证布局算法而不启动 Hyprland 插件，可以直接运行：

```sh
./build-cmake/hymission-layout-demo
./build-cmake/hymission-overview-logic-test
```

## 近期路线

当前主线路已经从“把 overview 跑起来”转到“补交互缺口和高级行为”：

1. overview 右键关闭窗口等直接操作
2. Alt-release / Alt-Tab 风格的按住主键进入、松开退出模式
3. 独立 `movefocus` dispatcher 和跨显示器 focus 切换
4. `forceallinone`、workspace 条带、拖拽、搜索等扩展能力

原因见 [`docs/architecture.md`](docs/architecture.md)。
