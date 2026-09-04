# 比例字体网格化渲染实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 比例字体（非等宽）下终端按字符网格正常渲染：每字符固定占一格、左对齐、裁剪到格子，删除旧的逐字符比例排版路径。

**架构：** 两条绘制路径（批次聚合 `drawContents` / 逐片段 `drawContentsLegacy`）做相同改造：比例字体（`_fixedFont == false`）时禁止跨列合并片段——每列独立成片段（宽字符两列），`calculateTextArea` 统一为网格公式后片段矩形即单格/双格；`drawCharacters` 在非等宽时把墨迹裁剪到片段矩形。等宽字体路径零改动（像素等价回归保障）。坐标反查（`getCharacterPosition`）与几何（`calculateTextArea`）删除比例累加分支，`textWidth()` 及 `_fixedFont_original` 一并删除。

**技术栈：** C++20、Qt 6.11.1（`QFontMetrics`/`QPainter`）、QTest（`Qt6::Test`）、CMake。

**规格：** `docs/superpowers/specs/2026-09-04-proportional-font-grid-design.md`（用户已逐节批准）。

**执行环境：** 在隔离 worktree（`.worktrees/proportional-font-grid`，从 master 切出）中执行，完成后合并回 master。Qt 前缀 `/home/zz/Qt/6.11.1/gcc_64`。测试必须经 `ctest --test-dir build` 运行（offscreen 平台已注入；直接跑测试二进制在 X11 下会假失败）。

**已知取舍（写入代码注释，不单独处理）：** 比例字体 + `_bidiEnabled`（默认开）下，阿语等上下文整形文本因逐格拆分失去跨字整形——终端网格语义与整形天然冲突，与 xterm/Konsole 行为一致，接受。

---

## 文件结构

- 修改 `lib/src/display/TerminalDisplay.cpp` — 全部核心改动：`fontChange`、两个 `drawContents*` 片段循环、`drawCharacters`、`calculateTextArea`、`getCharacterPosition`、删除 `textWidth`
- 修改 `lib/src/display/TerminalDisplay.h` — 删除 `textWidth` 声明与 `_fixedFont_original` 成员，更新 `_fixedFont` 注释
- 修改 `tests/tst_rendering.cpp` — 新增比例字体网格测试（4 用例 + 3 个辅助函数）

关键现状（行号以 master `669fb26` 为准，改动前以实际内容为准）：

- `fontChange`：`lib/src/display/TerminalDisplay.cpp:239-275`（`_fixedFont` 判定在 250-258，`_fixedFont_original` 赋值在 260）
- `drawCharacters`：`:946-1133`（整段文本绘制在 else 分支 1099-1127）
- `textWidth`：`:2567-2584`；`calculateTextArea`：`:2586-2603`
- `drawContents` 片段合并 while：`:2882-2892`；lineDraw 存/恢复块：`:2923-2925` 与 `:2956`
- `drawContentsLegacy` 片段合并 while：`:3043-3053`；lineDraw 存/恢复块：`:3084-3086` 与 `:3114`
- `getCharacterPosition` 比例分支：`:3833-3841`
- 头文件：`textWidth` 声明 `TerminalDisplay.h:841`；`_fixedFont`/`_fixedFont_original` 成员 `:1076-1077`
- 测试设施（`tests/tst_rendering.cpp` 已有，直接复用）：`initRenderEnv`(:73)、`renderDisplay(display, batching)`(:111)、`renderFull`(:178)、`pumpFrame`(:211)。点击坐标惯例：`QPoint(1 + col * fontWidth(), 1 + row * fontHeight())`（左边距 1px 基线，同 `tst_linewrap.cpp`）。公共测试钩子：`fontWidth()`/`fontHeight()`/`cellPixelWidth()`、`characterAtForTest(col,row)`、`render(&image)`、`getCharacterPosition`（`TerminalDisplay.h:529`，public 区）。

## 任务 1：比例字体网格回归测试（红灯先行）

**文件：**
- 测试：`tests/tst_rendering.cpp`（在类声明的 private slots 末尾追加 4 个槽，在文件末尾追加实现与辅助函数）

- [ ] **步骤 1：新增辅助函数**

在 `monospaceFont()` 定义之后追加：

```cpp
/**
 * @brief 选一个真实存在的比例字体（非等宽），用于网格化渲染测试。
 * @return 优先 DejaVu Sans/Liberation Sans/Arial/Segoe UI，再退任意非
 *         fixedPitch 族；找不到时返回 family 为空的 QFont（调用方须 QSKIP）。
 */
static QFont proportionalFont()
{
    static const QStringList preferred = {
        QStringLiteral("DejaVu Sans"), QStringLiteral("Liberation Sans"),
        QStringLiteral("Arial"),       QStringLiteral("Segoe UI"),
    };
    QFontDatabase db;
    const QStringList available = db.families();
    for (const QString &name : preferred)
        if (available.contains(name) && !db.isFixedPitch(name))
            return QFont(name);
    for (const QString &name : available)
        if (!db.isFixedPitch(name))
            return QFont(name);
    return {};
}

/**
 * @brief 与 initRenderEnv 相同，但使用指定字体（比例字体网格测试用）。
 */
static void initRenderEnvWithFont(const QFont &font, Vt102Emulation &emu,
                                  ScreenWindow *&win, TerminalDisplay &display)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    win = emu.createWindow();
    win->setWindowLines(24);
    display.setVTFont(font);
    display.setBlinkingCursor(false);
    display.setBlinkingTextEnabled(false);
    display.setScreenWindow(win);
    display.resize(800, 600);
}

/**
 * @brief 统计图像指定矩形内的墨迹（非背景）像素数。
 * @note 渲染底色为纯黑，默认前景为浅灰，阈值 32 避开抗锯齿边缘噪声。
 */
static int inkPixels(const QImage &img, const QRect &rect)
{
    int count = 0;
    const QRect r = rect.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); y++)
        for (int x = r.left(); x <= r.right(); x++) {
            const QRgb px = img.pixel(x, y);
            if (qRed(px) > 32 || qGreen(px) > 32 || qBlue(px) > 32)
                count++;
        }
    return count;
}

/**
 * @brief 显示网格某格的像素矩形（左边距 1px 基线，与既有用例点击坐标同式）。
 */
static QRect cellRect(const TerminalDisplay &display, int column, int line)
{
    return QRect(1 + column * display.fontWidth(),
                 1 + line * display.fontHeight(),
                 display.fontWidth(), display.fontHeight());
}
```

在类声明 `private slots:` 末尾（`void testDoubleHeightPixelEquivalence();` 之后）追加：

```cpp
    void testProportionalFontNarrowCharsGrid();
    void testProportionalFontWideCharsClippedAtGridRightEdge();
    void testProportionalFontMouseGridMapping();
    void testProportionalFontCjkDoubleCell();
```

- [ ] **步骤 2：新增 4 个测试用例实现**

在文件末尾（`QTEST_MAIN` 之前）追加：

```cpp
/**
 * @brief 比例字体下窄字符行按网格逐格排布：每个格子的左边界绘制一个 'l'。
 * @note 回归：旧比例排版按字形 advance 密排，80 个 'l' 实际只排到行宽约 1/3 处，
 *       中后段格子无墨迹（「没到右边界就换行、右侧留死区」的渲染侧根因）。
 *       顺带断言批次聚合与 Legacy 两路径在比例字体下仍逐像素相等。
 */
void TestRendering::testProportionalFontNarrowCharsGrid()
{
    const QFont font = proportionalFont();
    if (font.family().isEmpty())
        QSKIP("测试环境无比例字体，跳过");

    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnvWithFont(font, emu, win, display);

    const QByteArray content = "\033[?25l\033[H" + QByteArray(80, 'l');
    emu.receiveData(content.constData(), int(content.size()));
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup：吃掉 _drawTextTestFlag 一次性度量

    const QImage batched = renderDisplay(display, true);
    const QImage legacy = renderDisplay(display, false);
    QCOMPARE(legacy, batched);

    // 首格、中间格、末格都应有墨迹（逐格左对齐绘制的直接证据）
    QVERIFY(inkPixels(batched, cellRect(display, 0, 0)) > 0);
    QVERIFY(inkPixels(batched, cellRect(display, 40, 0)) > 0);
    QVERIFY(inkPixels(batched, cellRect(display, 79, 0)) > 0);
}

/**
 * @brief 比例字体下宽字形行铺满网格且右缘被裁剪：网格右缘之外不得有墨迹。
 * @note 回归：旧比例排版下 80 个 'W' 的累积 advance 远超网格宽度，
 *       越界墨迹直接画到网格右缘之外（仅靠部件边界裁剪）。
 */
void TestRendering::testProportionalFontWideCharsClippedAtGridRightEdge()
{
    const QFont font = proportionalFont();
    if (font.family().isEmpty())
        QSKIP("测试环境无比例字体，跳过");

    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnvWithFont(font, emu, win, display);

    const QByteArray content = "\033[?25l\033[H" + QByteArray(80, 'W');
    emu.receiveData(content.constData(), int(content.size()));
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup
    const QImage img = renderDisplay(display, true);

    // 末格有墨迹（铺满），网格右缘之外无墨迹（裁剪到格子）
    QVERIFY(inkPixels(img, cellRect(display, 79, 0)) > 0);
    const int gridRight = 1 + 80 * display.fontWidth();
    const QRect beyond(gridRight, 0,
                       display.width() - gridRight, display.fontHeight() + 2);
    QCOMPARE(inkPixels(img, beyond), 0);
}

/**
 * @brief 比例字体下鼠标点击坐标按网格反查列号。
 * @note 回归：旧实现按 textWidth 逐字累加反查，窄字符行的像素位置映射到
 *       远大于实际网格列的列号。上报行列 1 基（同 tst_linewrap 既有用例）。
 */
void TestRendering::testProportionalFontMouseGridMapping()
{
    const QFont font = proportionalFont();
    if (font.family().isEmpty())
        QSKIP("测试环境无比例字体，跳过");

    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnvWithFont(font, emu, win, display);

    const QByteArray content = "\033[?25l\033[H" + QByteArray(80, 'l');
    emu.receiveData(content.constData(), int(content.size()));
    pumpFrame(win);
    pumpFrame(win);

    display.setUsesMouse(false);   // 鼠标事件上报给终端程序（而非选区）
    QSignalSpy spy(&display, &TerminalDisplay::mouseSignal);

    // 点击显示行 0 第 40 格（0 起）：cx 应报 41、cy 应报 1
    QTest::mouseClick(&display, Qt::LeftButton, Qt::NoModifier,
                      QPoint(1 + 40 * display.fontWidth(), 1));
    QVERIFY(spy.size() >= 1);
    QCOMPARE(spy.at(0).at(1).toInt(), 41);
    QCOMPARE(spy.at(0).at(2).toInt(), 1);
}

/**
 * @brief 比例字体下 CJK 宽字符仍占两格，其后字符按网格续排。
 * @note 结构断言（后继格 character == 0）与字体度量无关；
 *       像素断言用「中 + 70 个 'l'」把网格/比例两种排版的落点差拉到最大。
 */
void TestRendering::testProportionalFontCjkDoubleCell()
{
    const QFont font = proportionalFont();
    if (font.family().isEmpty())
        QSKIP("测试环境无比例字体，跳过");

    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnvWithFont(font, emu, win, display);

    QByteArray content = "\033[?25l\033[H";
    content += QByteArray::fromUtf8("中");
    content += QByteArray(70, 'l');
    emu.receiveData(content.constData(), int(content.size()));
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup
    const QImage img = renderDisplay(display, true);

    // 结构断言：中 占格 0-1（格 1 为宽字符后继占位格）
    QCOMPARE(display.characterAtForTest(0, 0).character, char32_t(U'中'));
    QCOMPARE(display.characterAtForTest(1, 0).character, char32_t(0));
    // 网格断言：'l' 从格 2 起逐格续排，第 71 格（最后一个 'l'）有墨迹；
    // 旧比例排版下整行墨迹在约 1/3 行宽处就结束了
    QVERIFY(inkPixels(img, cellRect(display, 71, 0)) > 0);
}
```

- [ ] **步骤 3：构建并运行，确认新用例失败、既有用例不受影响**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64
cmake --build build --parallel
ctest --test-dir build --output-on-failure -R tst_rendering
```

预期：4 个新用例 FAIL（`testProportionalFontNarrowCharsGrid` 末格无墨迹 / `testProportionalFontWideCharsClippedAtGridRightEdge` 右缘外有墨迹 / `testProportionalFontMouseGridMapping` cx≠41 / `testProportionalFontCjkDoubleCell` 第 71 格无墨迹），其余既有用例全部 PASS。若新用例意外通过，停下排查（可能字体环境使断言失效，而非功能已实现）。

- [ ] **步骤 4：Commit**

```bash
git add tests/tst_rendering.cpp
git commit -m "test: 新增比例字体网格渲染回归测试（当前红灯）"
```

## 任务 2：几何统一——删除比例排版几何路径

**文件：**
- 修改：`lib/src/display/TerminalDisplay.cpp`（`calculateTextArea` 2586-2603、`getCharacterPosition` 3833-3841、`textWidth` 2567-2584、`fontChange` 260）
- 修改：`lib/src/display/TerminalDisplay.h:841`、`:1076-1077`

- [ ] **步骤 1：`calculateTextArea` 统一网格公式**

把 `calculateTextArea`（`TerminalDisplay.cpp:2586`）函数体中的三元表达式改为纯网格公式（保留逆映射注释块与 `textScale` 逻辑不变）：

```cpp
QRect TerminalDisplay::calculateTextArea(int topLeftX, int topLeftY,
                                            int startColumn, int line,
                                            int length,
                                            const QTransform &textScale) {
    // 永远网格定位：比例字体同样每字符一格（逐格片段 + 单格裁剪，
    // 见 drawContents/drawCharacters），不存在比例累积偏移
    const int left = _fontWidth * startColumn;
    const int top = _fontHeight * line;
    const int width = _fontWidth * length;
    // （保留原有逆映射注释与代码，一字不动）
    const QPoint origin = textScale.inverted().map(
            QPoint(_leftMargin + topLeftX, _topMargin + topLeftY + top));
    return {origin.x() + left, origin.y(), width, _fontHeight};
}
```

- [ ] **步骤 2：`getCharacterPosition` 删除比例累积分支**

把 `TerminalDisplay.cpp:3833-3841` 的：

```cpp
    int x =
            widgetPoint.x() + _fontWidth / 2 - contentsRect().left() - _leftMargin;
    if (_fixedFont)
        column = x / _fontWidth;
    else {
        column = 0;
        while (column + 1 < _usedColumns && x > textWidth(0, column + 1, line))
            column++;
    }
```

改为：

```cpp
    const int x =
            widgetPoint.x() + _fontWidth / 2 - contentsRect().left() - _leftMargin;
    column = x / _fontWidth;
```

（`x` 改为 const 需确认后续无写操作——该函数体内 `x` 之后未被修改。）

- [ ] **步骤 3：删除 `textWidth()` 及 `_fixedFont_original`**

- 删除 `TerminalDisplay.cpp:2567-2584` 整个 `textWidth` 函数（含上方 `// NOTE:` 注释行）。
- 删除 `TerminalDisplay.h:841` 的声明 `int textWidth(int startColumn, int length, int line) const;`
- 删除 `TerminalDisplay.h:1077` 的成员 `bool _fixedFont_original; // used only in textWidth()`
- 删除 `fontChange`（`TerminalDisplay.cpp:260`）中的 `_fixedFont_original = _fixedFont;`
- 把 `TerminalDisplay.h:1076` 的注释更新为新语义：

```cpp
    bool _fixedFont; // REPCHAR 各字形等宽判定结果：门控片段拆分/合并与超宽字形
                     // 检测，不再切换排版路径（比例字体同样永远网格渲染）
```

- [ ] **步骤 4：构建 + 运行 tst_rendering**

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure -R tst_rendering
```

预期：编译通过；既有等宽用例全部 PASS（本任务对 `_fixedFont == true` 路径是恒等变换）；4 个新用例仍 FAIL（渲染路径尚未改，属预期，下一步转绿）。

- [ ] **步骤 5：Commit**

```bash
git add lib/src/display/TerminalDisplay.cpp lib/src/display/TerminalDisplay.h
git commit -m "refactor: 删除比例排版几何路径，坐标与区域换算统一为网格公式"
```

## 任务 3：渲染改造——比例字体逐格片段与单格裁剪

**文件：**
- 修改：`lib/src/display/TerminalDisplay.cpp`（`drawContents` 2882/2923-2925/2956、`drawContentsLegacy` 3043/3084-3086/3114、`drawCharacters` 1099-1127）

- [ ] **步骤 1：`drawContents` 合并循环门控 + 删除 lineDraw 存/恢复块**

片段合并 while（`:2882`）首条件加 `_fixedFont &&`：

```cpp
            // 比例字体（_fixedFont == false）不合并片段：每列独立成片段，
            // 逐格绘制在格子左边界并由 drawCharacters 裁剪到格子（网格化渲染）；
            // 等宽字体保持原有合并（连字整形依赖整段绘制，逐字绘制会破坏连字）
            while (_fixedFont && x + len <= rlx &&
```

删除 `:2923-2925` 的：

```cpp
            bool save__fixedFont = _fixedFont;
            if (lineDraw)
                _fixedFont = false;
```

和 `:2956` 的：

```cpp
            _fixedFont = save__fixedFont;
```

（`calculateTextArea` 已在任务 2 网格化，该临时置 false 块不再有任何作用。`lineDraw` 变量本身仍被合并条件使用，保留。）

- [ ] **步骤 2：`drawContentsLegacy` 同样两处改动**

合并 while（`:3043`）改为 `while (_fixedFont && x + len <= rlx &&`（注释同步骤 1）；删除 `:3084-3086` 与 `:3114` 的存/恢复块。两条路径必须保持逐字节同语义的对应改动——`tst_rendering` 的像素等价断言直接比对两路径输出。

- [ ] **步骤 3：`drawCharacters` 非等宽时裁剪到片段矩形**

else 分支（`:1099-1127`，即非 quardCRT 路径）中，`painter.setLayoutDirection(Qt::LeftToRight);` 之后包一层条件裁剪：

```cpp
            painter.setLayoutDirection(Qt::LeftToRight);

            // 比例字体：片段已逐格拆分（单格；宽字符片段为双格），字形可能
            // 宽于格子——裁剪到片段矩形，越界部分不绘制、不留陈旧墨迹。
            // 等宽字体字形度量与格子一致，保持无裁剪现状（像素等价保障）。
            const bool clipToCell = !_fixedFont;
            if (clipToCell) {
                painter.save();
                painter.setClipRect(rect, Qt::IntersectClip);
            }
```

在 else 分支末尾（`}` 之前，`if (_bidiEnabled) {...} else {...}` 之后）对应恢复：

```cpp
            if (clipToCell)
                painter.restore();
```

注意：裁剪只包文本绘制，不影响函数尾部的 `drawStyledUnderline`（其自带裁剪逻辑）；`rect` 在此处即片段网格矩形，单格宽 `_fontWidth`、宽字符片段宽 `2 * _fontWidth`，无需另行计算。

- [ ] **步骤 4：构建 + 运行 tst_rendering，确认全部转绿**

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure -R tst_rendering
```

预期：全部 PASS——4 个新比例字体用例转绿，既有等宽用例（像素等价、连字、双高行等）保持绿。若等宽用例变红，说明改动波及了 `_fixedFont == true` 路径，逐条 diff 排查；不得通过放宽断言解决。

- [ ] **步骤 5：Commit**

```bash
git add lib/src/display/TerminalDisplay.cpp
git commit -m "feat: 比例字体按字符网格渲染——逐格片段、左对齐、单格裁剪"
```

## 任务 4：全量回归与验收

**文件：**
- 无代码改动（验证任务）

- [ ] **步骤 1：全量测试**

```bash
ctest --test-dir build --output-on-failure
```

预期：全部测试套件 PASS（tst_rendering / tst_linewrap / tst_emulation / tst_protocols / tst_charwidth / tst_history / tst_ligature / tst_kittygraphics / tst_osc52 / tst_benchmark 等）。重点关注 tst_ligature（连字依赖等宽路径整段绘制，不应受影响）与 tst_linewrap（行显示模式与坐标换算正交互叠）。

- [ ] **步骤 2：复现工具对照验收（/tmp/wrap_repro）**

用改造后的库重新构建 `/tmp/wrap_repro/prop_l.cpp`（源码在，链接 build 目录新产出的静态库与 Qt6），运行并检查输出：首行文本最右墨迹像素应贴近 `1 + columns * fontWidth`（网格右缘），而不是改造前的约 531/1200px。该步骤是人工/半自动验收，结果记入任务报告；若复现工具因环境原因不可用，以任务 1 的 4 个自动化用例为验收依据并在报告中注明。

- [ ] **步骤 3：注释与文档一致性检查**

确认：`TerminalDisplay.h` 的 `_fixedFont` 注释已更新（任务 2）；`textWidth`/`_fixedFont_original` 无任何残留引用（`grep -n "textWidth\|_fixedFont_original" lib/` 应为空）；本功能不新增公共 API 与设置项，`AGENTS.md`/`README` 无需变更。

## 自检记录

- 规格覆盖：渲染改造→任务 3；几何统一与比例路径删除→任务 2（`textWidth`/`getCharacterPosition`/`calculateTextArea`/`_fixedFont_original`/lineDraw 临时置 false 块）；测试→任务 1 + 任务 3 步骤 4 + 任务 4；`_fontWidth < 1` 钳制与 `fontChange` 判定保留→不改（任务 2 仅删 260 行赋值）；错误处理边界（超格裁剪/左对齐留空/空片段）→任务 3 步骤 3 + 既有判空逻辑不动。
- 无占位符：所有代码步骤含完整代码块，测试含完整断言。
- 类型一致性：`proportionalFont()`/`initRenderEnvWithFont()`/`inkPixels()`/`cellRect()` 在任务 1 步骤 1 定义，4 个用例引用一致；`renderDisplay`/`renderFull`/`pumpFrame`/`characterAtForTest` 为既有设施（签名已核对）。
