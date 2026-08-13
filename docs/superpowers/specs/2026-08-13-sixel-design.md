# Sixel 图形协议设计

日期：2026-08-13
状态：已批准（头脑风暴结论）
范围：ZzQTermWidget 第五轮强化。对标 MobaXterm/WindTerm 差距清单中的 Sixel 项；Kitty 图形协议、连字渲染、脏区算法重写留待后续轮次。

## 背景与目标

ZzQTermWidget 经四轮整理（lib 重组、char32_t 管线、协议三件套、渲染性能 -77%）后，功能与工程基座齐备：QTest 设施、像素等价性常驻测试、benchmark 基线、OSC 8 行级稀疏表模式。本轮目标：Sixel 图形协议——终端内嵌位图显示（lsix/img2sixel/tmux/ncmpcpp 专辑封面等生态），WindTerm 的招牌能力。

## 关键决策记录

| 决策点 | 结论 | 理由 |
|--------|------|------|
| 本轮范围 | 仅 Sixel | Kitty 图形协议复杂一个量级，复用本轮锚定层后续再做 |
| 滚动语义 | 图像随行滚动 + 滚入历史保留 | 体验最好（wezterm 式），行被清除/覆盖时图像销毁 |
| 默认行为 | 默认启用 + 资源上限 | 开箱即用对标 WindTerm；宽高 ≤4096、总像素预算 256MB 防恶意流 |
| 实现路径 | 行级图像锚定层 + 自研解码器（方案 A） | 复用 OSC 8 已验证的行级稀疏表模式；零新依赖、解码器可单测 |
| 绘制顺序 | 图像在文本层之下 | xterm/wezterm 语义，文字可压在图上 |
| 光标行为 | 图像锚定后文本光标移至图最后一行之下 | xterm 行为 |

## 架构

三个新单元 + 三个挂接点：

- 新建 `lib/src/emulation/SixelDecoder.{h,cpp}` —— 纯逻辑解码器，可单测
- 修改 `lib/src/emulation/Screen.{h,cpp}` —— 屏级图像表 + 行级放置引用
- 修改 `lib/src/emulation/Vt102Emulation.cpp` —— DCS sixel 分流
- 修改 `lib/src/display/TerminalDisplay.cpp` —— 图像叠加绘制
- 不改公共 API（`lib/include/` 不动）

## 1. Sixel 解码器

输入 `DCS P1;P2;P3 q <data> ST` 的 data 段与参数，输出 `QImage`（ARGB32）+ 透明底标志。

支持的语法子集（覆盖 lsix/img2sixel/tmux 生态）：

- `#Pc;Pu;Px;Py;Pz` 调色板定义/选择（RGB 与 HLS 色彩空间）
- `!Pn` 重复、`"Pan;Pad;Ph;Pv` 光栅属性
- `$` 回车、`-` 换行
- 6-bit 位带字符 `?`–`~`

参数语义：P1 宽高比忽略（按标准 1:2 处理）；P2=1 透明底（0 号色不画，文字背景透出）、P2=2 以背景色填底；P3 忽略。调色板 256 寄存器，图像私有，不污染终端调色板。

资源上限：宽/高各 ≤4096 像素，累计像素预算 256MB；超限丢弃整张图、吞到 ST 继续正常解析。

## 2. 图像存储（Screen 扩展）

- 屏级哈希表 `imageId → SixelImage{ QImage, 像素尺寸, 网格行数 }`，自增 ID。
- 行级稀疏表 `QVector<{ 起始列, imageId }>`，仅含图的行分配——与 OSC 8 链接段表同构：引用计数、随行滚动、滚入历史保留、清行销毁。
- 图像占用的网格行数 = ceil(像素高 / 行高)，每行一条引用。
- 生命周期挂钩点复用 OSC 8 既有模式：`addHistLine` / `moveImage` / `clearImage` / `resizeImage` / `setScroll` / `reset`。

## 3. DCS 解析挂接（Vt102Emulation）

- tokenizer 的 DCS 状态新增 sixel 分流：检测到 `DCS <params> q` 进入 sixel 数据累积；超长流走既有 tokenDiscard 机制吞吃。
- ST 后整体交解码器；解码成功则在**当前光标位置**锚定图像，文本光标下移到图像最后一行之下；随后输出的文字从图下方继续。
- 解码失败/超限：静默丢弃，不影响后续字节流。

## 4. 绘制层（TerminalDisplay）

- 文本绘制前先绘制图像：按网格锚点换算像素坐标，多行引用拼出完整图；部分滚出/覆盖的图按可见区裁剪。
- 文字画在图像之上。
- `drawContentsLegacy` 基准路径不受影响；像素等价性测试内容不含 sixel，无干扰。

## 5. 备选屏与 reset

主/备屏图像各自独立存储，随屏切换；`reset` 清空全部图像与引用。

## 错误处理

- 非法 sixel 语法：跳过该图，解析器状态复原，不崩溃不卡死。
- 调色板越界引用：钳制到有效范围（实现时定死并注释）。
- 解码中途遇到 CAN/SUB/ESC：中止该图。
- 资源超限：丢弃该图，吞到 ST。

## 测试

1. **解码器单测**（新 `tests/tst_sixel.cpp`）：最小 1×1 图、RLE 重复、调色板定义与引用（RGB/HLS）、透明底、换行/回车、光栅属性、宽高超限丢弃、总像素预算丢弃。
2. **存储生命周期测试**（复用 OSC 8 测试模式）：锚定 → 滚动 → 历史保留 → 清行销毁全链路。
3. **绘制层测试**：offscreen render 出 QImage，抽查关键像素颜色（不逐像素锁整张，避免字体环境敏感）。
4. 现有 7 个测试套件全量回归。

## 明确不做

- Kitty 图形协议（复用本轮锚定层，后续轮次）。
- sixel 动画与高色彩扩展。
- P1 宽高比非标准的重采样。
- 图像的鼠标交互（拖拽/点击）。
