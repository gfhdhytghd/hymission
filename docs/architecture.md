# Hymission 架构说明

本文档描述当前实现，而不是早期预期路线。重点是说明 `hymission` 已经如何接进 Hyprland、状态如何组织，以及哪些行为仍然故意后置。

## 1. 设计原则

- overview 是 compositor-side preview 层，不是对真实窗口 geometry 的重排
- 布局计算、命中测试和部分状态判断必须能脱离 Hyprland renderer 单独验证
- 输入命中以 preview box 为准，不能复用真实窗口命中区域
- runtime 可以临时覆盖 Hyprland 配置或 workspace 名字，但 overview 退出后必须恢复
- 多 monitor / 多 workspace 的语义放在 controller 层处理，不污染布局引擎接口

## 2. 当前模块划分

### 2.1 布局引擎

位置：

- [`src/mission_layout.hpp`](../src/mission_layout.hpp)
- [`src/mission_layout.cpp`](../src/mission_layout.cpp)

职责：

- 输入 `WindowInput::natural`
- 输出 `WindowSlot::target` 和 `WindowSlot::scale`
- 支持默认 row-based 布局和 `one_workspace_per_row`
- 不依赖 renderer hook、输入事件或 Hyprland 状态机

稳定接口：

- `MissionControlLayout::compute(...)`
- `Rect`
- `WindowInput`
- `WindowSlot`
- `LayoutConfig`

### 2.2 纯逻辑辅助

位置：

- [`src/overview_logic.hpp`](../src/overview_logic.hpp)
- [`src/overview_logic.cpp`](../src/overview_logic.cpp)

职责：

- preview hit test
- 方向键最近邻选择
- easing
- overview 期间 live focus / workspace change 的纯逻辑判定

这部分故意保持无 Hyprland 依赖，便于直接做单测。

### 2.3 Overview Controller

位置：

- [`src/overview_controller.hpp`](../src/overview_controller.hpp)
- [`src/overview_controller.cpp`](../src/overview_controller.cpp)

这是当前 runtime 的核心编排层，职责包括：

- 解析 dispatcher 参数和默认 scope 配置
- 收集 participating monitor / workspace / window
- 为每个窗口维护 natural rect、preview target、exit geometry 和 monitor 归属
- 管理 overview 打开、关闭、relayout、settle、workspace 过渡和 gesture session
- 接管 render / input / dispatcher / gesture hook
- 在 overview 期间临时覆盖 `input:follow_mouse`、`scrolling:follow_focus`、`animations:enabled`
- 在 `bar_single_mission_control = 1` 时，对多 workspace overview 临时改名为单个 `Mission Control`
- 处理 fullscreen backup / restore 以及 scrolling workspace 的退出收尾

### 2.4 插件入口

位置：

- [`src/main.cpp`](../src/main.cpp)

职责：

- 注册配置项
- 注册 dispatcher：
  - `hymission:toggle`
  - `hymission:open`
  - `hymission:close`
  - `hymission:debug_current_layout`
- 创建并持有 `OverviewController`

### 2.5 验证工具

位置：

- [`tools/layout_demo.cpp`](../tools/layout_demo.cpp)
- [`tools/overview_logic_test.cpp`](../tools/overview_logic_test.cpp)

职责：

- `hymission-layout-demo`：快速验证布局结果
- `hymission-overview-logic-test`：验证 hit test、方向导航、easing、workspace change 决策等纯逻辑

## 3. 当前 hook 面

当前实现已经不是“只做一个 debug dispatcher”，而是完整接进以下路径：

- render hook：
  - `shouldRenderWindow`
  - border / shadow draw
  - surface draw / tex box / bounding box / visible region / opaque region
  - `calculateUVForSurface`
- 输入与事件：
  - 鼠标移动
  - 鼠标左键点击
  - 键盘方向键、`Esc`、`Return`
  - window open / close / destroy / moveToWorkspace
  - workspace change
  - monitor change
- dispatcher hook：
  - `fullscreen`
  - `fullscreenstate`
  - `changeworkspace`
  - `focusWorkspaceOnCurrentMonitor`
- trackpad gesture hook：
  - 官方 `gesture = ..., dispatcher, hymission:*`
  - overview 内部对 workspace swipe 的接管与复用
  - `hymission:scroll,layout` 的连续 gesture：overview 外按 scrolling layout 方向发 `layoutMsg move`
  - niri mode 下，边缘 workspace strip 使用 active-centered overflow preview；标准 workspace gesture 仍驱动主 overview 的 workspace 过渡

结论仍然不变：`hymission` 以 render hook 为主路径，而不是 `IWindowTransformer`。

## 4. 运行时状态模型

### 4.1 主状态 `State`

`OverviewController::State` 当前至少承载这些信息：

- `phase`：`Inactive` / `Opening` / `Active` / `ClosingSettle` / `Closing`
- `ownerMonitor`、`ownerWorkspace`
- `collectionPolicy`
- `participatingMonitors`
- `managedWorkspaces`
- `windows`、`transientClosingWindows`
- `slots`
- `hoveredIndex`、`selectedIndex`
- `focusBeforeOpen`、`focusDuringOverview`、`pendingExitFocus`
- `animationProgress`、`relayoutProgress`
- `fullscreenBackups`

约束：

- `slots` 只在 overview 可见时有效
- `windows` 保存 preview 语义需要的附加信息，不能只存裸 `PHLWINDOW`
- rebuild 时优先保留仍存在窗口的 slot 顺序，减少 scrolling / live focus 抖动

### 4.2 手势与 workspace 过渡状态

为了把“overview 开关手势”和“overview 内切 workspace”拆开，当前 runtime 单独维护：

- `GestureSession`：控制 overview 自身的 opening / closing openness 和速度提交
- `WorkspaceTransition`：控制 overview-to-overview 的 source/target state、轴向、距离、动画模式
- `WorkspaceSwipeGestureContext`：记录当前是否接管了 Hyprland 原生 workspace swipe

这三层拆分的目的，是避免把 workspace 切换逻辑硬塞进普通开关动画里。

## 5. 主要数据流

### 5.1 打开 overview

1. dispatcher 或 gesture 进入 `beginOpen(...)`
2. 按默认配置和 scope override 生成 `CollectionPolicy`
3. 收集 participating monitor / workspace / window
4. 构造 `State`
5. 计算每个 monitor 上的 preview slots
6. 激活 render / input 相关 hook
7. 暂时关闭 `input:follow_mouse` 与 `scrolling:follow_focus`
8. 必要时改 workspace 名字、阻止 direct scanout
9. 开始 opening 动画

如果当前 scope 下没有可参与窗口，则只通知摘要，不进入 overview。

### 5.2 overview 期间

overview 可见期间，controller 持续做三件事：

- 把真实窗口绘制变成 preview 投影
- 用 preview box 做 hover / click / keyboard selection
- 在窗口集、workspace、monitor 变化时重建 state

补充约束：

- 若 `overview_focus_follows_mouse = 1` 且打开前 `input:follow_mouse != 0`，hover 和方向键选中会实时同步真实 focus
- 若上述实时 focus 导致切到其他 workspace，overview 不退出，而是重建到新的可见状态
- 若重建后 scope 内没有窗口，则 overview 自动退出

### 5.3 关闭 overview

关闭分两段：

1. 解析目标焦点窗口和退出几何
2. 对 scrolling / fullscreen 等需要额外收尾的情况先 settle，再做 closing 动画

关闭时仍坚持一个边界：overview 不依赖“恢复 overview 前窗口 layout”来退场，而是让真实窗口始终由 Hyprland 正常维护。

## 6. 多 monitor / 多 workspace 处理策略

当前 controller 已经支持：

- 多 participating monitor 同时 overview
- 默认 scope、`onlycurrentworkspace` 和 `forceall`
- 可选 `show_special`
- `workspace_change_keeps_overview = 1` 时的 overview-to-overview 切 workspace

策略上分两类：

- scope 只展示活动 workspace：
  - 允许在 overview 内继续切 workspace
  - 可接管原生 workspace swipe
- scope 同时展示多个 workspace：
  - 禁止 workspace 切换
  - 默认保持 bar workspace 正常编号显示；只有 `bar_single_mission_control = 1` 时才临时折叠成单个 `Mission Control`

## 7. 当前测试边界

已经纳入仓库、适合持续跑的有：

- `hymission-layout-demo`
- `hymission-overview-logic-test`
- `ctest --test-dir build --output-on-failure`

还没有自动化覆盖的主要部分：

- render hook 与 surface hook 的兼容性
- 多 monitor / scrolling / fullscreen 的真实运行时收尾
- trackpad gesture 与 workspace swipe 的组合路径

这些仍然主要依赖手工回归。

## 8. 仍然后置的内容

当前实现故意没有把这些提前做掉：

- 右键关闭窗口
- 独立任务切换器式 Alt-Tab
  当前已实现的是 `hymission:toggle` 专用的 switch mode，不包含独立任务切换器 UI / dispatcher / 排序语义
- 独立 `movefocus` dispatcher
- `forceallinone`
- workspace 条带
- 拖拽
- 搜索
- popup / subsurface 的完整 preview 语义

如果后续 Hyprland 内部渲染接口变化，优先调整 hook 层和 controller 层，尽量不改布局引擎与纯逻辑接口。
