# Stage 卡顿与重复卡片复查

> 本文记录历史修复。卡片比例和多列尺寸规则已由 [多列共用自适应尺寸](workspace-tiles-stage-adaptive-columns.md) 更正：以 workspace 最多的一列为共同基准。


输入截图 Screenshot-2026-10-17-192056-kitty-SuperPower-Plugin.png 只读、未提交。
本机仅查询了现有状态；未打开/关闭 Stage，未注入输入、reload 或运行 hyprpm update。
延续已确认规则：显示器比例的卡片，只展示该 tile 当前可见区域。

## 实际发现

前次测试没有覆盖 Stage / Tiles hook 安装顺序引发的嵌套 pass capture。
Tiles 与 Stage 均包装了 renderWindow，Stage 后安装时可在自身 capture 内再进入 Tile
capture。隔离负对照的稳定帧每帧出现 7 次嵌套捕获、4 次效果 framebuffer 源/目标
别名复用。嵌套效果重复使用同一个 scratch，也会采到错误的背景。

另有两类额外工作：WORKSPACE_WINDOWS 选择了整个 scrolling tape，包括完全不可见的
列；多 tile Stage pass 使用整屏 bounds。卡片也忽略 stage_show_active=0，把已经显示
在桌面的 workspace 又列出来。本机只读状态同样包含 active workspace 1/6 的卡片。

## 修复

- Tiles renderWindow subscriber 固定 priority 20，在 Stage 外层决定 viewport；
  capture 加重入保护，edge effect 不在已有 backdrop 内再次取同一 scratch。
- native tile 绘制按完整窗口边界 + 原生 workspace/floating offset 剔除视区外列；
  卡片按实际拟合后的完整边界剔除不可见预览，并仅维持可见预览的 liveness。
  CSD、阴影和跟手动画继续沿用原生 presentation。
- 稳定 Stage pass 的 bounds 收到 sidebar 各列的并集；有拖动/flight/overview 时
  保留动画范围。仍使用原生 surface、HDR/镜像附件和原柔边 shader，没有降低缩略图质量。
- stage_show_active=0 恢复在单/多 tile 下的含义；显示 active 卡片的场景需显式设 1。
  空列保留统一宽度，可拖入对应 tile 的 active workspace。
- tiles_state 无改动；Stage state 添加累计 render_metrics，供隔离回归检查实际 capture、
  draw/cull、效果区域与 framebuffer 复用次数。draw_ns 是 Stage 回调 CPU 时间。

## 同场景前后采样

release compositor e5a03b6a5ce64ec4c85954649bad4df083627574，
6144x3456 / scale=2 / top reserved=58；两个 tile，4 个 workspace，各 3 个真实 Wayland
客户端，scrolling .85、单列 fullscreen、niri_mode=0。每次取 35 个状态样本。
基线为 1c344e6 加只读计数；比较稳定采样区间的计数增量除以 draw 帧数，排除初始化。

| 每帧指标 | 修复前（125 帧） | 修复后（124 帧） |
| --- | ---: | ---: |
| 嵌套 capture | 7 | 0 |
| 效果 framebuffer 别名复用 | 4 | 0 |
| 卡片 window draw | 12 | 4 |
| edge effect 调用 | 25 | 8 |
| 进入柔边效果的区域面积计数 | 32,887,548 | 8,208,106 |
| Stage draw CPU 回调耗时 | 0.827 ms | 0.319 ms |

效果区域面积约减少 75%。这是同输入下的绘制工作量和 CPU 采样，**不是 GPU 计时、
FPS 倍数或本机 HDR 会话的卡顿验收**。本机物理桌面需用户安全更新后确认。
原始数据在 `build-tiles/feedback5-perf-{before,final}/stage-perf.json`，
截帧在 final 目录的 stage-perf.png。前后 shader/blur 参数和显示器比例相同。

入口：`--feedback3-regression stage-perf`。断言没有嵌套/别名，视区外预览已剔除，
固定场景每帧 effect 调用及区域有上界；还验证相反 hook 安装顺序、active 卡片隐藏、
空列不坍缩，以及从另一列拖入空列而不新增 tile。原有拖出分屏/Meta 路径由完整 smoke 覆盖。


## 最终验证与提交

- active 卡片/空列修复：a819296；绘制性能修复随本记录另行提交。
- 本机只读核对：hyprpm hymission state 的 hash 为 1c344e6，已加载 maps 的
  hymission.so inode 与该缓存文件一致。因此本轮不是把更早构建误当成新版本。
- 完整本地 CTest 16/16 PASS，无跳过：`feedback5-ctest.log`。
- release 完整 integration smoke PASS：`feedback5-final-full.log`。
  含 Meta、拖出分屏/跨列、原生移窗、fullscreen/maximize、两输出和禁用恢复。
- 默认关闭回归 PASS，观测 JSON 与参考 552bd339fe4809e0a031d277ca97f4dac6d0a5da
  完全一致：`feedback5-disabled.log`。
- 最终性能专项 PASS（含工作量上界、两种安装顺序及空列拖放）：
  `feedback5-perf-final.log`。ws-only 双侧切换/退出、顶部 CSD 像素、四指垂直真实私有
  pointer 手势、all overview 中间帧回归 PASS：`feedback5-workspace.log`、
  `feedback5-top.log`、`feedback5-gesture.log`、`feedback5-overview.log`。
- 一次并行构建期间的 smoke 在 previous history 场景失败，未改产品 history 解析；
  重跑通过。该 dispatcher/history 场景现显式把真实指针放到测试拥有的左 tile，
  避免异步 native refocus 按未受控的 hover 位置改变上下文；最终完整 smoke 通过。
  原始失败和未固定指针的重跑日志仍在 `feedback5-full.log`、`feedback5-full-retry.log`。
- 按 AGENTS.md 刷新 hyprland flake input，仍为 upstream main
  8010b4e5c66f97aa82cd5b3332b4ec613779b9a7，lock 未变；Nix `.#hymission`
  构建通过（4 cores、max-jobs=1）：`feedback5-nix.log`。
  Nix 内 14 PASS、2 GL 因 EGL 不可用跳过；本地完整 CTest 中两项均已通过。
- 各最终私有 configerrors 均为空，`git diff --check` 通过。截图/采样不提交。

未 push、未 reload、未更改本机配置。隔离采样证明了重复捕获/工作量问题及修复，
真机 HDR 桌面的卡顿和应用视觉表现仍需用户安全更新后验收。
