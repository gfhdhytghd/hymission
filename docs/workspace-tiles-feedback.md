# 2026-10-17 真机反馈修复

## 1. 屏幕左侧集中列

Stage 和 strip 的各 tile 列按水平顺序集中到物理输出左侧。总列宽受最窄 tile 限制，
Stage 桌面只扣除与整列带重叠的部分，其他 tile 不重复留白。修正列命中、绘制 clip、
overview 退出端点与 hold region；已有跨列拖放、Meta 桌面拖放和 split smoke 继续通过。

## 2. 顶部裁切的历史与回归

查阅 `git log --all --grep='clip\|top\|reserved\|bar'` 后，相关历史修复为：

- `21fdcdd`：native 桌面渲染保留输出的完整垂直范围，work area 仅控制布局，不能裁掉
  顶部/底部的 CSD、阴影及装饰。卡片/overview 的目标 clip 保持独立。
- `0d445ab`、`4eb92ef`：overview 布局先扣除 layer-shell 的 reserved area，再应用
  用户外边距，避免预览躲到 bar 后面。

Tiles 的外层 renderWindow hook 把 Stage nativeDesktop 的 work area 与整个 tile
做四边交集，再把这个显式 clip 交给 captureWindowPasses；因此内层旧版的
`desktopShadowViewport` 即使扩大垂直范围，仍被外层 crop 重新截掉。scrolling tile
还给上下边界增加了非原生的淡出带。

现在 nativeTileClip 只采用 Stage 桌面的横向边界，纵向保持 tile 的完整输出高度。
scrolling 渐变只保留左右方向；Stage 预览不再叠加 native tile 的布局 crop。
overview 的 reserved-aware 布局继续保留。几何回归覆盖非零 monitor 坐标及
0/80/160 顶部预留区，断言顶部和底部渲染范围不随 work area 收缩。

新增真实 Wayland 测试客户端，红色 CSD 头部与绿色主体可在截图中区分。私有
compositor 设置顶部 80、底部 24 的实际 reserved area，分别检查 tiles 关闭/开启时
native、Stage 桌面、Stage 卡片、all 和 ws overview 的红色顶部。无 push/reload。

## 3. 原生 workspace 动画与 swipe 中间帧

受控负对照在启用 Stage 的多 tile 场景中复现了 offset/alpha 被瞬间清零：Tiles 已启动
原生动画，Stage 的 deferred sync 又启动自己的 flights 并 warp 相关 workspace。
多 tile 现在只保留原生 workspace 动画，不建立 Stage flights；新加入的隐藏 workspace
先恢复可见 alpha/offset，避免沿用它上一次 native OUT 的透明/整屏位移。

继续调用 native startAnimation 取得 In/Out 时长、曲线、style 和回调；缩放行程时保留
原生 gaps_workspaces 的逻辑距离与百分比 style，方向包含 workspace_wraparound。
internal change 不自行启动动画。swipe 预览恢复 source/target 可见 alpha；commit 和
不足阈值的 revert 从松手时的当前 offset 收尾，取消会还原原有动画/alpha。邻区不参与。

私有 release compositor 状态采样：与 capability 关闭时的原生基准比较 slide、
slidevert、slidefade 百分比和 fade；另测反向与 wraparound。记录每 20ms 的
value/begun/goal、percent/curve_value、曲线名和 speed，验证中间帧、邻区 alpha/offset
及 Stage 无重复 flights。对 swipe end 前后在同一 Lua 调用内采样，验证 commit/revert
的首帧连续性，随后采集收尾过程。

## 4. all 与 ws overview 的范围

`all` 是 `forceall` 的显式别名。两者使用物理输出的统一窗口布局、选中放大范围、
点击命中和预览裁剪，不再用窗口原来的 tile 限制整屏预览。仅有效
`onlycurrentworkspace` policy 启用每 tile 窗口布局。状态输出新增
`window_layout_scope`；`tile_groups` 仅表示 ws 布局组，窗口 `tile_id` 仍可表示原归属。

strip 无论 overview 范围如何，都保持整屏左侧的每 tile 一列；整屏窗口布局只在左侧
为这整个列带留白。进入动画同样从整屏左侧滑入，不受原 top/right anchor 影响。
设计正文和最新修订同时更新，避免保留相互冲突的布局规定。

隔离 smoke 同时覆盖 `all` / `forceall`、left/top 配置、窗口跨原 tile 边界后的布局和
拖动命中、ws 窗口仍限制在原 tile、空 ws 组及 strip 点击仅改变对应 tile。
查看了 `feedback-scope/stage-three.png`、`overview-left.png`：三列集中在左侧，all
预览实际画到原 tile 外面，未再被原边界截断。

## 最终验证与证据范围

前三项提交：`2b778fa`、`1cd1428`、`0b8cbba`。第 4 项与此报告一起单独提交。
最终产物的验证命令（均在此 worktree；输出在忽略的 `build-tiles` 目录）：

```sh
cmake --build build-tiles -j 6
ctest --test-dir build-tiles --output-on-failure
python -B tools/tiles_isolated_smoke.py \
  --hyprland /home/wilf/.local/lib/hyprland-hdr/Hyprland \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --integration --output build-tiles/feedback-scope
# 同上，分别把 --integration 替换为 --top-clip-regression / --animation-regression：
# --output build-tiles/feedback-final-top / build-tiles/feedback-final-animation
python -B tools/tiles_disabled_regression.py \
  --plugin build-tiles/reference-build/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --output build-tiles/feedback-final-disabled-reference
python -B tools/tiles_disabled_regression.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --output build-tiles/feedback-final-disabled-current
```

- 本地 CTest **16/16 PASS，无跳过**；`feedback-final-ctest.log`。
- release 集成 smoke PASS；`feedback-scope.log`。真实 compositor commit
  `e5a03b6a5ce64ec4c85954649bad4df083627574`，二进制 SHA256
  `b08dd5a78a0f8f04ae3936d77a4b99da105c97b53e5d975cdd4fdbd88dc0ced6`。
- `tiles_disabled_regression` 两次 PASS，参考版本
  `552bd339fe4809e0a031d277ca97f4dac6d0a5da` 与最终版的
  `default-off-observations.json` 解析后完全相等，两个 `configerrors.txt` 都为空。
- 顶部截图矩阵 PASS；`feedback-final-top.log`。这是最终版本的正向截图覆盖；
  旧版也通过了同一 CSD 截图矩阵，因此不能声称这些截图单独复现了原裁切缺陷。
  修复根因由历史代码追踪和 nativeTileClip 几何回归约束。
- 动画采样 PASS；`feedback-final-animation.log`。使用修复前的受控负对照插件
  `libhymission-animation-negative.so`，同测试在 slide 的中间帧数量检查失败，
  复现 Stage 清空位移/alpha；`feedback-animation-negative.log`。
- 测试仅启动私有 runtime 的嵌套 compositor，未接触本机活动会话，不代表用户真机验收。
- 按仓库要求刷新 `hyprland` flake input，仍是当前 upstream main
  `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`，lock 无变化。
  `nix --extra-experimental-features 'nix-command flakes' --store
  /home/wilf/.cache/hymission-nix build -L .#hymission --no-link` PASS，日志
  `feedback-final-nix.log`。Nix 中 14 项 PASS、2 项 GL 测试因不可用的 EGL 环境 SKIP
  （stage-edge-render、snapshot-color）；本地完整 CTest 的这两项均 PASS。

全部为本地提交，未 push，未执行 hyprpm update 或本机 reload/load/unload。
