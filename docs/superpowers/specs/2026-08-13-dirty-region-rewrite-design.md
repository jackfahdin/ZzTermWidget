# 脏区算法重写（跨度级脏区 + 滚动像素搬迁）设计规格

日期：2026-08-13
状态：已批准（头脑风暴结论）
前置轮次：轮 4（渲染性能，重绘 6.1→1.4ms，文本批绘制 + 双路径像素比对设施）、轮 6（Kitty 图形，paintEvent 逐 rect 裁剪）

## 1. 背景与目标

轮 4 把重绘耗时压掉 77%，但 `updateImage()` 的脏区粒度仍是**整行**：任何一个字符格变脏，整行（`_fontWidth × columnsToUpdate`）都进 `dirtyRegion`（TerminalDisplay.cpp:1434-1442）。两类场景仍有浪费：

- **TUI 局部刷新**：光标移动、时钟跳动、进度条更新——每帧只动几格，却整行重绘。
- **整屏滚动吞吐**：持续输出时整屏内容不变只是上移，逐格比对 + 全量重绘是纯浪费；`ScreenWindow` 已有 `scrollCount()`/`scrollRegion()`/`resetScrollCount()` 滚动量报告机制（上游 konsole 同款优化的基础设施），当前未被绘制层利用。

目标：局部刷新只画脏跨度，纯滚动走像素搬迁，两场景都有 benchmark 证据。

## 2. 范围

包含（三步走，顺序即依赖序）：

1. **基准口径修正**：tst_benchmark TUI 用例改真实路径计量（updateImage + paintEvent 离屏执行），新增"局部刷新"与"整屏滚动"双场景基线。
2. **行内跨度级脏区**：dirtyMask 逐格结果聚合为每行脏跨度 `[minX, maxX]`（±1 格扩展），dirtyRect 只盖跨度。
3. **滚动像素搬迁**：纯整屏滚动时 `QWidget::scroll()` 搬像素 + 仅新入行走脏区比对；保守回退。

明确不做（YAGNI）：

- 不动 drawContents 文本批绘制内部实现。
- 不动 QRegion 合并策略（Qt 自动合并相邻矩形带）。
- 不做 tile 块级脏区、不引入多线程光栅化、不动公共头 `lib/include/qtermwidget.h`。

## 3. 详细设计

### 3.1 基准口径修正（tst_benchmark）

- 现状：TUI 用例经 `QWidget::render(&QImage)` 全区域渲染，计量的不是真实增量路径。
- 新口径：驱动 emulation 产出新帧 → 调 `updateImage()`（脏区比对计时）→ 对返回的 dirtyRegion 走离屏 paintEvent 等价路径（渲染计时）。两段可分开报告。
- 双场景：
  - 局部刷新：屏幕填满文本后，每帧改单行少格（模拟时钟/进度条）。
  - 整屏滚动：持续行输出使视图匀速上滚。
- 沿用轮 4 惯例：无硬断言，结果写 `build/benchmark-baseline.txt` 供前后人工对比。

### 3.2 行内跨度级脏区（updateImage 改造）

- 每行逐格比对得到 `dirtyMask`（现有代码），聚合出 `minX/maxX`；有脏格时跨度向两侧各扩 1 格并钳到 `[0, columnsToUpdate-1]`——吸收宽字符尾部变脏与斜体/衬线字形 ±1 格越界（与现有 dirtyMask 注释语义一致）。
- `dirtyRect` = 跨度像素矩形（`tLx + minX×_fontWidth` 起，宽 `(maxX-minX+1)×_fontWidth`）。
- 保持整行脏的例外（行为不变）：
  - 双高行（LINE_DOUBLEHEIGHT）。
  - 含图像放置行（`imagePlacements`/`kittyRefs` 两视图并集强制置脏逻辑，轮 5/6 设施）。
  - `_usedLines/_usedColumns` 收缩区、`graphicsDirty()` 整屏补刷、preedit 区。
- 风险与对策：轮 6 F2 的逐 rect `setClipRect` 叠加跨度脏区后，干净格字形越界进入脏区会被裁掉不复绘。±1 格扩展为第一道防线；`tst_rendering` 双路径逐像素比对为强制验证——若发现不足，升级为 `QFontMetrics::overhang()` 像素级扩展（规格内预留此升级路径，实施时以像素比对结果裁决）。
- drawContents 零改动（按 rect 逐格绘制的既有行为天然适配）。

### 3.3 滚动像素搬迁

- 检测：`updateImage` 前后经 `ScreenWindow::scrollCount()`（配 `resetScrollCount()`）获知本次更新的纯滚动行数 N；仅当滚动区等于整个内容区（`scrollRegion()` 覆盖全屏）且满足全部回退条件否定时才启用。
- 执行：`QWidget::scroll(0, -N×_fontHeight, 内容区rect)` 搬迁像素；新进 N 行走 3.2 的跨度级脏区比对重绘；`_image` 缓存行同步移位（保持既有 memcpy 同步语义）。
- 保守回退（任一命中即回退现有全量路径）：
  - 存在图像放置（`hasImages()`）、活动选区、双高行、备选屏切换帧、`_resizing`、跨度比对窗口与滚动量错位（内容有滚动外修改）。
- 内部开关：`setScrollOptimizationEnabled(bool)`（默认开），镜像轮 4 `setTextBatchingEnabled` 模式，供 benchmark A/B 与故障回退；内部接口，不进公共头。

### 3.4 数据流

```
Emulation 输出 → ScreenWindow（scrollCount 累积）
             → TerminalDisplay::updateImage：
                 纯滚动？ → QWidget::scroll 搬像素 + 新进 N 行跨度比对
                 否则     → 逐行逐格比对 → 跨度 dirtyRect → update(dirtyRegion)
             → paintEvent 逐 rect（轮 6 裁剪）→ drawBackground/图像/drawContents
```

## 4. 错误处理与回退

| 情形 | 行为 |
|------|------|
| 滚动检测条件任一不满足 | 回退全量脏区路径（现状行为） |
| 像素比对发现越界裁切 | 扩 ±1 格不够时升级 overhang 像素扩展（3.2 预留） |
| 滚动优化疑错 | `setScrollOptimizationEnabled(false)` 一键回退 |
| 图像/选区/双高/备选屏帧 | 一律走保守路径 |

## 5. 测试

- **tst_rendering 扩展**（沿用轮 4 双路径逐像素比对设施）：
  - 跨度脏区路径 vs 整行路径像素等价：单行少格变更、宽字符跨跨度边界、斜体越界、双高行、含图像行、选区高亮帧。
  - 滚动优化路径 vs 回退路径像素等价：连续滚动 1/N 行、滚动+局部修改混合帧、含图回退帧、选区回退帧。
- **tst_benchmark**：双场景新口径 + 基线文件更新。
- 全程 ctest 9 套件绿（含 rendering 扩充用例）。

## 6. 验收标准

- `ctest --test-dir build --output-on-failure` 全绿。
- benchmark 双场景对比基线文件有可复现的改进记录（局部刷新场景重绘像素量/耗时显著下降；滚动场景大幅跳过全量比对）。
- tst_rendering 新旧路径像素等价零差异。
- 注释中文 Doxygen；third_party 与公共头不动；README/CHANGELOG 收尾。

## 7. 已知遗留（有意留待）

- 字形越界超过 ±1 格的极端字体（装饰性连写）可能裁切——以像素比对实测裁决是否升级 overhang 方案。
- 部分滚动区（DECSTBM 子区域）不走像素搬迁（条件不满足自动回退）。
- 滚动优化与轮 6 终审遗留（kitty DECSTBM 错位根治）正交，互不阻塞。
