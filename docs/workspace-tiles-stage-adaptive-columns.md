# Stage 多列共用自适应尺寸

用户明确：是多个分块合起来理解为一个显示器场景；进一步要求沿用自适应，以 workspace
数量最多的分块为基准，不能把“精确 16:9”或“每列一张 16:9 卡片”作为尺寸目标。

前版把每个 tile 都分配了一张完整显示器比例的卡片，再把窄 tile 画面居中放入。
两个等宽 tile 时，这会重复整屏宽度预算，并在每张卡片内产生大量透明留白。
旧测试明确接受了这种留白，因而通过不能证明用户要求已满足。

## 实现规则

- 取同一物理显示器各列实际显示卡片数的最大值 M，计算可用高度扣除上下 padding 和
  M-1 个间距后的适应缩放；继续遵守 stage_card_min_width / stage_card_max_width。
  默认输出相对最大宽度仍是上限，卡片多时会继续缩小；最小宽度约束导致放不下时滚动。
- 所有列沿用同一个缩放、高度和纵向间距。列宽按 tile 的可用宽度占比拆分。
  workspace 少的列不会单独放大，空列保留宽度和拖放目标。焦点不决定尺寸。
- workspace 增删会更新 M，并让所有列同步调整。列 padding 和 desktop gap 只按实际
  面板布局计入 reservation，不把整屏卡片宽度乘以 tile 数。
- 窗口仍只展示该 tile 当前可见区域，等比缩放，不人为把单列套进显示器比例留白框。
  bar 的 reserved area、padding 和 gap 正常参与布局，外框不强求精确 16:9。
- 桌面继续整屏共同缩放和平移；各列共享完整 scene 的 reservation。
  overview exit 使用准备好的新 reference count、列宽和 reservation，避免读旧 live
  列的几何造成右侧卡片/flight 一帧跳动。
- 单 tile 和 tiles 关闭保持原 Stage 布局路径；拖拽 owner 与 activation 事务不变。

## 隔离回归

release compositor：e5a03b6a5ce64ec4c85954649bad4df083627574。
所有输出留在忽略目录 `build-tiles/stage-feedback6/`，不提交截图或构建产物。

- `negative.log`：缓存的 0cd6236 插件在新合并场景比例断言失败，重现旧布局。
- `--feedback3-regression stage-adaptive`：一列 8 张、另一列 1 张，最长列适应可用高度，
  短列同尺寸；切换焦点尺寸不变；删除多余 workspace 后所有列共同放大。
- `--feedback3-regression stage-multi`：HiDPI、顶部 reserved=58、scrolling .85、多窗口，
  实际像素可见且卡片没有人工 letterbox；40%/50% tile resize、三次 Stage 开关恢复，
  第二列 all overview 起点正确。
- `--feedback2-regression stage`：不等宽三个 tile，共同卡片高度、按 tile 分配列宽、
  中间帧整屏变换、真实窗口尺寸和输入逆映射；包含空 tile。
- `--feedback3-regression stage-perf`：继续断言无嵌套 capture、无 framebuffer 别名、
  绘制工作量有界，并验证空列跨列拖入。

这里只把私有 compositor 的几何、像素、输入和采样视为隔离证据，真机桌面仍需用户验收。
本轮没有操作本机 UI、reload、hyprpm update 或 push。

## 最终验证

- 本地完整 CTest 16/16 PASS、无跳过：`ctest-final.log`。
- 指定 release 的完整 integration smoke：`confirmed-full.log` PASS；包含 Stage
  跨列/组/桌面拖放、拖出分屏、取消/Meta、overview 和两输出等既有路径。
- 默认关闭回归：`confirmed-disabled.log` PASS，观测 JSON 与参考
  552bd339fe4809e0a031d277ca97f4dac6d0a5da 完全一致。
- 最多 workspace 基准专项：`confirmed-adaptive.log` PASS。
  8 张与 1 张时，两列均高 195 逻辑像素、宽约 179.35；删除多余窗口后为 2 张与
  1 张，两列均高 334、宽 307.2。测试在 6144x3456 / scale=2 / top reserved=58
  下执行，采集实际客户端截帧；切换焦点不改变共同尺寸。
- 不等宽三 tile 的共同变换/输入中间帧：`final-scene.log` PASS；多窗口 scrolling、
  resize/开关/第二列起点：`final-multi.log` PASS；all overview flight/alpha：
  `final-overview.log` PASS；无嵌套/别名和空列拖入：`final-perf.log` PASS。
- 四指跟手五帧、ws-only 切换退出、顶部 CSD、原生跨 tile 拖动和上一轮手势结束
  生命周期回归均 PASS：`gesture.log`、`workspace.log`、`top.log`、`drag.log`、`lifecycle.log`。
- 刷新 flake input 后 upstream main 仍为
  8010b4e5c66f97aa82cd5b3332b4ec613779b9a7，lock 无变化。
  最终 Nix 构建 PASS（`nix-confirmed.log`，4 cores / max-jobs=1）；Nix 内 14 PASS，
  2 GL 因 EGL 不可用跳过，本地相应两项均已通过。
- 私有 configerrors 为空；`git diff --check` 通过。本轮截图、采样及构建产物不提交。
