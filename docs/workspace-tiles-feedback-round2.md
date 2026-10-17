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
