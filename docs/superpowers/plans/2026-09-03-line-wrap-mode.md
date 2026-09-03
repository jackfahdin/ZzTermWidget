# 行显示模式（软折叠 / 横向滚动条）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 为终端组件提供可设置的行显示模式——`NoWrap`（不换行 + 按需横向滚动条，默认）与 `SoftWrap`（显示层软折叠），均不修改屏幕缓冲区。

**架构：** 全部改动在显示视图层 `TerminalDisplay` 完成。`Screen`/`ScreenWindow` 只新增只读查询（行有效长度、行切片）；折叠数学做成 `DisplayLayout.h` 纯函数；`_image` 网格维持「可见行 × 可见列」，由 `updateImage` 前的视图合成步骤按模式填充。

**技术栈：** C++20、Qt6 Widgets、QTest、CMake。

**规格：** `docs/superpowers/specs/2026-09-03-line-wrap-mode-design.md`

**构建与测试：**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

---

## 文件结构

- 修改 `lib/src/emulation/Screen.h` / `Screen.cpp` — 新增 `getLineLength()`、`getLineSlice()` 只读查询。
- 修改 `lib/src/emulation/ScreenWindow.h` / `ScreenWindow.cpp` — 新增 `windowLineLength()`、`getWindowLineSlice()` 窗口相对转发。
- 新建 `lib/src/display/DisplayLayout.h` — 折叠数学纯函数（`foldCountForLine`、`buildFoldMap`、`displayRowOffsetOfLine`），无 Qt 依赖之外的任何状态。
- 修改 `lib/include/qtermwidget.h`、`lib/src/widget/qtermwidget.cpp` — `LineWrapMode` 枚举与 setter/getter 转发。
- 修改 `lib/src/display/TerminalDisplay.h` / `TerminalDisplay.cpp` — 模式状态、视图合成、横向滚动条、坐标换算。
- 新建 `tests/tst_linewrap.cpp`；修改 `tests/CMakeLists.txt:2` 的 `QTERMWIDGET_TESTS` 列表加一行 `tst_linewrap`。

---

### 任务 1：Screen 行查询 API

**文件：**
- 修改：`lib/src/emulation/Screen.h`（在 `getImage` 声明附近，`Screen.h:608`）
- 修改：`lib/src/emulation/Screen.cpp`
- 测试：`tests/tst_linewrap.cpp`（新建）、`tests/CMakeLists.txt`

背景：`screenLines[y]` 只增长到实际写入的列（`Screen.cpp:755-758`），`count()` 即有效长度；历史行走 `history->getLineLen(line)`（参照 `Screen::copyLineToStream` `Screen.cpp:1320` 的现成模式）。

- [ ] **步骤 1：编写失败的测试**

新建 `tests/tst_linewrap.cpp`：

```cpp
#include <QtTest>
#include "Screen.h"

/**
 * @brief 行显示模式（软折叠/横向滚动条）回归测试。
 */
class TestLineWrap : public QObject
{
    Q_OBJECT
private slots:
    void screenLineLength();
    void screenLineSlice();
};

void TestLineWrap::screenLineLength()
{
    Screen screen(4, 10);
    // 空行有效长度为 0
    QCOMPARE(screen.getLineLength(0), 0);
    // 写入 3 个字符后长度为 3（不写满整行）
    screen.displayCharacter(U'a');
    screen.displayCharacter(U'b');
    screen.displayCharacter(U'c');
    QCOMPARE(screen.getLineLength(0), 3);
    // 越界行返回 0
    QCOMPARE(screen.getLineLength(99), 0);
}

void TestLineWrap::screenLineSlice()
{
    Screen screen(4, 10);
    const char *text = "abcdefgh";
    for (const char *p = text; *p; ++p)
        screen.displayCharacter(*p);

    Character dest[4];
    // 从第 2 列起取 4 个字符
    screen.getLineSlice(0, 2, 4, dest);
    QCOMPARE(dest[0].character, U'c');
    QCOMPARE(dest[3].character, U'f');
    // 起始列越过有效长度：全部补默认空格
    screen.getLineSlice(0, 8, 4, dest);
    QVERIFY(dest[0].isSpace());
    QVERIFY(dest[3].isSpace());
}

QTEST_GUILESS_MAIN(TestLineWrap)
#include "tst_linewrap.moc"
```

`tests/CMakeLists.txt:2` 的 `QTERMWIDGET_TESTS` 列表中加入一行 `tst_linewrap`。

- [ ] **步骤 2：运行测试验证失败**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64 && cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure
```

预期：编译失败，`getLineLength`/`getLineSlice` 未声明。

- [ ] **步骤 3：实现 Screen 查询**

`Screen.h`（`getImage` 声明旁）：

```cpp
    /**
     * @brief 返回绝对行号对应行的有效长度（行尾未写入部分不计）。
     * @param line 绝对行号（0 为历史最早行）。
     * @return 有效长度；行号越界返回 0。
     */
    int getLineLength(int line) const;

    /**
     * @brief 把绝对行号 line 的 [startCol, startCol+count) 字符拷入 dest。
     * @note 越出有效长度的格补 defaultChar；光标所在格打 RE_CURSOR（与 getImage 一致）。
     *       不做选区反色——选区高亮由绘制层经 isSelected 处理，避免双重反转。
     */
    void getLineSlice(int line, int startCol, int count, Character* dest) const;
```

`Screen.cpp` 实现（模式照抄 `copyLineToStream` `Screen.cpp:1320-1376`）：

```cpp
int Screen::getLineLength(int line) const {
    const int histLines = getHistLines();
    if (line < 0 || line >= histLines + getLines())
        return 0;
    if (line < histLines)
        return history->getLineLen(line);
    return screenLines[line - histLines].count();
}

void Screen::getLineSlice(int line, int startCol, int count, Character* dest) const {
    for (int i = 0; i < count; ++i)
        dest[i] = defaultChar;
    const int histLines = getHistLines();
    if (line < 0 || line >= histLines + getLines() || startCol < 0 || count <= 0)
        return;
    if (line < histLines) {
        const int copyCount = qMin(count, history->getLineLen(line) - startCol);
        if (copyCount > 0)
            history->getCells(line, startCol, copyCount, dest);
    } else {
        const ImageLine &imgLine = screenLines[line - histLines];
        const int copyCount = qMin(count, imgLine.count() - startCol);
        for (int i = 0; i < copyCount; ++i)
            dest[i] = imgLine[startCol + i];
    }
    // 光标高亮，与 getImage 的 RE_CURSOR 行为一致
    if (line - histLines == cuY && cuX >= startCol && cuX < startCol + count)
        dest[cuX - startCol].rendition |= RE_CURSOR;
}
```

- [ ] **步骤 4：运行测试验证通过**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure
```

预期：PASS。再跑全量 `ctest --test-dir build --output-on-failure` 确认无回归。

- [ ] **步骤 5：Commit**

```bash
git add lib/src/emulation/Screen.h lib/src/emulation/Screen.cpp tests/tst_linewrap.cpp tests/CMakeLists.txt
git commit -m "feat: Screen 新增行有效长度与行切片只读查询"
```

---

### 任务 2：ScreenWindow 窗口相对转发

**文件：**
- 修改：`lib/src/emulation/ScreenWindow.h`（`getImage` 声明附近，`ScreenWindow.h:77`）
- 修改：`lib/src/emulation/ScreenWindow.cpp`
- 测试：`tests/tst_linewrap.cpp`

- [ ] **步骤 1：编写失败的测试**

向 `tests/tst_linewrap.cpp` 追加（需 `#include "ScreenWindow.h"`）：

```cpp
// private slots 追加：
    void screenWindowForwarding();

// 实现：
void TestLineWrap::screenWindowForwarding()
{
    Screen screen(4, 10);
    ScreenWindow window(&screen);
    window.setWindowLines(4);
    screen.displayCharacter(U'x');
    screen.displayCharacter(U'y');

    QCOMPARE(window.windowLineLength(0), 2);

    Character dest[2];
    window.getWindowLineSlice(0, 0, 2, dest);
    QCOMPARE(dest[0].character, U'x');
    QCOMPARE(dest[1].character, U'y');
}
```

- [ ] **步骤 2：运行测试验证失败**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure
```

预期：编译失败，方法未声明。

- [ ] **步骤 3：实现转发**

`ScreenWindow.h`：

```cpp
    /**
     * @brief 返回窗口相对行 relLine 的有效长度（委托 Screen::getLineLength）。
     */
    int windowLineLength(int relLine) const;

    /**
     * @brief 把窗口相对行 relLine 的 [startCol, startCol+count) 字符拷入 dest。
     */
    void getWindowLineSlice(int relLine, int startCol, int count, Character* dest);
```

`ScreenWindow.cpp`：

```cpp
int ScreenWindow::windowLineLength(int relLine) const {
    return _screen->getLineLength(currentLine() + relLine);
}

void ScreenWindow::getWindowLineSlice(int relLine, int startCol, int count, Character* dest) {
    _screen->getLineSlice(currentLine() + relLine, startCol, count, dest);
}
```

- [ ] **步骤 4：运行测试验证通过**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure
```

预期：PASS。

- [ ] **步骤 5：Commit**

```bash
git add lib/src/emulation/ScreenWindow.h lib/src/emulation/ScreenWindow.cpp tests/tst_linewrap.cpp
git commit -m "feat: ScreenWindow 透出行有效长度与行切片查询"
```

---

### 任务 3：DisplayLayout 折叠数学纯函数

**文件：**
- 新建：`lib/src/display/DisplayLayout.h`
- 测试：`tests/tst_linewrap.cpp`

- [ ] **步骤 1：编写失败的测试**

`tests/tst_linewrap.cpp` 追加（`#include "DisplayLayout.h"`）：

```cpp
// private slots 追加：
    void foldCount();
    void foldMap();

// 实现：
void TestLineWrap::foldCount()
{
    QCOMPARE(foldCountForLine(0, 10), 1);   // 空行计 1 段
    QCOMPARE(foldCountForLine(5, 10), 1);
    QCOMPARE(foldCountForLine(10, 10), 1);  // 恰好整除
    QCOMPARE(foldCountForLine(11, 10), 2);
    QCOMPARE(foldCountForLine(25, 10), 3);
    QCOMPARE(foldCountForLine(5, 0), 1);    // 退化列数防御
}

void TestLineWrap::foldMap()
{
    // 窗口 3 行：长度 25、3、0；可见显示行 5
    const QVector<int> lengths = {25, 3, 0};
    const auto rows = buildFoldMap(lengths, 10, 5);
    QCOMPARE(rows.size(), 5);
    QCOMPARE(rows[0], (DisplayRow{0, 0}));
    QCOMPARE(rows[1], (DisplayRow{0, 10}));
    QCOMPARE(rows[2], (DisplayRow{0, 20}));
    QCOMPARE(rows[3], (DisplayRow{1, 0}));
    QCOMPARE(rows[4], (DisplayRow{2, 0}));
    // maxRows 截断：只取前 2 个显示行
    QCOMPARE(buildFoldMap(lengths, 10, 2).size(), 2);
}

void TestLineWrap::displayRowOffset()
{
    // 全缓冲前缀和：行 0(25)→3 段，行 1(3)→1 段，行 2(0)→1 段
    const QVector<int> lengths = {25, 3, 0};
    QCOMPARE(displayRowOffsetOfLine(lengths, 10, 0), 0);
    QCOMPARE(displayRowOffsetOfLine(lengths, 10, 1), 3);
    QCOMPARE(displayRowOffsetOfLine(lengths, 10, 2), 4);
}
```

- [ ] **步骤 2：运行测试验证失败**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure
```

预期：编译失败，`DisplayLayout.h` 不存在。

- [ ] **步骤 3：实现纯函数**

新建 `lib/src/display/DisplayLayout.h`（header-only，仅依赖 QtCore）：

```cpp
#pragma once

#include <QVector>

/**
 * @brief 显示行 →（缓冲区窗口相对行， 起始列偏移）的映射项。
 */
struct DisplayRow {
    int bufferLine;
    int columnOffset;

    bool operator==(const DisplayRow &) const = default;
};

/**
 * @brief 计算一条有效长度为 effectiveLength 的行折叠后的显示段数。
 * @note 空行计 1 段；columns <= 0 时防御性返回 1。
 */
inline int foldCountForLine(int effectiveLength, int columns) {
    if (columns <= 0)
        return 1;
    return qMax(1, (effectiveLength + columns - 1) / columns);
}

/**
 * @brief 从窗口顶行起生成显示行映射，最多 maxRows 个显示行。
 * @param windowLineLengths 窗口各缓冲区行的有效长度。
 */
inline QVector<DisplayRow> buildFoldMap(const QVector<int> &windowLineLengths,
                                        int columns, int maxRows) {
    QVector<DisplayRow> rows;
    for (int line = 0; line < windowLineLengths.size() && rows.size() < maxRows; ++line) {
        const int segments = foldCountForLine(windowLineLengths[line], columns);
        for (int seg = 0; seg < segments && rows.size() < maxRows; ++seg)
            rows.append({line, seg * columns});
    }
    return rows;
}

/**
 * @brief 全缓冲行 line 的首个折叠段在全缓冲显示行序列中的偏移（前缀和）。
 * @note 用于软折叠模式下垂直滚动条 range/value 的显示行换算。
 */
inline int displayRowOffsetOfLine(const QVector<int> &lineLengths, int columns, int line) {
    int offset = 0;
    for (int i = 0; i < line && i < lineLengths.size(); ++i)
        offset += foldCountForLine(lineLengths[i], columns);
    return offset;
}
```

- [ ] **步骤 4：运行测试验证通过**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure
```

预期：PASS。

- [ ] **步骤 5：Commit**

```bash
git add lib/src/display/DisplayLayout.h tests/tst_linewrap.cpp
git commit -m "feat: 新增软折叠显示映射纯函数 DisplayLayout"
```

---

### 任务 4：公共 API `LineWrapMode` 与 TerminalDisplay 模式骨架

**文件：**
- 修改：`lib/include/qtermwidget.h`（`ScrollBarPosition` 枚举旁，`:42-49`）
- 修改：`lib/src/widget/qtermwidget.cpp`（转发模式参照 `setScrollBarPosition` `:485-487`，双向参照 `osc52Enabled` `:477-483`）
- 修改：`lib/src/display/TerminalDisplay.h` / `TerminalDisplay.cpp`
- 测试：`tests/tst_linewrap.cpp`

- [ ] **步骤 1：编写失败的测试**

`tests/tst_linewrap.cpp` 追加（`#include "qtermwidget.h"`，本用例需要 QApplication——把整个文件的 main 宏从 `QTEST_GUILESS_MAIN` 改为 `QTEST_MAIN`）：

```cpp
// private slots 追加：
    void lineWrapModeApi();

// 实现：
void TestLineWrap::lineWrapModeApi()
{
    QTermWidget widget;
    QCOMPARE(widget.lineWrapMode(), QTermWidget::LineWrapMode::NoWrap); // 默认
    widget.setLineWrapMode(QTermWidget::LineWrapMode::SoftWrap);
    QCOMPARE(widget.lineWrapMode(), QTermWidget::LineWrapMode::SoftWrap);
    widget.setLineWrapMode(QTermWidget::LineWrapMode::NoWrap);
    QCOMPARE(widget.lineWrapMode(), QTermWidget::LineWrapMode::NoWrap);
}
```

- [ ] **步骤 2：运行测试验证失败**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure
```

预期：编译失败，`LineWrapMode` 未定义。

- [ ] **步骤 3：实现 API 与骨架**

`lib/include/qtermwidget.h`（`ScrollBarPosition` 枚举之后）：

```cpp
    /**
     * @brief 行显示模式：窗口缩窄时已输出的超长行如何呈现。
     */
    enum class LineWrapMode {
        NoWrap,   ///< 不自动换行：超出宽度的内容可经横向滚动条查看（默认）
        SoftWrap  ///< 软折叠：超宽的行在显示层折叠成多行，缓冲区不变
    };
```

`QTermWidget` 内声明（`setScrollBarPosition` 旁）：

```cpp
    /** @brief 设置行显示模式，立即重算布局并重绘。 */
    void setLineWrapMode(LineWrapMode mode);
    /** @brief 返回当前行显示模式。 */
    LineWrapMode lineWrapMode() const;
```

`lib/src/widget/qtermwidget.cpp`：

```cpp
void QTermWidget::setLineWrapMode(LineWrapMode mode) {
    m_terminalDisplay->setLineWrapMode(mode);
}

QTermWidget::LineWrapMode QTermWidget::lineWrapMode() const {
    return m_terminalDisplay->lineWrapMode();
}
```

`TerminalDisplay.h`（成员区 `_scrollbarLocation` 旁，`:1038`）：

```cpp
    QTermWidget::LineWrapMode _lineWrapMode = QTermWidget::LineWrapMode::NoWrap;
    QScrollBar *_hScrollBar = nullptr;   ///< NoWrap 模式的横向滚动条（任务 5 接线）
    int _hScrollOffset = 0;              ///< 水平视口偏移（列）
    QVector<DisplayRow> _displayRows;    ///< SoftWrap：显示行 →（缓冲区行, 列偏移）
```

头部加 `#include "DisplayLayout.h"`。public 声明（`setScrollBarPosition` 旁，`:124`）：

```cpp
    /** @brief 设置行显示模式（仅影响显示层，不改变上报给 shell 的列数）。 */
    void setLineWrapMode(QTermWidget::LineWrapMode mode);
    /** @brief 返回当前行显示模式。 */
    QTermWidget::LineWrapMode lineWrapMode() const { return _lineWrapMode; }
```

`TerminalDisplay.cpp` 实现（参照 `setScrollBarPosition` `:2858-2873`）：

```cpp
void TerminalDisplay::setLineWrapMode(QTermWidget::LineWrapMode mode) {
    if (_lineWrapMode == mode)
        return;
    _lineWrapMode = mode;
    _hScrollOffset = 0;
    _displayRows.clear();
    propagateSize();   // 重算几何并通知仿真层
    update();
}
```

- [ ] **步骤 4：运行测试验证通过**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure && ctest --test-dir build --output-on-failure
```

预期：新用例 PASS，全量无回归。

- [ ] **步骤 5：Commit**

```bash
git add lib/include/qtermwidget.h lib/src/widget/qtermwidget.cpp lib/src/display/TerminalDisplay.h lib/src/display/TerminalDisplay.cpp tests/tst_linewrap.cpp
git commit -m "feat: 新增 QTermWidget::LineWrapMode 公共 API 与显示层模式骨架"
```

---

### 任务 5：视图合成（updateImage 填充改造）

**文件：**
- 修改：`lib/src/display/TerminalDisplay.h` / `TerminalDisplay.cpp`（`updateImage` `:1432`）
- 测试：`tests/tst_linewrap.cpp`

核心思路：在 `updateImage` 中把 `newimg` 的来源从 `_screenWindow->getImage()` 换成一个「视图合成」步骤——按当前模式把窗口各行切片拼成 `_lines × _columns` 网格，**后续脏区比对循环（`:1555-1683`）完全不动**。NoWrap 无偏移且无超宽行时保留原 `getImage()` 快路径。

- [ ] **步骤 1：编写失败的测试**

```cpp
// private slots 追加：
    void softWrapComposition();
    void hscrollComposition();

// 实现（夹具模式参照 tests/tst_rendering.cpp:70-80）：
void TestLineWrap::softWrapComposition()
{
    Vt102Emulation emu;
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display(nullptr);
    display.setVTFont(monospaceFont());   // 复用 tst_rendering 的等宽字体辅助，或内联 QFont(QStringLiteral("Monospace"))
    display.setBlinkingCursor(false);
    display.setScreenWindow(win);
    emu.setImageSize(2, 10);              // 缓冲 2 行 × 10 列

    // 输出一条 25 字符的超长行（历史 + 屏幕窗口共同容纳）
    emu.receiveData(QByteArray("abcdefghijklmnopqrstuvwxy"));

    display.setLineWrapMode(QTermWidget::LineWrapMode::SoftWrap);
    display.resize(10 * display.fontWidth() /* 近似 */, 5 * display.fontHeight());
    // 驱动显示更新
    QTest::qWait(50);

    // 合成后 _image 第 0/1/2 显示行应为该行的三段切片（经公开读口验证，
    // 具体读口用 display 的现有受保护成员或新增的测试友元，实现时择一）
    // 验证点：显示行 0 首字符 'a'，显示行 1 首字符 'k'，显示行 2 首字符 'u'
    // （通过给 TerminalDisplay 增加 Q_INVOKABLE 测试钩子 characterAtForTest(x,y) 读取 _image）
    QCOMPARE(display.characterAtForTest(0, 0).character, U'a');
    QCOMPARE(display.characterAtForTest(0, 1).character, U'k');
    QCOMPARE(display.characterAtForTest(0, 2).character, U'u');
}
```

`TerminalDisplay.h` 加测试钩子（注释标注仅供测试）：

```cpp
    /** @brief 仅供测试：读取 _image 中 (x,y) 处的字符。 */
    Character characterAtForTest(int x, int y) const { return _image[loc(x, y)]; }
    /** @brief 仅供测试：字体度量。 */
    int fontWidth() const { return _fontWidth; }
    int fontHeight() const { return _fontHeight; }
```

注意：`hscrollComposition` 用例在任务 6 横向滚动条落地后才有意义，本任务先写 `softWrapComposition`。

- [ ] **步骤 2：运行测试验证失败**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure
```

预期：编译失败（钩子不存在）或用例失败（尚未合成）。

- [ ] **步骤 3：实现视图合成**

`TerminalDisplay.h` private 声明：

```cpp
    /**
     * @brief 按当前模式把窗口内容合成到 dest（_lines × _columns 网格）。
     * @return true 表示已合成；false 表示调用方应走 getImage() 快路径。
     */
    bool composeViewImage(Character *dest);
    /** @brief SoftWrap：重建 _displayRows 并返回全缓冲各有效行长度。 */
    QVector<int> allLineLengths() const;
    /** @brief 可见窗口各行有效长度（窗口相对 0..windowLines-1）。 */
    QVector<int> windowLineLengths() const;
    /** @brief 当前可见内容的最大有效行宽。 */
    int maxVisibleLineWidth() const;
```

`TerminalDisplay.cpp` 实现：

```cpp
QVector<int> TerminalDisplay::windowLineLengths() const {
    QVector<int> lengths;
    if (!_screenWindow)
        return lengths;
    const int n = _screenWindow->windowLines();
    lengths.reserve(n);
    for (int i = 0; i < n; ++i)
        lengths.append(_screenWindow->windowLineLength(i));
    return lengths;
}

int TerminalDisplay::maxVisibleLineWidth() const {
    int maxLen = 0;
    for (int len : windowLineLengths())
        maxLen = qMax(maxLen, len);
    return maxLen;
}

QVector<int> TerminalDisplay::allLineLengths() const {
    QVector<int> lengths;
    if (!_screenWindow)
        return lengths;
    Screen *screen = _screenWindow->screen();
    const int total = screen->getHistLines() + screen->getLines();
    lengths.reserve(total);
    for (int i = 0; i < total; ++i)
        lengths.append(screen->getLineLength(i));
    return lengths;
}

bool TerminalDisplay::composeViewImage(Character *dest) {
    if (!_screenWindow)
        return false;
    const int winLines = _screenWindow->windowLines();

    if (_lineWrapMode == QTermWidget::LineWrapMode::SoftWrap) {
        _displayRows = buildFoldMap(windowLineLengths(), _columns, _lines);
        for (int y = 0; y < _lines; ++y) {
            if (y < _displayRows.size()) {
                const DisplayRow &row = _displayRows[y];
                _screenWindow->getWindowLineSlice(row.bufferLine, row.columnOffset,
                                                  _columns, dest + y * _columns);
            } else {
                for (int x = 0; x < _columns; ++x)
                    dest[y * _columns + x] = Character();   // 默认空格
            }
        }
        return true;
    }

    // NoWrap：存在超宽行或已偏移时按切片合成（含水平偏移）
    const int maxLen = maxVisibleLineWidth();
    if (maxLen <= _columns && _hScrollOffset == 0)
        return false;   // 走 getImage() 快路径
    for (int y = 0; y < _lines; ++y) {
        if (y < winLines)
            _screenWindow->getWindowLineSlice(y, _hScrollOffset, _columns,
                                              dest + y * _columns);
        else
            for (int x = 0; x < _columns; ++x)
                dest[y * _columns + x] = Character();
    }
    return true;
}
```

`updateImage()`（`:1457` 附近）改造：

```cpp
    // 原：Character *const newimg = _screenWindow->getImage();
    Character *composed = nullptr;
    Character *newimg = nullptr;
    if (_lineWrapMode != QTermWidget::LineWrapMode::NoWrap || _hScrollOffset > 0
        || maxVisibleLineWidth() > _columns) {
        composed = new Character[_lines * _columns];
        if (composeViewImage(composed))
            newimg = composed;
        else {
            delete[] composed;
            composed = nullptr;
        }
    }
    if (!newimg)
        newimg = _screenWindow->getImage();
    // ... 函数末尾（update(dirtyRegion) 之后）delete[] composed;
```

注意：`updateImage` 内 `lines/columns` 取自 `windowLines()/windowColumns()`，合成路径下比对网格是 `_lines × _columns`，需把 `columnsToUpdate` 固定为 `_columns`、行索引直接用合成网格 stride `_columns`。具体做法：在合成路径把局部变量 `lines = _lines; columns = _columns;`。同时 NoWrap 快路径条件变化时 `_resizing` 帧整屏置脏（沿用 `:1687-1701` 收缩区补脏机制即可覆盖）。

- [ ] **步骤 4：运行测试验证通过**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure && ctest --test-dir build --output-on-failure
```

预期：PASS 且无回归（重点看 tst_rendering/tst_emulation）。

- [ ] **步骤 5：Commit**

```bash
git add lib/src/display/TerminalDisplay.h lib/src/display/TerminalDisplay.cpp tests/tst_linewrap.cpp
git commit -m "feat: updateImage 支持按行显示模式合成视图图像"
```

---

### 任务 6：横向滚动条（NoWrap 模式）

**文件：**
- 修改：`lib/src/display/TerminalDisplay.h` / `TerminalDisplay.cpp`
  （构造函数 `:378-392`、`calcGeometry` `:4135`、`setSize` `:4195`、`updateImage` 滚动条段、`getCharacterPosition` `:3343`、`wheelEvent` `:3661`、keyPressEvent）
- 测试：`tests/tst_linewrap.cpp`

- [ ] **步骤 1：编写失败的测试**

```cpp
// private slots 追加：
    void hscrollRangeAndVisibility();
    void hscrollAutoReset();

// 实现：
void TestLineWrap::hscrollRangeAndVisibility()
{
    Vt102Emulation emu;
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display(nullptr);
    display.setVTFont(monospaceFont());
    display.setScreenWindow(win);
    emu.setImageSize(2, 10);
    display.resize(10 * display.fontWidth(), 2 * display.fontHeight());
    display.show();
    QTest::qWait(50);

    // 无超宽内容：横向滚动条隐藏
    QVERIFY(!display.hScrollBarVisibleForTest());

    // 输出 25 字符超长行（缓冲列数 10，行数据保留 25 格）
    emu.receiveData(QByteArray("abcdefghijklmnopqrstuvwxy"));
    QTest::qWait(50);

    // range = maxLen - _columns = 25 - 10 = 15，滚动条出现
    QVERIFY(display.hScrollBarVisibleForTest());
    QCOMPARE(display.hScrollBarMaximumForTest(), 15);
}
```

`TerminalDisplay.h` 测试钩子：

```cpp
    /** @brief 仅供测试：横向滚动条是否可见。 */
    bool hScrollBarVisibleForTest() const { return _hScrollBar && _hScrollBar->isVisible(); }
    /** @brief 仅供测试：横向滚动条 maximum。 */
    int hScrollBarMaximumForTest() const { return _hScrollBar ? _hScrollBar->maximum() : -1; }
```

- [ ] **步骤 2：运行测试验证失败**

预期：编译失败（钩子/成员不存在）。

- [ ] **步骤 3：实现横向滚动条**

构造函数（`_scrollBar` 创建后，`:392` 附近）：

```cpp
    _hScrollBar = new QScrollBar(Qt::Horizontal, this);
    _hScrollBar->setAutoFillBackground(true);
    connect(_hScrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        if (_hScrollOffset == value)
            return;
        _hScrollOffset = value;
        updateImage();   // 偏移变化：合成路径重建，整屏重绘
    });
    _hScrollBar->hide();
```

范围更新——`updateImage()` 中 `setScroll(...)` 调用（`:1461`）之后追加：

```cpp
    // 横向滚动条：仅 NoWrap 模式、存在超宽行时出现
    if (_lineWrapMode == QTermWidget::LineWrapMode::NoWrap && _hScrollBar) {
        const int range = qMax(0, maxVisibleLineWidth() - _columns);
        const bool wasVisible = _hScrollBar->isVisible();
        _hScrollBar->setRange(0, range);
        if (_hScrollOffset > range) {
            _hScrollOffset = range;
            _hScrollBar->setValue(range);
        }
        _hScrollBar->setVisible(range > 0);
        if (wasVisible != _hScrollBar->isVisible())
            updateImageSize();   // 显隐切换改变可用高度，重算几何
    } else if (_hScrollBar) {
        _hScrollBar->hide();
    }
```

注意防递归：`updateImageSize()` 会触发再次 `updateImage`，可见性已稳定后不会再次进入显隐分支，无需额外守卫；若实测抖动再加布尔守卫。

几何——`calcGeometry()`（`:4143-4161`）在 `_contentHeight` 计算后追加：

```cpp
    if (_hScrollBar && _hScrollBar->isVisible()) {
        _hScrollBar->resize(_contentWidth, _hScrollBar->sizeHint().height());
        _hScrollBar->move(_leftMargin, contentsRect().bottom() - _topBaseMargin
                                       - _hScrollBar->height() + 1);
        _contentHeight -= _hScrollBar->height();
    }
```

（`_lines` 在此之后由 `_contentHeight / _fontHeight` 得出，自动扣除。）`setSize()`（`:4195`）的 `newSize` 高度对称加 `_hScrollBar->isVisible() ? _hScrollBar->sizeHint().height() : 0`。

坐标换算——`getCharacterPosition`（`:3343`）算出 `column` 后、函数返回前：

```cpp
    if (_lineWrapMode == QTermWidget::LineWrapMode::NoWrap)
        column += _hScrollOffset;
```

绘制偏移：因 `_image` 已由 `composeViewImage` 按 `_hScrollOffset` 合成，`drawContents`/`calculateTextArea`/`imageToWidget` 均不改；仅 `drawContents` 与 `drawContentsLegacy` 中的选区查询改为缓冲坐标——把 `_screenWindow->isSelected(x, y)`（`:2529`、`:2679`）改为：

```cpp
    _screenWindow->isSelected(_lineWrapMode == QTermWidget::LineWrapMode::NoWrap
                                  ? x + _hScrollOffset : x, y)
```

（SoftWrap 的行/列换算在任务 7 统一处理，本任务先处理 NoWrap。）

Shift+滚轮——`wheelEvent`（`:3661`）函数开头插入：

```cpp
    if (_lineWrapMode == QTermWidget::LineWrapMode::NoWrap
        && (ev->modifiers() & Qt::ShiftModifier)
        && _hScrollBar && _hScrollBar->isVisible()) {
        const int steps = ev->angleDelta().y() / 120;
        _hScrollBar->setValue(_hScrollBar->value() - steps * 4);  // 每格 4 列
        ev->accept();
        return;
    }
```

自动回零——`keyPressEvent` 中发送按键给仿真层之前插入：

```cpp
    if (_hScrollBar && _hScrollBar->isVisible() && _hScrollOffset != 0)
        _hScrollBar->setValue(0);   // 打字即回到光标处
```

并在 `updateImage` 的横向滚动条段内追加：垂直滚动条处于底部（`_scrollBar->value() == _scrollBar->maximum()`）且新输出到达时同样 `setValue(0)`——用 `_resizing == false && composed 路径外` 不足以判断新输出，直接以「位于底部」为条件即可（回看历史时不回零）。

- [ ] **步骤 4：运行测试验证通过**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure && ctest --test-dir build --output-on-failure
```

预期：PASS 且无回归（重点 tst_rendering 的选区用例）。

- [ ] **步骤 5：Commit**

```bash
git add lib/src/display/TerminalDisplay.h lib/src/display/TerminalDisplay.cpp tests/tst_linewrap.cpp
git commit -m "feat: NoWrap 模式新增按需横向滚动条与水平视口偏移"
```

---

### 任务 7：软折叠交互与坐标换算收尾

**文件：**
- 修改：`lib/src/display/TerminalDisplay.h` / `TerminalDisplay.cpp`
  （`getCharacterPosition` `:3343`、`setScroll` 调用点 `:1461`、`scrollBarPositionChanged` `:2796`、`findLineStart/End` `:3432/:3463`、`findWordStart/End`、热点绘制 `paintFilters` `:2032`/`hotSpotRegion` `:1372`、`updateCursor` `:2720`、`preeditRect` `:1995`、`inputMethodQuery` `:3993`）
- 测试：`tests/tst_linewrap.cpp`

- [ ] **步骤 1：编写失败的测试**

```cpp
// private slots 追加：
    void softWrapCoordinateMapping();
    void softWrapScrollRange();

// 实现要点：
void TestLineWrap::softWrapCoordinateMapping()
{
    // 夹具同 softWrapComposition（缓冲 10 列，25 字符行折叠为 3 段）
    // 验证 displayToBuffer：显示 (0,1) → 缓冲 (10,0)；显示 (3,2) → 缓冲 (23,0)
    // 通过测试钩子 mapDisplayToBufferForTest(x, y) 暴露内部换算
}

void TestLineWrap::softWrapScrollRange()
{
    // 缓冲 10 列；构造历史总长折叠后 30 显示行，可见 5 显示行
    // 验证垂直滚动条 maximum == 30 - 5 = 25
    // 拖动到 value=3（第 1 缓冲行第 2 段起点）→ ScreenWindow 窗口顶行落在缓冲行 1
}
```

测试钩子：

```cpp
    /** @brief 仅供测试：显示网格坐标 → 缓冲窗口相对坐标。 */
    QPoint mapDisplayToBufferForTest(int x, int y) const;
```

- [ ] **步骤 2：运行测试验证失败**

预期：编译失败或用例失败。

- [ ] **步骤 3：实现坐标换算与滚动**

显示 → 缓冲（private）：

```cpp
QPoint TerminalDisplay::mapDisplayToBuffer(int x, int y) const {
    if (_lineWrapMode == QTermWidget::LineWrapMode::SoftWrap
        && y >= 0 && y < _displayRows.size()) {
        const DisplayRow &row = _displayRows[y];
        return {x + row.columnOffset, row.bufferLine};
    }
    return {x + (_lineWrapMode == QTermWidget::LineWrapMode::NoWrap ? _hScrollOffset : 0), y};
}
```

`getCharacterPosition`（`:3343`）末尾改为：

```cpp
    const QPoint buf = mapDisplayToBuffer(column, line);
    column = buf.x();
    line = buf.y();
```

（此后 `_iPntSel.ry() += _scrollBar->value()`（`:2933` 等）保持缓冲行语义，无需改动。）

`drawContents`/`drawContentsLegacy` 的 `isSelected(x, y)`（`:2529`、`:2679`）统一改为：

```cpp
    const QPoint buf = mapDisplayToBuffer(x, y);
    ... _screenWindow->isSelected(buf.x(), buf.y()) ...
```

垂直滚动条换算——`updateImage` 中 `setScroll` 调用点（`:1461`）：

```cpp
    if (_lineWrapMode == QTermWidget::LineWrapMode::SoftWrap) {
        const QVector<int> lengths = allLineLengths();
        int total = 0;
        for (int len : lengths)
            total += foldCountForLine(len, _columns);
        setScroll(displayRowOffsetOfLine(lengths, _columns, _screenWindow->currentLine()),
                  total);
    } else {
        setScroll(_screenWindow->currentLine(), _screenWindow->lineCount());
    }
```

`scrollBarPositionChanged`（`:2796`）中 `scrollTo` 的参数在 SoftWrap 下从显示行值反推缓冲行：

```cpp
    int target = _scrollBar->value();
    if (_lineWrapMode == QTermWidget::LineWrapMode::SoftWrap) {
        const QVector<int> lengths = allLineLengths();
        int acc = 0, line = 0;
        for (; line < lengths.size(); ++line) {
            const int c = foldCountForLine(lengths[line], _columns);
            if (target < acc + c)
                break;
            acc += c;
        }
        target = qMin(line, qMax(0, _screenWindow->lineCount() - _screenWindow->windowLines()));
        // 已知简化：窗口顶落在该缓冲行首段，段内精度丢失（v1 接受）
    }
    _screenWindow->scrollTo(target);
```

词/行边界——`findLineStart`（`:3432`）/`findLineEnd`（`:3463`）在 SoftWrap 下扩展到同一缓冲行的折叠段边界：进入函数时先经 `mapDisplayToBuffer` 得缓冲行，然后对该缓冲行在 `_displayRows` 中的首/末段分别取段首列 0 / 段内有效长度结尾；`LINE_WRAPPED` 判定逻辑只对硬换行生效（`_lineProperties` 按缓冲行索引，经 `mapDisplayToBuffer(x,0).y()` 取行号）。`findWordStart/End` 跨段扫描时列坐标用缓冲列（`mapDisplayToBuffer` 逐点换算）。

热点正向映射（缓冲 → 像素）——新增 private 辅助：

```cpp
/**
 * @brief 缓冲窗口行 bufLine 的列区间 [startCol, endCol] 对应的显示段列表。
 * @return 每项为 (显示行, 段内起始显示列, 段内结束显示列)；不可见时为空。
 */
QVector<QTriple<int, int, int>> displaySegmentsForRange(int bufLine, int startCol, int endCol) const;
```

实现：NoWrap 下若 `endCol >= _hScrollOffset && startCol < _hScrollOffset + _columns`，返回单项 `{bufLine, startCol - _hScrollOffset, endCol - _hScrollOffset}`（钳到 `[0, _columns-1]`）；SoftWrap 下遍历 `_displayRows` 中 `bufferLine == bufLine` 的行求交集。`paintFilters`（`:2060-2085`）、`hotSpotRegion`（`:1372`）、`mouseMoveEvent` 的 `_mouseOverHotspotArea`（`:3043-3067`）改为按段列表逐段换算像素。

光标/输入法——`updateCursor`（`:2720`）、`preeditRect`（`:1995`）、`inputMethodQuery(ImCursorRectangle)`（`:3993`）调用 `imageToWidget` 前，把 `cursorPosition()` 的缓冲坐标经反向换算（缓冲 → 显示）：

```cpp
/** @brief 缓冲窗口相对坐标 → 显示网格坐标；不可见时返回 QPoint(-1, -1)。 */
QPoint mapBufferToDisplay(int bufX, int bufY) const;
```

NoWrap：`{bufX - _hScrollOffset, bufY}`（x 越界返回 (-1,-1)，光标不可见时由各调用点现有钳制逻辑兜底）；SoftWrap：在 `_displayRows` 中找 `bufferLine == bufY && columnOffset <= bufX < columnOffset + _columns` 的段。光标不可见时 `updateCursor` 跳过绘制即可。

图形（Kitty/Sixel）锚定缓冲坐标不平移——`drawImagesBelowText`/`drawImagesAboveText`（`:2191/:2233`）**不改**；软折叠下图形所在行的显示映射由 `_displayRows` 决定（该行列偏移 0 的段），与设计文档「图形行不折叠」一致：合成时在 `composeViewImage` 中检查 `_screenWindow->getLineProperties()`/`imagePlacements`，含图形的行只生成 `columnOffset == 0` 的单段。实现方式：`windowLineLengths()` 不变，`composeViewImage` 的 SoftWrap 分支在 `buildFoldMap` 前把含图形行的有效长度钳到 `_columns`：

```cpp
        QVector<int> lengths = windowLineLengths();
        const auto props = _screenWindow->getLineProperties();
        for (int i = 0; i < lengths.size() && i < props.size(); ++i) {
            if (_screenWindow->screen()->imagePlacements(/* 绝对行 */ _screenWindow->currentLine() + i).size() > 0
                || _screenWindow->screen()->linkSegments(_screenWindow->currentLine() + i).size() > 0
                || (props[i] & (LINE_DOUBLEWIDTH | LINE_DOUBLEHEIGHT)))
                lengths[i] = qMin(lengths[i], _columns);
        }
```

（双宽/双高行同样不折叠，避免字形错位——探索报告指出双高行依赖相邻显示行副本，折叠会破坏配对。）

- [ ] **步骤 4：运行测试验证通过**

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_linewrap --output-on-failure && ctest --test-dir build --output-on-failure
```

预期：PASS 且全量无回归（重点 tst_rendering 热点/选区、tst_kittygraphics）。

- [ ] **步骤 5：Commit**

```bash
git add lib/src/display/TerminalDisplay.h lib/src/display/TerminalDisplay.cpp tests/tst_linewrap.cpp
git commit -m "feat: 软折叠模式坐标换算、滚动条显示行范围与热点分段映射"
```

---

### 任务 8：集成冒烟与文档收尾

**文件：**
- 测试：`tests/tst_linewrap.cpp`
- 修改：`AGENTS.md`（如「目录结构」或测试约定受影响则更新——本功能不改结构，仅需确认无需更新）

- [ ] **步骤 1：编写集成冒烟测试**

```cpp
// private slots 追加：
    void integrationNoWrapHScroll();
    void integrationSoftWrapAfterShrink();

// integrationNoWrapHScroll：QTermWidget 级（QTEST_MAIN 已就位），向其 PTY 会话不可控，
// 故沿用 Vt102Emulation + TerminalDisplay 夹具：
//   1. 输出 25 字符行 → 缩窄 widget 到 10 列 → 横向滚动条出现；
//      Shift+滚轮一步 → characterAtForTest(0,0) 变为 'e'（偏移 4 列）。
//   2. 切换到 SoftWrap → 横向滚动条隐藏，显示行 1 首字符为 'k'。
//   3. 切回 NoWrap → 偏移回零，characterAtForTest(0,0) 为 'a'。
```

- [ ] **步骤 2：运行全量测试**

```bash
cmake --build build --parallel && ctest --test-dir build --output-on-failure
```

预期：全部 PASS。

- [ ] **步骤 3：复查注释与文档一致性**

通读 diff，确认所有新增/修改注释为简体中文 Doxygen 风格（AGENTS.md 强制），规格文档行为描述与实现一致（特别是「已知简化：垂直滚动按缓冲行对齐，段内精度丢失」——将此行补记到规格文档「软折叠」一节）。

- [ ] **步骤 4：Commit**

```bash
git add tests/tst_linewrap.cpp docs/superpowers/specs/2026-09-03-line-wrap-mode-design.md
git commit -m "test: 行显示模式集成冒烟测试与规格补记"
```

---

## 自检结论

- **规格覆盖度：** API（任务 4）、软折叠映射（任务 3、5、7）、横向滚动条（任务 6）、坐标换算/选区/热点/光标（任务 7）、测试（各任务 + 任务 8）、边界（任务 7 的图形/双高行不折叠、越界补空格在任务 1）均有对应任务。
- **已知简化（v1 接受）：** 软折叠下垂直滚动条拖动按缓冲行对齐，段内精度丢失，拖动后 `setScroll` 回写 value 会产生轻微吸附。
- **风险点：** `updateImage` 合成路径与既有快路径（`fastScroll`/`scrollImage`）的交互——合成帧禁用 `QWidget::scroll` 像素搬迁快路径（`scrollImage` 入口判断 `_lineWrapMode == NoWrap && _hScrollOffset == 0` 才允许）；`updateImageSize()` 与横向条显隐的互调需防抖动。
