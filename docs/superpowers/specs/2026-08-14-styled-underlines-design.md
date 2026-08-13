# 花样下划线与独立下划线色（SGR 4:x / 58 / 59）设计规格

日期：2026-08-14
状态：已批准（头脑风暴结论）
上游语义参考：kitty/wezterm/Windows Terminal 的 SGR 4 子参数与 SGR 58/59 实现；ECMA-48 子参数（冒号分隔）

## 1. 背景与目标

现代 CLI（git delta、helix、lsd、systemd 等）越来越多使用花样下划线（波浪=诊断标记）与独立下划线色。当前 ZzQTermWidget 只支持 SGR 4 单线下划线（`RE_UNDERLINE` + `font.setUnderline`，TerminalDisplay.cpp:959），且 CSI 解析器把 `:` 与 `;` 都拍平成参数分隔——`4:3` 目前被错误解释为"下划线+斜体"。

目标：全量支持 SGR `4:0/4:1/4:2/4:3/4:4/4:5`（关/单/双/波浪/点/虚）+ SGR 58 独立下划线色（256 色板与真彩，分号与冒号两种形式）+ SGR 59 复位，同时修正冒号子参数解析（`4;3` 与 `4:3` 不再混淆）。

## 2. 范围

包含：

- CSI 解析器逐参数前导分隔符记录（`';'`/`':'`），仅 SGR 处理消费该信息。
- Character 扩展：3 位下划线样式字段（rendition 空闲位 11-13）+ `underlineColor` 字段。
- SGR 语义：`4` 单线、`4:0` 关、`4:1..4:5` 五种样式、`24` 关；`58;5;n`、`58;2;r;g;b`、`58:5:n`、`58:2::r:g:b`（容忍空位）、`59` 复位。
- 绘制：默认单线无独立色保持 QFont underline 现状路径；其余手绘（双/点/虚 QPen、波浪 QPainterPath）。
- 三层测试：解析 / 存储与脏区 / 离屏像素。

明确不做（YAGNI）：

- SGR 53（overline）的颜色扩展、DECRQSS 对下划线状态的查询回报、超链接悬停下划线样式联动。
- 除 SGR 外其他序列的子参数解释（如 `4:3` 之外的冒号用法维持拍平行为）。

## 3. 详细设计

### 3.1 CSI 解析器分隔符记录（Vt102Emulation.cpp）

- 参数累积三处（约 :486/:499/:525 的 `eec(';') || eec(':')`）在 `addArgument()` 时同步记录该参数的前导分隔符到随 token 携带的数组（首参数记 0/无分隔符）。
- token 结构携带分隔符数组；只有 SGR（最终字符 `m`）的处理读取它。其余 CSI 序列只被记录、不被解释——行为零变化。
- 容量与参数数组一致（现有参数上限），越界按既有截断语义。

### 3.2 Character 扩展（lib/src/util/Character.h）

- rendition 位 11-13 存下划线样式枚举：0=单线、1=双线、2=波浪、3=点线、4=虚线；位 14-15 保留。
- `RE_UNDERLINE`（位 2）保留为"有下划线"汇总位：任何非关样式都置位，旧代码（含 :946 的判定）零影响。
- 新增 `CharacterColor underlineColor` 字段（Character 16→20 字节；`COLOR_SPACE_DEFAULT` 表示跟随前景色）。
- **强制**：`operator==` / `operator!=` / `equalsFormat` 纳入样式位（rendition 已整体比较，天然覆盖）与 `underlineColor` 比较——漏掉则轮 7 脏区比对漏检样式变化（显示事故）。
- 提供样式读写辅助（如 `underlineStyle()` / 设置接口），中文 Doxygen。

### 3.3 SGR 语义（Vt102Emulation SGR 处理）

| 序列 | 语义 |
|------|------|
| `4`（无子参数） | 单线下划线（现状） |
| `4:0` | 关下划线 |
| `4:1` / `4:2` / `4:3` / `4:4` / `4:5` | 单线 / 双线 / 波浪 / 点线 / 虚线 |
| `24` | 关下划线（现状） |
| `58;5;n`、`58:5:n` | 下划线色 = 256 色板 n |
| `58;2;r;g;b`、`58:2::r:g:b` | 下划线色 = 真彩（冒号形式容忍色彩空间空位） |
| `59` | 下划线色复位为跟随前景 |

- `4;3`（分号）= 下划线 + 斜体，与 `4:3`（波浪）严格区分——方案 A 核心价值，专门回归测试。
- 非法样式值（`4:6` 及以上）、参数不足的 58：忽略该参数，不影响其余 SGR 参数。
- 分号形式的 `58` 后跟不足参数时不得吞掉后续独立 SGR 参数（如 `58;2;1;2;3;1m` 的尾巴处理参照 konsole 语义：58 按定长消费）。

### 3.4 绘制（TerminalDisplay）

- 现状路径保留：样式=单线且 underlineColor 为 DEFAULT 时，仍走 `font.setUnderline`（:946-959 段）——零回归。
- 手绘路径（非单线样式，或独立下划线色）：
  - 关闭该片段的字体下划线，文本绘制后按样式手绘：双线/点线/虚线用 QPen（penStyle 区分），波浪用 QPainterPath 正弦段（振幅与波长随字体度量）。
  - 几何：y = baseline + `fontMetrics.underlinePos()`，线宽取 `fontMetrics.lineWidth()`（下限 1px）。
  - 颜色：underlineColor 为 DEFAULT 时回落该格前景色。
  - 宽字符片段跨两格连续绘制；手绘在 drawContents 的片段绘制上下文内进行（DECDH 的 scale 变换同样生效）。
- 批次/Legacy 双路径一致处理（轮 4 像素等价设施强制比对）。

### 3.5 数据流

```
CSI … m → 参数+分隔符记录 → SGR 处理（样式位/underlineColor 落到 Screen 当前格式）
        → Character（rendition 位 11-13 + underlineColor，参与相等性比较）
        → updateImage 脏区比对（样式变化格必脏）
        → drawContents 片段绘制（默认字体下划线 / 手绘样式线）
```

## 4. 错误处理

| 情形 | 行为 |
|------|------|
| 非法样式值（4:6+） | 忽略该参数 |
| 58 参数不足/越界 | 忽略该参数，不吞后续独立 SGR |
| 冒号出现在非 SGR 序列 | 维持拍平行为（现状） |
| 参数数组越界 | 既有截断语义 |

## 5. 测试（追加进现有套件，不新增文件）

- **解析层**（tst_emulation）：`4:0/4:1/4:2/4:3/4:4/4:5` 各样式落到 Character 样式位；`4;3` 下划线+斜体不混淆（关键回归）；58 两种形式与 59 复位；非法值忽略。
- **存储/脏区层**（tst_emulation 或 tst_rendering）：样式/颜色变更格在 updateImage 后必脏（借轮 7 lastDirtyRegion 钩子断言区域）。
- **渲染层**（tst_rendering 离屏像素）：波浪线波形像素存在且区别于单线；独立下划线色像素断言（如红文本绿下划线）；双/点/虚可区分；默认单线路径与改造前像素一致（批次 vs Legacy 双路径比对保持绿）。

## 6. 验收标准

- `ctest --test-dir build --output-on-failure` 9 套件全绿（含新增用例）。
- `4;3` / `4:3` 区分有专测；Character 相等性纳入新字段有专测（防脏区漏检）。
- 注释中文 Doxygen；公共头 `lib/include/qtermwidget.h` 与 `lib/third_party/` 不动。
- README/CHANGELOG 收尾。

## 7. 已知遗留（有意留待）

- 波浪线波形参数（振幅/波长）按字体度量经验取值，不做配置项。
- 非等宽字体下手绘线位置与文本基线对齐沿用字体度量，与既有绘制同精度。
- DECRQSS 查询不到下划线样式状态（上游多数终端亦不回报）。
