# Workspace Strip / Cross-Workspace Drag 计划

这份文档记录 `hymission` 针对“当 `only_active_workspace = 1` 时在当前 workspace overview 旁显示全 workspace 缩略图条带，并支持点击切 workspace 与拖拽窗口到其他 workspace”的可行性研究结论，以及当前冻结下来的实现计划。

## 0. 当前实现状态

截至 2026-03-07，第一版已经落地了一个可用切片：

- `only_active_workspace = 1` 时会显示 workspace strip
- 主 overview 仍只展示当前活动 workspace 的大预览
- strip 负责展示全 workspace 缩略图、切换和跨 workspace 拖拽目标
- 支持 `workspace_strip_anchor = top|left|right`
- strip click 会切换到目标 workspace，并保持 overview 打开
- strip 会合成现有 workspace、empty gap 槽位和 trailing new-workspace 槽位
- overview 内已支持本地拖拽：左键按下窗口、移动超过阈值后进入 ghost 拖拽，释放到 strip 目标时移动窗口到目标 workspace；释放到 new-workspace 槽位时会先创建 workspace
- strip 缩略图已改为基于 Hyprland `renderWorkspace(...)` 的离屏 snapshot，来自真实 workspace 渲染结果
- strip / selection chrome 的 overlay 渲染已改为在 `RENDER_POST_WINDOWS` 注入自定义 render-pass element，而不是直接在 stage callback 里 immediate OpenGL 绘制
- strip / selection chrome / backdrop 的 immediate overlay 坐标已统一转换到 monitor-local physical render space，避免 scale != 1 时出现 1/2 尺寸或偏移
- 纯逻辑层已补 `anchor` / band reservation / slot layout / strip hit-test 测试

当前仍未完成的部分：

- 兼容 Hyprland 全局 `bindm ... movewindow` 的原生 drag controller 接管
- special workspace 在 strip 内的完整展示语义
- 更细的 drop feedback 和手工交互验证

## 1. 结论

- 可行，风险中等。
- 不建议直接移植 `Hyprspace` 源码；更合理的是借它的交互模型，把 workspace strip 作为 `hymission` 现有 overview controller / render 管线上的一层新 UI。
- 当前 `hymission` 已经具备多 workspace 收集、多 monitor overview、workspace transition 和 preview hit test 的核心基础；主要缺口不是数据收集，而是 workspace 级 strip 几何、命中测试和拖拽状态机。
- `Hyprspace` 最值得借的是两类语义：
  - 把 workspace 视为独立 hit target
  - drop 时直接提交到目标 workspace
- `Hyprexpo` 最值得借的是渲染架构：
  - 把 overview overlay 挂进 Hyprland render pass，而不是在 render stage callback 里直接画
  - strip 的真实 workspace thumbnail 已经参考它的 offscreen workspace texture 合成思路落地
- `Hyprspace` 的具体实现不适合作为移植基底：
  - 当前本地 Hyprland `0.54.0` 头文件下它已出现 API 漂移
  - 它大量依赖旧私有 hook 和渲染期对象篡改

## 2. 冻结的产品语义

- 触发条件：
  - 当 overview scope 只展示活动 workspace，即 `only_active_workspace = 1` 时，显示 workspace strip
  - 当 overview scope 已经展示多个 workspace 时，不显示 strip
- strip 位置：
  - 第一版支持 `top`、`left`、`right`
  - 默认值为 `top`
  - 同一时间只显示一个 strip，不同时显示顶边和侧边
- 点击语义：
  - 点击 workspace 缩略图会切到目标 workspace
  - overview 保持打开
  - 切换后按最新可见 workspace 集合 rebuild overview
- 拖拽语义：
  - 第一版当前已实现 overview-local drag：按下窗口后移动超过阈值，overview 内部进入 ghost 拖拽
  - drop 到目标 workspace 后只移动窗口，不自动切过去
  - drop 到“新 workspace”槽位时，提交时再创建真实 workspace
  - 尚未接入 Hyprland 全局 `bindm ... movewindow` 的原生 drag controller
- strip 内容：
  - 显示当前 participating monitor 上的 normal workspace
  - 显示这些 workspace 之间的 empty gap 槽位
  - 额外显示一个 trailing new-workspace 槽位
  - special workspace 继续跟随 `show_special` 的既有语义，但第一版尚未在 strip 上完整呈现 special workspace 目标项

## 3. 架构与实现计划

### 3.1 状态模型

- 在 `OverviewController::State` 之外新增一层独立的 `WorkspaceStripEntry`
- `managedWorkspaces` 继续只表示真实参与 overview 的 workspace
- `WorkspaceStripEntry` 负责表示 strip 上的目标项，至少承载：
  - `monitorId`
  - `workspace`
  - `workspaceId`
  - `workspaceName`
  - `syntheticEmpty`
  - `isNewWorkspaceSlot`
  - `rect`
  - `windowIndexes`
  - hover / drop state
- 不把 empty / new 槽位塞进 `managedWorkspaces`

原因：

- `managedWorkspaces` 目前还承担 fullscreen backup、窗口收集、workspace name override 和切换拦截等职责
- synthetic entry 如果混进 `managedWorkspaces`，会污染现有 controller 语义

### 3.2 几何与纯逻辑

- 新增一个独立纯逻辑模块，负责：
  - strip anchor 对应的几何解算
  - strip band 预留
  - slot layout
  - strip hit test
- 当前 empty gap / trailing new-workspace 槽位合成仍在 controller 层
- 主 overview 布局改为：
  - 当 strip 启用时，先根据 strip anchor 和 thickness 从 monitor 可用区域扣出 strip band
  - 再把剩余区域交给现有 `MissionControlLayout`
- `one_workspace_per_row` 保持原有语义，不和 strip 合并

### 3.3 渲染策略

- 现有 per-window surface transform 路径继续用于主 overview preview
- strip 自己走一条 overlay 渲染路径，在 `RENDER_POST_WINDOWS` 注入自定义 render-pass element，再由 element 的 `draw()` 完成绘制：
  - strip 背景
  - active / inactive / hovered / drop-target chrome
  - workspace 标题
  - 缩略图
- strip thumbnail 当前实现为：
  - 按 strip tile 的实际显示尺寸为每个 entry 分配低分辨率 framebuffer
  - 临时切目标 monitor 的 `m_activeWorkspace`
  - 通过符号解析调用 Hyprland 私有 `CHyprRenderer::renderWorkspace(...)`
  - 把结果作为 texture 合成回 strip tile
- 这条路径提供真实 workspace 图像，而不是 controller 自己画的 window-box minimap
- `new workspace` 槽位仍保持占位 + `+` 图标，不走 snapshot
- snapshot framebuffer 尺寸按 `thumb logical size * monitor->m_scale` 分配；overlay 几何和文字尺寸也按 `m_scale` 转换，避免 logical / physical 混用导致 UI 缩成 1/2
- 不复用 `Hyprspace` 那套通过 `m_currentlyDraggedWindow` / `m_dragMode` 欺骗 renderer 的旧实现

原因：

- `hymission` 当前的 preview 管线本质上是“把正常窗口渲染重定向到唯一一个 overview slot”
- strip 如果要显示 workspace thumbnail，就必须新增主动二次绘制能力
- 当前 Hyprland 渲染链下，直接在 `RENDER_POST_WINDOWS` stage callback 里 immediate `renderRect` / `renderTexture` 可能出现“band 已预留但 overlay 不可见”；挂进 render pass 后可复用 compositor 的正常绘制路径
- 不采用 `hyprexpo` 那套 `renderWorkspace` hook + 整屏 takeover；strip 仍然只是现有 overview 上的一层局部 overlay

### 3.4 输入与拖拽

- strip 命中优先级高于 window preview
- overview 可见且 strip 可见时，左键窗口激活从“按下即激活”改成“释放时确认激活”
- 当前实现是 overview 自己维护一套轻量拖拽状态：
  - 左键按下 preview 先进入 pending pressed 状态
  - 指针移动超过阈值后切换为 overview-local drag session
  - 渲染一个 overview drag ghost 跟随指针
  - 只允许 drop 到 strip entry
- 未来如果要兼容 Hyprland 原生 `bindm ... movewindow`，再把这套逻辑切到 drag controller 接入
- drop 提交：
  - 目标是现有 workspace 时，直接 `moveWindowToWorkspaceSafe(...)`
  - 目标是 trailing new-workspace 槽位时，先 `createNewWorkspace(...)` 再移动窗口
  - 成功后 rebuild overview
  - 不自动切换到目标 workspace
- 如果释放时没有形成 drag session，且释放点仍在原窗口 preview 上，则按既有语义激活窗口并退出 overview

### 3.5 workspace 切换策略

- 现有“多 workspace overview 期间阻止外部 workspace 切换”的策略保留
- strip click 是一个受控例外
- strip click 不走当前 `only_active_workspace = 1` 的 overview-to-overview swipe 语义
- 提交路径为：
  - 切换目标 monitor 的 active workspace
  - 在原 overview 上按新状态 rebuild
  - 保持 overview 打开
- synthetic workspace 只在 click / drop 提交时真正创建

## 4. 配置与接口变更

第一版新增最小配置面：

- `workspace_strip_anchor = top|left|right`
- `workspace_strip_thickness`
- `workspace_strip_gap`

约束：

- 不新增 dispatcher
- strip 是否显示继续由 `only_active_workspace = 1` 决定
- empty / new workspace 槽位在第一版固定开启，不再额外暴露开关

## 5. 测试计划

- 纯逻辑测试：
  - `top` / `left` / `right` 三种 anchor 的 strip 几何
  - strip band reservation / slot layout
  - strip hit test
- controller 回归：
- active-workspace-only 模式下 strip click 切 workspace 但不退出
- drop 只 move window、不切 workspace
- synthetic workspace 仅在提交时创建
  - `only_active_workspace = 0` 时 strip 不出现
- 手工验证：
  - top / left / right 三种 anchor
  - 多 monitor
  - `one_workspace_per_row` 开 / 关
  - `show_special` 开 / 关
  - scrolling workspace
  - `overview_focus_follows_mouse` 开 / 关
  - overview-local drag 到 existing / new workspace
  - 全局 `bindm ... movewindow` 起手拖拽到 existing / new workspace

## 6. 非目标

第一版明确不做：

- 同时显示多个 strip
- strip 内完整复刻 layer surfaces / panels
- 为 strip 单独做 `show_empty_workspace` / `show_new_workspace` 风格配置面
- `hyprsplit` / `split-monitor-workspaces` 兼容层
- 复刻 `Hyprspace` 的旧 auto-drag 交互
