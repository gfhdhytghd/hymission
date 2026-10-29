# Release 二进制的符号查找错位修复

日期：2026-10-17。纯插件修复，不修改 Hyprland 核心，不 push、不 reload 宿主。

## 二进制与复现边界

明确指定 `--hyprland /home/wilf/.local/lib/hyprland-hdr/Hyprland` 运行完整私有
`tiles_isolated_smoke --integration`。版本为 Release、commit
`e5a03b6a5ce64ec4c85954649bad4df083627574`，其 SHA-256 为
`b08dd5a78a0f8f04ae3936d77a4b99da105c97b53e5d975cdd4fdbd88dc0ced6`。
安装文件、`~/data/Hyprland/build-hdr-release/Hyprland` 和宿主进程的 `/proc/<pid>/exe`
哈希相同。只读取进程身份，没有向宿主 IPC 发命令或注入输入。

**无故障注入的全新隔离会话，旧插件也通过了完整 smoke。** 因而不能声称自然复现了
宿主当时的触发条件，或已确定宿主那次究竟哪个 `dlsym` 失败。
下述负对照在这个 Release 二进制上触发实际的失败分支，复现了完全相同的报错。

## 根因与负对照

该版本 `HyprlandAPI::findFunctionsByName` 从两份 nm 输出按 `lineNo` 配对；遍历中
匹配名字但 `dlsym` 返回空时直接 `continue`，绕过循环尾的 `lineNo++`。此后的
`match.signature` 和 `address` 仍来自当前 mangled 名，`match.demangled` 却属于前面的
符号。两份 nm 输出原本逐行对齐，并不能避免循环内发生的错位。

原 Hymission matcher 把 `match.demangled` 当作可信输入。严格查找会误判符号缺失；
历史宽松查找甚至可能把别的函数地址配给目标签名，所以只缩小 `update` 的查询范围
不能全面解决问题。

测试专用 `--symbol-lookup-failure` 只对私有子进程加载 shim，让排序靠前的
`_ZN10CDMABuffer6updateERKN9Hyprutils4Math7CRegionE` 的 `dlsym` 返回空，触发核心
原有的 `continue` 分支。Release 核心和插件的其他符号查询流程保持实际代码路径。
同一次 API 查询取得：

```text
signature:     _ZN29CUnifiedWorkspaceSwipeGesture6updateEd
api_demangled: CUnifiedWorkspaceSwipeGesture::cancel()
canonical:     CUnifiedWorkspaceSwipeGesture::update(double)
```

旧插件因而出现原样错误：
`tiles disabled: expected one exact symbol: CUnifiedWorkspaceSwipeGesture::update(double)`。
普通查询返回 119 项，注入后 118 项；目标符号仍只有一个。结果数量多本身不是根因。

该 shim 仅用于这台 glibc x86-64 机器的隔离负对照；编译使用 `-O2`，反汇编核实
转发给 libc dlsym 是尾调用，以保留其他 `RTLD_NEXT` 查询的原调用者。它不安装到
宿主，不传给 Weston 父进程或真实 Wayland 客户端。

## 修复及全部查找路径检查

`src/hook_symbols.hpp` 现在用 `abi::__cxa_demangle(match.signature)` 自行解码，再规范化
空白并比较；解码失败时拒绝该 C++ 候选，不回退到 API 的 demangled。普通 C 符号
使用其原始名字。每个候选只解码一次，仍对不同地址的歧义拒绝、同地址别名去重。

检查了全部生产 `findFunctionsByName` 调用点，均经过这个 matcher：

- Tiles `attach`：所有新增严格 hook，包括 swipe begin/update/end/cancel。
- Stage 可选 `blurMainFramebuffer`：另一处严格查找。
- Stage 既有 hooks、overview 的所有签名候选及 action hooks：同样受益，保留各自
  原有的宽松回退规则，但回退只比较从真实 mangled 名解码的名字。

测试 fixture 的 mouse-release spy 原有的直接 demangled 精确比较也改用同一严格
matcher。生产匹配路径已不再读取 API 的 demangled；诊断报告有意保留它以记录错位。

单测覆盖错误 API 名字、伪装成目标的无关符号、非法 mangled 名、空白规范化、
同地址重复、不同地址歧义，以及既有宽松回退。

## 验证

所有 smoke 命令都明确使用上述 Release 二进制：

```sh
python -B tools/tiles_isolated_smoke.py \
  --hyprland /home/wilf/.local/lib/hyprland-hdr/Hyprland \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --integration --output build-tiles/release-symbol-fixed
# 加 --symbol-lookup-failure，输出到 release-symbol-injected-fixed，验证实际 API 错位。
```

| 对照 | 结果 |
| --- | --- |
| 旧插件、无注入 | 完整 integration PASS：`build-tiles/release-symbol-negative.log` |
| 旧插件、注入 failed-dlsym | 预期失败，报错完全一致：`build-tiles/release-symbol-injected-negative-final.log`；原始状态见同名目录的 `tiles-enable-failure.json` |
| 修复插件、无注入 | 完整 integration PASS：`build-tiles/release-symbol-fixed.log` |
| 修复插件、注入 failed-dlsym | 完整 integration PASS：`build-tiles/release-symbol-injected-fixed.log`；`update-symbols.json` 仍证明 API 名字错位 |

完整 CTest **16/16 PASS，无跳过**：`build-tiles/symbol-ctest.log`。
`tiles_disabled_regression` 的参考/当前版本观测 JSON 完全相等，日志为
`build-tiles/symbol-disabled-reference.log`、`build-tiles/symbol-disabled-current.log`。
修复版两组完整 smoke 和两组关闭态运行的 `configerrors.txt` 均为空。

以上是私有 compositor 的真实 Release 核心与合成输入验收，没有 reload 宿主，也不
代替物理设备/锁屏/HDR 验收。本轮没有复跑 ASan。

再次更新 Hyprland flake input，结果仍为 upstream main
`8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`，lock 未改变。最终 Nix 构建 **PASS**：
14 项通过、两项 GL 测试因 EGL/GLES 不可用跳过；两项在上述本地 CTest 都通过。
产物 `/nix/store/03jjz8fs0r29lql3qxr0qxlfk7w7xbly-hymission-0.10.2`，日志
`build-tiles/symbol-nix-final-build.log`。没有把本机 e5a03b6 的 runtime 结果当成上游
8010b4e 的 runtime 验收。
