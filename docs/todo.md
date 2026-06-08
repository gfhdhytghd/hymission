# Hymission TODO

这份 TODO 只按 `../hycov/README.md` 对照整理，不额外参考 `hycov` 源码实现细节。

## 相对 hycov 的缺口

| 功能 | hymission 当前状态 | 处理结论 |
| --- | --- | --- |
| 多 workspace overview 条带 + 跨 workspace 拖拽 | 已实现首版：strip 显示、点击切换、overview 内拖拽 drop；原生 `bindm ... movewindow` 集成和高保真缩略图未完成 | `In Progress`，见 [`workspace_strip_plan.md`](workspace_strip_plan.md) |
| overview 中右键关闭窗口 | 未实现；当前只有左键激活 | `TODO` |
| 触控板手势进入 overview / 在 overview 中手势切换 workspace | 已实现；支持官方 `gesture = ..., dispatcher, hymission:*` 和 overview-to-overview workspace swipe | `Done` |
| Alt-release / Alt-Tab 风格的“按住主键进入，松开主键退出”模式 | 已实现 toggle-only switch mode：仅影响 `hymission:toggle`，支持 `switch_toggle_auto_next` 和 `switch_release_key` | `Done` |
| 独立的 `movefocus` dispatcher（方向切换 / 循环切换） | 未实现；当前只支持 overview 内方向键导航 | `TODO` |
| 跨显示器的 `movefocus` | 未实现 | `TODO`，依赖独立 `movefocus` 语义先落地 |
| `forceall` / `onlycurrentworkspace` / `forceallinone` 这类 overview 范围切换 | `forceall` 和 `onlycurrentworkspace` 已实现；仅 `forceallinone` 未实现 | `TODO`，仅剩 `forceallinone` |
| special workspace 窗口纳入 overview | `show_special` 和 `forceall` 已支持参与 monitor 上当前可见的 special workspace | `Done` |
| 多显示器同时 overview | 已实现；各 participating monitor 独立布局和 backdrop | `Done` |
| 窗口关闭后自动退出 / 重建 overview | 已实现；窗口集变化会重建，结果为空时自动退出 | `Done` |
| 退出 overview 后自动 fullscreen / maximize | 未实现 | `Not Planned`，与 hymission “不重排真实窗口状态”的主设计不一致 |
| 退出 overview 后把 floating 窗口抬到最上层 | 未实现 | `Not Planned`，当前不想把 overview 变成真实窗口状态修复器 |

## 建议实现顺序

overview 右键关闭窗口

多 workspace overview 条带剩余收尾

独立 `movefocus` dispatcher
跨显示器 `movefocus`
`forceallinone`
