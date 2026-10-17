# Stage 多窗口截图复查

输入截图：Screenshot-2026-10-17-190531-kitty-SuperPower-Plugin.png（只读，未提交）。
用户确认的呈现规则：**保持显示器比例，只展示该 tile 当前可见区域**。
本轮继续纯插件、默认关闭、本地提交，不 push、不 reload。

## 根因与修复

1. 第三轮只有每 workspace 一个窗口的缩放用例，未覆盖带非零相机偏移的 scrolling tape。
   viewport 已缩小，但 camera offset 仍按旧宽度计算，三列窗口在某些卡片中全部跑到左边。
   旧版负对照 workspace 501 的三个预览 x 约为 -724、-459、-196，卡片却在 x=20。
   新增 recheckWorkArea broker 外层（priority 20，Stage 为 10，Tile geometry 为 0），
   根据最终 work-area 主轴尺寸变化同步缩放 camera offset 和 inhibition offset；
   覆盖 tile 调整、Stage 及 tiles 关闭的恢复，不额外创建动画 owner。
2. 修正相机后，旧 clip 仍允许 tile 当前视区以外的相邻列画进卡片横向留白。
   单独相机修复的负对照在留白采到了绿色客户端像素 (0,255,0)。现在按 tile desktop
   到卡片的同一等比居中变换计算 workspace viewport，并供窗口/边缘效果/capture、
   Meta 拖动和 drop/overview flight 使用。卡片背景及宽高比维持显示器比例。
3. 第二列 sidebar 的 overview 端点曾与其原 home tile 相交，实际 sidebar 已在整屏左边，
   因此 clip 会空。现在与物理输出相交，端点仍受上述 workspace viewport 限制。
   回归测试验证隐藏 workspace 504 从第二列实际缩略图位置进入 all overview。

## 隔离证据

复现配置为 6144x3456、scale=2、顶部 reserved=58、scrolling .85、
fullscreen_on_one_column=true、niri_mode=0。四个 workspace，每个三个真实 Wayland
客户端。测试入口：

```sh
python -B tools/tiles_isolated_smoke.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --feedback3-regression stage-multi --output build-tiles/feedback4-stage
```

- 原版本负对照：`feedback4-stage-negative.log`，有内容的 workspace 全部处于视区外。
- 仅相机修复负对照：`feedback4-stage-camera.log`，相邻列漏进卡片留白。
- 最终 `feedback4-stage.log` PASS：实际客户端像素可见、留白无相邻列、三次 Stage 开关
  后所有客户端位置/尺寸恢复误差不超过 3 逻辑像素；40%/50% tile resize 后仍可见，第二列 overview 起点正确。
- 截帧与状态：`build-tiles/feedback4-stage/stage-multi-scrolling.png`、
  `stage-multi-state.json`、`stage-multi-native-before.json`。

这些是私有 compositor 的重现和像素证据，不能代替用户真实应用及物理桌面验收。
没有向本机会话注入输入或更改配置，没有执行 hyprpm update/plugin load/unload。


## 最终检查

- 完整本地 CTest 16/16 PASS，无跳过：`feedback4-final-ctest.log`。
- 指定 release 二进制 `~/.local/lib/hyprland-hdr/Hyprland` 的完整 integration smoke PASS：
  `feedback4-final-full.log`。该二进制为 e5a03b6a5ce64ec4c85954649bad4df083627574。
- `tiles_disabled_regression` PASS：`feedback4-final-disabled.log`；观测 JSON 与参考
  552bd339fe4809e0a031d277ca97f4dac6d0a5da 完全一致。
- ws-only 双侧切换 + Stage 开关 + close/Esc、all overview 中间帧、顶部 CSD 像素和
  四指垂直真实私有 pointer 手势路径回归 PASS：`feedback4-workspace.log`、
  `feedback4-overview.log`、`feedback4-top.log`、`feedback4-gesture.log`。
- 按 AGENTS.md 刷新 flake input，upstream main 仍为
  8010b4e5c66f97aa82cd5b3332b4ec613779b9a7，lock 未变；最终 Nix `.#hymission`
  构建 PASS：`feedback4-final-nix.log`。Nix 内 14 PASS、2 GL 因 EGL 不可用跳过；
  这两个 GL 测试已在上述本地完整 CTest 中通过。
- 所有最终隔离测试 configerrors 为空，`git diff --check` 通过。


## Stage active 卡片配置补修

恢复 stage_show_active=0 在每个 tile 下的含义，已在桌面呈现的 workspace 不重复出卡片。
空列仍保留共同的 band/card 几何和水平位置，count=0、maxScroll=0，允许从另一列拖入
该 tile 的 active workspace。需要 active 卡片参与拖出分屏的集成 fixture 显式设
stage_show_active=1。性能专项同时覆盖隐藏 active 卡片与空列拖放，完整 smoke 保留
显式显示 active 卡片时的既有交互。
