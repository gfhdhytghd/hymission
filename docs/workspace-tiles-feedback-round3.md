# 第三轮真机反馈修复

## 1. 空 tile 的 hover workspace 上下文

原先只有窗口 focus 或鼠标点击能发布 focused tile；空桌面上没有 window focus 事件，
workspace 切换仍使用上一 tile 的兼容 active workspace。现在在原生 mouseMoveUnified
入口按 mouse_move_focuses_monitor/refocus 策略发布指针所在 tile，不 warp 指针；
Stage 缩放后的坐标使用同一逆映射，拖动/overview/特殊 workspace 不抢上下文。
`--feedback3-regression hover` 在真实空 tile 上移动指针再切换 workspace，通过。

## 2. ws overview 切换后的空白

复现到目标 workspace 身份、alpha 都正确，但 renderOffset 仍是 [-1920, 0]。
ws-only 路径不使用 all overview 的 workspace render-state 备份，切换绑定时又跳过
原生动画，因而保留了隐藏 workspace 旧的屏外终点。现在在 overview 内改变 tile 绑定时
同时归零 renderOffset、恢复 alpha；正常桌面切换继续使用原生动画。
`--feedback3-regression workspace` 验证左右两侧、Stage 开/关、close/Esc 共 8 种组合，
退出后采样 offset/alpha 并截取真实客户端像素，均通过。测试关闭自动搜索输入，
避免 fixture 的键盘信号发往已获得焦点的独立 GTK 搜索客户端。

## 3. 移除 strip focused tile 的蓝框

删除多 tile strip 的整列蓝色描边；继续使用同一个连续玻璃背景。
workspace 本身的 active/selection 标记及 tile 的 IPC focused 状态保留。

## 4. Stage HiDPI / scrolling 几何

按本机配置复现：6144x3456、scale=2、顶部 reserved=58、scrolling、单列 fullscreen、
niri_mode=0。旧实现把整屏 canvas 用作每张卡片的窗口投影原点，各 tile 分别挤在卡片的
左右半边；现在把 tile 自己的 desktop 等比居中放进统一的显示器比例卡片。背景仍铺满
整张卡片，拖动缩略图的缩放使用同一个变换。原生 scrolling 的非覆盖 fullscreen
使用 logicalBox() 高度；该路径现在也读取 Stage 映射后的 tile，work-area 从物理显示器
重建后只变换一次，避免重复 reserved/缩放。

`--feedback3-regression stage` 的旧版本负对照在卡片中心断言失败；修复后所有卡片
位置/高度、红色 CSD 顶部与绿色内容实际像素、两侧桌面窗口缩放范围均通过。
截帧：build-tiles/feedback3-stage/stage-hidpi-scrolling.png。
ws overview 的全部 8 种退出组合在 Stage 修复后再次通过。

## 5. 原生窗口与 overview 跨 tile 拖放

原生 drag controller 只认识物理 monitor，松手不把窗口迁往另一个 tile 的 workspace，
源 tile/Stage clip 还会裁掉跨边界的窗口。现在由同一个原生 controller 完成跟手移动、
分组与平铺恢复，move 拖动期间放开 tile/Stage 桌面边界。dragEnd broker 外层在原生及
Stage sidebar 落点处理后验证目标绑定，通过现有 moveWindow/publish activation
事务迁往落点 tile；被 Stage 消费的卡片落点不重复提交。Esc、pinned、special、
overview owner、resize 和默认关闭路径不提交此迁移。

ws-only overview 的空 tile 没有 thumbnail hit 区域，现增加 tile desktop fallback，
预览、窗口/组移动和 settlement 继续复用 OverviewDrag owner 与原事务。
`--feedback3-regression drag` 使用原生 dragBegin/mouseMove/dragEnd，验证平铺/浮动、
双向、Stage 开关、Esc 取消、已有窗口的目标及 ws overview 双向空 tile drop，全部通过；
原生拖动中截帧验证窗口在对面 tile 实际可见且未取得第二个 tiles 交互 owner。
旧实现负对照在松手 workspace 归属断言失败。

## 验证与交付边界

- 每项单独提交：d8a1d31（hover）、9076592（ws 退出空白）、4823f24（列蓝框）、
  e9d68cd（Stage）、ca768e5（跨 tile 拖放）。本文件及设计同步随各项/最终验证提交。
- `cmake --build build-tiles -j 6` 通过；完整 CTest **16/16 PASS，无跳过**，
  `build-tiles/feedback3-ctest.log`。
- release compositor：`~/.local/lib/hyprland-hdr/Hyprland`，
  e5a03b6a5ce64ec4c85954649bad4df083627574，SHA256
  b08dd5a78a0f8f04ae3936d77a4b99da105c97b53e5d975cdd4fdbd88dc0ced6。
  完整 `tiles_isolated_smoke --integration` 通过，`feedback3-full.log`。
- `tiles_disabled_regression` 当前/参考两次通过，解析后的 observations JSON 完全一致；
  参考插件 commit 552bd339fe4809e0a031d277ca97f4dac6d0a5da，
  `feedback3-disabled-{current,reference}.log`。
- `--feedback3-regression hover/workspace/stage/drag` 全部通过；hover 追加了 Stage
  缩放后的空 tile，panel 追加 ws-only focused 列边缘实际像素无蓝框断言。
  `feedback3-hover.log`、`feedback3-workspace-stagefix.log`、`feedback3-stage.log`、
  `feedback3-drag.log`、`feedback3-panel.log`。
- 既有四指真实私有 pointer 事件路径、中间帧 workspace 动画、all overview、3-tile Stage
  整屏缩放及顶部 CSD 像素回归全部通过：`feedback3-gesture.log`、
  `feedback3-animation.log`、`feedback3-all-overview.log`、`feedback3-stage-global.log`、
  `feedback3-top.log`。各隔离测试的 configerrors 为空。
- 按 AGENTS.md 刷新 hyprland flake input，upstream main 仍为
  8010b4e5c66f97aa82cd5b3332b4ec613779b9a7，lock 未变。
  Nix `.#hymission` 构建通过（`feedback3-nix.log`）；内置 14 项通过、2 项 GL 测试
  因无 EGL 环境跳过，本地完整 CTest 中两项均通过。

所有截帧、状态采样和负对照在 build-tiles 的私有 compositor 内，未向本机会话注入输入。
它们不代替物理触控板、实际应用内容或用户桌面验收；本机视觉验收待用户安全更新后确认。
截图未提交，未 push、未 reload，未执行 hyprpm update 或本机会话 plugin load/unload。
