# 工作区分块：可行性与设计评审稿

状态：**第一阶段设计，等待 Claude 审阅；尚未实现。**

日期：2026-10-17。范围仅为此文档和本地提交；不修改 Hyprland 源码、用户配置或运行中的插件，不 push，不运行 `hyprpm update`，不 load/unload。

## 1. 结论与证据边界

推荐语义是：**同一物理 monitor 上同时呈现多个真实 workspace，每个 workspace 有独立 viewport；键盘焦点对应的 tile 决定普通工作区切换的目标区域。** 窗口保留真实 workspace 身份、原生 layout 和客户端 surface。分块不是 workspace 截图，也不是把多个 workspace 的窗口迁入一个 workspace。

推荐工程路线为 **Hyprland 核心增加 workspace presentation/viewport 支持，Hymission 提供分块管理、dispatcher、拖缝及 overview/Stage 集成**。目前源码有可复用的 workspace 布局空间、workspace 场景选择和原生 surface 绘制，但没有完整的多 viewport 切换事务。纯插件可以做受限原型；本次调研没有证明它能无遗漏满足“所有切换入口、其他分块不变”，不能把原型作为完整交付。

这意味着下一阶段若采纳推荐路线，需将小范围核心扩展纳入实施范围，并确定对应 compositor 的交付方式。这里的“小范围”指职责集中，不代表仅改一两个函数。若要求始终只支持未打补丁的上游 Hyprland，则必须单独评审纯插件适配成本，不能默默缩减需求。本阶段不开始任一路线的实现。

### 1.1 本次实际检查的版本

| 对象 | 本次检查结果 | 能证明什么 |
| --- | --- | --- |
| Hymission 工作树 | `feat/workspace-tiles`，起点 `552bd339fe4809e0a031d277ca97f4dac6d0a5da`；开始时干净 | 本设计针对当前开发分支的实际代码 |
| `~/data/Hyprland` | `e5a03b6a5ce64ec4c85954649bad4df083627574`；本地 `main` 相对 `origin/main` ahead 77 / behind 6；存在未跟踪目录 | 用户指定本地源码，包含本地修改，不能直接称为上游 HEAD |
| 上游 `main` | `git ls-remote` 当场返回 `25f09b26f9db7dbca4a4b3c265fdb96802db122c`；与本地 `origin/main`、本仓库现有 `flake.lock` 一致 | 关键路径另用 `git show origin/main:<path>` 核对，未 checkout/fetch/修改源树 |
| 构建、运行时、视觉 | 本阶段均未运行 | **没有新增开发兼容性、HDR 视觉、输入或真机验收结论** |

下文 Hyprland 行号以 `25f09b26…` 为准；本地树路径相同但行号可能不同。尤其 `Monitor.cpp` 和 `Renderer.cpp` 存在本地差异，不能将本地 CM 行为当作上游保证。`docs/compatibility/hyprland-master.md` 是历史记录，不据此推断当前构建结果。

后续报告开发兼容性前，仍须刷新 `hyprland` flake input、运行 `nix build -L .#hymission`（必要时使用仓库已有的本地 store 方式）、提交 lock 与必要修复，并记录解析出的上游 revision 和跳过的测试。这次仅提交设计文档，不刷新 lock 或为文档变更启动完整构建。

## 2. 源码发现：为什么不能只多画几个工作区

以下均为实际源码路径/符号；拟增加的接口在后文明确标为提案。

| 路径与入口（Hyprland 上游 revision 同上） | 观察 | 对设计的约束 |
| --- | --- | --- |
| `src/output/Monitor.hpp:76`；`Monitor.cpp:1352`，`CMonitor::changeWorkspace` | monitor 只有一个 `m_activeWorkspace`；切换先隐藏旧 workspace，再显示新 workspace；同时处理动画、pinned、焦点、layout、IPC、VRR、suspended states | 不能调用原函数后再 `setVisible(true)` 就认为完成；同步事件可能已看到中间状态 |
| `src/workspace/HLWorkspace.cpp:66,160`，`visible` / `isVisibleNotCovered` | `visible` 是可写布尔值；后者仍按唯一 active workspace 和 special 判断 | 需要统一“可见”“未被覆盖”“焦点所属”，而非长期滥用 `m_forceRendering` |
| `src/layout/space/Space.cpp:77`，`CSpace::recheckWorkArea` | 每个 workspace 有 `CSpace`，但基础区域取 `monitor->logicalBoxMinusReserved()`，再应用 workspace/global gaps | 可保留原生 layout 算法，需按 workspace 提供独立区域，不能修改全局 monitor reserved area |
| `src/render/scene/MonitorScene.cpp:13`；`Renderer.cpp:1165,2706,2721` | 默认场景仍以 monitor active workspace 开始；已有 `WORKSPACE_WINDOWS` / `WORKSPACE_WITH_SHELL` 选择；geometry 重载是等比例缩放，比例不符会放弃 geometry | scene 选择可复用，但给窄 tile 直接传 `renderWorkspace(..., geometry)` 不会产生正确的窄布局 |
| `src/render/Renderer.cpp:233,297`，`shouldRenderWindow` 两个重载 | 可见性、动画、fullscreen、跨 monitor 浮窗共同参与筛选 | 不能只 hook 一个重载；必须保留 fullscreen、浮窗和动画过滤 |
| `src/desktop/state/ViewHitTester.cpp:33,146` | 有 `windowAtWorkspace`；普通 tiled/fullscreen hit test 取 monitor 唯一 active workspace，浮窗则可遍历 visible workspaces | 可先按 tile 选 workspace 再走原生命中；仅过滤原函数的一个返回值会漏掉被别区窗口挡住的正确候选 |
| `src/desktop/state/FocusState.cpp:98,170`，`rawWindowFocus` | 只有目标 workspace 不可见时才调用 `changeWorkspace`；可见窗口的焦点切换可能完全绕过它 | 点击另一可见 tile、focuswindow、urgent 激活等必须同步 tile 焦点及兼容 active 指针 |
| `src/config/shared/actions/ConfigActions.cpp:1020,1088,1124`；`config/lua/objects/LuaMonitor.cpp` | string/typed actions、`hl.dsp.focus`、monitor 对象均有入口；普通 action 会先聚焦目标 workspace 的 monitor | 低层 monitor hook 太晚，可能已把操作路由到另一块屏幕 |
| `src/state/workspace/PlacementController.cpp:153,241` | swap 和 move 某些分支直接写 `m_activeWorkspace`（203、204、346 行） | `changeWorkspace` 不是唯一写入点；迁移、交换、断屏都要事务化 |
| `src/state/workspace/Resolver.cpp:126`；`desktop/history/WorkspaceHistoryTracker.cpp` | 相对/previous selector 依赖 monitor active 和 history | 每 tile 需自己的解析上下文和历史，不能串用其他 tile 的 previous |
| `src/managers/input/UnifiedWorkspaceSwipeGesture.cpp:174,214,358` | begin 绑定 active workspace；位移按 monitor 宽高；update/end 使用 resolver；session 校验依赖 active 指针 | swipe 不能仅在松手时替换 dispatcher，预览、距离、取消及连续分段都要适配 |
| `src/managers/fullscreen/handler/FullscreenHandler.cpp:207,237` | fullscreen 取 `monitor->logicalBox()`；maximize 取 `space()->workArea()`；设置与同步各有路径 | 缩小 layout work area 不会自动缩小真正 fullscreen |
| `src/output/Monitor.cpp:207`；`render/Renderer.cpp:2404,2733` | 帧事件路径专门处理 active / special workspace；存在未实际渲染时的通知路径 | 多 tile 必须检查 frame callbacks、presentation feedback、damage、suspension，避免第二块画面停止更新 |
| `src/ipc/s1/Commands.cpp:604`；`protocols/ExtWorkspace.cpp:169` | IPC activeworkspace 和 ext-workspace active 位都从单一 active 指针派生 | 对旧 bar 保留单焦点语义；额外提供可见 tile 拓扑 |

可复现的固定版本源入口：[Monitor 切换](https://github.com/hyprwm/Hyprland/blob/25f09b26f9db7dbca4a4b3c265fdb96802db122c/src/output/Monitor.cpp#L1352)、[布局区域](https://github.com/hyprwm/Hyprland/blob/25f09b26f9db7dbca4a4b3c265fdb96802db122c/src/layout/space/Space.cpp#L77)、[workspace renderer](https://github.com/hyprwm/Hyprland/blob/25f09b26f9db7dbca4a4b3c265fdb96802db122c/src/render/Renderer.cpp#L2706)、[swipe](https://github.com/hyprwm/Hyprland/blob/25f09b26f9db7dbca4a4b3c265fdb96802db122c/src/managers/input/UnifiedWorkspaceSwipeGesture.cpp#L174)。

### 2.1 Hymission 已有复用点和冲突点

- `src/main.cpp` 已注册 `hl.plugin.hymission.stage_open` 等立即执行函数，旧配置模式另注册 `hymission:*` dispatcher；`luaDispatchResult` 把错误转为 Lua error。新接口沿用这种风格，不伪装成已存在的 `hl.dsp` 核心函数。
- `src/action_hooks.hpp::install` 已精确匹配 string workspace actions 和 fullscreen actions；`src/stage_controller.cpp::Impl` 已 hook work area、raw focus、hit test、workspace animation、changeWorkspace、renderWindow 和 render pass。不能再让 Tiles 对同一地址独立打第二套 hook，依赖安装顺序串联。
- `StageController::Impl::captureWindowPasses` / `drawWindowPasses`、`StageEdgeEffect::draw`、`stage::EdgeViewport` 已实现原生 surface 的硬裁剪、渐进 blur/fade、damage 和 CM framebuffer 处理。这是边缘效果唯一实现来源。
- `OverviewController::restoreOverviewRenderState` 和 `restoreWorkspaceTransitionRenderState` 仍按 monitor active 指针恢复 visibility/alpha；若不改，退出 overview 会隐藏其他 tile。strip 的 active 标记、collection、flight 端点也需接入 tile 拓扑。
- 当前没有字面名为 `ws-only` 的独立 dispatcher/配置入口。最接近的是 `onlycurrentworkspace` scope，以及 `isStripOnlyOverviewState`（没有主窗口预览但仍有 strip）。本设计同时覆盖这两种解释；不把 `ws-only` 当作已经存在的 API。

## 3. 路线比较

| 路线 | 优点 | 主要问题 | 结论 |
| --- | --- | --- | --- |
| A. 纯插件：真实 workspace 多可见 + 每 workspace 区域 hook + 原生渲染/输入适配 | 不需用户替换 compositor；能复用大量 Stage 代码；可分步验证 | 要覆盖 actions 的 typed/string 入口、monitor、placement、raw focus、resolver/history、swipe、fullscreen、frame scheduling、visibility 等；内联/直接写字段无法靠同一个 hook 拦截；事件重入与 ABI 风险高 | 可作为隔离原型/对照，不能仅凭静态分析宣称完整可行 |
| B. 核心提供 presentation topology，插件负责交互与效果 | 状态修改可在核心事务边界完成；渲染、输入、协议共享同一事实；可测试所有原生入口 | 需要核心补丁或上游接受扩展；版本升级需跟进；跨仓库交付成本 | **推荐**，最能保证原需求 |
| C. 多个 headless/虚拟 monitor，再将其内容映射到物理 monitor 的 tile | 每个虚拟 monitor 自带 active workspace、layout 和 fullscreen | 输出身份、scale、CM、layer-shell、捕获、pointer 坐标和 bar 全变；仍需物理输出合成及跨输出输入；不是免费获得多视口 | 不推荐作为当前实现 |
| D. 一个 workspace 内合并窗口/自定义 layout，或交互截图 | UI 容易做、hook 少 | 丢失 workspace 身份/规则/历史；截图不能直接承载正常客户端交互；无法让现有 workspace dispatcher 语义成立 | 不满足需求 |

“仅在 render 前换 active 指针、render 后恢复”也不采用：它把可观察的全局状态临时变成谎言；布局、焦点、IPC、帧回调和异步 pass 不保证在同一栈内完成。

### 3.1 推荐路线的核心扩展边界（拟议，并非现有 API）

核心保存 monitor 的 presentation 列表，提供版本化能力接口：枚举/修改 viewport、按输入点查询 viewport、按 workspace 查询 layout/fullscreen/render 区域、请求 activation、只更换焦点 viewport、查询已呈现 workspaces。插件提交拓扑事务，不直接写 monitor 的尺寸、reserved area 或 workspace 可见字段。

核心接入点为 §2 的 native 路径：`Config::Actions`、`CMonitor::changeWorkspace`、placement、raw focus、workspace visibility、`CSpace`、scene renderer、frame scheduler、fullscreen、resolver/history、unified swipe。single-tile 时走现有语义。核心保留 session lock、seat 和输出协议的所有权；插件故障时核心能独立收敛回一个 workspace。

接口最低能力应包括 `topology`、`activation`、`work-area`、`input`、`swipe`、`fullscreen`、`presentation-feedback`；缺少任一项就拒绝启用多 tile，返回具体错误。能力检查不能只确认符号存在，还要确认语义版本。目标是减少长期私有 hook，并不声称 Hyprland 当前已有稳定 ABI。

### 3.2 若评审决定探索纯插件，需要哪些 hook

| 功能面 | 候选函数/事件 | 稳定性与验证要求 |
| --- | --- | --- |
| activation | `Config::Actions::changeWorkspace` / `changeWorkspaceOnCurrentMonitor` 的 string 与 typed 重载；`CMonitor::changeWorkspace` | 私有签名、行为高变；必须在 action 改 monitor focus 前拦截；避免双重提交 |
| placement / lifecycle | `CPlacementController::moveWorkspaceToMonitor` / `swapActiveWorkspaces`；workspace destroy、monitor disconnect/layoutChanged | 直接字段写入须完整包围；事后 event listener 只能 reconcile，不能保证原子性 |
| work area / scrolling | `CSpace::recheckWorkArea`；`CScrollingAlgorithm::focusOnInput` 及其可见比例/相机几何调用点 | 私有区域与算法内部结构不稳定；Stage 已依赖这些点；需逐 layout 审计 monitor box 的直接读取 |
| rendering | `CMonitorScene::draw` 或 workspace-window scene 入口；两个 `shouldRenderWindow` 重载；`renderWindow` / pass / fadeouts / surface bounds / UV；damage/帧调度路径 | 高频、最高风险；必须只过滤 tile 的 workspace，shell/pinned/special 只画一次 |
| input/focus | `CViewHitTester::windowAt` / `windowAtWorkspace`；`CFocusState::rawWindowFocus`；鼠标移动/按钮、touch/tablet、pointer constraint 路径 | 只取消 event 或只看返回窗口不足以保证候选正确；必须验证客户端本地坐标和隐式 grab |
| swipe / history | `CUnifiedWorkspaceSwipeGesture::begin/update/end/cancel`、trackpad begin 计数门槛、resolver 的上下文和 history | Stage/overview 已 hook 部分路径，需一个共同 owner；函数 inline、重载变化会使 hook 不完整 |
| fullscreen / frame | `IFullscreenHandler::setTargetSizeAndPosition` / `syncTargetSizeAndPosition`；可见性、suspended、frame callback/scanout 判定 | 只改 setting 路径会被 sync 覆盖；漏帧通知会造成非焦点 tile 停帧 |

把现有 action hooks 与 Stage/overview 的重叠 hook 整理为统一 broker，再调度 `Topology → Overview/Stage presentation → Native`，不在各 controller 互相补 hook。此 broker 是拟重构组件，当前尚无跨 controller 的统一实现。每个必需 hook 用完整 demangled signature 唯一匹配、编译期函数类型检查、原子安装/回滚；找不到或多重匹配则多 tile 不可用。此表是调查出的最低覆盖面，不是已经完成的充分性证明。

## 4. 数据模型与不变量

以下是逻辑模型，不绑定尚未存在的 C++ 类型名：

```text
MonitorTiles {
  monitorRef, monitorIdentity, generation,
  mode = Fit, viewport, contentExtent, scrollOffset = 0,
  orderedTiles[], focusedTileId, transactionState
}
Tile {
  id: monotonic uint64,
  weight, allocationBox, layoutBox, fullscreenBox, clipBox,
  workspaceRef + typedIdentity, previousHistory[], lastFocusedWindowRef
}
PresentedBinding { workspaceIdentity -> (monitorIdentity, tileId) }
TileInteraction {
  kind: None | Resize | WorkspaceSwipe | OverviewDrag,
  monitorRef, tileId, generation, sessionSerial,
  source/targetWorkspaceRef, initialGeometry, pointerGrab
}
```

核心 topology 是唯一权威。Hymission 仅持有带 generation 的快照及交互状态。身份复用 `src/workspace_key.hpp` 已适配的 typed id/address/type，展示名可变；不能把显示名、数组下标或插件负数 key 直接当稳定核心身份。引用用 weak handle，销毁后不解引用，tile ID 生命周期内不复用。

必须维持：

1. 每个 tile 有且仅有一个 normal workspace；每个真实 workspace 至多出现在一个 tile。special、pinned 和 shell 有单独规则。
2. 每个 monitor 的 `m_activeWorkspace` 作为兼容投影，始终等于该 monitor `focusedTileId` 的 workspace。其他 tile 仍 visible；visible 不意味着键盘 activated。seat 全局仍只有一个键盘焦点。
3. 切换事务只修改目标 tile 的 binding/history/动画，其他 tile 的 workspace、窗口、相机和几何不变。改比例属于显式拓扑修改，可影响相邻 tile。
4. Fit 模式在 monitor **完整逻辑宽度**上分配正宽度，`Σ width = monitor.width`，连续边界无洞无交叠。分缝线和 hit slop 是叠加层，不额外占宽。
5. `allocationBox` 覆盖显示器分配区；`layoutBox` 是 allocation 与 monitor 去掉 layer-shell reserved 的可用区相交、再应用 gaps；fullscreen 用 allocation；clip 保证任何内容不越 tile。左/右输出 exclusive zone 只影响与其重叠的 tile。
6. 逻辑布局使用浮点；物理像素的相邻边界从同一组累计边界一次量化，并固定首尾，避免各 tile 独立取整产生漏缝。窄视口、变换和最大整数溢出都需验证。
7. 活跃/预览/退出动画的可见集合分开记录；预览租约不改 workspace 的永久呈现绑定。空但已绑定的 workspace 有生命周期 lease，不能被空工作区清理自动删掉。

`mode/contentExtent/scrollOffset` 为未来横向 scroll 预留。几何函数只读取 content 坐标和 viewport 变换；首版仅允许 `Fit`、`contentExtent == viewport.width`、offset 为 0。不要提前开放可配置的 scroll 模式。

### 4.1 比例算法

新增 tile 默认平分当前 tile，在其右边插入，不重新均分整屏。`tile_resize` 和拖缝均只在一对相邻 tile 间转移宽度；默认使用右邻，最右 tile 使用左邻。绝对宽度和增量均以显示逻辑像素为基础，支持显式百分比。

拖缝固定左右 tile 及 generation，用初始边界加 pointer delta，保持二者总宽不变；达到最小宽度时钳制。无足够宽度的新增或显式 resize 返回错误，不偷偷改变其他 tile。每帧最多重算一次受影响的空间，释放前保留 grab；Esc 还原初始比例，断屏/销毁取消并 reconcile。resize 期间禁止开始 swipe，反之亦然。

## 5. 切换、焦点与特殊行为

### 5.1 路由表

| 操作 | 多 tile 行为 |
| --- | --- |
| 键位、workspace action、Lua `hl.dsp.focus({workspace=...})`、IPC、Hymission strip/内部切换、swipe | 捕获发起 monitor 的 focused tile；使用同一个 activation 事务；不依赖用户重绑所有按键 |
| 点击另一 tile 的窗口/空白 | 按 input box 先选 tile；更新 active 投影与事件，再聚焦该窗口/空白；不对原 tile 运行 workspace-out 动画或迁移 pinned |
| 仅鼠标经过另一 tile | 遵循原 `input:follow_mouse`；关闭时不偷键盘焦点，开启时先切 tile 再 native focus；不能仅凭 pointer 所在位置决定随后键盘 dispatcher 的目标 |
| 直接 focuswindow / urgent / toplevel activation | 已呈现窗口聚焦其所属 tile；隐藏窗口在当前 tile 激活其 workspace，再聚焦；显式 focusmonitor 保留其原用途 |
| 相对 `m/r/e/+/-`、named、previous/back-and-forth | 保留已支持的 selector 语法和排序基础，解析上下文改为 tile；相对浏览跳过其他 tile 已呈现 workspace；previous 从该 tile 历史取未被占用的有效目标；无目标按原 create/wrap 配置或取消，不循环卡死 |
| 目标已在当前 tile | no-op；back-and-forth 另按当前 tile 历史处理 |
| 明确点名的目标已在另一 tile / 另一 monitor 呈现 | 返回 `workspace already presented`，其他 tile 不变。strip 标注占用并禁用该切换目标；不会暗中交换或克隆 workspace |
| 目标是其他 monitor 的隐藏 workspace | 普通切换不隐式迁移，返回明确错误；同屏隐藏 workspace 可激活。显式 monitor-placement 操作走下述独立管理事务 |
| 显式 move/swap workspace、窗口跨 monitor | 属于有意改变归属的管理操作；走 topology-aware placement 事务，补齐源空缺 workspace，保留其他 binding。不能借普通 workspace action 的 on-current-monitor 隐式 swap 绕过占用保护 |

这里对原生跨屏 workspace 自动聚焦/搬移行为作了明确取舍：多 tile 时优先满足“操作只切当前分块”。`on_current_monitor` 入口也不能偷偷换掉其他可见 tile；跨屏管理需要显式操作。单 tile monitor 且未启用 tiles 时保持原行为。

### 5.2 一次 activation 事务

1. 在 action 选择/改变 monitor 之前捕获 `(monitor, tileId, generation, origin)`；monitor 对象显式操作可指定该 monitor 的 focused tile，`noFocus` 保留全局 seat 焦点。
2. 解析 target，检查类型/规则归属/占用/锁屏/生命周期；需要新 workspace 时先创建可回滚对象，不发出完成事件。
3. 取消同 tile 的竞争交互；预备 target viewport、layout、fullscreen 和原生 surface 呈现。
4. 原子提交 binding、focused tile 的 active 投影、history 与可见集合。旧 workspace 仅在不再呈现且没有过渡租约时隐藏；更新 suspension、idle inhibitor 和反馈。
5. 统一处理焦点、warp、动画和 IPC，再通知观察者一次完整状态。回调重入只排入下一事务，不继续使用旧 generation；错误在提交前回滚，避免半新半旧状态。

纯 tile 焦点变化不是 workspace 替换，必须有独立路径：禁止照搬 native `changeWorkspace` 的隐藏旧 workspace、重排 pinned 和整屏动画副作用。

### 5.3 输入、拖动与 grab

输入优先级：session lock / exclusive grab → monitor 级 special 与 pinned → overview/Stage 命中 → 分缝 grab → tile 内 native workspace hit test。只有缝的窄 hit slop 截获 resize；不能吞掉客户端普通 resize 和点击。

tile 内窗口使用真实全局几何，不对普通 surface 输入做截图坐标反算。popup、subsurface、装饰、XWayland、触摸和 tablet 的候选集合及 clip 必须与画面一致；视觉上被裁掉的溢出部分不能抢另一个 tile 的输入。已有 implicit grab 的按下/抬起保持配对，不能中途切 tile 丢 release。relative pointer、locked/confined pointer 继续由核心管理；被约束的客户端不能通过移动越界夺焦点或开始拖缝。

拖窗口跨 tile 仅在显式 drop 时迁入目标 tile 的 workspace，保留目标 workspace 当前 binding；普通指针穿越不移动窗口。失败/取消回原 workspace 和几何。移动焦点窗口后其新所属 tile 成为 focused tile。

### 5.4 Swipe

使用现有 unified workspace swipe 的阈值、反向、速度提交、cancel 和 forever/create-new 规则，扩展其 session 为固定 `(monitor, tile, generation)`。trackpad 外层开始门槛也按该 tile 的可用候选计数，不能仍数整屏所有 workspace。

水平位移按 tile allocation 宽度，垂直按其高度；source/target 都裁在同一 tile，其他 tile 不动。update 只持预览租约；end 使用同一 activation 事务；cancel 恢复它拥有的 offset/alpha/force 状态，不覆盖已完成的外部改变。连续分段仍绑定初始 tile，至多按既有规则创建空 workspace；目标被别 tile 占用、focus/placement 改变、断屏或 generation 改变时取消。Stage、overview 和 Tiles 由统一 interaction owner 决定谁驱动动画，不能同一 swipe 启动两套 native/Stage 位移。

### 5.5 Fullscreen、special、pinned

- **fullscreen 默认限定在 tile allocationBox**；maximize 限定 layoutBox。设置和同步路径都改用同一 viewport 查询；保留客户端 fullscreen 协议状态，不通过临时清 fullscreen 假装支持。按 workspace 保留遮挡顺序和 allowed-over-fullscreen 浮窗，不能让 A 的 fullscreen 覆盖 B。需要 layout 自己处理 fullscreen 的算法也纳入验证。
- layer-shell 是 monitor 级，不复制 N 个 bar。top layer 是否被 fullscreen 覆盖应按与 fullscreen tile 的交集处理；overlay、锁屏规则继续由核心控制。整屏 fullscreen 不在首版偷偷作为默认行为；未来可加显式 presentation mode。
- **special workspace 保持 monitor 级覆盖层**，一次画/输入一次；显示时冻结普通 tile 交互/拖缝/swipe，关闭后恢复原 focused tile。显式普通 workspace 切换可遵循 `hide_special_on_workspace_change` 先关闭再路由到该 tile；禁用该选项时返回 special 阻塞错误。special 本身不成为 tile，也不参与相对候选。
- **pinned 浮窗保持 monitor 级唯一实例**，可以跨 tile 覆盖，位于正常 tile 内容之上；不能因 tile focus 更换而迁移 N 次或重复绘制。焦点落在 pinned 时保留其下最近的 focused tile 作为 workspace 操作上下文，客户端/窗口归属需与 native pin/unpin 规则一致。普通浮窗仍受其 workspace tile clip；unpin 后归入当前 focused tile 的 workspace。

## 6. 渲染、边缘、HDR/CM 与性能

### 6.1 场景组织

每个物理 monitor 的 shell background/bottom 只绘一次；按 tile 的 workspace 场景绘制 tiled、浮窗、popup、fullscreen、fadeout；之后绘 monitor 级 special/pinned/top/overlay，保持原生 z-order。`WORKSPACE_WINDOWS` 是可用的选择基础，但不是“多调 N 次 MONITOR 模式”：后者会重复普通可见窗口和 shell。fullscreen 分支也需按 tile 的 workspace 单独选择。

真实窗口由 `CSpace` 重排到 tile 区域，呈现默认 1:1；不把整屏工作区缩成窄图。clip 要落实到 surface、阴影、边框、blur、popup、fadeout 和 pass 可见/不透明区域，不能只设置一次 GL scissor 后假定所有 deferred pass 继承它。tile 外不出现任何未经处理的 workspace 内容。

损伤裁剪与布局几何使用同一拓扑快照；帧回调/展示反馈按本帧实际呈现 surface 去重，所有可见 tile 保持更新；隐藏历史工作区不应得到全速重绘。output enter/leave、fractional scale 和首选 buffer scale 仍绑定真实物理 monitor，不伪造 N 个输出。

### 6.2 必须复用 Stage 边缘实现

scrolling 超出 tile 边界的内容先硬限制在 tile，再在**边界内侧**按现有 Stage 样式渐进模糊、渐变透明；不会向相邻 tile 泄漏原 workspace 的窗口像素。使用 `stage::edgeViewport` / `edgeViewportForWindow` 构造左右边缘；仅在对应方向有 scrolling 溢出时启用，垂直 scrolling 同理用 top/bottom。

复用 `stage_scrolling_desktop_edge_width`（当前默认 32 逻辑像素）与 `stage_scrolling_preview_edge_width`（16），以及 `src/stage_edge_shader.hpp`、`stage_edge_framebuffer.hpp`、`StageEdgeEffect::draw`。首版不复制 shader、blur 算法或添加第二套边缘调参。宽度按已有逻辑压缩，避免窄 tile 左右 fade 重叠。

将现有捕获/执行 pass 与 effect 所有权抽为 Stage/Tiles/overview 共用的内部服务。特别是 Stage 的 `blurFramebufferThunk` 当前读的是它自己 `edgeEffect.backdrop()`；另建一个 Tiles effect 却不接 backdrop 会让 native blur 读错目标。共享服务须提供当前绘制作用域的 backdrop，并统一 RenderContextScope、RAII 状态恢复和 scratch lease；嵌套调用不能复用仍被引用的 scratch。每个窗口一次有效 edge 处理，Stage 与 tile 边界先合成最终 viewport，避免两次 blur/fade。

effect 分配/shader/blur hook 不可用时延续现有 **硬裁剪降级并记录错误**；不得无裁剪继续画。降级仍可交互，但状态查询明确 `edge_effect: hard_clip`；不能据此宣称已验收软边缘需求。

### 6.3 HDR / color management

留在同一原生 render context 和输出 CM 流程。沿用 `StageEdgeEffect::bufferFor` 依据 destination DRM format 选带 alpha 格式、继承 imageDescription、同步 mirror attachment 的做法；FP16/扩展线性 HDR 不落到默认 RGBA8，截图 mirror 的编码也不能与主附件混用。source → work buffer → output resolve 只走原生正确转换，不因 tile 重复 tone-map。

输出的 HDR metadata、CTM、VRR 和 presentation mode 仍是 monitor 级资源。多个 tile 中一个客户端 fullscreen 不能触发原生“独占全屏客户端”优化，把它的 CTM/输出元数据直接用于其他 tile；须审核 renderer/scanout 的 fullscreen 自动判定，并保留正常合成时的输出 CM 策略。这部分需要混合 SDR/HDR 客户端的运行时验证，不能由 framebuffer 格式匹配推断正确。

monitor 的 scale/transform/尺寸/CM/目标格式变化后完整替换附件并 damage 全旧新区域；分数 scale 的硬 clip 内向取整沿用现有保护。失败时释放未完成附件并退回硬 clip。SDR GLES 像素测试只能证明对应 shader/clip 合约，不能证明 HDR 显示、色彩视觉或任意设备驱动正常。

### 6.4 性能预算与调度

正常成本是可见 surface/pass 数的增加，不应按 tile 数重复遍历并绘制整屏内容。目标为构建一次 workspace→window 索引后按 tile 收集，clip cull；每帧复杂度接近 `O(visible windows + tiles)`，避免 `tiles × all windows` 的长期扫描。

边缘 scratch 可在顺序执行且无嵌套时按 monitor/context 复用。当前 effect 的主/横向两张 scratch 是整 monitor 大小：3840×2160、RGBA16F 约 126.6 MiB；每附件另有同规格 mirror 时约 253.1 MiB，不含 destination/驱动开销。不能给每 tile 各建一份。先保留已验证的格式/尺寸合约，用局部 damage 限制采样，再用实测决定是否另作紧凑 scratch 优化。

多 tile 时禁用原生 solitary-client/direct scanout 及不支持该合成模式的 tearing fast path；动画/视频在非焦点 tile 也须按输出帧节奏更新。静态 tile 不主动请求连续帧；resize 合并为每帧一次，overview 缩略图按变化失效；frame time、GPU 时间、内存与单 tile 基线比较，不能以“可见了”代替性能验收。

## 7. Overview、ws-only 与 Stage

拓扑作用域与视觉作用域分开：overview 可以暂时展示更多 workspace，但真实 tile binding 不随预览改变。

- `onlycurrentworkspace`：对 participating monitor 的**每个可见 tile**收集其当前 workspace，在原 tile 区域内排预览；focused tile 是键盘/切换上下文，`only` 不应使其他可见 tile 消失。
- all/forceall：保持可浏览隐藏 workspace 的能力；显示当前 tile 边界/归属标记，预览 workspace 的呈现租约与真实 binding 分开。选中已呈现 workspace 的窗口时聚焦它所属 tile；选中隐藏 workspace 时只替换发起/选定 tile。若原 all-scope 模式禁止 workspace 切换，继续禁止，不暗中放开。
- ws-only 的目标语义：展示可见分块及各自 workspace 缩略图/标签，标明 focused tile；strip 切换作用于所选 tile，其他 tile 的已占用目标可见但禁用。当前 strip-only 空窗口路径同样必须维持 tile chrome，空 workspace 仍有可点击区域。这里定义行为，不声称已有 `ws-only` 字符串入口；后续若需新增显式入口另在评审中确认。
- `WorkspaceOverride`、`WorkspaceStripEntry`、window slot/drag target、`WorkspaceTransition` 增加 `(monitor, tileId)`，不能再按 monitor 唯一 ownerWorkspace 推导端点。native/overview hit box 使用同一 tile clip；预览跨 tile drop 有明确目标 workspace。
- overview 退出按**当前 topology + generation**恢复可见集合/alpha；不能把“非 `m_activeWorkspace`”全部隐藏，也不能用打开时快照覆盖期间的 resize、断屏或真实切换。flight 的目的地取当前 workspace 对应 viewport，取消时不把全部工作区退回打开前状态。
- Stage 的 sidebar/desktop、scroll camera、smartisan 左右切换、flights、fullscreen covered 状态以 tile 为 scope。可见 sidebar 从本 tile allocation 内部占空间，不再次从整个 monitor reserved area 扣除。overview 暂挂 Stage 时保留每 tile Stage 状态，由单一 owner 绘制/接管动画。
- 每 tile 的 Stage desktop viewport 再与 tile clip 合并后交给共享 edge service；小 tile 放不下 sidebar 时按 Stage 几何规则收缩/隐藏该 sidebar，并报告状态，不能挤占相邻 tile。Stage 内部选择也走共同 activation 事务。

## 8. DSP、配置与 Lua API 提案

**以下均为待实现接口；本提交不会注册任何新函数或配置。** 命名采用现有单层 `hl.plugin.hymission.*`，Lua 函数立即执行，错误沿用 Lua error / `SDispatchResult`。legacy parser 存在时注册对应 `hymission:*`；Lua 模式通过插件 API 调用，与 `src/main.cpp` 当前兼容分支一致。

| legacy dsp 名称 | Lua 函数提案 | 参数/语义 |
| --- | --- | --- |
| `hymission:tile_add` | `tile_add({workspace="name:dev", width="40%"})` | 当前焦点 monitor/当前 tile 右边新增；默认平分当前 tile 并创建未占用 normal workspace；传入 hidden 同屏 workspace 时直接使用；新增 tile 得焦点 |
| `hymission:tile_resize` | `tile_resize({width="60%", neighbor="right"})` | 调整当前 tile 绝对宽度；或传 `{delta="+80px"}`；width/delta 二选一；neighbor 省略时按 §4.1；不能改总宽 |
| `hymission:tile_remove` | `tile_remove()` | 删除当前 tile，将其宽度交给左邻（无左邻则右邻），焦点落到接收者；不能删最后一个 |
| `hymission:tile_focus` | `tile_focus("left")` / `tile_focus("right")` | 显式改变 focused tile，不切 workspace；到边界 no-op |

legacy 参数使用同一解析器：`tile_add` 为空或 `workspace=name:dev width=40%`，`tile_resize` 为 `width=60% neighbor=right` 或 `delta=+80px`，`tile_remove` 无参，`tile_focus` 为 `left|right`。只接受列出的 key=value 字段，不拼 shell；workspace selector 仅允许可定位单个 normal workspace 的绝对形式。resize 百分比是 monitor 完整逻辑宽度的比例；add 的 width 表示新 tile 宽度，从当前 tile 扣除。`width="40%"` 在当前 tile 本身不足 40% 时失败。

`hl.plugin.hymission.tiles_state()` 提供只读表；同内容提供 `hyprctl -j hymission-tiles-state` 查询。字段至少含 schema/version、backend/capabilities、monitor、generation、mode、focused tile、每 tile 的稳定 ID/workspace identity/各 box/比例、interaction owner、edge 降级原因。可加 `hymission-tiles` 自定义 socket2 event 通知 generation；bar 收到事件后按 generation 读完整状态，不解析日志。

配置提案（数字开关沿用当前插件约定）：

```lua
-- 设计示例：这些 tile_* 配置和函数尚未实现。
hl.config({ plugin = { hymission = {
    tiles_enabled = 0,       -- 默认关闭；设 1 才允许新增多 tile
    tile_min_width = 240,    -- 逻辑像素，含 allocation 宽度
    tile_divider_hit_width = 8, -- 缝两侧合计命中宽，叠加层
    -- soft edge 继续用现有 stage_scrolling_*_edge_width
} } })

hl.bind("SUPER + CTRL + N", function()
    hl.plugin.hymission.tile_add()
end)
hl.bind("SUPER + CTRL + equal", function()
    hl.plugin.hymission.tile_resize({ delta = "+80px" })
end)
hl.bind("SUPER + CTRL + W", hl.plugin.hymission.tile_remove)
hl.bind("SUPER + CTRL + left", function()
    hl.plugin.hymission.tile_focus("left")
end)
```

`tiles_enabled=1` 仅激活管理能力，不在配置重读时重复添加 tile。关闭时收敛为一个全宽 tile，保留关闭前 focused tile 的 workspace。finite/范围/未知字段检查先于任何修改；`tile_min_width > monitor.width` 时保留单 tile 并拒绝 add；divider hit slop 不能覆盖整个最窄 tile。

### 8.1 删除、停用与故障归位

删除 tile 时**不杀窗口、不把多个 workspace 的窗口静默合并**。被删 tile 的 workspace 变为此物理 monitor 上的普通隐藏 workspace，移除 binding/可见 lease，保留窗口、布局算法、身份与正常历史可达性；其布局区域恢复该 monitor 常规 work area，浮窗做有界归位。接收 tile 保留自己的 workspace 和 focus candidate，仅增宽。删除产生的空 workspace 是否回收遵循 native 生命周期与 persistent 规则，不强删。

显式关闭 tiles / 插件正常卸载：取消所有交互、释放预览/effect 资源、保留最后 focused workspace 为唯一 active，其余变隐藏、恢复原生 work area，再解绑服务。核心负责兜底，不能依赖即将卸载的插件继续运行。应用退出/断屏时 stale ID 的 pending callback 因 generation 不符失效。

## 9. 多显示器、IPC 与生命周期

每 monitor 独立有 tile 顺序/比例/focused tile；全局 seat 决定当前键盘操作的 monitor。不同 scale、transform、HDR/SDR 混合输出各用自己的物理转换和 CM scratch。mirror 输出复制最终物理画面，不独立分配可交互 tile；非交互/镜像目标拒绝 add。

断屏使用 native workspace placement 与 monitor 规则迁移真实 workspace；源 topology 原子失效，目标当前 tile binding 不被自动挤掉，迁入 workspace 默认成为隐藏可达 workspace（没有剩余输出时遵循 native headless/恢复流程）。焦点交给 native 选中的 surviving monitor/tile。重新接屏首版恢复单 tile，不自动凭 monitor 数字 ID 重播旧 topology；持久化属于后续功能。分辨率/旋转变化保留权重重新分配；若最小宽度放不下，确定性地合并非焦点 tile，使其 workspace 隐藏，并保留 focused workspace。

旧 IPC `activeworkspace`、`monitors.activeWorkspace` 和 ext-workspace active 位继续表示该 monitor 的 focused tile workspace；普通 bar 会看到唯一 active，其他可见 workspace 可能显示为 inactive，这是明确的兼容语义。不能轮流写 active 指针让 bar 看似多个 active。焦点换 tile 时发一次与新 active 相符的 native 事件，并发 tiles generation 事件；拓扑内多个可见关系由 tiles_state 提供。

有 tiles 时禁用 `bar_single_mission_control` 对多 workspace 的临时重命名并给诊断原因，避免覆盖真实名字/身份或误导 bar。rename workspace 不改变 tile binding。窗口规则、新窗口 placement、空工作区清理、persistent workspace、monitor workspace 规则均需以核心 topology 查询收敛；可见空 tile 不能无声消失。

新窗口默认进入 focused tile 的 workspace；显式 workspace/window rule 仍先决定真实归属。rule 将窗口放入另一可见 tile 时不移动其 workspace，是否自动聚焦遵循原生规则；若接受聚焦则同步 focused tile。落在隐藏 workspace 且规则要求 silent 时保留隐藏，不借创建窗口改掉当前 tile。workspace 规则强制 monitor 归属与 tile 请求冲突时返回错误，禁止先迁移再回滚规则的循环。

## 10. 分阶段实施与停止条件

本文件通过评审之前，以下全是计划，**不执行任何实现阶段**。

| 阶段 | 工作与交付 | 进入下一阶段的门槛 |
| --- | --- | --- |
| 0. 评审与基线 | Claude 确认推荐路线、核心变更范围、跨屏/占用/fullscreen/ws-only 语义；刷新上游锁并建立插件构建基线 | 锁定 revision、构建与现有检查结果可复现；不把历史 compatibility 文档当结果 |
| 1. 拓扑/纯逻辑 | geometry、binding、history、generation、事务、add/resize/remove/focus；核心能力合约与生命周期兜底 | 不变量/失败回滚/随机序列测试；缺能力时不能启用 |
| 2. 原生多 viewport | work area、可见性、场景、frame feedback、hit test、raw focus、配置/API；先在隔离 compositor 验证双 tile | 两边可实时操作/出新帧，空 tile 正常；其他 tile 未受键位/action/IPC 影响 |
| 3. 完整原生路由 | selector/history、monitor/typed actions、placement、swipe、fullscreen/special/pinned、grab/drag、断屏 | 路由矩阵无旁路；cancel/重入一致；未通过功能不能宣称完成 |
| 4. Hymission 集成 | 共用 edge service、Stage 每 tile 状态、overview/ws-only/strip、出入动画、HDR/CM/capture | 无重复绘制/双动画/越界输入；主 framebuffer 和 mirror 均验证 |
| 5. 收尾 | 性能实测、配置文档、完整构建/回归、本地分阶段提交 | 全部需求验收记录；真机未做项明确 PENDING；发布/push/reload 另按授权 |

若核心扩展无法交付，或纯插件必需 hook 无法可靠覆盖，不退回“截图交互版本”充数：停在可复现的原型/缺口清单，重新评审路线。开发 API 修改仅进入本开发分支体系，不未经验证传播到 stable/Omarchy。

## 11. 测试方案与证据分级

### 11.1 无 compositor 的模型测试

新增独立 tiles logic 测试：多种宽度/权重/NaN/Inf/越界参数、add/remove/resize 交错、边界钳制、物理像素覆盖、named/typed identity/rename/destroy、busy target、previous/back-and-forth、generation stale callback、事务失败与回滚、空 workspace lease、分辨率缩小/断屏迁移。固定种子的随机操作序列每步检查 §4 不变量，不能只镜像实现重写一遍。

### 11.2 隔离 compositor 的合约与输入测试

使用与构建 revision 一致的独立 nested/headless Hyprland 和私有 IPC 环境，不接管当前会话。不自动假设 headless 支持所有 GL/CM/手势能力；不可用项记 SKIP 并说明。

以 A/B 两个 tile，workspace 1/2 和隐藏 workspace 3/4 为基础，覆盖：

| 测试组 | 操作 | 必须观察的断言 |
| --- | --- | --- |
| 全入口路由 | 键位、string/typed Actions、Lua focus、monitor:set_workspace(...) 对象入口、IPC、Hymission strip、overview 内部切换、直接窗口激活 | A:1→3 时 B 的 workspace/窗口盒/相机不变；focus B 后切 B；读取 active 投影与完整 topology |
| 冲突/跨屏 | 目标是 B 的 2、其他输出可见/隐藏 workspace、on-current-monitor、显式 swap/move | 普通切换拒绝且无部分迁移；管理操作按事务完整补源空缺；第二输出不被普通切换连带改变 |
| 帧/交互 | 两边运行动画客户端，改变非焦点 tile surface，popup/subsurface/浮窗/X11/空白/touch/tablet | 两边持续出新帧、无误 suspend；画面 clip 与命中一致；button press/release 配对、约束不绕过 |
| 手势 | begin/update/commit/cancel、反向、阈值、forever/create-new、过程中外部 focus/resize/destroy | 只有绑定 tile 平移；取消不损坏其他动画；无双 controller 提交、无 stale session |
| fullscreen 等 | 两边各 fullscreen/maximize、允许覆盖浮窗、layout-handled fullscreen、special 开闭、pinned/unpin | 全屏限定自身 tile；shell 策略一致；special/pinned 只一份；关闭恢复 focused tile |
| 生命周期 | 删除活动/空 tile/最后 tile、停用、客户端退出、移屏、断屏、hotplug、scale/transform 改变 | workspace/窗口可达、无越界指针和 orphan、active/visible/suspended 一致；最后 tile 删除报错 |
| overview/Stage | onlycurrentworkspace/all、空 strip-only、stage 开关/左右翻转、ws 切换/跨 tile drop、途中退出或断屏 | binding 稳定；退出不隐藏其他 tile；flight 落在正确 viewport；scroll camera 只改变所属 workspace |

对象入口已在目标 `LuaMonitor.cpp` 核对：`active_workspace` 是查询属性，`set_workspace` 是方法；测试必须覆盖方法内部先走 placement、再走 monitor 切换的顺序，不能误把它当成只读属性测试。

### 11.3 渲染与性能测试

扩展现有 `tools/stage_edge_render_test.cpp`、native framebuffer contract/snapshot color tests（实际目标名以 `CMakeLists.txt` 为准）：用不同纯色/透明 pattern 的邻接 tile，检查越界像素、fade/blur 轮廓、damage 洞、closing fadeouts、fractional scale、旋转、scratch/mirror 格式切换及失败硬裁剪。测试重用 effect 的同一路径，增加多 viewport/嵌套 lease 场景，不另造 shader。

记录 1/2/3/4 tile 在同总窗口数与增加可见窗口数两种负载下的 CPU/GPU 帧时间 p50/p95/p99、missed frames、VRAM/附件数量、静态帧请求数；另测 scrolling 视频、resize 和 overview。检查 direct scanout 被正确退出/恢复。性能门槛在隔离基线完成后、功能优化前固定，不能事后根据结果移动标准。

完整编译验证运行仓库 CTest 和开发 flake build；native-symbol/GL 目标未注册或 skip 不能计作通过。真实 HDR/SDR 混合显示、10-bit/FP16、截屏 mirror、硬件触控板 swipe、fractional scaling、VRR/tearing、锁屏与 pointer lock 仍需隔离硬件会话验收。构建、软件像素检查、隔离运行、真机输入与视觉证据分开记录。

## 12. 风险与评审决策

最高风险是原生 active/visible 假设散落多个子系统、同步事件重入、私有 ABI 漂移、双渲染/双动画、blur scratch/CM 附件错误和 tile 外输入泄漏。其次是 sidebar 在窄 tile 的可用性、pinned/fullscreen/special 的用户预期、bar 单 active 投影、客户端最小尺寸导致的裁剪以及多输出规则冲突。以上不能仅以 build 通过关闭。

请评审以下具体决策：

1. 是否接受“核心 viewport 支持 + Hymission 交互”作为完整功能路线，以及核心变更的后续范围；若坚持纯插件，是否先只批准覆盖面原型。
2. 是否接受独占 workspace：已在其他 tile 呈现的目标报错，相对浏览跳过；普通跨屏 workspace 切换不搬动/切换其他分块。
3. 是否接受 tile 内 fullscreen、monitor 级 special/pinned，以及删除 tile 后 workspace 保持隐藏可达的默认行为。
4. 是否接受 ws-only 同时覆盖 active-workspace overview 与空 strip-only 路径，显式新入口另行确定。

**到此停止。提交此设计后等待 Claude 审阅，不开始实现、不修改运行时、不更新/重载插件。**
