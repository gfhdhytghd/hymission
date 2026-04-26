# Hymission v1 规格

本文档定义 `hymission` 的目标行为。README 负责介绍仓库，本文档负责冻结产品语义。

## 1. 产品定义

`hymission` 的目标是提供一个 Mission Control 风格的窗口 overview：

- 作用范围由 dispatcher 参数和默认 scope 配置共同决定
- overview 中每个窗口显示为一个等比缩放的 preview
- preview 在视觉上尽量保留窗口原始空间关系
- overview 退出后，不改变窗口的真实位置、大小和客户端逻辑分辨率

明确不是：

- 一个临时 overview layout
- 一个要求客户端重绘新尺寸的 per-window scaling 功能
- 一个全工作区管理器或完整桌面壳层

## 2. v1 范围

v1 必做：

- scope-aware overview：支持默认配置范围、`onlycurrentworkspace` 和 `forceall`
- 打开 / 关闭 overview
- 官方 trackpad gesture 驱动的 overview 开关，视觉进度跟手且支持中途反向打断
- 当前 workspace scope 下的 workspace 条带 / 缩略图入口
- compositor-side preview 几何计算
- preview 渲染
- 鼠标点击 preview 激活对应窗口并退出 overview
- `Esc` 退出 overview
- 键盘方向导航切换选中窗口

v1 不做：

- 跨 workspace 拖拽窗口
- 完整 surface 级别的 workspace 缩略图
- 搜索过滤
- group / stack 展开
- popup 的完整 preview 语义
- 修改客户端渲染分辨率

## 3. overview 参与对象

v1 overview 纳入：

- scope 选中的普通 workspace 上
- 已映射
- 可见
- 非 desktop component
- 有有效渲染尺寸的普通窗口
- scope 参与 monitor 上可见的 pinned 浮窗，即使其 `m_workspace` 仍指向之前的 workspace
- 当 `show_special = 1` 或 `forceall` 时，scope 参与 monitor 上当前可见的 special workspace 窗口

v1 overview 排除：

- desktop components
- scope 之外的窗口
- 未映射窗口
- 不可见窗口
- popup / subsurface 的独立 overview 管理

说明：

- popup 可以在后续版本考虑跟随主窗口整体处理
- v1 文档直接把 popup 定义为未完整支持，避免实现时误扩范围
- pinned 只按“当前 monitor 上当前可见”纳入，不把其他 monitor 的 sticky 浮窗带进来

## 4. 布局语义

### 4.1 输入语义

布局引擎接收：

- 窗口自然矩形 `natural`
- 当前 overview 可用区域 `area`
- 布局参数 `LayoutConfig`

自然矩形定义：

- 坐标相对当前 monitor 的左上角
- 宽高取当前窗口实际渲染尺寸

### 4.2 输出语义

布局引擎输出 `WindowSlot`：

- `natural`: 输入窗口原始矩形
- `target`: overview 中目标预览矩形
- `scale`: 最终等比缩放系数

约束：

- preview 必须等比缩放
- 不裁切内容
- 不允许为适配布局而改变窗口逻辑宽高比
- 单个 preview 的最终 scale 不得超过 `max_preview_scale`

### 4.3 当前默认算法

v1 默认布局算法为 row-based strategy；`layout_engine = natural` 时可切换到 Apple-like 自然求解器：

- 小窗口先做最小边长钳制
- 根据窗口相对 monitor 高度决定 small-window boost
- 按窗口中心点 `y` 值排序，构造候选 row layout
- 每行内部按窗口中心点 `x` 值排序
- 评估不同 row count
- 用“可读性优先、空间利用率次之”的评分选择最佳候选

评分权重：

- `layout_scale_weight` 优先
- `layout_space_weight` 次之

这意味着 v1 默认偏向更大的 preview，而不是机械追求铺满屏幕。

Natural engine 额外目标：

- 以窗口进入 overview 前的中心点作为弱锚点
- 通过缩放二分和局部排斥消除重叠
- 尽量保持原始 left/right/top/bottom 相对方位
- 在无法满足自然排布或强制 workspace 行分组时回退到 row-based strategy

可选配置补充：

- `one_workspace_per_row = 1` 时，布局引擎不再搜索最佳 row count
- 同一 participating monitor 上，每个 workspace 的窗口固定占一行
- 行顺序按当前 overview scope 中的 workspace 顺序自上而下排列
- 默认顺序为普通 workspace 按 workspace id 升序，其后是当前 scope 纳入的 special workspace
- 每行内部仍优先跟随窗口当前自然几何的水平顺序；如果 overview 已打开且同窗口集 rebuild，则优先保留既有 preview 顺序

稳定性补充：

- 如果 overview 已经打开，且 rebuild 前后参与窗口集合与参与 monitor 集合未变化，则 preview 应优先保持既有 slot 顺序和 monitor 归属
- scrolling 或 focus-driven 几何波动可以触发 preview 尺寸更新，但不应仅因这些波动就把 preview 彼此换位

### 4.4 特殊窗口

- 单窗口：overview 不应把窗口无意义缩到很小，应尽量保持大尺寸居中显示
- 极小窗口：允许温和放大，但仍受 `max_preview_scale` 约束
- 极大窗口：缩小到 overview 中可见为止，但不裁切
- floating 窗口：仍参与 overview，按其自然矩形参与布局
- fullscreen 窗口：参与 overview，但仅作为一个普通 preview 参与布局，不保持真实 fullscreen 占位

## 5. 渲染语义

v1 采用 compositor-side preview 渲染。

必须满足：

- overview 期间不把客户端真实窗口改成 overview box
- 不向客户端发送 resize 作为主实现路径
- preview 渲染与窗口自然状态解耦

实现含义：

- 真实窗口仍由 Hyprland 正常管理
- overview 只影响 preview 的绘制和输入命中
- overview 结束后无需“恢复 layout 状态”，因为真实窗口从未被改写

## 6. 输入与交互

### 6.1 打开 / 关闭

v1 dispatcher 名称固定为：

- `hymission:toggle`
- `hymission:open`
- `hymission:close`

`hymission:toggle` 和 `hymission:open` 支持以下可选参数：

- 空参数：走默认 scope 配置
- `onlycurrentworkspace`：只展示 anchor monitor 的当前普通 workspace，不纳入 special workspace
- `forceall`：跨所有 monitor 展示所有普通 workspace，并额外纳入当前可见的 special workspace

gesture-only 参数：

- `recommand`：仅支持 `gesture = ..., dispatcher, hymission:toggle,recommand`；hidden 时正向进入 `forceall`，反向进入 `onlycurrentworkspace`

约束：

- 默认情况下，`toggle,*` 在 overview 已可见时执行关闭，参数在关闭路径上忽略
- 当 `toggle_switch_mode = 1` 且当前 overview 处于由 `hymission:toggle` 打开的 switch session 时，可见状态下再次触发 `toggle` 必须改为循环切到下一个 overview target，而不是关闭；直到 `switch_release_key` 松开才提交并退出
- 上述 switch mode 只作用于 `hymission:toggle`；`open` / `close` / gesture 路径不参与
- `open,*` 在 overview 已可见时，如果 scope 与当前不同，应直接重建到新的 scope；如果 scope 相同，则 no-op
- 未知参数必须返回 dispatcher error，不得静默回退

### 6.1.1 Trackpad Gesture

- 只接管 Hyprland 官方 gesture 语法里的 `dispatcher, hymission:toggle,...` / `dispatcher, hymission:open,...`
- 推荐写法：`gesture = 4, vertical, dispatcher, hymission:toggle,forceall`
- `recommand` 只允许出现在 `hymission:toggle` 的 gesture 配置里；dispatcher 不支持该参数
- 不支持非官方简写 `gesture = 4, vertical, hymission:toggle,forceall`
- `vertical` 和 `horizontal` 都要求具备跟手动画；`horizontal` 体感上等价于把左右映射成上下
- `up` / `down` / `left` / `right` 继续走 Hyprland 默认 dispatcher gesture 语义
- 默认语义是 state-aware：overview 关闭时按配置方向打开；`hymission:toggle,*` 在 overview 已可见时允许任意方向发起退出；`hymission:open,*` 在 overview 已可见时仍保持 no-op
- `recommand` 语义是双段式：hidden 时正向进入 `forceall`，反向进入 `onlycurrentworkspace`，且 compact side 固定为 `onlycurrentworkspace`，不受 `only_active_workspace` 默认 scope 影响
- `recommand` 在 overview 已可见时，两侧都允许任意方向先退出到 hidden
- `recommand` 从一侧切到另一侧时，只有“确实指向对侧 scope”的那个方向允许连贯两段式；必须先收回到 hidden，穿过 hidden 后还必须继续滑过一段 transfer gap，另一侧 scope 才允许开始展开
- `recommand` 在 overview 已可见时，另一个方向的滑动只能退出到 hidden，不得继续进入对侧 scope
- `recommand` 释放时如果瞬时速度与当前 scope 展开方向相反，优先回到 hidden，不得直接跨到对侧 scope
- `gesture_invert_vertical = 1` 时，上述方向对调
- 如果手势开始方向与当前状态不匹配，例如 overview 关闭时直接下滑，则整个手势应 no-op，不得先拉出半开 overview
- 手指未抬起时允许直接反向拖动，把 overview 进度拉回
- 从 hidden 起手的同一次手势只允许 opening/cancel；即使已经把 overview 拉到 fully open，也必须抬手后再发起新的 visible-start 退出或跨侧返回
- 松手采用 `50% + velocity` 提交规则；未提交时回弹到起始状态
- gesture 驱动的 close 如果会触发 scrolling focus settle 或 fullscreen restore，close 动画必须从手势释放时的当前可见进度继续，不得先跳回完全展开
- 当当前 overview scope 只展示活动 workspace，且 `workspace_change_keeps_overview = 1` 时，Hyprland 原生 `gesture = ..., workspace` 必须在 overview 内被接管为 monitor-local 的 overview-to-overview 连续滑动
- 上述 workspace gesture 必须复用当前 Hyprland 本地实现的 `workspace_swipe_distance`、`workspace_swipe_invert`、`workspace_swipe_min_speed_to_force`、`workspace_swipe_cancel_ratio`、`workspace_swipe_create_new`、`workspace_swipe_direction_lock`、`workspace_swipe_direction_lock_threshold`、`workspace_swipe_forever`、`workspace_swipe_use_r` 和 `general:gaps_workspaces`
- overview 内的 workspace gesture 中间帧不得出现原生普通 workspace 切换动画；屏幕上只能看到 source overview 与 target overview 的滑动过渡

### 6.2 鼠标

- 鼠标移动到 preview 上时，高亮该 preview
- 如果 `overview_focus_follows_mouse = 1`，鼠标移动到 preview 上时，overview 内部当前选中项也随之切换
- 左键点击 preview：激活对应窗口并退出 overview
- 点击 overview 空白区域：退出 overview；当 `overview_focus_follows_mouse = 0` 时不切换窗口，当 `overview_focus_follows_mouse = 1` 时提交当前选中的 preview
- overview 激活期间可以临时关闭真实窗口侧的 `input:follow_mouse`，避免 compositor 继续按真实窗口命中区域驱动 focus；退出 overview 后必须恢复原值
- 当 `overview_focus_follows_mouse = 1` 且 overview 打开前 `input:follow_mouse != 0` 时，鼠标 hover 到 preview 上必须实时同步真实活动窗口
- 上述实时同步在 active-workspace overview 中，如果 hover 目标位于其他 workspace，应在真实 workspace 切换后过渡到目标 workspace 的 overview
- 上述实时同步在 multi-workspace overview 中，即使 hover 引起真实 focus 跨 workspace，也不得因此整盘重建 overview、漂移 `ownerWorkspace`，或打乱既有 preview 槽位顺序；只允许更新当前选中项、真实 focus 和必要的 strip active 状态
- 对 scrolling 工作区，如果退出 overview 会改变真实 focus，则必须先等真实 layout 收敛到目标 focus 对应的位置，再开始 close 动画；close 动画不得先飞回 overview 打开前的旧几何

### 6.3 键盘

- `Esc`：退出 overview；当 `overview_focus_follows_mouse = 0` 时不改变当前活动窗口，当 `overview_focus_follows_mouse = 1` 时提交到当前选中的 preview
- `Left/Right/Up/Down`：在 preview 间移动选择
- 当 `overview_focus_follows_mouse = 1` 且 overview 打开前 `input:follow_mouse != 0` 时，方向键改变选中项也必须实时同步真实活动窗口
- 方向选择规则：按 preview box 几何关系选择对应方向的最近邻
- `Return`：激活当前选中窗口并退出 overview

`hjkl` 是否支持不作为 v1 强制项；若实现，应与方向键语义一致。

### 6.4 overview 打开期间的集合变化

- 如果 overview 打开期间有窗口关闭、打开、移动 workspace 或 monitor，且该变化会影响当前 scope，overview 应重建当前可见状态
- 重建时应尽量保留仍然存在的窗口 preview 顺序与 monitor 归属，避免因瞬时 scrolling / focus 抖动把 preview 洗牌
- 如果重建后 scope 内已经没有可参与窗口，overview 应自动退出
- 如果当前处于 overview-to-overview workspace 过渡中，窗口集变化应先取消这次过渡，再按最新窗口集重建

## 7. 多显示器与工作区语义

v1 的 overview scope 由默认配置和 dispatcher override 共同决定。

默认配置：

- `only_active_workspace = 1`：每个参与 monitor 只纳入其当前活动普通 workspace
- `only_active_workspace = 0`：每个参与 monitor 纳入其全部普通 workspace
- `only_active_monitor = 1`：只纳入光标所在 monitor
- `only_active_monitor = 0`：纳入所有 monitor
- `show_special = 1`：额外纳入参与 monitor 上当前可见的 special workspace
- `show_special = 0`：不纳入 special workspace

dispatcher override：

- `onlycurrentworkspace`：忽略默认 scope 配置，只展示 anchor monitor 的当前普通 workspace
- `forceall`：忽略默认 scope 配置，跨所有 monitor 展示所有普通 workspace，并额外纳入当前可见的 special workspace

workspace strip 补充语义：

- 当当前 overview scope 只展示活动 workspace 时，应显示 workspace strip
- strip 的进入和退出都必须与 overview 主动画共用同一视觉进度，不能只在 opening 时滑入、close 时直接消失
- strip 点击切 workspace 后 overview 应继续保持打开，并切换到新 workspace 对应的 overview
- `workspace_strip_empty_mode = existing` 时，strip 默认只显示真实存在的普通 workspace，再追加 trailing new-workspace 槽位
- `workspace_strip_empty_mode = continuous` 时，strip 对每个正整数 workspace gap 最多补一个 synthetic empty 槽位，表示“下一个可创建的编号 workspace”；named workspace 的负 id 不参与跨度展开
- synthetic empty 槽位的缩略图应优先显示目标 monitor 的背景 / 壁纸层；如果没有可用背景层，允许退回 clear-color 占位
- trailing new-workspace 槽位继续保留独立 `+` 卡片语义，不复用 empty workspace 的缩略图渲染

多 monitor 语义：

- 每个参与 monitor 都有自己的 overview backdrop 和 slot 布局区域
- preview 几何使用全局坐标，所以鼠标命中和方向导航允许跨 monitor
- 没有参与窗口的 monitor 不应进入 overview 渲染

不做：

- 把所有 monitor 的窗口压缩进单一 monitor 画布

workspace 切换补充语义：

- 如果当前 overview scope 只展示活动 workspace，且 `workspace_change_keeps_overview = 1`，则键盘切 workspace、dispatcher 切 workspace 或原生 workspace swipe 成功后，overview 应直接过渡到新 workspace 的 overview，而不是先退出或先解散再重建
- 上述过渡应只作用于触发切换的 monitor；其他 participating monitor 保持当前 overview 不动
- 上述过渡提交时必须屏蔽 Hyprland 原生普通 workspace in/out 动画，只保留 overview 自己的滑动
- 如果当前 overview scope 只展示活动 workspace，但 `workspace_change_keeps_overview = 0`，则 workspace 变化成立后，overview 应退出到正常工作区
- 如果当前 overview scope 同时展示了多个 workspace，则 overview 内必须禁止用户主动发起的 workspace 切换，包括 keyboard dispatcher 和原生 workspace swipe
- 上述限制不包含 `overview_focus_follows_mouse = 1` 带来的 hover-time 真实 focus 跟随；这类跨 workspace focus 可以发生，但不得因此重建整个 overview 或改写其布局锚点
- 多 workspace overview 期间，只有当 `bar_single_mission_control = 1` 时，bar 才应临时折叠成单个 `Mission Control` 项；退出 overview 后恢复原名
- overview 不得通过持续 `rawWindowFocus(...)` 把工作区切换强行拉回原 workspace

## 8. 当前配置面

当前已注册配置项如下：

- `outer_padding_top`
- `outer_padding_right`
- `outer_padding_bottom`
- `outer_padding_left`
- `row_spacing`
- `column_spacing`
- `min_window_length`
- `min_preview_short_edge`
- `small_window_boost`
- `max_preview_scale`
- `min_slot_scale`
- `layout_engine`
- `layout_scale_weight`
- `layout_space_weight`
- `expand_selected_window`
- `overview_focus_follows_mouse`
- `gesture_invert_vertical`
- `only_active_workspace`
- `only_active_monitor`
- `show_special`
- `workspace_change_keeps_overview`
- `one_workspace_per_row`
- `workspace_strip_anchor`
- `workspace_strip_empty_mode`
- `workspace_strip_thickness`
- `workspace_strip_gap`
- `bar_single_mission_control`

约束：

- 旧配置 `outer_padding` 允许继续作为统一回退值存在，但新的方向配置优先级更高
- `outer_padding*`、`row_spacing`、`column_spacing`、`min_window_length`、`min_preview_short_edge`、`small_window_boost`、`max_preview_scale`、`min_slot_scale`、`layout_engine`、`layout_scale_weight`、`layout_space_weight` 当前只控制布局算法
- `layout_engine = grid` 保持既有 row-search 几何；`layout_engine = natural` / `apple` / `expose` / `mission-control` 使用 Apple-like 自然求解器，优先保留窗口进入 overview 前的相对方位并消除重叠，且不按窗口数量主动回退 row-search；`one_workspace_per_row = 1` 时仍保留 workspace 行语义，并在每个 workspace band 内运行自然求解
- `expand_selected_window` 让 overview 当前选中项在布局阶段获得额外权重，从而放大并挤开相邻 preview；它依赖 `selectedIndex`，因此在 `overview_focus_follows_mouse = 1` 时通常也会跟随 hover 触发 relayout
- `overview_focus_follows_mouse` 控制 overview 内部选中项是否跟随鼠标，以及在允许时是否把当前选中项实时同步到真实 focus；当 overview 打开前 `input:follow_mouse = 0` 时，它退化为“只改 overview 内部选中项 + 退出时提交”
- `gesture_invert_vertical` 只影响被插件接管的 vertical overview gesture；它不改变普通 dispatcher、键盘输入或 Hyprland 其他 gesture 的方向
- 如果退出 overview 时提交的真实目标窗口仍不在屏内，允许临时保持该窗口为真实 focus，直到下一次真实鼠标事件；只有当目标窗口在当前 monitor 上存在可见区域时，才允许顺带移动光标去对齐真实 focus
- `only_active_workspace`、`only_active_monitor`、`show_special` 只影响默认 scope；`onlycurrentworkspace` 和 `forceall` dispatcher 参数优先级更高
- `workspace_strip_anchor`、`workspace_strip_empty_mode`、`workspace_strip_thickness` 和 `workspace_strip_gap` 只在当前 overview scope 只展示活动 workspace 时生效
- `workspace_strip_empty_mode` 当前只支持 `existing` 和 `continuous`；默认值为 `existing`
- `workspace_change_keeps_overview` 只在当前 overview scope 只展示活动 workspace 时生效；当前 scope 同时展示多个 workspace 时，workspace 切换必须被禁止
- `workspace_change_keeps_overview = 1` 时，workspace 切换的视觉语义是 overview-to-overview 过渡，而不是普通 workspace 动画 + overview 重建
- `bar_single_mission_control` 只在当前 overview scope 同时展示多个 workspace 时生效；默认建议保持 `0`，这样 bar 继续显示正常的编号 workspace；`1` 时通过临时 workspace rename 为外部 bar 提供“只保留一个 Mission Control 项”的过滤前缀，不承诺对 shell / dock 做更深的直接集成
- 除 `overview_focus_follows_mouse` 外，overview 状态机、动画、输入等配置不在 v1 第一阶段暴露
- 在没有充分稳定前，不新增大量面向最终用户的细粒度行为开关

## 9. 验收标准

以下场景视为 v1 达标：

- 当前 workspace 有 1 个窗口时，overview 能打开，preview 大尺寸居中
- 当前 workspace 有多个不同大小窗口时，preview 保持原比例且布局不退化成死板平均网格
- 鼠标点击 preview 能正确激活对应窗口并退出 overview
- `Esc` 能稳定退出 overview
- 方向键能在 preview 之间稳定移动选择
- overview 结束后窗口真实位置、尺寸、fullscreen 状态不因 overview 本身而被破坏

## 10. 非目标

以下能力明确不属于当前文档定义范围：

- 搜索应用或窗口
- 独立任务切换器式 Alt-Tab
- 将窗口拖入新 workspace
- 窗口堆叠分组的展开与聚合
- 与外部 shell、dock、waybar 的深度集成
