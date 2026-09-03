# 行显示模式设计：软折叠 / 不换行 + 横向滚动条

日期：2026-09-03
状态：已确认（用户逐节批准）

## 背景与目标

窗口缩窄时，终端中已输出的超长行会被裁剪隐藏（`Screen::resizeImage` 只改列数，行数据保留但不显示）。目标：提供可设置的行显示策略，类似 WindTerm——

- **软折叠模式**：超宽的行在显示层折叠成多行，全部内容可见；
- **不换行模式**：超出宽度的内容通过按需出现的横向滚动条查看（默认）。

约束：两种模式均不修改 `Screen` 缓冲区，不影响上报给 shell 的列数（SIGWINCH 语义不变），运行时可切换。

## 总体方案

折叠与滚动均实现在显示视图层 `TerminalDisplay`（方案 A），不触碰 `Screen`/`ScreenWindow` 的缓冲与历史逻辑。

## 对外 API

`QTermWidget` 公共头新增，风格对齐 `setScrollBarPosition`：

```cpp
enum class LineWrapMode {
    NoWrap,   ///< 不自动换行：超出宽度的内容可经横向滚动条查看（默认）
    SoftWrap  ///< 软折叠：超宽的行在显示层折叠成多行，缓冲区不变
};

void setLineWrapMode(LineWrapMode mode);
LineWrapMode lineWrapMode() const;
```

- 运行时可切换，切换后立即重算布局并重绘。
- 默认值 `NoWrap`。

## 软折叠：显示映射表

- 数据结构：`QVector<DisplayRow>`，`DisplayRow = { 缓冲区行号, 起始列偏移 }`，存于 `TerminalDisplay`。
- 一条有效长度为 W 的逻辑行贡献 `ceil(W / 可见列数)` 个显示行；空行贡献 1 个。
- 重建时机：`resizeEvent`、内容更新（`updateImage`）、模式切换。输入为 ScreenWindow 当前窗口各行有效长度（行尾空白不计）。
- 渲染：`updateImage` 按映射表从 ScreenWindow 取字符填入 `_image`（仍为 可见行 × 可见列 网格），下游绘制代码不变。
- 坐标换算：鼠标点击、选区、双击选词等把（显示行， 列）反查为（缓冲区行， 列）后走现有逻辑；选区仍以缓冲区坐标存储，跨折叠段选中天然正确。
- 滚动耦合：软折叠改变一屏容纳的缓冲区行数，垂直滚动条范围/步进按映射表重算；回看历史同样按折叠后显示行滚动。

边界情况：

- 折叠按字符网格的列偏移计算，宽字符（CJK）不会被拆半。
- Kitty/Sixel 图形所在行不折叠（图形锚定缓冲区坐标），该行超长部分仍裁剪，避免图像错位。

## 横向滚动条（NoWrap 模式）

- 新增 `QScrollBar(Qt::Horizontal)`，加入 `TerminalDisplay` 现有布局（与垂直滚动条对称，置于内容区底部）。
- 范围：当前 ScreenWindow 可见窗口内各行最大有效长度 `maxLen`；`range = 0 .. max(0, maxLen - 可见列数)`。内容更新与 resize 时重算；`range == 0` 自动隐藏，否则显示。
- 渲染：绘制 `_image` 时列号 + `value()` 偏移（视口平移）；鼠标/选区坐标反向加偏移。
- 交互：拖动、Shift+滚轮横向滚动；键盘输入或新输出引起光标移动时自动回零。
- 软折叠模式下始终隐藏。
- Kitty/Sixel 图形不随滚动条平移（锚定缓冲区坐标），与现有裁剪行为一致。

## 错误处理与边界

- 折叠/平移的列偏移恒在 `[0, 行有效长度)` 内，越界取空白字符。
- 窗口极窄（可见列数 = 1）时映射表与滚动条范围仍按同一公式计算，无特判。
- 模式切换、resize、滚动条拖动均为可逆显示操作，不产生缓冲区副作用。

## 测试

新增 `tests/tst_linewrap.cpp`（QTest）：

- 布局表纯函数：单行超长、多行混合、空行、恰好整除、宽字符行。
- 横向滚动条：最大行宽 → range 计算、按需出现/隐藏、自动回零。
- 坐标换算：显示坐标 ↔ 缓冲区坐标互转，含跨折叠段选区。
- 模式切换：NoWrap ↔ SoftWrap 运行时切换后映射表与滚动条状态正确。
- 集成冒烟：输出超长行后缩窄窗口，两种模式下验证可见内容与滚动条存在性。

## 影响面

- 主要改动：`lib/src/display/TerminalDisplay.{h,cpp}`、`lib/include/qtermwidget.h`（API）、`tests/`。
- 不改动：`Screen`、`ScreenWindow`、解析器、PTY、历史读回。
