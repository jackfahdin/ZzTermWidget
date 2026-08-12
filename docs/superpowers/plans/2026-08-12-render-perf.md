# 渲染性能优化实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 依据已批准规格 `docs/superpowers/specs/2026-08-12-render-perf-design.md` 实现渲染性能优化——建立可重复的 benchmark 基线（解析吞吐 / 全量重绘 / TUI 局部重绘三用例），热点验证后削减 `drawContents`/`updateImage` 绘制路径开销，全程以像素等价性测试为安全网，前后数字对比入 CHANGELOG。

**架构：** 改动落点：`tests/tst_benchmark.cpp`（新建，QBENCHMARK 基线）、`tests/tst_rendering.cpp`（新建，像素等价性常驻测试）、`lib/src/display/TerminalDisplay.{h,cpp}`（批次聚合开关 + Legacy 基准路径 + 每格开销削减）、`tests/CMakeLists.txt`（注册）。不改公共 API（`lib/include/` 不动）。

**技术栈：** CMake 4.3.3、Qt 6.11.1（`/home/zz/Qt/6.11.1/gcc_64`，含 Qt6::Test）、C++20。benchmark 数字仅在 **Release** 构建下有参考意义。

**通用构建命令（下称"构建"）：**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64
cmake --build build --parallel
```

**通用测试命令（下称"ctest"）：** `ctest --test-dir build --output-on-failure`（测试属性已设 `QT_QPA_PLATFORM=offscreen`）。直接跑单个测试二进制时需自带环境变量，如：`QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark`。

**注释约定（强制）：** 新增/修改注释用简体中文 Doxygen 风格；移植遗留的英文注释可保留。`lib/third_party/` 不动。

**注意：** 在 main 分支直接提交（用户已授权）；commit 用约定式提交（中文描述）；不要 push。

**TDD 说明（本轮特殊性）：** 本轮不新增行为，传统 RED→GREEN 不适用于优化本身。安全网是**像素等价性常驻测试**：`drawContents` 保留改造前逐片段路径（`drawContentsLegacy`）作为基准，测试对同一份屏幕内容分别用两条路径离屏渲染成 QImage 并逐像素比对（`QImage::operator==`）。因此任务顺序固定：**先固化基准路径与等价性测试（任务 3），再动优化刀（任务 4）**——任务 3 时两条路径代码相同，测试平凡通过；任务 4 后它成为真正的回归门。benchmark 用例进 ctest 但**不设硬性性能断言**（机器差异大），仅保证可编译可运行；数字人工对比。

**勘察结论（实现依据，均已逐行核实）：**

- `drawContents`（`lib/src/display/TerminalDisplay.cpp:1968-2116`）**已有按样式段的批次聚合**：while 循环（:2033-2069）以 `foregroundColor == && backgroundColor == && rendition == && doubleWidth == && isLineChar() ==` 加宽度类别（`bigWidth`/`smallWidth`）为键合并连续字符，每 run 一次 `drawTextFragment`（:2096）→ `drawCharacters` → 单次 `drawText`（:1070/:1076）。规格"逐字符 drawText"的前提过时，本轮优化点是每格/每片段的剩余开销（见下）。
- 每格开销 ①：`fm.horizontalAdvance(QString::fromUcs4(&c, 1))`（:2022）与 while 条件内（:2039）——逐格 QString 堆分配 + 字体引擎查询。一次 `drawContents` 内字体不变，可按码点缓存。
- 每片段开销 ②：`drawCharacters`（:924）无条件计算 `_charWidth->string_font_width(text)` 与 `CharWidth::string_unicode_width(text)`（:972-973），但仅当 `_fix_quardCRT_issue33`（默认 false，`TerminalDisplay.h:1008`）且两者不等时才使用——可惰性化。
- 每片段开销 ③：无行缩放（非 `LINE_DOUBLEWIDTH`/`LINE_DOUBLEHEIGHT`）时 `setWorldTransform`（:2090）与其逆变换（:2101）是恒等操作，可跳过；`calculateTextArea`（:1952-1966）内部对恒等变换求逆结果不变，像素不受影响。
- `updateImage`（:1313）内逐格分组（:1393-1452）构建的 `disstrU`（:1359 分配、:1512 释放）与 `unistr`（:1440）**从未被读取**，是死代码；但其中逐格 `horizontalAdvance`（:1403、:1422）照样花钱。该分组唯一有副作用的输出是 `updateLine`（任何脏格即置位）。移除后脏区语义不变（脏行判定等价，宽字符尾部脏格场景脏区只会更大不会更小）。
- 聚合键即 `Character::equalsFormat()`（`lib/src/util/Character.h:154`）比较的三元组 + lineDraw/宽度类别；选区状态**不**入键（run 起点采样 `_screenWindow->isSelected(x, y)`，:2096）——保持现状不动，选区覆盖层、链接下划线（`paintFilters` :1812）、光标（`drawTextFragment` 内 :1114）与文本的先后关系不变。
- 等宽设施：`_fixedFont`/`_fontWidth`/`_fontHeight`/`_fontAscent` 由 `fontChange`（:228-264）维护；run 起点 x 按网格列算（`calculateTextArea` :1960），先例充分。
- 字体策略：现有绘制未设 `NoFontMerging`/style strategy。像素基线以现状为准，**不引入字体策略变更**（加了反而改变像素输出，违反硬约束）。
- 例外路径（保持原样、不参与改动）：宽字符双格（:2020-2021 检测、`len++` 跳尾部 :2065-2068）、`RE_EXTENDED_CHAR` 查表展开（:1996-2009、:2045-2057）、bidi 分支（:1064-1077）、quardCRT issue #33 逐字对齐分支（:997-1052）、`drawLineCharString` 制表符自绘（:696/:1055-1056）。
- 测试设施：`tests/tst_protocols.cpp` 示范离屏构造（栈上 `TerminalDisplay`、`setScreenWindow`、`resize(800,600)`，`resizeEvent` 同步进 `updateImageSize` :2151-2154）；`updateImage()` 是 public slot（`TerminalDisplay.h:454`），可在 `emu.receiveData`（同步解析）后直接调用，绕过 `bufferedUpdate` 定时器；`QWidget::render(&QImage)` 对隐藏 widget 可用且确定性强于 `grab()`；`paintFilters` 依赖 `QCursor::pos()` 在 offscreen 下恒定；`setBlinkingCursor(false)`/`setBlinkingTextEnabled(false)` 关掉闪烁源。
- `tests/CMakeLists.txt` 统一 foreach 注册（追加目标名即可，`QT_QPA_PLATFORM=offscreen` 属性自动继承）。QBENCHMARK 不依赖 GUI 后端，offscreen 可跑，结果打印到 stdout。
- 首次绘制有 `_drawTextTestFlag` 一次性度量流程（:1745-1747、:266），benchmark/等价性测试需先 warmup render 一次再计量/比对。

**对规格的三处执行调整（勘察驱动，已向用户确认）：**

1. 规格任务③"批次聚合实现"落实为**每格/每片段开销削减**（宽度缓存、惰性整段宽度、恒等变换跳过、`updateImage` 死代码移除），因为样式段聚合本身已存在；不改动既有聚合键与例外路径。
2. 规格"禁 kerning/连字整形"条款：不新增任何字体 style strategy——现状整段 drawText 的整形行为即像素基线，动它反而破约束。
3. 规格"固化改造前基准图"落实为**常驻双路径运行时比对**（`drawContentsLegacy` 永驻 + A/B 开关），而非仓库内 PNG 基线——光栅化结果随平台/字体变化，PNG 基线不可移植；另提供 `ZZQTW_RENDER_DUMP` 环境变量可在改造前导出基准 PNG 备查。

---

## 文件结构

- 修改：`lib/src/display/TerminalDisplay.{h,cpp}`、`tests/CMakeLists.txt`、`CHANGELOG`、`README.md`
- 创建：`tests/tst_benchmark.cpp`、`tests/tst_rendering.cpp`

---

## 任务 1：benchmark 基线设施（tst_benchmark.cpp 三用例）

**文件：**
- 创建：`tests/tst_benchmark.cpp`
- 修改：`tests/CMakeLists.txt`

- [ ] **步骤 1：新建 tests/tst_benchmark.cpp（完整内容）**

```cpp
#include <QtTest>
#include <QFontDatabase>
#include <QImage>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "Screen.h"
#include "TerminalDisplay.h"

/**
 * @brief 渲染性能 benchmark 基线：解析吞吐 / 全量重绘 / TUI 局部重绘三用例。
 * @note 不设硬性性能断言（机器差异大），仅保证可编译可运行；优化前后各跑一遍，
 *       数字人工对比并记入 CHANGELOG。数值仅在 Release 构建下有参考意义。
 */
class TestBenchmark : public QObject
{
    Q_OBJECT
private slots:
    void testParseThroughput();
    void testDrawFullRepaint();
    void testTuiPartialRepaint();
};

/**
 * @brief 构造混合内容负载：普通文本 / SGR 颜色转义 / CJK 宽字符 / 样式与制表符混排。
 * @param lineCount 生成行数。
 * @return UTF-8 编码的字节流。
 */
static QByteArray buildMixedPayload(int lineCount)
{
    QByteArray payload;
    for (int i = 0; i < lineCount; i++) {
        switch (i % 4) {
        case 0:
            payload += "build/output line " + QByteArray::number(i)
                     + ": plain ascii text lorem ipsum dolor sit\r\n";
            break;
        case 1:
            payload += "\033[38;5;" + QByteArray::number(16 + (i % 216))
                     + "m256色前景 \033[48;2;30;120;200mRGB底色\033[0m 混合样式输出\r\n";
            break;
        case 2:
            payload += "CJK 宽字符混排：中文测试文本 abc 123 コンソール 端末エミュレータ\r\n";
            break;
        default:
            payload += "\033[1;3;4m粗斜下划线\033[0m 制表符 ─│┌┐└┘ 与 emoji 😀 混排\r\n";
            break;
        }
    }
    return payload;
}

/**
 * @brief 构造显示测试环境：仿真 + 窗口 + 离屏显示组件（24x80，等宽字体，关闪烁）。
 */
static void initDisplayEnv(Vt102Emulation &emu, ScreenWindow *&win, TerminalDisplay &display)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    win = emu.createWindow();
    win->setWindowLines(24);
    display.setVTFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    display.setBlinkingCursor(false);
    display.setBlinkingTextEnabled(false);
    display.setScreenWindow(win);
    display.resize(800, 600);
}

/**
 * @brief 解析吞吐：向 Emulation+Screen 喂大块混合输出，计量 receiveData 吞吐。
 */
void TestBenchmark::testParseThroughput()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    ScreenWindow *win = emu.createWindow();
    win->setWindowLines(24);
    const QByteArray payload = buildMixedPayload(2000); // 约 200KB 混合内容
    QBENCHMARK {
        emu.receiveData(payload.constData(), int(payload.size()));
    }
}

/**
 * @brief 绘制吞吐：预填满屏混合内容后，循环全量重绘（render 到 QImage）。
 */
void TestBenchmark::testDrawFullRepaint()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initDisplayEnv(emu, win, display);
    const QByteArray content = buildMixedPayload(24);
    emu.receiveData(content.constData(), int(content.size()));
    display.updateImage();
    QImage image(display.size(), QImage::Format_ARGB32);
    display.render(&image); // warmup：吃掉 _drawTextTestFlag 一次性度量
    QBENCHMARK {
        display.render(&image);
    }
}

/**
 * @brief TUI 局部重绘：模拟 nvim 帧负载——光标行内容 + 状态行交替小区域更新。
 */
void TestBenchmark::testTuiPartialRepaint()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initDisplayEnv(emu, win, display);
    const QByteArray content = buildMixedPayload(24);
    emu.receiveData(content.constData(), int(content.size()));
    display.updateImage();
    const QByteArray frame =
            "\033[2;5H\033[38;5;45mmain.cpp\033[0m"            // 光标行内容更新
            "\033[24;1H\033[7m NORMAL  main.cpp  12:5  utf-8 \033[0m" // 状态行重写
            "\033[2;10H";                                       // 光标归位
    QImage image(display.size(), QImage::Format_ARGB32);
    display.render(&image); // warmup
    QBENCHMARK {
        emu.receiveData(frame.constData(), int(frame.size()));
        display.updateImage();   // public slot，绕过 bufferedUpdate 定时器直接驱帧
        display.render(&image);
    }
}

QTEST_MAIN(TestBenchmark)
#include "tst_benchmark.moc"
```

- [ ] **步骤 2：注册到 tests/CMakeLists.txt**

`QTERMWIDGET_TESTS` 列表改为：

```cmake
set(QTERMWIDGET_TESTS
    tst_charwidth
    tst_emulation
    tst_osc52
    tst_history
    tst_protocols
    tst_benchmark
)
```

- [ ] **步骤 3：构建并跑基线，固化改造前数字**

```bash
# 构建（见头部命令，必须 Release）后：
QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark | tee build/benchmark-baseline.txt
ctest --test-dir build --output-on-failure
```

预期：三用例均 PASS，stdout 含每个用例的每次迭代耗时（walltime）。`build/benchmark-baseline.txt` 为改造前基线（不入库，供任务 5 对比）。

- [ ] **步骤 4：Commit**

```bash
git add -A
git commit -m "test: 新增渲染性能 benchmark 基线（解析吞吐/全量重绘/TUI 局部重绘，无硬性断言）"
```

---

## 任务 2：热点验证（决策门）

**文件：** 无代码改动（临时插桩不提交）。

- [ ] **步骤 1：perf 可用性检查与采样**

```bash
perf --version && cat /proc/sys/kernel/perf_event_paranoid
perf record -g --call-graph dwarf -o build/perf-draw.data \
    env QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark testDrawFullRepaint
perf report --stdio -i build/perf-draw.data | head -100
perf record -g --call-graph dwarf -o build/perf-tui.data \
    env QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark testTuiPartialRepaint
perf report --stdio -i build/perf-tui.data | head -100
```

- [ ] **步骤 2：perf 不可用时用 QElapsedTimer 分段插桩（临时，验证后 `git checkout` 回退）**

在 `TerminalDisplay::drawContents`（`lib/src/display/TerminalDisplay.cpp:1968`）入口与循环内临时插入：

```cpp
    // 临时插桩（不提交）：分段计量 run 分组扫描 vs drawTextFragment
    static qint64 totalNs = 0, fragNs = 0;
    static int calls = 0;
    QElapsedTimer profTimer;
    profTimer.start();
```

`drawTextFragment(...)` 调用（:2096）改为：

```cpp
            const qint64 t0 = profTimer.nsecsElapsed();
            drawTextFragment(paint, textArea, unistr, &_image[loc(x, y)], tooWide, _screenWindow->isSelected(x, y));
            fragNs += profTimer.nsecsElapsed() - t0;
```

函数末尾（:2116 `}` 前）：

```cpp
    totalNs += profTimer.nsecsElapsed();
    if (++calls % 100 == 0)
        qInfo() << "drawContents profile: total" << totalNs / 1000 << "us /100calls, fragments"
                << fragNs / 1000 << "us (" << (totalNs ? fragNs * 100 / totalNs : 0) << "%)";
```

`QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark testDrawFullRepaint` 跑一遍读输出；同样方式给 `updateImage` 的逐格分组段（:1393-1452）计时，跑 `testTuiPartialRepaint`。验证完毕 `git checkout -- lib/src/display/TerminalDisplay.cpp` 回退。

- [ ] **步骤 3：写热点报告并过决策门**

把结论写入 `build/hotspot-report.txt`（不入库），按以下门裁决：

- `drawTextFragment` 内 QPainter 文本光栅化（`drawText`/字体引擎）占比 >50% → 现状 run 聚合已到位，剩余收益有限，任务 4 的 4.1/4.3 照做但调低收益预期，继续。
- 字符宽度/字体度量（`horizontalAdvance`、`QFontMetrics`、`string_font_width`、`CharWidth::string_unicode_width`）占比显著（>20%）→ 任务 4 全部子项（含 4.4 的 updateImage 死代码移除）即正确刀口，继续。
- 热点落在解析层（`Vt102Emulation`/`Screen`）或其他未预料处 → **停止任务 4，向用户回报热点报告并请求裁决**（不预设结论硬改）。

- [ ] **步骤 4：无需 commit**（报告落 `build/`，临时插桩已回退；确认 `git status` 干净）

---

## 任务 3：像素等价性测试先行（固化基准路径）

**文件：**
- 修改：`lib/src/display/TerminalDisplay.{h,cpp}`
- 创建：`tests/tst_rendering.cpp`
- 修改：`tests/CMakeLists.txt`

**说明：** 本任务不改任何绘制行为——`drawContents` 现有函数体即"批次聚合路径"（初始与 Legacy 逐字相同），测试此刻平凡通过；任务 4 改动后它成为真正的回归门。

- [ ] **步骤 1：TerminalDisplay.h 增加开关与 Legacy 声明**

public 段（`setBidiEnabled` :404 附近）追加：

```cpp
    /**
     * @brief 开关文本批次聚合绘制路径。
     * @param enabled true = 批次聚合路径（默认）；false = 改造前逐片段路径。
     * @note 供像素等价性测试 A/B 双渲染比对，也是聚合路径出问题时的回退手段。
     */
    void setTextBatchingEnabled(bool enabled) { _textBatchingEnabled = enabled; }

    /** @brief 查询文本批次聚合绘制路径是否启用。 */
    bool isTextBatchingEnabled() const { return _textBatchingEnabled; }
```

private 段（`drawContents` 声明 :735 之前）追加：

```cpp
    /**
     * @brief 改造前的逐片段文本绘制路径（drawContents 批次聚合的像素基准与回退）。
     * @param paint 目标画笔。
     * @param rect 需要重绘的区域。
     */
    void drawContentsLegacy(QPainter &paint, const QRect &rect);
```

private 成员（`_drawLineChars` 附近）追加：

```cpp
    bool _textBatchingEnabled = true; ///< 文本批次聚合路径开关（测试 A/B 与回退用）
```

- [ ] **步骤 2：TerminalDisplay.cpp 拆出 Legacy 路径并加 dispatch**

将现 `drawContents`（:1968-2116）函数体**逐字复制**为 `drawContentsLegacy`（只改函数名），然后把 `drawContents` 开头改为：

```cpp
void TerminalDisplay::drawContents(QPainter &paint, const QRect &rect) {
    // 批次聚合开关：关闭时走改造前的逐片段路径——
    // 该路径是像素等价性测试的常驻基准，也是聚合路径出问题时的回退手段
    if (!_textBatchingEnabled) {
        drawContentsLegacy(paint, rect);
        return;
    }

    QPoint tL = contentsRect().topLeft();
    // ……（其后函数体保持现状不变）
```

- [ ] **步骤 3：新建 tests/tst_rendering.cpp（完整内容）**

```cpp
#include <QtTest>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "TerminalDisplay.h"

/**
 * @brief 像素等价性常驻测试：批次聚合路径与逐片段 Legacy 路径双渲染逐像素比对。
 * @note 渲染性能优化（drawContents 改造）的安全网：两条路径对同一份屏幕内容的
 *       离屏渲染结果必须逐像素相等。比对在同一进程同一字体环境下进行，
 *       不依赖仓库内 PNG 基线（光栅化结果随平台/字体变化，不可移植）。
 *       设环境变量 ZZQTW_RENDER_DUMP=<目录> 可把双路径渲染结果落盘备查。
 */
class TestRendering : public QObject
{
    Q_OBJECT
private slots:
    void testBatchingPixelEquivalence();
    void testBatchingPixelEquivalenceAfterPartialUpdate();
};

/**
 * @brief 构造渲染测试环境：仿真 + 窗口 + 离屏显示组件（24x80，等宽字体，关闪烁保确定性）。
 */
static void initRenderEnv(Vt102Emulation &emu, ScreenWindow *&win, TerminalDisplay &display)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    win = emu.createWindow();
    win->setWindowLines(24);
    display.setVTFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    display.setBlinkingCursor(false);
    display.setBlinkingTextEnabled(false);
    display.setScreenWindow(win);
    display.resize(800, 600);
}

/**
 * @brief 构造覆盖各绘制路径的屏幕内容：普通文本、SGR 样式（粗/斜/下划线/删除线/反显）、
 *        256 色与 RGB 色、CJK 宽字符、组合字符（ExtendedCharTable 路径）、制表符自绘、
 *        双倍宽行（世界变换路径）。
 */
static QByteArray buildRenderContent()
{
    QByteArray s;
    s += "\033[H";                                                // 光标回左上角
    s += "plain ascii text row\r\n";
    s += "\033[1;3;4;9mstyled: bold italic underline strike\033[0m\r\n";
    s += "\033[38;5;196m256color fg\033[0m \033[48;2;10;200;30mrgb bg\033[0m\r\n";
    s += "CJK 宽字符混排 abc 中文测试 123\r\n";
    s += "combining: e\xcc\x81 o\xcc\x88 a\xcc\xa7\r\n";          // e+́ o+̈ a+̧（组合字符）
    s += "box: \xe2\x94\x80\xe2\x94\x82\xe2\x94\x8c\xe2\x94\x90\xe2\x94\x94\xe2\x94\x98\r\n"; // ─│┌┐└┘
    s += "\033#6" "double width line\r\n";                        // DECDWL 双倍宽行
    s += "\033[7mreverse video\033[0m\r\n";
    return s;
}

/**
 * @brief 用指定绘制路径把显示组件离屏渲染为 QImage。
 * @param display 目标显示组件。
 * @param batching true = 批次聚合路径；false = Legacy 逐片段路径。
 */
static QImage renderDisplay(TerminalDisplay &display, bool batching)
{
    display.setTextBatchingEnabled(batching);
    display.updateImage();
    QImage image(display.size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    display.render(&image);
    const QByteArray dumpDir = qgetenv("ZZQTW_RENDER_DUMP");
    if (!dumpDir.isEmpty())
        image.save(QString::fromLocal8Bit(dumpDir) + QLatin1Char('/')
                   + (batching ? QStringLiteral("batched") : QStringLiteral("legacy"))
                   + QStringLiteral(".png"));
    return image;
}

/**
 * @brief 整屏内容：两条绘制路径的渲染结果逐像素相等。
 */
void TestRendering::testBatchingPixelEquivalence()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    const QByteArray content = buildRenderContent();
    emu.receiveData(content.constData(), int(content.size()));

    const QImage batched = renderDisplay(display, true);
    const QImage legacy = renderDisplay(display, false);
    if (batched != legacy) {
        // 排障辅助：不一致时落盘到临时目录人工比对
        batched.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-batched.png")));
        legacy.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-legacy.png")));
    }
    QCOMPARE(batched, legacy); // operator== 即整图逐像素比对
}

/**
 * @brief 局部更新后再渲染（近似 TUI 帧负载）：两条路径仍逐像素相等。
 */
void TestRendering::testBatchingPixelEquivalenceAfterPartialUpdate()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    const QByteArray content = buildRenderContent();
    emu.receiveData(content.constData(), int(content.size()));
    renderDisplay(display, true); // 首帧，让 _image 就位

    const QByteArray edit =
            "\033[5;10H\033[38;5;45mEDIT\033[0m"   // 局部改写 + 颜色
            "\033[7;1Hxy";                          // 另一处小改动
    emu.receiveData(edit.constData(), int(edit.size()));

    const QImage batched = renderDisplay(display, true);
    const QImage legacy = renderDisplay(display, false);
    if (batched != legacy) {
        batched.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-batched-partial.png")));
        legacy.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-legacy-partial.png")));
    }
    QCOMPARE(batched, legacy);
}

QTEST_MAIN(TestRendering)
#include "tst_rendering.moc"
```

- [ ] **步骤 4：注册到 tests/CMakeLists.txt**

`QTERMWIDGET_TESTS` 列表追加 `tst_rendering`（紧随 `tst_benchmark`）。

- [ ] **步骤 5：构建 + ctest，确认通过（此刻两条路径代码相同，平凡通过）；导出改造前基准图备查**

```bash
# 构建后：
QT_QPA_PLATFORM=offscreen ZZQTW_RENDER_DUMP=$PWD/build ./build/tests/tst_rendering
ls -l build/legacy.png build/batched.png   # 改造前基准图（不入库）
ctest --test-dir build --output-on-failure
```

- [ ] **步骤 6：Commit**

```bash
git add -A
git commit -m "test: 新增绘制路径像素等价性常驻测试与批次聚合开关（Legacy 基准路径固化）"
```

---

## 任务 4：绘制路径开销削减（批次聚合落地）

**文件：**
- 修改：`lib/src/display/TerminalDisplay.cpp`

**前置条件：** 任务 2 决策门通过（热点在绘制路径内）。**明确不动**：聚合键与 while 分组条件语义、例外路径（宽字符/ExtendedCharTable/bidi/quardCRT/drawLineCharString）、选区与覆盖层先后关系、字体 style strategy、`updateImage` 脏区管线语义。规格"明确不做"清单（脏区重写/GPU/连字/Sixel）不越界。

- [ ] **步骤 1：drawContents 每格字体宽度缓存（仅改批次聚合路径，Legacy 不动）**

`drawContents` 中 `QFontMetrics fm(font());`（:1978）之后插入：

```cpp
    // 每格字体宽度查询缓存：一次绘制内字体不变，同一码点只查一次字体引擎。
    // ASCII 走定长数组，其余码点走散列；未命中按原逻辑逐字查询（取值与改造前一致）。
    std::array<int, 128> asciiAdvanceCache;
    asciiAdvanceCache.fill(-1);
    QHash<char32_t, int> advanceCache;
    const auto charAdvance = [&fm, &asciiAdvanceCache, &advanceCache](char32_t ch) -> int {
        if (ch < 128) {
            int &cached = asciiAdvanceCache[ch];
            if (cached < 0)
                cached = fm.horizontalAdvance(QChar(QLatin1Char(static_cast<char>(ch))));
            return cached;
        }
        auto it = advanceCache.find(ch);
        if (it == advanceCache.end())
            it = advanceCache.insert(ch, fm.horizontalAdvance(QString::fromUcs4(&ch, 1)));
        return it.value();
    };
```

文件头部 includes 确认含 `<array>`（无则补）。

:2022 处：

```cpp
            int charWidth = fm.horizontalAdvance(QString::fromUcs4(&c, 1));
```

改为：

```cpp
            const int charWidth = charAdvance(c);
```

while 条件内 :2039 处：

```cpp
                        !(_fixedFont && (nxtC = _image[loc(x+len,y)].character) && (nxtCharWidth = fm.horizontalAdvance(QString::fromUcs4(&nxtC, 1))) < _fontWidth) &&
```

改为：

```cpp
                        !(_fixedFont && (nxtC = _image[loc(x+len,y)].character) && (nxtCharWidth = charAdvance(nxtC)) < _fontWidth) &&
```

（注：`RE_EXTENDED_CHAR` 单元格的 `character` 是哈希码而非码点，改造前后都是直接拿去查宽度，缓存按同值键控，行为逐位一致。）

- [ ] **步骤 2：drawContents 跳过恒等世界变换（仅批次聚合路径）**

:2078-2101 段：

```cpp
            // Create a text scaling matrix for double width and double height lines.
            QTransform textScale;

            if (y < _lineProperties.size()) {
                if (_lineProperties[y] & LINE_DOUBLEWIDTH)
                    textScale.scale(2, 1);

                if (_lineProperties[y] & LINE_DOUBLEHEIGHT)
                    textScale.scale(1, 2);
            }

            // Apply text scaling matrix.
            paint.setWorldTransform(textScale, true);

            // calculate the area in which the text will be drawn
            QRect textArea = calculateTextArea(tLx, tLy, x, y, len, textScale);

            // paint text fragment
            drawTextFragment(paint, textArea, unistr, &_image[loc(x, y)], tooWide, _screenWindow->isSelected(x, y));

            _fixedFont = save__fixedFont;

            // reset back to single-width, single-height _lines
            paint.setWorldTransform(textScale.inverted(), true);
```

改为：

```cpp
            // Create a text scaling matrix for double width and double height lines.
            QTransform textScale;

            if (y < _lineProperties.size()) {
                if (_lineProperties[y] & LINE_DOUBLEWIDTH)
                    textScale.scale(2, 1);

                if (_lineProperties[y] & LINE_DOUBLEHEIGHT)
                    textScale.scale(1, 2);
            }

            // 无行缩放时跳过恒等世界变换的压栈/还原，减少每片段 QPainter 状态操作；
            // calculateTextArea 内部对恒等变换求逆结果不变，像素输出不受影响
            const bool hasTextScale = !textScale.isIdentity();
            if (hasTextScale)
                paint.setWorldTransform(textScale, true);

            // calculate the area in which the text will be drawn
            QRect textArea = calculateTextArea(tLx, tLy, x, y, len, textScale);

            // paint text fragment
            drawTextFragment(paint, textArea, unistr, &_image[loc(x, y)], tooWide, _screenWindow->isSelected(x, y));

            _fixedFont = save__fixedFont;

            // reset back to single-width, single-height _lines
            if (hasTextScale)
                paint.setWorldTransform(textScale.inverted(), true);
```

- [ ] **步骤 3：drawCharacters 整段宽度惰性计算**

`drawCharacters`（:971-973）：

```cpp
    // 计算整段文本的字体度量宽度与 Unicode 宽度
    int font_width = _charWidth->string_font_width(text);
    int width = CharWidth::string_unicode_width(text);
```

改为：

```cpp
    // quardCRT issue #33 对齐修正判定用的整段宽度：仅在该修正开关开启时才计算。
    // 开关默认关闭（TerminalDisplay.h:1008），惰性化避免每个文本片段重复扫描两遍字符串
    int font_width = 0;
    int width = 0;
    if (_fix_quardCRT_issue33) {
        font_width = _charWidth->string_font_width(text);
        width = CharWidth::string_unicode_width(text);
    }
```

（下游 `if (_fix_quardCRT_issue33 && font_width != width)` 判定不变；开关关闭时两个值为 0 但分支不进入。）

- [ ] **步骤 4：updateImage 移除逐格分组死代码**

`updateImage` 中删除以下死代码（`disstrU`/`unistr` 从未被读取，分组结果只产出 `updateLine`）：

- 删除 :1352-1354 的局部变量 `cf`、`_clipboard`、`cr` 声明（仅死代码使用）；
- 删除 :1359 的 `char32_t *disstrU = new char32_t[columnsToUpdate];` 与 :1512 的 `delete[] disstrU;`；
- :1383 的 `QFontMetrics fm(font());` 删除（仅死代码使用）；
- :1385-1453 的逐格循环整体替换为：

```cpp
        if (!_resizing) // not while _resizing, we're expecting a paintEvent
            for (x = 0; x < columnsToUpdate; ++x) {
                if ((newLine[x].rendition & RE_BLINK) != 0) {
                    _hasBlinker = true;
                }

                // 任何脏格都要求整行重绘。改造前此处按样式逐格分组构建文本串，
                // 但该串从未用于绘制（死代码），且宽字符尾部单独变脏时不置位；
                // 直接化后脏区只会更大不会更小，重绘正确性不受影响
                if (dirtyMask[x]) {
                    updateLine = true;
                }
            }
```

- [ ] **步骤 5：构建 + ctest，像素等价性测试此刻起承担真实回归职责**

```bash
# 构建后：
ctest --test-dir build --output-on-failure
```

预期：7 个套件全绿；`tst_rendering` 两例 PASS 证明批次聚合路径与 Legacy 路径逐像素一致。若 `tst_rendering` 失败：到 `QDir::temp()` 取 `zzqtermwidget-batched*.png` / `zzqtermwidget-legacy*.png` 人工比对定位，修到一致为止——**不允许**以降低比对强度过关。

- [ ] **步骤 6：Commit（两笔，各自先跑 `ctest --test-dir build -R tst_rendering --output-on-failure` 确认绿）**

```bash
# 步骤 1-3 完成后：
git add -A
git commit -m "perf: drawContents 每格字体宽度缓存、整段宽度惰性计算、跳过恒等世界变换"
# 步骤 4 完成后：
git add -A
git commit -m "perf: updateImage 移除未使用的逐格分组与重复宽度查询（死代码，行为不变）"
```

---

## 任务 5：复跑 benchmark 对比 + 收尾

**文件：**
- 修改：`CHANGELOG`、`README.md`

- [ ] **步骤 1：Release 构建后复跑 benchmark，与基线对比**

```bash
QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark | tee build/benchmark-after.txt
diff <(grep -E 'RESULT|per iteration' build/benchmark-baseline.txt) \
     <(grep -E 'RESULT|per iteration' build/benchmark-after.txt) || true
```

确认两个目标场景（`testDrawFullRepaint`、`testTuiPartialRepaint`）无回退、`testParseThroughput` 无回退（规格硬要求：不允许任一场景回退）。若出现回退：回到任务 2/4 定位，不达标不收尾。

- [ ] **步骤 2：全量回归**

```bash
ctest --test-dir build --output-on-failure
```

7 个套件（tst_charwidth / tst_emulation / tst_osc52 / tst_history / tst_protocols / tst_benchmark / tst_rendering）全绿。

- [ ] **步骤 3：CHANGELOG 顶部新增条目（实测数字在执行时填入）**

```text
ZzQTermWidget 渲染性能优化 / 2026-08-12
===============================
 * benchmark 基线设施：解析吞吐 / 全量重绘 / TUI 局部重绘三用例（tests/tst_benchmark.cpp，进 ctest 但无硬性性能断言）。
 * 像素等价性常驻测试：批次聚合路径与 Legacy 逐片段路径双渲染逐像素比对（tests/tst_rendering.cpp）。
 * drawContents 开销削减：每格字体宽度按码点缓存、quardCRT issue #33 整段宽度惰性计算、无行缩放时跳过恒等世界变换。
 * updateImage 移除未使用的逐格分组（含每格 horizontalAdvance 查询），脏行判定直接化，行为不变。
 * 实测（本机，Release）：全量重绘 <改造前> → <改造后>（<±x%>）；TUI 局部重绘 <改造前> → <改造后>（<±x%>）；解析吞吐 <改造前> → <改造后>（<±x%>）。
```

- [ ] **步骤 4：README.md "## 测试"节（:68 起）追加一段**

```markdown
性能与渲染回归：

- `tst_benchmark`：渲染性能基线（解析吞吐 / 全量重绘 / TUI 局部重绘），进 ctest 但无硬性性能断言，数字仅 Release 构建下有参考意义。
- `tst_rendering`：像素等价性测试，批次聚合与 Legacy 两条绘制路径双渲染逐像素比对，是绘制路径改造的安全网。
```

- [ ] **步骤 5：Commit**

```bash
git add -A
git commit -m "docs: CHANGELOG/README 记录渲染性能优化与 benchmark 前后对比"
```
