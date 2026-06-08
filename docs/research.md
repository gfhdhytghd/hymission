# Research Notes

`hymission` 明确不是从零发明 overview 布局算法，而是基于现有开源实现做裁剪和改写。本文档记录每个上游实现借用了什么、没有借什么，以及为什么。

## 1. 参考源总表

| 项目 | 参考文件 | 借用内容 | 未借用内容 | 原因 |
| --- | --- | --- | --- | --- |
| GNOME Shell | `js/ui/workspace.js` | row-based overview layout、行数评分、small-window boost、cell 内居中 | 整套 Clutter/GObject UI 和 workspace shell 行为 | 布局算法体量适中，适合作为 `hymission` v1 核心 |
| Hyprspace | `src/Render.cpp` | compositor-side 预览思路、先算目标 box 再投影渲染 | 它自己的 workspace overview 交互模型 | 它证明了 Hyprland 上可以不改客户端尺寸来做 overview 预览 |
| KWin | `src/plugins/private/expolayout.*` | layered packing、对移动距离更强的关注 | 复杂的 strip packing 与 Qt/QML 对象模型 | 算法更强，但首版迁移成本过高，不适合先上 |

## 2. GNOME Shell

源码：

- https://github.com/GNOME/gnome-shell/blob/main/js/ui/workspace.js

### 借用点

`hymission` 当前布局引擎直接借用了以下思路：

- 先根据窗口大小推导一个稳定的窗口权重缩放
- 用总宽度估算理想 row width
- 按窗口 `centerY` 排序，尽量保持窗口原有纵向关系
- 判断“当前窗口继续留在这一行，是否让这一行更接近理想宽度”
- 尝试多个 row count，选分数更好的候选
- 在固定 cell 中居中实际 preview，并限制最大 preview scale

### 没借的部分

- GNOME 的 UI actor 树
- workspace shell 本身的状态机
- overview 中其他 shell 组件
- 依赖 Clutter 的动画和分配机制

### 原因

GNOME 的 row-based 策略已经足够接近 Mission Control 的“整齐但不死板”的感受，同时实现复杂度远低于 KWin 的 ExpoLayout。对 `hymission` 来说，它是最佳的 v1 起点。

## 3. Hyprspace

源码：

- https://github.com/KZDKM/Hyprspace/blob/main/src/Render.cpp

### 借用点

`hymission` 从 Hyprspace 借用的是实现方向，不是具体布局算法：

- 先算 preview 目标矩形，再考虑怎么画进去
- overview 预览应当由 compositor 侧完成
- 不需要让客户端为 overview 改变逻辑尺寸

### 没借的部分

- 它的 workspace overview 交互模型
- 它对 layer 的组织方式
- 它的完整渲染与输入逻辑

### 原因

Hyprspace 更像“在 Hyprland 上怎么把窗口画成 overview 预览”的路线证明，而不是 `hymission` 的最终产品模型。它提供的是方向：window preview 可以是投影，而不是重排窗口。

## 4. KWin ExpoLayout

源码：

- https://github.com/KDE/kwin/blob/master/src/plugins/private/expolayout.h
- https://github.com/KDE/kwin/blob/master/src/plugins/private/expolayout.cpp

### 借鉴点

KWin 提供了比 GNOME 更强的布局目标：

- strip / layered packing
- 更平衡的空间利用
- 更强调保留窗口原始运动趋势
- 对极端窗口宽高比分布更稳

### 当前未迁移

- layered packing 实现
- 二分 strip width 的搜索过程
- Qt/QML 对象模型

### 原因

KWin 的算法更适合作为 v2 或 v3 的升级方向。`hymission` 当前首先要完成“有闭环的 Hyprland overview 原型”，不是先引入一套更重的 packing 实现。

## 5. 为什么当前选择 GNOME 路线

当前 `hymission` 选择 GNOME 路线的原因是：

- 实现规模可控
- 足够接近 Mission Control 视觉目标
- 已经能处理大小差异较大的窗口集合
- 可以很自然地保持等比缩放
- 更容易脱离渲染链单独测试

这和当前仓库状态匹配：先把“布局引擎 + overview renderer”跑通，再考虑更复杂的 packing。

## 6. 与当前代码的映射

### 已直接落地到当前代码的来源

在 [`src/mission_layout.cpp`](../src/mission_layout.cpp) 中，已经能看到以下映射：

- `computeWindowScale(...)`
  - 对应 small-window boost 思路
- `keepSameRow(...)`
  - 对应理想 row width 判断
- `buildCandidate(...)`
  - 对应按不同行数构造布局候选
- `materializeSlots(...)`
  - 对应 cell 内 preview 的最终定位

### 尚未落地的来源

- Hyprspace 的 compositor-side render path
- KWin 的 layered packing

这两部分都只被记录为方向，还没有写进当前代码。

## 7. 对后续实现的约束

这份 research note 不是纯参考列表，它还约束后续实现：

- v1 不能再回退成固定网格 overview
- v1 不应改成客户端 resize 驱动的 preview
- v1 不要直接引入 KWin 那套完整 layered packing，除非当前 row-based 路线已被验证不足

也就是说，后续实现必须围绕“GNOME 风格布局 + Hyprspace 风格 compositor-side 预览”继续推进。
