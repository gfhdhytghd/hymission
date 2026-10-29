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

新增测试 fixture 从私有 compositor 已注册 IPointer 的 swipeBegin/Update/End 信号
输入四指触控板事件，经 PointerManager、InputManager、EventBus、手势匹配、
Hymission 包装层再到 unified；不直接调用 dispatcher 或
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

## 4. Stage 使用整屏比例和共同桌面场景

原先按每个 tile 的 desktopWidth 分别计算卡片高度，只从左侧 tile 的 work area 扣除
集中列带，导致列间比例不一致，其他 tile 不压缩。现在按显示器决定统一卡片宽度，
所有 workspace 卡片都使用完整显示器宽高比。总列带最多占可用屏幕的一半，避免
多列耗尽桌面空间。每张卡片以该 workspace 的整屏画布呈现，保留其窗口在整屏中的
相对位置；窗口和缓存背景使用相同画布变换。

所有 tile 共享同一 Stage 桌面仿射变换：为总列带预留一次空间，横纵等比缩小，
向右平移并垂直居中；不再分别在某一个 tile 内扣除 sidebar 宽度。持久 allocation
和 workspace binding 不变，Stage JSON 的 desktop_geometry 暴露当前视觉区域。
对应的真实 tiled/floating work area、渲染 clip、fullscreen 限界、鼠标命中、焦点 warp、
分割线拖动和 Stage transfer 落点/预览一并映射；workspace 动画的行程和 gap 也进入
同一缩放空间。滑动期间裁剪在共同场景内，静止时继续保留原生纵向 CSD/阴影保护。
单 tile / capability 关闭保留原路径。

`--feedback2-regression stage` 用 1320/300/300 不等宽三分块，检查卡片均为 16:9、
高度一致、Stage 打开过程所有分块共享横纵缩放比例、真实客户端宽高同比压缩、
映射后的输入命中，以及 focused tile 为空且隐藏空卡片时仍使用共同场景。修复前在
卡片比例检查失败（约 1.083 / 0.278 / 0.278），修复后通过。
纯逻辑回归另覆盖非零/负输出坐标和顶部 reserved 区域，断言三块边界连续、右边界
固定和纵向居中。设计文档同步更新。

截图：`build-tiles/feedback2-stage/stage-global-{before,open,closed}.png`；
中间状态：同目录 `stage-global-frames.json`；负对照日志 `feedback2-stage-negative.log`。

## 最终验证

最终版本的运行记录都在忽略的 `build-tiles/` 下。测试使用本机指定 release compositor
`~/.local/lib/hyprland-hdr/Hyprland`，commit `e5a03b6a5ce64ec4c85954649bad4df083627574`，
SHA256 `b08dd5a78a0f8f04ae3936d77a4b99da105c97b53e5d975cdd4fdbd88dc0ced6`。
全部测试只启动自己的私有 runtime / 嵌套输出，不连接本机活动 compositor。

```sh
cmake --build build-tiles -j 6
ctest --test-dir build-tiles --output-on-failure
python -B tools/tiles_isolated_smoke.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --integration --output build-tiles/feedback2-full
# 同上分别运行：
# --feedback2-regression panel    --output build-tiles/feedback2-final-panel
# --feedback2-regression gesture  --output build-tiles/feedback2-final-gesture
# --feedback2-regression overview --output build-tiles/feedback2-final-overview
# --feedback2-regression stage    --output build-tiles/feedback2-stage
# --animation-regression         --output build-tiles/feedback2-final-animation
# --top-clip-regression          --output build-tiles/feedback2-final-top
python -B tools/tiles_disabled_regression.py \
  --plugin build-tiles/reference-build/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --output build-tiles/feedback2-disabled-reference
python -B tools/tiles_disabled_regression.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --output build-tiles/feedback2-disabled-current
```

- 本地 CTest **16/16 PASS，无跳过**；`feedback2-final-ctest.log`。
- release 完整集成 smoke、四项专项、原生动画和顶部截图回归全部 PASS；日志使用对应
  输出目录同名 `.log`。第 2 项同时包含原生事件路径中间状态与实际渲染像素验证。
- default-off 两次 PASS；参考插件 commit `552bd339fe4809e0a031d277ca97f4dac6d0a5da`。
  观测 JSON 解析后完全一致，配置错误均为空，包含新增四指垂直 Stage swipe 场景。
- 按 AGENTS.md 刷新 flake 的 hyprland input，当前 upstream main 仍为
  `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`，lock 无变化。
  `nix --extra-experimental-features 'nix-command flakes' --store
  /home/wilf/.cache/hymission-nix build -L .#hymission --no-link` PASS；
  `feedback2-final-nix.log`。Nix 内 14 PASS、2 GL 测试因 EGL 环境不可用跳过；
  本地完整 CTest 中这两项均 PASS。

这证明隔离输入/绘制路径和构建通过，不替代用户物理触控板及本机会话验收。
仅本地提交；未 push，未执行 hyprpm update、reload 或本机会话的 plugin load/unload。
