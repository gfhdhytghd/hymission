# 第三轮真机反馈修复

## 1. 空 tile 的 hover workspace 上下文

原先只有窗口 focus 或鼠标点击能发布 focused tile；空桌面上没有 window focus 事件，
workspace 切换仍使用上一 tile 的兼容 active workspace。现在在原生 mouseMoveUnified
入口按 mouse_move_focuses_monitor/refocus 策略发布指针所在 tile，不 warp 指针；
Stage 缩放后的坐标使用同一逆映射，拖动/overview/特殊 workspace 不抢上下文。
`--feedback3-regression hover` 在真实空 tile 上移动指针再切换 workspace，通过。
