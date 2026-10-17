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
