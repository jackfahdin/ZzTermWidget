# 渲染性能优化设计：benchmark 基线 + 文本批次聚合

日期：2026-08-12
状态：已批准（头脑风暴结论）
范围：ZzQTermWidget 第四轮强化。对标 MobaXterm/WindTerm 差距清单中的性能项；Sixel/图形协议与连字渲染留待后续轮次。

## 背景与目标

ZzQTermWidget 经三轮整理（lib 重组、char32_t 管线、协议三件套）后功能面已较完整，测试基座（QTest + ctest + 三平台 CI）就绪。本轮目标：渲染性能优化——先建立可重复的 benchmark 基线，再改造已知热点（`drawContents` 逐字符 `drawText`），用基线量化收益。

两个目标场景，不设单一指标、不允许任一场景回退：

1. **大块输出吞吐**（cat 大文件、编译刷屏）——瓶颈在解析+滚屏+全量重绘。
2. **TUI 高频重绘帧率**（nvim/lazygit 局部更新）——瓶颈在逐字符 drawText 的 QPainter 调用开销。

## 关键决策记录

| 决策点 | 结论 | 理由 |
|--------|------|------|
| 本轮范围 | 仅渲染性能（基线+热点） | Sixel/连字量级大且会碰同一渲染管线，分开做 |
| 目标场景 | 吞吐与帧率两者都要 | 批次聚合对两者同向有利 |
| 硬约束 | 像素级一致 | 批量绘制禁 kerning/连字整形，可用 QImage 逐像素比对做安全网 |
| 实现路径 | 基线先行 + 批次聚合（方案 A） | 剖析作为验证环节而非独立方案；脏区重写下一轮 |
| 连字 | 明确不做 | 与像素一致约束冲突 |

## 架构

改动落点：

- `tests/tst_benchmark.cpp`（新建）——QBENCHMARK 基线，接入现有 tests 设施
- `lib/src/display/TerminalDisplay.cpp` —— `drawContents` 文本绘制路径批次聚合
- 不改公共 API（`lib/include/` 不动）

## 1. Benchmark 基线

新建 `tests/tst_benchmark.cpp`（`QTEST_MAIN` + `QBENCHMARK`，offscreen 可跑，`QT_QPA_PLATFORM=offscreen` 沿用现有测试属性）：

1. **解析吞吐**：构造 Emulation+Screen，喂大块混合输出（普通文本 / 颜色转义 / CJK / 宽字符），QBENCHMARK 计量吞吐。
2. **绘制吞吐**：TerminalDisplay 挂离屏表面，预填内容后 QBENCHMARK 循环全量重绘。
3. **TUI 局部重绘**：模拟高频小区域更新（光标行 + 状态行交替），近似 nvim 帧负载。

基线在优化前后各跑一遍，对比数字写入任务报告与 CHANGELOG。benchmark 用例进 ctest 但**不设硬性性能断言**（机器差异大），仅保证可编译可运行。

## 2. 热点验证

批次聚合动刀前，先用 `perf record` 或 QElapsedTimer 分段插桩跑绘制吞吐用例，确认逐字符 `drawText` 确为热点：

- 若热点落在别处（字符宽度计算、字体度量、属性解析），先回报再调整刀口——不预设结论硬改。
- 顺带审计 `konsole_wcwidth` / 字体度量是否存在可缓存的重复计算；若有，作为小优化一并做（保持行为不变）。

## 3. 批次聚合改造

改造点：`TerminalDisplay::drawContents` 的文本绘制循环。

- **聚合键**：同一行内连续的（前景色 / 背景色 / rendition / 字体）相同字符合成一个 run，每 run 一次 QPainter 调用。QPainter 调用数从 O(字符数) 降到 O(样式段数)。
- **例外路径**：宽字符（双格）、组合字符、ExtendedCharTable 字符、双向文本等按现有逻辑单独成批，不破坏既有正确性路径。
- **像素一致手段**：批内每个字符 x 坐标按网格列独立计算；绘制禁用 kerning/连字整形（font style strategy / NoFontMerging 等手段），保证与逐格绘制逐像素一致。
- **不动**：选择区高亮、链接下划线（paintFilters）、光标等覆盖层逻辑；`updateImage`/脏区管线。

## 4. 回归与验证

- **像素等价性测试（本轮最关键安全网）**：构造含多样式 / CJK / 宽字符 / 组合字符的屏幕内容，offscreen 渲染改造前后各一张 QImage，逐像素比对相等。作为常驻测试保留。
- 现有 5 个测试套件（tst_charwidth / tst_emulation / tst_osc52 / tst_history / tst_protocols）全量回归。
- benchmark 前后对比数字入报告与 CHANGELOG。

## 错误处理

- 聚合逻辑遇到不认识的单元格类型：回退现有逐字符路径（保守优先正确性）。
- benchmark 设施失败不影响功能测试（独立测试目标）。
- perf 不可用的环境：用 QElapsedTimer 插桩替代，不阻塞流程。

## 明确不做

- 脏区算法重写（下一轮候选，视本轮收益再定）。
- GPU/OpenGL 渲染。
- 连字渲染（与像素一致硬约束冲突）。
- Sixel / Kitty 图形协议（下一轮）。
