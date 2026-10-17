# 工作区分块：可行性与设计评审稿

状态：**设计已审，正式采用路线 A（纯插件）；核心扩展路线未采纳。实施按阶段本地提交。**

最新用户约束：每个物理 monitor 最多 3 个 tile；tile 交互以原生多 monitor 为参照。每 tile 有独立 active workspace/history，切换至另一 tile 已呈现 workspace 时聚焦该 tile（on-current-monitor 显式请求采用原生 swap 语义）；不再占用报错。提供跨 tile 移窗、方向焦点与 monitor 风格状态。

日期：2026-10-17。范围包括设计、纯插件实现及隔离验证，按阶段本地提交；不修改 Hyprland 源码、用户配置或宿主运行中的插件，不 push，不运行 `hyprpm update`，不对宿主 load/unload。

## 1. 结论与证据边界

推荐语义是：**同一物理 monitor 上同时呈现多个真实 workspace，每个 workspace 有独立 viewport；键盘焦点对应的 tile 决定普通工作区切换的目标区域。** 窗口保留真实 workspace 身份、原生 layout 和客户端 surface。分块不是 workspace 截图，也不是把多个 workspace 的窗口迁入一个 workspace。

正式工程路线为 **纯插件：真实 workspace 多可见、每 workspace work area、统一 hook broker 与原生 surface 渲染/输入适配**。不修改 Hyprland 核心，不依赖核心补丁，不使用截图交互。原路线 B 未采纳。以下源码缺口通过插件事务、hook 和状态收敛处理；确实无法覆盖的路径必须记录具体证据，同时继续其他阶段。

### 1.1 初始设计评审时检查的版本

| 对象 | 本次检查结果 | 能证明什么 |
| --- | --- | --- |
| Hymission 工作树 | `feat/workspace-tiles`，起点 `552bd339fe4809e0a031d277ca97f4dac6d0a5da`；开始时干净 | 本设计针对当前开发分支的实际代码 |
| `~/data/Hyprland` | `e5a03b6a5ce64ec4c85954649bad4df083627574`；本地 `main` 相对 `origin/main` ahead 77 / behind 6；存在未跟踪目录 | 用户指定本地源码，包含本地修改，不能直接称为上游 HEAD |
| 上游 `main` | `git ls-remote` 当场返回 `25f09b26f9db7dbca4a4b3c265fdb96802db122c`；与本地 `origin/main`、本仓库现有 `flake.lock` 一致 | 关键路径另用 `git show origin/main:<path>` 核对，未 checkout/fetch/修改源树 |
| 构建、运行时、视觉 | 本阶段均未运行 | **没有新增开发兼容性、HDR 视觉、输入或真机验收结论** |

下文 Hyprland 行号以 `25f09b26…` 为准；本地树路径相同但行号可能不同。尤其 `Monitor.cpp` 和 `Renderer.cpp` 存在本地差异，不能将本地 CM 行为当作上游保证。`docs/compatibility/hyprland-master.md` 是历史记录，不据此推断当前构建结果。

实施阶段已刷新 flake input 至上游 `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`，构建和隔离验证的最终结果见 [验收记录](workspace-tiles-validation.md)。上表保留初始调查快照，不代表最终测试状态。实现阶段使用默认 pkg-config 的 git 头文件逐阶段构建/测试；本机验证禁止 coverage 构建。

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
| A. 纯插件：真实 workspace 多可见 + 每 workspace 区域 hook + 原生渲染/输入适配 | 不需用户替换 compositor；能复用大量 Stage 代码；可分步验证 | 要覆盖 actions 的 typed/string 入口、monitor、placement、raw focus、resolver/history、swipe、fullscreen、frame scheduling、visibility 等；内联/直接写字段无法靠同一个 hook 拦截；事件重入与 ABI 风险高 | **已采纳**；实现并按入口逐项验证，未验证内容保留证据边界 |
| B. 核心提供 presentation topology，插件负责交互与效果 | 状态修改可在核心事务边界完成；渲染、输入、协议共享同一事实；可测试所有原生入口 | 需要核心补丁或上游接受扩展；版本升级需跟进；跨仓库交付成本 | **未采纳：违反纯插件硬性要求** |
| C. 多个 headless/虚拟 monitor，再将其内容映射到物理 monitor 的 tile | 每个虚拟 monitor 自带 active workspace、layout 和 fullscreen | 输出身份、scale、CM、layer-shell、捕获、pointer 坐标和 bar 全变；仍需物理输出合成及跨输出输入；不是免费获得多视口 | 不推荐作为当前实现 |
| D. 一个 workspace 内合并窗口/自定义 layout，或交互截图 | UI 容易做、hook 少 | 丢失 workspace 身份/规则/历史；截图不能直接承载正常客户端交互；无法让现有 workspace dispatcher 语义成立 | 不满足需求 |

“仅在 render 前换 active 指针、render 后恢复”也不采用：它把可观察的全局状态临时变成谎言；布局、焦点、IPC、帧回调和异步 pass 不保证在同一栈内完成。

### 3.1 未采纳路线

核心 presentation topology/版本化能力接口均未采纳，不作为运行依赖，也不修改核心源码。插件维护 topology、可见性、work area、输入/动画上下文和失败回滚。必需 hook 不完整时整体禁用 tiles，以 initError 上报，不以部分功能静默运行。

### 3.2 正式路线：统一 broker 与必需 hook

| 功能面 | 候选函数/事件 | 稳定性与验证要求 |
| --- | --- | --- |
| activation | `Config::Actions::changeWorkspace` / `changeWorkspaceOnCurrentMonitor` 的 string 与 typed 重载；`CMonitor::changeWorkspace` | 私有签名、行为高变；必须在 action 改 monitor focus 前拦截；避免双重提交 |
| placement / lifecycle | `CPlacementController::moveWorkspaceToMonitor` / `swapActiveWorkspaces`；workspace destroy、monitor disconnect/layoutChanged | 直接字段写入须完整包围；事后 event listener 只能 reconcile，不能保证原子性 |
| work area / scrolling | `CSpace::recheckWorkArea`；`CScrollingAlgorithm::focusOnInput` 及其可见比例/相机几何调用点 | 私有区域与算法内部结构不稳定；Stage 已依赖这些点；需逐 layout 审计 monitor box 的直接读取 |
| rendering | `CMonitorScene::draw` 或 workspace-window scene 入口；两个 `shouldRenderWindow` 重载；`renderWindow` / pass / fadeouts / surface bounds / UV；damage/帧调度路径 | 高频、最高风险；必须只过滤 tile 的 workspace，shell/pinned/special 只画一次 |
| input/focus | `CViewHitTester::windowAt` / `windowAtWorkspace`；`CFocusState::rawWindowFocus`；鼠标移动/按钮、touch/tablet、pointer constraint 路径 | 只取消 event 或只看返回窗口不足以保证候选正确；必须验证客户端本地坐标和隐式 grab |
| swipe / history | `CUnifiedWorkspaceSwipeGesture::begin/update/end/cancel`、trackpad begin 计数门槛、resolver 的上下文和 history | Stage/overview 已 hook 部分路径，需一个共同 owner；函数 inline、重载变化会使 hook 不完整 |
| fullscreen / frame | `IFullscreenHandler::setTargetSizeAndPosition` / `syncTargetSizeAndPosition`；可见性、suspended、frame callback/scanout 判定 | 只改 setting 路径会被 sync 覆盖；漏帧通知会造成非焦点 tile 停帧 |

把现有 action hooks 与 Stage/overview 的重叠 hook 整理为统一 broker，再调度 `Topology → Overview/Stage presentation → Native`，不在各 controller 互相补 hook。实现为 `hook_broker.hpp/.cpp`：每地址一处 patch，controller 通过有序 lease 共享 continuation。每个必需 hook 用完整 demangled signature 唯一匹配、编译期函数类型检查、原子安装/回滚；找不到或多重匹配则多 tile 不可用。此表是调查出的最低覆盖面，不是已经完成的充分性证明。

## 4. 数据模型与不变量

以下模型由 `tiles_logic::Model` 及 `TilesController` 维护：

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
  kind: None | Resize | WorkspaceSwipe | OverviewDrag | StageDrag,
  monitorRef, tileId, generation, sessionSerial,
  source/targetWorkspaceRef, initialGeometry, pointerGrab
}
```

插件 topology 是 tiles 的唯一权威，并与原生 workspace/monitor 状态同步。controller 使用带 generation 的快照及交互状态。身份复用 `src/workspace_key.hpp` 已适配的 typed id/address/type，展示名可变；不能把显示名、数组下标或插件负数 key 直接当稳定核心身份。引用用 weak handle，销毁后不解引用，tile ID 生命周期内不复用。

必须维持：

1. 每个物理 monitor 最多 3 个 tile，第四次 add 在任何 mutation 前报错。每个 tile 有且仅有一个 normal workspace；每个真实 workspace 至多出现在一个 tile。special、pinned 和 shell 有单独规则。
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
| 相对 `m/r/e/+/-`、named、previous/back-and-forth | 保留已支持的 selector 语法和排序基础，解析上下文改为 tile；相对浏览遵循同屏 tile 的多屏策略，swipe 的候选跳过其他 tile 已呈现 workspace；previous 从该 tile 历史取未被占用的有效目标；无目标按原 create/wrap 配置或取消，不循环卡死 |
| 目标已在当前 tile | no-op；back-and-forth 另按当前 tile 历史处理 |
| 明确点名的目标已在另一 tile / 另一 monitor 呈现 | 普通 workspace 请求聚焦该 workspace 所在 tile，不克隆；`on_current_monitor` 请求按原生多屏语义交换两个 tile 的 active workspace，更新各自历史和区域；strip 同样路由 |
| 目标是其他 monitor 的隐藏 workspace | 普通切换聚焦目标的真实 monitor 并在其 focused tile 激活，不隐式迁移；同屏隐藏 workspace 可激活。显式 monitor-placement 操作走下述独立管理事务 |
| 显式 move/swap workspace、窗口跨 monitor | 属于有意改变归属的管理操作；走 topology-aware placement 事务，补齐源空缺 workspace，保留其他 binding。on-current-monitor 对另一 tile 的可见 workspace 使用有意的 active-workspace swap |

每 tile 尽量表现为独立 monitor：普通切换到已呈现 workspace 聚焦其 tile，on-current-monitor 则交换源/目标 tile 的 active workspace；遵循原生 focus/warp 配置。物理 monitor 绑定规则仍匹配真实输出，不伪造输出协议对象；在匹配输出内根据 focused tile 定位。单 tile 且未启用时严格保持原行为。

方向焦点到本 tile 边界时进入相邻 tile，仍优先采用原生布局内候选；`tile_focus left/right` 显式跨区，`tile_move_window left/right` 将焦点窗口移入邻区 active workspace。新窗口跟随 focused tile，移窗与 focus 走同一事务维护 history/active 投影。

### 5.2 一次 activation 事务

1. 在 action 选择/改变 monitor 之前捕获 `(monitor, tileId, generation, origin)`；monitor 对象显式操作可指定该 monitor 的 focused tile，`noFocus` 保留全局 seat 焦点。
2. 解析 target，检查类型/规则归属/占用/锁屏/生命周期；需要新 workspace 时先创建可回滚对象，不发出完成事件。
3. 取消同 tile 的竞争交互；预备 target viewport、layout、fullscreen 和原生 surface 呈现。
4. 原子提交 binding、focused tile 的 active 投影、history 与可见集合。旧 workspace 仅在不再呈现且没有过渡租约时隐藏；更新 suspension、idle inhibitor 和反馈。
5. 统一处理焦点、warp、动画和 IPC，再通知观察者一次完整状态。回调重入只排入下一事务，不继续使用旧 generation；错误在提交前回滚，避免半新半旧状态。

纯 tile 焦点变化不是 workspace 替换，必须有独立路径：禁止照搬 native `changeWorkspace` 的隐藏旧 workspace、重排 pinned 和整屏动画副作用。

### 5.3 输入、拖动与 grab

输入优先级：session lock / exclusive grab → monitor 级 special 与 pinned → overview/Stage 命中 → 分缝 grab → tile 内 native workspace hit test。只有缝的窄 hit slop 截获 resize；不能吞掉客户端普通 resize 和点击。

tile 内窗口使用真实全局几何，不对普通 surface 输入做截图坐标反算。popup、subsurface、装饰、XWayland、触摸和 tablet 的候选集合及 clip 必须与画面一致；视觉上被裁掉的溢出部分不能抢另一个 tile 的输入。已有 implicit grab 的按下/抬起保持配对，不能中途切 tile 丢 release。relative pointer、locked/confined pointer 继续由原生 Hyprland 管理；被约束的客户端不能通过移动越界夺焦点或开始拖缝。

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
- all/forceall：保持可浏览隐藏 workspace 的能力；窗口统一按整屏布局，不以原 tile 限制命中或裁剪；strip 仍显示 tile 归属。预览 workspace 的呈现租约与真实 binding 分开。选中已呈现 workspace 的窗口时聚焦它所属 tile；选中隐藏 workspace 时只替换发起/选定 tile。若原 all-scope 模式禁止 workspace 切换，继续禁止，不暗中放开。
- ws-only 的目标语义：展示可见分块及各自 workspace 缩略图/标签，标明 focused tile；strip 切换作用于所选 tile，其他 tile 的已占用目标可见并能聚焦对应 tile。当前 strip-only 空窗口路径同样必须维持 tile chrome，空 workspace 仍有可点击区域。这里定义行为，不声称已有 `ws-only` 字符串入口；后续若需新增显式入口另在评审中确认。
- `WorkspaceOverride`、`WorkspaceStripEntry`、window slot/drag target、`WorkspaceTransition` 增加 `(monitor, tileId)`，不能再按 monitor 唯一 ownerWorkspace 推导端点。native/ws overview hit box 使用同一 tile clip，all overview 使用物理输出范围；预览跨 tile drop 有明确目标 workspace。
- overview 退出按**当前 topology + generation**恢复可见集合/alpha；不能把“非 `m_activeWorkspace`”全部隐藏，也不能用打开时快照覆盖期间的 resize、断屏或真实切换。flight 的目的地取当前 workspace 对应 viewport，取消时不把全部工作区退回打开前状态。
- Stage 的 sidebar/desktop、scroll camera、smartisan 左右切换、flights、fullscreen covered 状态以 tile 为 scope。可见 sidebar 按 tile 顺序集中在整屏最左侧，整块输出共享一个等比缩放/平移，所有 tile 一起为集中列带让位。overview 暂挂 Stage 时保留每 tile Stage 状态，由单一 owner 绘制/接管动画。
- 每 tile 的 Stage desktop viewport 再与 tile clip 合并后交给共享 edge service；小 tile 放不下 sidebar 时按 Stage 几何规则收缩/隐藏该 sidebar，并报告状态，不能挤占相邻 tile。Stage 内部选择也走共同 activation 事务。

### 7.1 已采纳的分组与拖拽协同（需求更新 2）

workspace strip 按 tile 从左到右各一列，集中在整屏最左侧，focused tile 有独立描边；点击 workspace 时捕获该列 tile ID，不能按当前全局 focus 猜测。仅 onlycurrentworkspace 的主窗口区域在每个 tile 的 allocation/layout 区域内分别调用布局引擎；all/forceall 在物理输出内统一布局所有窗口。空 tile 仍保留 strip 列。

Stage 采用 **每个 tile 一列、所有列集中在整个 monitor 最左侧**。卡片尺寸按完整显示器计算，所有列使用同一宽度和显示器宽高比；列带总宽最多占可用输出的一半。所有 tile 共用同一个桌面仿射变换，保持相邻边界连续，整体等比缩放并向右平移、垂直居中。列顺序对应所属 tile，状态查询包含 tile ID；缩略图不包含其他 tile 已呈现 workspace 的重复桌面。tiles 启用时保留 active workspace 的 Stage 缩略图，单 tile 也能直接拖出 Split View。

Stage 缩略图可以跨列左右拖入另一 tile 的 active workspace，也可以落在其桌面区域；单窗口缩略图移动窗口，workspace/stage 组缩略图移动组内全部成员，保持组关系。目标为空白/已呈现 workspace 的区别在预览阶段确定。source workspace 不被销毁，未移动的窗口保持原位。

单 tile 时从 Stage sidebar 拖出缩略图到桌面左/右半区，可预览原地 Split View：在落点侧新建 tile，源桌面保留另一侧；两个 tile 时同样在目标 tile 的左/右半拆出第三个，三个时不再提供 split 落点。桌面落点规则为：单 tile 的两半、来源 tile 的两半，或其他 tile 的外侧 20% 区域提供 split；其他 tile 的中部以及 sidebar 列提供移动落点。预览明确展示最终动作。新增宽度受 tile_min_width 限制；预览不满足条件时显示无效落点，不能松手后偷偷改变其他 tile。若目的仅为向已有 tile 移动，预览明确为该 tile 的桌面 drop，不创建额外 tile。

平时不绘制 tile 边界线，边界命中区仍支持拖动调整宽度。拖动缩略图时绘制半透明目标区域与分割线，反馈最终新/旧 tile 的几何。松手前 Esc、拖回原 sidebar 缩略图、锁屏、源窗口消失、断屏或拓扑 generation 变化均取消。取消不创建 workspace，不改变 binding/窗口/历史。drop 先验证完整计划和源成员，再调用与 tile_add / 窗口移动相同的事务；失败回滚新 workspace、几何和焦点。

Tiles/Stage/overview 共用一个交互 owner（类型区分 resize、workspace swipe、overview drag、Stage transfer/split）。一个 drag 从按下到 release/cancel 固定 source ID、generation 和窗口引用，只有 owner 能消费按钮/驱动 flight；不同时触发原生 drag 和 overview ghost。预览、commit、settle 之间移交 owner，确保每个窗口每帧只被一个 controller 绘制一次。

## 8. DSP、配置与 Lua API

以下接口已实现。命名采用现有单层 `hl.plugin.hymission.*`，Lua 函数立即执行，错误沿用 Lua error / `SDispatchResult`。legacy parser 存在时注册对应 `hymission:*`；Lua 模式通过插件 API 调用，与 `src/main.cpp` 当前兼容分支一致。

| legacy dsp 名称 | Lua 函数 | 参数/语义 |
| --- | --- | --- |
| `hymission:tile_add` | `tile_add({workspace="name:dev", width="40%"})` | 当前焦点 monitor/当前 tile 右边新增；默认平分当前 tile 并创建未占用 normal workspace；传入 hidden 同屏 workspace 时直接使用；新增 tile 得焦点 |
| `hymission:tile_resize` | `tile_resize({width="60%", neighbor="right"})` | 调整当前 tile 绝对宽度；或传 `{delta="+80px"}`；width/delta 二选一；neighbor 省略时按 §4.1；不能改总宽 |
| `hymission:tile_remove` | `tile_remove()` | 删除当前 tile，将其宽度交给左邻（无左邻则右邻），焦点落到接收者；不能删最后一个 |
| `hymission:tile_move_window` | `tile_move_window("left")` / `tile_move_window("right")` | 移动焦点窗口到相邻 tile 的 active workspace |
| `hymission:tile_focus` | `tile_focus("left")` / `tile_focus("right")` | 显式改变 focused tile，不切 workspace；到边界 no-op |

legacy 参数使用同一解析器：`tile_add` 为空或 `workspace=name:dev width=40%`，`tile_resize` 为 `width=60% neighbor=right` 或 `delta=+80px`，`tile_remove` 无参，`tile_focus` 为 `left|right`。只接受列出的 key=value 字段，不拼 shell；workspace selector 仅允许可定位单个 normal workspace 的绝对形式。resize 百分比是 monitor 完整逻辑宽度的比例；add 的 width 表示新 tile 宽度，从当前 tile 扣除。`width="40%"` 在当前 tile 本身不足 40% 时失败。

`hl.plugin.hymission.tiles_state()` 提供只读表；同内容提供 `hyprctl -j hymission-tiles-state` 查询。字段至少含 `version/backend/enabled/initError/max_tiles`、每 monitor 的 `generation/tiles`、每 tile 的稳定 ID、workspace 身份、history，以及 monitor 风格 `name/x/y/width/height/activeWorkspace/focused` 字段（最多三个条目）、interaction owner、edge 降级原因。可加 `hymission-tiles` 自定义 socket2 event 通知 generation；bar 收到事件后按 generation 读完整状态，不解析日志。

配置与 API（数字开关沿用当前插件约定）：

```lua
-- 默认关闭；以下 API 已由插件注册。
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

显式关闭 tiles / 插件正常卸载：取消所有交互、释放预览/effect 资源、保留最后 focused workspace 为唯一 active，其余变隐藏、恢复原生 work area，再解绑服务。插件析构时先归位再卸 hook，不保留指向插件的回调。应用退出/断屏时 stale ID 的 pending callback 因 generation 不符失效。

## 9. 多显示器、IPC 与生命周期

每 monitor 独立有 tile 顺序/比例/focused tile；全局 seat 决定当前键盘操作的 monitor。不同 scale、transform、HDR/SDR 混合输出各用自己的物理转换和 CM scratch。mirror 输出复制最终物理画面，不独立分配可交互 tile；非交互/镜像目标拒绝 add。

断屏使用 native workspace placement 与 monitor 规则迁移真实 workspace；源 topology 原子失效，目标当前 tile binding 不被自动挤掉，迁入 workspace 默认成为隐藏可达 workspace（没有剩余输出时遵循 native headless/恢复流程）。焦点交给 native 选中的 surviving monitor/tile。重新接屏首版恢复单 tile，不自动凭 monitor 数字 ID 重播旧 topology；持久化属于后续功能。分辨率/旋转变化保留权重重新分配；若最小宽度放不下，确定性地合并非焦点 tile，使其 workspace 隐藏，并保留 focused workspace。

旧 IPC `activeworkspace`、`monitors.activeWorkspace` 和 ext-workspace active 位继续表示该 monitor 的 focused tile workspace；普通 bar 会看到唯一 active，其他可见 workspace 可能显示为 inactive，这是明确的兼容语义。不能轮流写 active 指针让 bar 看似多个 active。焦点换 tile 时发一次与新 active 相符的 native 事件，并发 tiles generation 事件；拓扑内多个可见关系由 tiles_state 提供。

有 tiles 时禁用 `bar_single_mission_control` 对多 workspace 的临时重命名并给诊断原因，避免覆盖真实名字/身份或误导 bar。rename workspace 不改变 tile binding。窗口规则、新窗口 placement、空工作区清理、persistent workspace、monitor workspace 规则均需以插件 topology 查询收敛；可见空 tile 不能无声消失。

新窗口默认进入 focused tile 的 workspace；显式 workspace/window rule 仍先决定真实归属。rule 将窗口放入另一可见 tile 时不移动其 workspace，是否自动聚焦遵循原生规则；若接受聚焦则同步 focused tile。落在隐藏 workspace 且规则要求 silent 时保留隐藏，不借创建窗口改掉当前 tile。workspace 规则强制 monitor 归属与 tile 请求冲突时返回错误，禁止先迁移再回滚规则的循环。

## 10. 分阶段实施与停止条件

设计已审，按以下阶段编译、测试和单独本地提交：

1. 更新本设计并建立默认 pkg-config 构建/既有测试基线。
2. 纯逻辑 geometry、binding、history、generation、事务、最多三个 tile、add/resize/remove/focus 与随机序列不变量。
3. 统一 broker、CSpace 多视口、可见性/clip、frame callback、hit test/raw focus、Lua/legacy API 和 tiles_state。
4. string/typed actions、resolver/history、strip 路由、tile swipe，原生多屏 focus/swap、跨 tile 移窗与方向焦点。
5. 拖缝、Esc 回滚、最小宽度、swipe 互斥。
6. 复用 StageEdgeEffect 的 blur/fade 与硬裁剪降级诊断。
7. overview/ws-only/Stage、fullscreen、special/pinned、多显示器与生命周期回归。

不 push、不 reload 当前会话；只在独立 nested/headless 会话验证。纯插件某路径如确实无法实现，记录符号/调用链/隔离实验等证据后继续其余阶段。硬件输入、HDR 真机视觉未验证项标记 PENDING。

## 11. 测试方案与证据分级

### 11.1 无 compositor 的模型测试

新增独立 tiles logic 测试：多种宽度/权重/NaN/Inf/越界参数、add/remove/resize 交错、边界钳制、物理像素覆盖、named/typed identity/rename/destroy、presented target focus/swap、previous/back-and-forth、generation stale callback、事务失败与回滚、空 workspace lease、分辨率缩小/断屏迁移。固定种子的随机操作序列每步检查 §4 不变量，不能只镜像实现重写一遍。

### 11.2 隔离 compositor 的合约与输入测试

使用与构建 revision 一致的独立 nested/headless Hyprland 和私有 IPC 环境，不接管当前会话。不自动假设 headless 支持所有 GL/CM/手势能力；不可用项记 SKIP 并说明。

以 A/B 两个 tile，workspace 1/2 和隐藏 workspace 3/4 为基础，覆盖：

| 测试组 | 操作 | 必须观察的断言 |
| --- | --- | --- |
| 全入口路由 | 键位、string/typed Actions、Lua focus、monitor:set_workspace(...) 对象入口、IPC、Hymission strip、overview 内部切换、直接窗口激活 | A:1→3 时 B 的 workspace/窗口盒/相机不变；focus B 后切 B；读取 active 投影与完整 topology |
| 冲突/跨屏 | 目标是 B 的 2、其他输出可见/隐藏 workspace、on-current-monitor、显式 swap/move | 已呈现目标聚焦对应 tile；on-current-monitor 交换绑定；管理操作按事务完整补源空缺；无克隆/丢失窗口 |
| 帧/交互 | 两边运行动画客户端，改变非焦点 tile surface，popup/subsurface/浮窗/X11/空白/touch/tablet | 两边持续出新帧、无误 suspend；画面 clip 与命中一致；button press/release 配对、约束不绕过 |
| 手势 | begin/update/commit/cancel、反向、阈值、forever/create-new、过程中外部 focus/resize/destroy | 只有绑定 tile 平移；取消不损坏其他动画；无双 controller 提交、无 stale session |
| fullscreen 等 | 两边各 fullscreen/maximize、允许覆盖浮窗、layout-handled fullscreen、special 开闭、pinned/unpin | 全屏限定自身 tile；shell 策略一致；special/pinned 只一份；关闭恢复 focused tile |
| 生命周期 | 删除活动/空 tile/最后 tile、停用、客户端退出、移屏、断屏、hotplug、scale/transform 改变 | workspace/窗口可达、无越界指针和 orphan、active/visible/suspended 一致；最后 tile 删除报错 |
| overview/Stage | onlycurrentworkspace/all、空 strip-only、stage 开关/左右翻转、ws 切换/跨 tile drop、途中退出或断屏 | binding 稳定；退出不隐藏其他 tile；flight 落在正确 viewport；scroll camera 只改变所属 workspace |

对象入口已在目标 `LuaMonitor.cpp` 核对：`active_workspace` 是查询属性，`set_workspace` 是方法；测试必须覆盖方法内部先走 placement、再走 monitor 切换的顺序，不能误把它当成只读属性测试。

### 11.3 渲染与性能测试

扩展现有 `tools/stage_edge_render_test.cpp`、native framebuffer contract/snapshot color tests（实际目标名以 `CMakeLists.txt` 为准）：用不同纯色/透明 pattern 的邻接 tile，检查越界像素、fade/blur 轮廓、damage 洞、closing fadeouts、fractional scale、旋转、scratch/mirror 格式切换及失败硬裁剪。测试重用 effect 的同一路径，增加多 viewport/嵌套 lease 场景，不另造 shader。

记录 1/2/3 tile 在同总窗口数与增加可见窗口数两种负载下的 CPU/GPU 帧时间 p50/p95/p99、missed frames、VRAM/附件数量、静态帧请求数；另测 scrolling 视频、resize 和 overview。检查 direct scanout 被正确退出/恢复。性能门槛在隔离基线完成后、功能优化前固定，不能事后根据结果移动标准。

完整编译验证运行仓库 CTest 和开发 flake build；native-symbol/GL 目标未注册或 skip 不能计作通过。真实 HDR/SDR 混合显示、10-bit/FP16、截屏 mirror、硬件触控板 swipe、fractional scaling、VRR/tearing、锁屏与 pointer lock 仍需隔离硬件会话验收。构建、软件像素检查、隔离运行、真机输入与视觉证据分开记录。

## 12. 风险与实施记录

最高风险仍为原生 active/visible 假设、同步事件重入、私有 ABI 漂移、双渲染/动画、CM 附件错误、tile 外输入泄漏及原生多屏规则语义不一致。实现必须完整签名唯一匹配、必需 hook 安装失败整体回滚，默认关闭时保留现有路径。

实施进度写 `.tiles-progress.md`，该文件不提交；各阶段单独提交并记录测试结果。已采纳的新语义优先于早期讨论：最多三个 tile；可见目标聚焦/显式 swap；monitor 风格条目；方向焦点和相邻移窗；删除后 workspace 隐藏可达。

### 12.1 实现收敛点

调用方先验证 draft/generation，`TilesController::publish` 验证全部移动成员，调用原生窗口迁移，再一次发布 topology 和 active 投影；tile API、overview 移窗、Stage 移窗与拆分共用此入口。预览阶段不创建 workspace。每个 output 保留空 workspace 的生命周期引用，移除 output 前先标记失效，避免原生断屏迁移期间给正在删除的 output 补工作区。

hook broker 为每个 lease 提供独立 continuation，原生重入从链头开始，preview helper 在 hook 外调用 original 也从自己的后继开始。内部借用 ABI 参数，仅在调用真正 subscriber/native 函数时构造按值参数；包含 shared ownership、右值引用与优先级重排的回归测试。

fullscreen 的 set/sync 都使用 tile allocation；layout-aware scrolling fullscreen 及相机几何仅在相应 workspace 调用作用域内提供 tile box。原生鼠标处理的 fullscreen 查询同样按指针所在 tile 作用域解析，不临时改写全局 active workspace。TOP layer 按未被 fullscreen 覆盖的 tile 裁剪，pinned/special 仍为物理 monitor 级。

topology generation 变化会使旧拖拽失效，并通知 Stage/overview 重建分组；overview 的选中放大、邻居避让与命中都限制在同一 tile。strip 预览裁取本 tile 的原生 surface 视区，避免把完整物理 monitor 压成窄条。边缘渲染继续只使用 Stage 的原生 pass capture/effect 服务。

设计中的硬件输入、HDR/混合输出、性能预算及协议边界是验收要求，不能因软件隔离测试通过而视为已完成；实际覆盖与 PENDING 项逐项列于验收记录。


## 2026-10-17 真机反馈后的交互修订

多 tile 时，Stage sidebar 和 workspace strip 的所有列都集中在物理输出最左侧，
每个 tile 一列，顺序与 tile 的水平顺序一致。列的点击、跨列窗口/组拖放及拖出 split
仍使用原 tile ID 和统一交互 owner。Stage 按完整显示器计算卡片比例和共同大小，限制总列宽；所有 tile 使用同一个
整体桌面缩放和平移，不再只压缩被左侧列带覆盖的 tile。
多 tile 下不再使用 Smartisan 左右交替列的位置。单 tile / capability 关闭保持原行为。

布局 work area 继续避开 bar 的 reserved area；渲染裁剪独立于布局区域，native
桌面内容的垂直装饰/阴影可扩展到输出边缘，overview 和 Stage 预览则使用自己的
目标坐标和预览 clip，不叠加 native tile clip。保留旧版顶部保护及回归测试。

多 tile workspace 切换采用原生 workspace 动画配置（方向、时长、曲线、style），
只把水平/垂直行程映射到当前 tile 的范围。窗口在固定 tile viewport 内移动，其他
分块不参与。swipe 的 commit/cancel 从当前预览位移连续收尾，不先跳回桌面或重新
从整屏边缘启动动画。通过中间帧状态采样和私有 compositor 截图核实。

all / forceall 在每个物理输出内统一排列所有 workspace 的窗口，恢复全屏 overview
的布局与命中语义；不按 tile 分组窗口。仅 onlycurrentworkspace（ws overview）把每个
正在呈现的 tile workspace 分别排在其 tile 区域。strip 的左侧多列归属不随这两种
窗口布局范围变化，点击仍只切换对应列的 tile。


## 第二轮真机反馈后的呈现规则

多列 workspace strip 共用一次整屏左侧玻璃背景绘制，延伸到顶部 reserved 区域；
列间不画背景接缝或分隔线；第三轮反馈取消 focused 列的整列描边，仅保留 workspace 自身的 active/selection 标记。

Tile live scene 仅借用 WORKSPACE_WINDOWS 的窗口选择，renderWindow 使用原生
MONITOR presentation 的位移和 alpha；触控板事件经过 InputManager 和完整手势匹配
路径验证，不能用仅改变动画变量的测试代替实际画面跟手验证。

all overview 中，其他 tile 正在呈现的 workspace 与 focused workspace 一样保留当前
实际屏幕起点和终点，不能应用隐藏 workspace 的 offscreen 规则。

多 tile 的 Stage workspace 卡片用统一的完整显示器宽高比、卡片宽度和高度，
不能按各 tile 的宽高比拉长卡片。第三轮反馈后，窗口按所属 tile 的 desktop viewport
等比居中映射到卡片，保留 workspace 内部相对位置，不能携带 tile 在整屏中的横向偏移；
背景铺满共同的显示器比例卡片，拖放使用对应的等比映射。桌面部分的
所有 tile 共用缩放和平移，输入命中、拖放预览、分割线命中和焦点 warp 使用同一变换
的逆映射。tile 的持久 allocation/topology 不变，Stage 状态另暴露 desktop_geometry。
单 tile 与 capability 关闭继续使用既有 Stage 几何与动画路径。


## 第三轮真机反馈后的输入与退出规则

空 tile 的指针 hover 按原生 mouse_move_focuses_monitor/refocus 规则发布焦点 workspace，
普通 workspace dispatch/swipe 因此以该 tile 为上下文；Stage 使用同一个桌面逆映射。
overview、special workspace 和进行中的拖动保留既有 owner，不由 hover 改写上下文。

ws-only overview 内切换 tile workspace 时，目标 binding 的原生 renderOffset 和 alpha
必须同时恢复到可呈现状态；只跳过原生切换动画而保留隐藏 workspace 的屏外位移会造成
退出后空白。桌面正常切换保留原生动画路径。Stage 下 scrolling 的非覆盖 fullscreen
和 work-area 采用同一缩放几何，保留 reserved 与 CSD 顶部空间。

原生 move 拖动可以跨 tile 边界绘制；原生 controller 是唯一移动 owner。
松手后先完成原生分组/平铺恢复和 Stage sidebar 落点处理，再对未消费的桌面落点验证
目标 tile/workspace 绑定，通过现有 activation 事务移窗。Esc 取消、pinned、special、
resize 和 capability 关闭不提交迁移。ws-only overview 拖入空 tile 时以其 desktop
作为 fallback drop target，窗口和整组继续使用现有 OverviewDrag/settlement 事务。


## Stage 多窗口截图复查

用户确认：卡片保持完整显示器的比例，只展示所属 tile 当前可见区域。
卡片中的 tile viewport 等比居中，窗口绘制、blur/fade、capture clip、拖放和
Stage/overview flight 端点均使用这块映射后的 viewport；不能把卡片两侧留白
当成 scrolling tape 的额外可见范围。左侧第二/第三列的输出 clip 仍是物理显示器，
不能使用原 tile 的 desktop 区域来裁剪已经集中到最左边的卡片。

scrolling camera offset 与 inhibition offset 是 viewport 的逻辑像素距离。
tile 分割/调整、Stage 开关和关闭 tiles 导致 work-area 主轴尺寸改变时，
在 native -> tile geometry -> Stage reservation 的最终 work area 外层按尺寸比例
重标定这两个 offset，保留相同的相机位置，避免多窗口全部落到缩略图之外。
默认关闭没有该 hook；普通单 tile 路径不额外取得 viewport 所有权。


## Stage 绘制所有权与性能修订

Tile 的 live renderWindow viewport subscriber 固定在 Stage 外层（priority 20），
由一个 pass capture 负责最终 tile viewport。嵌套 capture 不再次包装 pass；
柔边效果在已有 backdrop 作用域内不复用同一 scratch framebuffer。
完整 scrolling tape 的窗口选择必须在实际 tile/卡片视区再次剔除不可见列，
使用包含 CSD/阴影的边界以及原生 workspace/floating 动画位移，不裁掉顶部装饰。
隐藏 workspace 仅为实际可见预览维持 liveness。稳定多列 sidebar 的 Stage pass
使用各列边界的并集，drag/flight/overview 保留覆盖相应动画区域的边界。

stage_show_active=0 对每个 tile 均生效，已在桌面呈现的 workspace 不再重复出卡片；
stage_show_active=1 保留显示 active 卡片的行为。空列保留共同列宽和水平顺序，
可以作为对应 tile active workspace 的拖入目标，不因没有卡片而错投左侧 tile。
