# 四指 workspace 手势结束崩溃修复

## 真机证据与根因

2026-10-17 19:40 的两份真机 core：PID 1705970、1856337，命令均为
`~/.local/lib/hyprland-hdr/Hyprland --watchdog-fd 4`；区别于私有测试 compositor。
对应 release 为 e5a03b6a5ce64ec4c85954649bad4df083627574；hyprpm 缓存记录的
hymission 提交为 d8d74c74072ed5a2dc34e7785225475145b56ba3。
原始 core、缓存插件副本和 GDB 输出仅保存在忽略目录 `build-tiles/crash-1940/`。

两份 core 都从 `CTrackpadGestures::gestureEnd` 进入 workspace 手势的原生结束路径。
手势对象和 vtable 仍有效；错误出在 native trampoline：

- release 中 `CUnifiedWorkspaceSwipeGesture::end()` 位于 ELF offset `0xc94650`，
  首条指令为 `e9 6b e3 ff ff`，相对跳转至 `endSegment()`（`0xc929c0`）。
- Hyprland HookSystem 重定位 RIP-relative 内存操作数，但把这条相对跳转原样复制到
  另一个地址。复制后的跳转仍用原 displacement，因而指向错误的位置。
- 第二份 core 的 landing trampoline 位于 ELF-relative `0x1062320`，错跳到
  `0x1060690` 的 BSS；第一份 core 因不同分配位置错跳进 `opaqueRegion()`。
  原始信号是 SIGSEGV，crash handler 随后 abort，故 coredumpctl 显示 SIGABRT。

这不是符号 demangled 错位问题，也不是 Stage 卡片对象失效。canonical mangled lookup
保持现有修复。本次问题是找对函数之后，原生 hook 对它的跳转指令重定位错误。

检查第二份 core 的 59 个 active hook，其中 57 个目的地址属于 hymission；还发现
`CScrollMoveTrackpadGesture::update()` 的复制区有短 `jne`，同样需要修复。
仓库的产品 function hook 均经过 `hook_broker.cpp`，所以修复集中于同一适配层。

## 纯插件修复

broker 在 attach 前复用 Hyprland 自身的指令边界 probe；attach 后、返回事件循环前，
修正 landing trampoline 内的相对 call/jump/条件跳转。

- near 分支保持长度，重新计算 displacement。
- short 分支转到同一 32-byte slot 中的 jump island，再跳到真实目标；不改 flags、
  寄存器或原有指令位置。
- 指向复制区内的分支映射到对应的 landing 指令边界。
- Hyprland 已有的 RIP 内存重定位和最终 return jump 保留不动。
- 容量、距离或目标指令边界不满足条件时，撤回 hook 并返回失败。

不修改 compositor，不依赖某个运行时地址或某条 gesture 名称。detach/reinstall 仍由
原 HookSystem 管理。private probe 的依赖限制在 broker 的 native API 适配文件内。

## 复现与验证

新入口：

```sh
python -B tools/tiles_isolated_smoke.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --gesture-lifecycle-regression --output build-tiles/crash-1940/relocated
```

输入从私有设备的四指 swipe signal 进入 PointerManager / InputManager / matcher / gesture，
没有用 dispatcher 代替手势。旧缓存插件在首次原生 `trackpad("end")` 稳定 SIGSEGV，
私有 crash report 的错误 PC 为 ELF-relative `0x1060610`；修复后相同场景通过。

覆盖 tiles 关闭、开启但单 tile、多 tile、合并回单 tile、再次禁用，正反方向、取消、
零位移；另以 dwindle / scrolling 覆盖 scroll_move 条件分支的未跳转 / 跳转路径。
已有动画专项继续检查跟手中间帧、方向、曲线/速度、邻 tile 稳定性。

最终测试结果和开发 HEAD 构建记录补于本文件末尾。本机只进行了日志、core、缓存文件
等只读检查；没有 reload、hyprpm update、手势注入或修改本机配置。隔离验证不能代替
用户真机会话验收。

## 最终检查

- 完整本地 CTest：16/16 PASS、无跳过（`ctest-final.log`）。
- release 完整 `tiles_isolated_smoke --integration`：PASS（`final-full.log`）。
- `tiles_disabled_regression`：PASS，观测 JSON 与参考提交
  552bd339fe4809e0a031d277ca97f4dac6d0a5da 完全一致（`final-disabled.log`）。
- 新四指原生结束 / scroll_move 生命周期回归：PASS（`final-lifecycle.log`）。
- 既有真实四指跟手五帧、native/tile 动画采样、顶部 CSD 像素、ws-only 两侧切换退出、
  all overview 起终点 / alpha，以及 Stage 无嵌套 / 无别名效果回归均 PASS：
  `gesture.log`、`animation.log`、`top.log`、`workspace.log`、`overview.log`、`stage.log`。
- 各私有 configerrors 为空；`git diff --check` 通过。没有提交截图、core 或构建产物。

验证范围补充：检查旧私有测试的 core 时还看到退出清理阶段的 native scrolling /
fullscreen / pointer-constraint 析构崩溃，栈与本次运行中的 gestureEnd 不同。
另外启动完全不加载任何插件的同一 release 私有 compositor，仅创建 scrolling workspace
后终止，也得到退出码 -11（`native-cleanup-control.log`，私有 Lua 无 plugin.load）。
因此以上 PASS 指手势 / 功能 / 渲染测试阶段，不宣称该 release 的进程退出路径无崩溃。

开发兼容检查：刷新 hyprland flake input 后仍为 upstream main
8010b4e5c66f97aa82cd5b3332b4ec613779b9a7，lock 无变化。
最终 Nix `.#hymission` 构建成功（`nix-final.log`，4 cores / max-jobs=1）；
Nix 内 14 PASS、2 GL 测试因 EGL 环境不可用跳过，本地对应两项均已通过。
