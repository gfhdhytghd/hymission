# hymission

`hymission` 是一个面向 Hyprland 的 Mission Control 风格 overview 插件项目。目标不是做“临时切到另一个 layout”，而是在 compositor 侧把当前工作区窗口投影成一组可交互的预览卡片，尽量接近 macOS Mission Control 的视觉和操作方式。

当前仓库还处于原型阶段，但已经有两块可复用基础能力：

- 一个独立的窗口预览布局引擎
- 一个最小 Hyprland 插件入口，用来读取配置并调试当前布局结果

详细设计见：

- [`docs/spec.md`](docs/spec.md)
- [`docs/architecture.md`](docs/architecture.md)
- [`docs/research.md`](docs/research.md)

## 当前状态

已经实现：

- `MissionControlLayout::compute(...)`，输入窗口自然几何，输出 overview 目标预览几何
- `hymission:debug_current_layout` dispatcher
- 一组当前仅用于布局引擎的配置项
- 独立 demo 程序 `hymission-layout-demo`

尚未实现：

- 真实 overview 打开/关闭
- preview 渲染
- 鼠标命中测试与点击激活
- 键盘导航
- 动画过渡
- 多 workspace overview 条带
- 拖拽、搜索、窗口分组

README 只描述仓库现状和最小使用方式；行为定义以 [`docs/spec.md`](docs/spec.md) 为准。

## 设计目标

- 预览保持窗口原始宽高比，不裁切内容
- 不改变客户端逻辑分辨率，不向客户端发送 resize 作为 overview 主路径
- 尽量保留窗口原有的上下、左右空间关系，而不是退化成死板网格
- 把布局算法和渲染路径拆开，便于单测和替换

## 当前公开接口

### Dispatcher

目前只有一个调试用 dispatcher：

```conf
bind = SUPER, M, hymission:debug_current_layout
```

执行后会：

- 取光标所在显示器
- 收集该显示器当前活动工作区中的可见窗口
- 计算 Mission Control 风格 preview slots
- 用通知显示摘要结果

当前没有 `toggle/open/close` overview dispatcher；这些接口只在规格文档中作为未来预留命名。

### 当前配置项

以下配置项已经在插件入口注册，但目前仅用于布局计算，不代表最终用户配置面已经冻结：

```conf
plugin {
    hymission {
        outer_padding = 48
        row_spacing = 32
        column_spacing = 32
        min_window_length = 120
        small_window_boost = 1.35
        max_preview_scale = 0.95
        min_slot_scale = 0.10
        layout_scale_weight = 1.0
        layout_space_weight = 0.10
        overview_focus_follows_mouse = 0
    }
}
```

语义摘要：

- `outer_padding`: overview 内容与显示器边缘的内边距
- `row_spacing`: preview 行间距
- `column_spacing`: preview 列间距
- `min_window_length`: 小窗口参与布局前的最小边长钳制
- `small_window_boost`: 对小窗口的放大权重
- `max_preview_scale`: 预览最大缩放上限
- `min_slot_scale`: 布局最小缩放下限
- `layout_scale_weight`: 行数候选评分中对缩放大小的权重
- `layout_space_weight`: 行数候选评分中对空间利用率的权重
- `overview_focus_follows_mouse`: overview 期间是否让 hover 的 preview 成为当前 focus，并在退出时保留最后一次 hover focus

示例：开启 overview 期间 focus 跟随鼠标

```conf
plugin {
    hymission {
        overview_focus_follows_mouse = 1
    }
}
```

## 布局引擎

布局引擎定义在：

- [`src/mission_layout.hpp`](src/mission_layout.hpp)
- [`src/mission_layout.cpp`](src/mission_layout.cpp)

输入：

- 一组窗口自然矩形 `WindowInput::natural`
- 一个 overview 可用区域 `Rect area`
- 一组布局参数 `LayoutConfig`

输出：

- 每个窗口对应一个 `WindowSlot`
- `WindowSlot::target` 是 overview 中的目标预览矩形
- `WindowSlot::scale` 是该窗口最终等比缩放值

当前算法不是固定栅格，而是 row-based layout：

- 先按窗口中心点的 `y` 值大致分行
- 再在每行按 `x` 值排序
- 评估不同 row count
- 选择总分最高的候选布局

具体来源和取舍见 [`docs/research.md`](docs/research.md)。

## 构建

### CMake

```sh
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build -j"$(nproc)"
```

产物：

- 插件：`build/libhymission.so`
- demo：`build/hymission-layout-demo`

### Meson

```sh
meson setup build-meson
meson compile -C build-meson
```

## 最小调试流程

1. 编译插件
2. 在 `hyprland.conf` 中加载 `libhymission.so`
3. 绑定 `hymission:debug_current_layout`
4. 触发 dispatcher，确认通知里能看到 preview 数量和前几个目标矩形

如果只想验证布局算法而不启动 Hyprland 插件，可以直接运行：

```sh
./build/hymission-layout-demo
```

## 近期路线

近期实现顺序已经在文档中定死：

1. 保持当前布局引擎接口稳定
2. 用 render hook 接 overview 渲染
3. 加入打开/关闭状态机
4. 加入命中测试和点击激活
5. 加入键盘导航
6. 最后再做动画、多 workspace 条带和高级交互

原因见 [`docs/architecture.md`](docs/architecture.md)。
