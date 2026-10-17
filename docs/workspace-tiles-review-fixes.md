# Workspace tiles 审查修复验收

日期：2026-10-17。审查起点：`2ee664b`；分支：`feat/workspace-tiles`。
按审查项本地提交。纯插件、默认关闭、最多三个 tile 的约束保持。
没有 push、修改宿主配置、运行 `hyprpm update` 或对宿主插件 load/unload/reload。

## 修复对应关系

| 审查项 | 修复及本地提交 |
| --- | --- |
| 1. Stage 拖拽卡死 | `eb0b0c2`：release 收尾先于 rendering/input suppression 检查；overview 建立状态前取消 transfer，窗口关闭、monitor pre-remove、锁屏回调取消并释放 owner；`10f84dd` 补齐 source destroy listener 和锁屏路由准备保护；保留被消费 press 的 release 配对。 |
| 2. 单 tile 原生路径、动画 | `ed2730a`：单 tile typed actions/history 回到原生路径；reconcile 不再清零 render offset/alpha。多 tile 隐藏目标仍经过原生 action 的 pointer constraint/cursor 策略，再在 monitor 边界发布 tile 状态。入/出场使用原生 workspace animation 配置，横向位移按 tile 缩放，出场 surface 保留独立 clip/frame lease；swipe 取消恢复先前未完成的位移动画。`3d18293` 另修正全局 animations 开关读取。 |
| 3. detach/remove 后崩溃 | `8469b16`：最后一个 lease detach 时清空 originals，重新 attach 恢复；detach 后 continuation 调用已恢复的原生入口，remove 后旧槽位判空。Stage/overview 清空缓存函数，Stage preview 检查 hook/lease 是否有效。测试 mock 同样在 detach 时销毁 trampoline。 |
| 4. 逐帧符号扫描 | `c18bbbf`：可选 edge hook 的失败结果按 config generation 缓存；普通 release 不清掉失败缓存，reload 才允许重试。测试连续 10,000 次失败请求只执行一次 probe，并覆盖 release/reload。 |
| 5. OverviewDrag owner 泄漏 | `deee8c2`：close、beginClose 的所有提前返回、deactivate、析构均有清理兜底；Esc、拖动窗口关闭、monitor 变化立即释放。原有 release/drop、rebuild、capture suppression、group 变更及锁屏 deactivate 路径继续经过统一清理。 |
| 6. 签名匹配回归 | `3b3988f`：规范化空白，兼容模板 `> >`/`>>`；同地址别名去重。既有 Stage/overview/action hooks 先匹配规范化完整签名，再使用历史 qualified substring 回退；不同地址的重载歧义仍拒绝。Tiles 新增 hooks 保持完整签名要求。 |
| Stage 列内操作被抢占 | `3d18293`：press 只记录候选；越列或进入有效 split 区后才 beginTransfer。列内重排和 Meta thumbnail drag 保留各自路径，并与 transfer 移交同一 owner。tile 重排使用 release 清空 pressed 前的 workspace 快照；关闭 tiles 时保留原路径。 |
| 默认关闭的几何/语义 | `0188ed7`：card width cap 用物理 monitor 宽度；点击带延伸到物理输出边缘，与 cardHit 一致；screenWorkspace 实时读原生 active；overviewDestination 恢复 activeWorkspace 参数及原检查。`e57fc19` 验证实际应用了左右 reserved area。 |
| broker 热路径 | `d259fe3`：broker 内部借用参数，引用保留类别，平凡值转发；getTexBox/draw 的参数对象拷贝为零。独立基准测量结果见下文。 |
| ext-workspace | `d6694c3`：真实协议客户端验证可见 workspace 激活；它经原生 `CExtWorkspaceResource::commit → CMonitor::changeWorkspace` 到现有 tile hook，不需要第二个协议专用 hook。 |

后续完整运行还发现并修复两项边界问题：`95158b9` 让 scrolling layout 的
fullscreen 重算、查询和覆盖区域遵循 tile 几何；`3fce598` 修复拖拽预览在浮点
平移坐标下因舍入产生倒置 clamp 上下界的问题，并加入确定性回归。
`c1058e1` 保证 Release 构建也执行 broker 生命周期断言；layout 测试同时启用
标准库断言，防止原有未定义行为被静默放过。

## tiles_enabled=0 对照回归

参考源码取自本地 `origin/hyprland-master`：
`552bd339fe4809e0a031d277ca97f4dac6d0a5da`。使用 `git archive` 放入
`build-tiles/reference-src`，独立构建，没有修改该引用或其他工作树。
两边都在私有 nested Hyprland 中运行相同脚本、真实 simple-shm 客户端和输入 fixture。

```sh
python -B tools/tiles_disabled_regression.py \
  --plugin build-tiles/reference-build/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --output build-tiles/default-reference
python -B tools/tiles_disabled_regression.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --output build-tiles/default-current-final
```

**PASS：两份 `default-off-observations.json` 完全相同。** 覆盖卡片尺寸、桌面
reservation、卡片点击、输出边缘点击、overview 窗口几何、进入/退出阶段、点击另一 workspace 的 overview 窗口后的实际切换和 Stage 恢复。
第二种情形显式设置 `reserved={left=240,right=70,...}`，并断言 compositor 已应用
这些区域，避免配置未生效的假对照。该情形输出宽 1920、card width 为 384；修复前
使用剩余宽度计算上限的路径会得到不同结果。普通情形输出宽 1280、card width 为 256。

`overview::initFailed` 的额外清理是有意的失败收敛：action hooks 会把原生
workspace/fullscreen 操作转入 overview controller；必需 render/input hook 安装失败后
继续保留这些路由会形成部分接管。现在回滚该 controller 已获得的 leases、清空缓存及
action originals，恢复原生操作，并通知 tiles 禁用。broker 只移除相应 lease，不拆掉
其他 controller 的有效订阅。这不改变正常初始化后的关闭态行为。

## 完整测试与隔离运行

**PASS：默认 pkg-config Debug 构建，CTest 16/16，无跳过。** 包括 hook detach/
reattach/remove、失败缓存、签名空白/别名/歧义、40,000 次模型操作，以及原有软件 GL
与 native framebuffer contract 测试。日志：`build-tiles/review-final-ctest.log`。

**PASS：完整隔离交互**，命令沿用原验收的 `--integration`，最终产物目录为
`build-tiles/review-final-isolated`。新增断言覆盖：

- capability 开启但单 tile 时，typed 切换返回后以及后续 reconcile 后原生动画仍运行；
  多 tile 切换有动画且邻区 offset 不变。
- Stage 同列重排确实迁移窗口，Meta+点击进入原 thumbnail 路径，press 不抢占 transfer；
  越列/拆分后才显示 transfer，Esc/拖回取消不改变 binding。
- Stage 拖动中打开 overview 释放 owner；真实 source client 在拖动中关闭释放 owner；
  带真实 source client 的第二个 nested output 在拖动中删除后 owner 为零，存活输出
  的三个 tile 保持可达。
- overview 拖拽中直接 close 或 Esc 取消后 owner 为零；其他原有 Stage/overview、
  fullscreen、scrolling、pin/special、双输出 focus/swap/binding 和停用用例继续通过。
- 独立 ext-workspace Wayland 客户端激活另一可见 tile，只改变焦点，不移动其他 binding。

**PASS：ASan 插件的同一完整隔离测试**，包括真实 client 关闭和 output 删除。
目录 `build-tiles/review-asan-final`，没有 ASan 错误日志。
仅插件使用 ASan，native compositor/驱动未插桩；`detect_leaks=0`。
这不是完整 compositor 内存审计，也不是实际锁屏/触摸板设备验收。

## broker 热路径测量

```sh
c++ -std=c++23 -O2 -Isrc tools/hook_broker_benchmark.cpp \
  -o build-tiles/hook-broker-benchmark
build-tiles/hook-broker-benchmark
```

本次 mock 单 lease，5 组、每组 2,000,000 次调用。getTexBox 约 2.85 ns 原生 /
5.88 ns 经 broker，draw 约 0.65 ns 原生 / 3.26 ns 经 broker；参数对象复制计数为 0。
测的是内部函数分发，不包含真实 compositor patch、GPU 帧或整机负载，不能外推为
分块功能的帧时间验收。

非平凡原生 by-value ABI 参数仍在真正的 hook/native 调用边界各获得独立副本。
尝试将它们全部 move 的实验虽然通过纯 C++ 单测，却在真实隔离 Stage 运行时崩溃，
已撤回；负证据保留在 `build-tiles/perf-isolated`。不能拿该实验的复制计数宣称原生
ABI 安全。当前实现只对引用和已知平凡值执行零对象复制转发；复测完整隔离和 ASan 均通过。

## 上游构建与私有 ABI 风险

再次刷新 Hyprland flake input，解析结果仍是上游 `main`
`8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`，lock 无需变化。
本地 store 的 `nix build -L .#hymission --no-link` **PASS**：最终插件产物为
`/nix/store/k26nz30nl9ry1npf4y68vd3wj7smy3ny-hymission-0.10.2`。14 项通过，2 项 GL 测试
因 EGL/GLES 不可用跳过；这两项在本地 CTest 均通过。
日志为 `build-tiles/review-nix-build.log`。不声称该上游 revision 的 runtime 验收。

新 Tiles 代码的 `private public` 仅留在 `tiles_controller.cpp`，包围需要直接访问
work-area boxes 的 `CSpace` 和 native swipe session 状态的
`CUnifiedWorkspaceSwipeGesture`；ScrollingAlgorithm/ScrollingFullscreenHandler 已移出
该宏作用域。既有 Stage/pass/framebuffer 适配中的同类用法没有扩展为公共插件接口。
这些都是私有布局/ABI 依赖，完整签名匹配与构建不能证明所有字段布局长期稳定；升级
compositor 时仍需针对对应 headers/binary 复测。

物理 pointer constraints、锁屏协议的设备级输入、HDR/混合输出、VRR/tearing 和整机
CPU/GPU 帧时间仍按原验收记录标为 **PENDING**，未用本次软件对照或微基准替代。

## 提交后的最终复跑

上述文档和回归脚本整理提交后，再次运行并全部通过：

```sh
ctest --test-dir build-tiles --output-on-failure
python -B tools/tiles_disabled_regression.py \
  --plugin build-tiles/reference-build/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --output build-tiles/handoff-disabled-reference
python -B tools/tiles_disabled_regression.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --output build-tiles/handoff-disabled-current
python -B tools/tiles_isolated_smoke.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --integration --output build-tiles/handoff-smoke
```

CTest **16/16 PASS，无跳过**，日志 `build-tiles/handoff-ctest.log`。
关闭态两份 `default-off-observations.json` 解析后完全相等；完整隔离 smoke **PASS**。
三次私有 compositor 的 `configerrors.txt` 均为空。对应脚本日志为
`build-tiles/handoff-disabled-reference.log`、`build-tiles/handoff-disabled-current.log`、
`build-tiles/handoff-smoke.log`。这一轮没有改变插件实现；最后只补录本文验收结果。

## 合入前复审修复（2026-10-17）

六项各自提交，未合入其他分支、未 push、未操作宿主插件：

| 复审项 | 本地提交及处理 |
| --- | --- |
| 1. 单 tile Stage 卡片激活 | `b38136c`：activate 仅在目标 monitor 有多个 tile 时调用 tiles activation；单 tile 使用原生 changeWorkspace。该次卡片激活不让 Stage animation hook 强制 instant，也不建立第二套飞行动画；同步刷新直接读取原生 active workspace，避免 deferred model reconcile 后再次建立 Stage 动画。 |
| 2. Meta thumbnail drag | `20f0650`：Meta 按下或已有 thumbnailDrag 时，均不建立/升级 split transfer；松开 Meta 后正在进行的 thumbnail drag 仍保留桌面拖放路径。 |
| 3. internal 切换 | `451a510`：把 monitor changeWorkspace 的 internal 参数传入 apply，只有非 internal 调用才 releaseAllMouseButtons，与原生 Monitor.cpp 一致。 |
| 4. outgoing workspace 引用 | `cfbed82`：降到一个 tile 时在 apply/reconcile 清空 outgoing；禁用时即使当前锁屏也清理 outgoing，collapse 再显式清理。状态新增每个 monitor 的 outgoing_workspace_count，便于回归检查动画租约。 |
| 5. 默认关闭行为 | `e4e145f`：关闭 tiles 时，Stage 的 suppressed release 直接放行，保留原 swallowed button；pressed 在 suppression 检查前重置 silentDropWindow。overview Esc 先清拖拽状态，再继续原有 search/Esc/close 逻辑；开启 tiles 时保留仅取消拖拽的行为。 |
| 6. broker 错误返回 | `098f7b0`：失效 continuation 对 expected/ActionResult 返回带错误的 unexpected，对 success/error 结构返回 success=false 和说明；detach 后仍走有效原生入口，remove 后不再默认返回成功。 |

最终本地 Debug 构建及完整 **CTest 16/16 PASS，无跳过**。
新增隔离断言覆盖单 tile 卡片点击时以及延迟 reconcile 后的原生动画，Stage 飞行窗口数
为零；Meta 拖入拆分区域、途中松开 Meta 后继续拖到桌面，不产生 tile；真实 native
releaseAllMouseButtons spy 验证 internal 调用为 0 次、external 为 1 次；动画仍在运行时
移除 tile 和禁用 capability 都释放 outgoing。broker 单测覆盖失效 bridge 和 continuation
的失败结果及错误文本，并保留 detach 后原生成功调用的检查。

`tiles_disabled_regression` 对参考版本和最终版本分别运行；两份观测 JSON 完全相等。
新增场景使用实际 `hymission-capture-input begin/end`：suppressed release 放行，结束
suppression 后仍消费原按键的 release；suppressed press 不留下额外 release ownership。
overview 窗口拖动中按 Esc 会关闭 overview，并保持所有窗口的 workspace 不变。
原有无预留区/实际左右预留区两组对照继续通过。

完整 `tiles_isolated_smoke --integration` **PASS**，三次私有 compositor 的配置错误
均为空。日志和产物为：

- `build-tiles/rereview-ctest.log`
- `build-tiles/rereview-disabled-reference.log` 及 `build-tiles/rereview-disabled-reference/`
- `build-tiles/rereview-disabled-current.log` 及 `build-tiles/rereview-disabled-current/`
- `build-tiles/rereview-smoke.log` 及 `build-tiles/rereview-smoke/`

本轮未复跑 ASan；前节 ASan 结果对应前一轮实现。物理输入、锁屏协议、HDR 与设备
视觉验收仍为 PENDING，未将本轮私有 compositor 的合成输入当作设备验收。

本轮再次更新 Hyprland flake input，revision 仍为
`8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`，lock 未变。
最终实现的 Nix 构建 **PASS**，14 项通过、两项 GL 测试因 EGL/GLES 不可用跳过；
本地完整 CTest 中这两项通过。最终产物为
`/nix/store/2j4ai5dz3pfsvhvvcwywc6r192pn94sk-hymission-0.10.2`，日志
`build-tiles/rereview-nix-final-build.log`。仍未进行该上游 revision 的 runtime 验收。

## Release 符号查询的后续修复

使用本机 Release e5a03b6 运行完整隔离验收，并通过实际 failed-dlsym 分支的受控
负对照复现 update 符号报错。统一 matcher 已改为自行解码原始 mangled 名，覆盖
全部严格查找和既有宽松查找。根因、自然复现边界、原始查询记录及完整测试结果见
[Release 符号查找修复](workspace-tiles-symbol-lookup.md)。
