# Hymission v1 规格

本文档定义 `hymission` 的目标行为。README 负责介绍仓库，本文档负责冻结产品语义。

## 1. 产品定义

`hymission` 的目标是提供一个 Mission Control 风格的窗口 overview：

- 作用范围是当前显示器的当前工作区
- overview 中每个窗口显示为一个等比缩放的 preview
- preview 在视觉上尽量保留窗口原始空间关系
- overview 退出后，不改变窗口的真实位置、大小和客户端逻辑分辨率

明确不是：

- 一个临时 overview layout
- 一个要求客户端重绘新尺寸的 per-window scaling 功能
- 一个全工作区管理器或完整桌面壳层

## 2. v1 范围

v1 必做：

- 当前 monitor / 当前 workspace 的窗口 overview
- 打开 / 关闭 overview
- compositor-side preview 几何计算
- preview 渲染
- 鼠标点击 preview 激活对应窗口并退出 overview
- `Esc` 退出 overview
- 键盘方向导航切换选中窗口

v1 不做：

- 跨 workspace 拖拽窗口
- workspace 条带 / workspace 缩略图
- 搜索过滤
- group / stack 展开
- popup 的完整 preview 语义
- 修改客户端渲染分辨率

## 3. overview 参与对象

v1 overview 纳入：

- 当前 monitor 的活动 workspace 上
- 已映射
- 可见
- 非 desktop component
- 有有效渲染尺寸的普通窗口

v1 overview 排除：

- desktop components
- 当前 workspace 之外的窗口
- 未映射窗口
- 不可见窗口
- popup / subsurface 的独立 overview 管理

说明：

- popup 可以在后续版本考虑跟随主窗口整体处理
- v1 文档直接把 popup 定义为未完整支持，避免实现时误扩范围

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

v1 默认布局算法固定为 row-based strategy：

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

v1 预留 dispatcher 名称：

- `hymission:toggle`
- `hymission:open`
- `hymission:close`

当前仓库还未实现这些 dispatcher，但后续实现必须使用这些名称，不再另起命名。

### 6.2 鼠标

- 鼠标移动到 preview 上时，高亮该 preview
- 左键点击 preview：激活对应窗口并退出 overview
- 点击 overview 空白区域：v1 不切换窗口，直接退出 overview

### 6.3 键盘

- `Esc`：退出 overview，不改变当前活动窗口
- `Left/Right/Up/Down`：在 preview 间移动选择
- 方向选择规则：按 preview box 几何关系选择对应方向的最近邻
- `Return`：激活当前选中窗口并退出 overview

`hjkl` 是否支持不作为 v1 强制项；若实现，应与方向键语义一致。

## 7. 多显示器与工作区语义

v1 固定为：

- 只作用于当前光标所在 monitor
- 只展示该 monitor 当前活动 workspace

不做：

- 多 monitor 同时 overview
- 跨 monitor 统一 overview 画布
- workspace 条带

这些都留到 v2 之后处理。

## 8. 当前配置面

当前已注册配置项如下：

- `outer_padding`
- `row_spacing`
- `column_spacing`
- `min_window_length`
- `small_window_boost`
- `max_preview_scale`
- `min_slot_scale`
- `layout_scale_weight`
- `layout_space_weight`

约束：

- 这些配置项当前只控制布局算法
- overview 状态机、动画、输入等配置不在 v1 第一阶段暴露
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
- 任务切换器式 Alt-Tab
- 将窗口拖入新 workspace
- 窗口堆叠分组的展开与聚合
- 与外部 shell、dock、waybar 的深度集成
