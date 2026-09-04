# 比例字体网格化渲染设计

日期：2026-09-04
状态：已确认（用户逐节批准）

## 背景与目标

组件列宽 `_fontWidth` 取 REPCHAR（大小写字母+数字+`./+@`）的平均 advance（`TerminalDisplay.cpp:247`）。字体非等宽时 `_fixedFont = false`，绘制走逐字符比例排版：一行窄字符（如 `l`）实际像素宽度远小于 `columns × _fontWidth`，表现为「没到右边界就换行、右侧留死区」。

目标：比例字体也按字符网格正常渲染——每个字符固定占一格、左对齐、裁剪到格子。不要求用户更换字体（不能过滤字体）。

已确认决策：

- 窄字符在格子内**左对齐**（与 xterm 等一致）；
- 列宽基准保持 REPCHAR 平均宽不变，比平均宽的字形允许轻微裁切（与上游 Konsole 一致）；
- 旧比例排版路径**直接移除**，永远网格渲染。

## 渲染改造

- `drawCharacters`（`TerminalDisplay.cpp:931` 附近）：片段内逐字符在格子左边界 `x = _leftMargin + tLx + column * _fontWidth` 处绘制，裁剪矩形为单格（宽 `_fontWidth`），超出部分裁掉。
- CJK 宽字符占两格：裁剪宽度 `2 * _fontWidth`；`RE_EXTENDED_CHAR`、双宽探测逻辑不动。
- 双宽/双高行的 `textScale` 世界变换路径不动（格子缩放，与本改造正交）。
- 等宽字体下逐字符绘制与现状像素一致，`tst_rendering` 现有像素等价断言必须全绿（回归保障）。

## 几何统一与比例路径删除

- `getCharacterPosition`：删除 `_fixedFont == false` 的 textWidth 累加分支，永远 `x / _fontWidth`。
- `textWidth()`：函数及全部调用点删除，调用方统一 `len * _fontWidth`。
- `calculateTextArea`：删除非等宽分支，永远 `_fontWidth * column`。
- 鼠标、选区、热点、光标、IME 预编辑矩形统一单一网格公式；与行显示模式的 `mapDisplayToBuffer` 三件套正交叠加。
- `_fixedFont` 保留但语义收窄为「字体宽度判定结果」，不再切换排版路径；`fontChange` 判定逻辑保留。
- 检查 `_fixedFont_original` 及连字/特殊绘制路径中临时置 false 的代码块，能删则删，不能删则注释说明。

## 错误处理与边界

- `_fontWidth < 1` 时钳 1（现有防御保留）。
- 字形比格子宽 → 裁剪，不越界绘制；比格子窄 → 左对齐留空，不拉伸。
- 空片段、零长文本不绘制。

## 测试

- 回归：现有 `tst_rendering`（等宽字体）全绿。
- 新增（`tst_rendering.cpp` 或新文件）：
  - 比例字体下 `l`×N 行：逐格左边界像素位置断言（网格定位而非比例密排）；
  - `W`×N 行：文本右边界铺满 `columns * _fontWidth`；
  - 比例字体下鼠标点击坐标反查按网格命中；
  - CJK 宽字符比例字体下仍占两格。

## 影响面

- 主要改动：`lib/src/display/TerminalDisplay.{h,cpp}`；测试 `tests/`。
- 不改动：`Screen`/`ScreenWindow`/解析器/公共 API（无新设置项）。
