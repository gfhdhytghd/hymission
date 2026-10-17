# 第二轮真机反馈修复（2026-10-17）

## 1. strip 共用玻璃面板

原实现为每列分别提交 blur rect，彼此采样背景不同，focused 列整边描边又重合在
列接缝上。现在整个左侧 strip 只有一次背景 blur rect，覆盖物理输出完整高度，
包括 reserved 顶栏；缩略图布局仍避开 reserved area。focused 列只使用内缩描边。
单 tile 和关闭 tiles 的面板路径不变。

`--feedback2-regression panel` 在真实私有 compositor 截图，检查列间无背景突变，
且面板延伸到顶部预留区。修复前同测试在 header/background discontinuity 失败，
修复后通过。日志：`build-tiles/feedback2-panel{,-negative}.log`；
截图：`build-tiles/feedback2-panel/shared-panel.png`。

用户提供的 `.tiles-feedback-strip.png` 仅用于查看，保持未跟踪，不提交。

## 2. 四指垂直手势实际渲染

读取本机配置确认：`hyprland-main.lua:63` 注册四指 vertical `action="workspace"`，
workspaces/In/Out 样式为 slidevert。插件加载/配置完成时将原生 workspace 手势替换成
CHymissionWorkspaceTrackpadGesture，正常桌面最终经过 CWorkspaceSwipeGesture 和
unified swipe。不是 Super+四指 overview 或四指水平 layout scroll。

真正根因在绘制端：tile scene 使用 WORKSPACE_WINDOWS 仅选择一个 workspace，
但这个快照场景的 presentation 会主动清除 workspace offset/alpha。原先的动画状态
测试虽然有中间位移，实际窗口绘制仍没有位移。实时 tile renderWindow 现在恢复
原生 MONITOR presentation，然后继续在固定 tile viewport 内裁剪；预览/快照以及
单 tile、tiles 关闭路径不变。

新增测试 fixture 从 InputManager::onSwipeBegin/Update/End 输入四指触控板事件，
经过 EventBus、手势匹配、Hymission 包装层再到 unified；不直接调用 dispatcher 或
unified 来替代输入。属于隔离 compositor 的合成触控板事件，并非本机物理设备输入。

`--feedback2-regression gesture` 每次 40 delta，采集五个跟手状态和截图；断言垂直
位移、邻区静止，以及截图中 incoming 红色 CSD 头部真正按 delta 移动。修复前同测试
状态检查通过但渲染红色头部检查失败，修复后通过。另覆盖 tiles 关闭/单 tile 的
原有 Stage swipe；disabled regression 新增同一输入场景与参考插件做一致性比较。
日志：`build-tiles/feedback2-gesture{,-negative}.log`。

## 3. all overview 的可见分块端点

原来的 offscreen endpoint 判断仅比较物理 monitor.m_activeWorkspace，把非 focused
但已由 tile 呈现的 workspace 误判成隐藏区域。现在当前呈现的任意 tile workspace
都保留实际起点/终点；真正隐藏的 workspace 仍沿用原有屏外/Stage 卡片端点策略。
已有 alpha/visibility 恢复按当前 tile topology 进行，不退回 monitor 唯一 active 假设。

状态输出增加 animationFromGeometry/animationToGeometry/exitGeometry/workspaceAlpha，
便于采样核实端点与中间帧。`--feedback2-regression overview` 使用三个真实静态
Wayland 窗口，在 Stage 关闭/开启两种情况下检查起点等于实际桌面位置、打开中间帧
位于起终点之间、alpha 保持可见、退出终点回到原位及最终桌面几何不变。修复前同测试
复现 x=981 的已显示窗口瞬间改为 x=1952 的屏外起点；修复后通过。
日志：`build-tiles/feedback2-overview{,-negative}.log`，状态帧与截图在相应输出目录。
