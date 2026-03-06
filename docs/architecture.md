# Hymission 架构说明

本文档定义 `hymission` 的实现边界和首版技术路线，避免后续实现时在 hook、transformer、layout 之间来回摇摆。

## 1. 设计原则

- overview 是独立渲染层，不是对真实窗口状态的重排
- 布局计算必须可脱离 Hyprland 渲染链单独测试
- 输入命中必须针对 preview box，而不是直接复用真实窗口命中区域
- 首版优先做最小闭环，不同时引入 workspace 条带、拖拽、搜索等复杂行为

## 2. 当前模块

### 2.1 布局引擎

位置：

- [`src/mission_layout.hpp`](../src/mission_layout.hpp)
- [`src/mission_layout.cpp`](../src/mission_layout.cpp)

职责：

- 输入窗口自然矩形
- 输出 preview 目标矩形
- 不依赖 Hyprland renderer 内部状态

稳定接口：

- `MissionControlLayout::compute(...)`
- `Rect`
- `WindowInput`
- `WindowSlot`
- `LayoutConfig`

### 2.2 插件入口

位置：

- [`src/main.cpp`](../src/main.cpp)

当前职责：

- 注册配置项
- 注册 `hymission:debug_current_layout`
- 从当前 monitor / workspace 收集窗口自然几何
- 调用布局引擎生成 slots

### 2.3 独立 demo

位置：

- [`tools/layout_demo.cpp`](../tools/layout_demo.cpp)

职责：

- 不依赖 Hyprland runtime
- 直接验证布局引擎输出是否合理

## 3. v1 技术路线选择

### 结论

v1 首选 `renderWindow` / render hook 路线，不以 `IWindowTransformer` 作为 overview 主路径。

### 原因

`IWindowTransformer` 更适合：

- 单窗口后处理
- 对某个窗口内容做缩放、旋转或 shader 变换

但 Mission Control 风格 overview 需要：

- 同一画面里同时组织多个窗口 preview
- 统一控制 overview 开关、层级和动画
- 自己维护 preview 命中区域
- 未来扩展到 workspace 级 UI

因此，overview 首版更适合采用 render hook：

- 从 Hyprland 渲染链中读取窗口绘制时机
- 在 overview 激活时改为绘制 preview 投影
- 在 overview 未激活时完全不接管正常绘制

## 4. 目标数据流

首版 overview 数据流固定如下：

1. overview 打开
2. 选择当前光标所在 monitor
3. 收集该 monitor 的活动 workspace 中可参与 overview 的窗口
4. 为每个窗口生成自然矩形 `natural`
5. 调用布局引擎生成 `WindowSlot::target`
6. renderer 按 `target` 把窗口内容投影为 preview
7. 输入模块基于 `target` 做 hover / click / keyboard selection
8. 用户选择目标窗口或退出 overview
9. overview 关闭，恢复正常渲染路径

注意：

- overview 生命周期里，真实窗口状态不应被 overview 逻辑重写
- preview 是渲染产物，不是窗口真实 geometry

## 5. 状态模型

后续实现需要一个明确的 overview 状态对象，至少包含：

- `active`
- `ownerMonitor`
- `windows`
- `slots`
- `hoveredIndex`
- `selectedIndex`

约束：

- `slots` 只在 overview 打开期间有效
- monitor 切换或 workspace 切换时，overview 应直接关闭或重新计算，不允许悬空复用旧 slots
- overview 不应依赖“恢复旧 layout”来退出

## 6. 输入命中模型

overview 激活后，输入不能继续以真实窗口 box 为准。

必须使用 preview hit testing：

- 鼠标 hover：检查指针是否落在任一 `target` 内
- 鼠标 click：命中则激活对应窗口
- 键盘方向切换：根据 `target` 的几何关系选最近邻

这也是首版不选 transformer 作为主路径的核心原因之一：transformer 只能变内容，不能天然给出 overview 自己的命中体系。

## 7. 演进顺序

实现顺序固定如下：

1. 保持布局引擎接口稳定
2. 增加 overview 状态管理
3. 接入 render hook，绘制 preview
4. 增加 hover / click hit testing
5. 增加键盘选择和激活
6. 增加开关 dispatcher
7. 最后再做动画

以下内容必须后置：

- 多 workspace 条带
- 拖拽
- 搜索
- popup 跟随语义
- 复杂过渡动画

## 8. 兼容与边界

首版实现默认假设：

- 目标环境是当前仓库对应的 Hyprland headers
- overview 仅关心当前活动 workspace 的主窗口集合
- popup 和 subsurface 不作为独立 overview 实体

如果后续 Hyprland 内部渲染接口变化：

- 优先调整 render hook 层
- 尽量不改布局引擎接口

这保证 layout 算法仍然可复用、可独立验证。
