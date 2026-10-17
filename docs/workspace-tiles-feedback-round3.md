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
