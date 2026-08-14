# 编程连字渲染（可选开关）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 为 TerminalDisplay 增加可选（默认关，opt-in）的编程连字渲染：ASCII 运算符序列（`->`、`!=`、`==>` 等）在字体具备恰好等宽的连字字形时整段整形绘制（Qt 内建 HarfBuzz 整形自动产出连字字形），否则静默回退现有整段路径；配套跨度脏区扩展（连字序列任一格变脏 → 脏跨度扩到整个序列）与像素等价/回退/互斥/脏区测试。

**架构：** 四组件分层——(1) `LigatureHelper`（lib/src/display/ 新内部工具，纯逻辑可独立单测：候选字符掩码 + 可连字子区间探测 + 整形宽度校验）；(2) `TerminalDisplay` 开关 API + 测试观测钩子（头文件内联，照抄现有 setter 惯例）；(3) `drawCharacters()` 默认非 bidi、非 #33 修复的整段路径内插入连字拆分绘制（开关关闭时零行为变化）；(4) `updateImage()` 跨度脏区块内加连字序列边界扩展（精确扩展，非整行升级）。测试追加进现有 tst_rendering/tst_benchmark 套件 + 新增 tst_ligature 套件（套件总数 9 → 10）。

**技术栈：** Qt6（本机 6.11.1，前缀 `/home/zz/Qt/6.11.1/gcc_64`，build/ 已配置）、C++20、QTest、CMake。构建 `cmake --build build --parallel`；测试 `ctest --test-dir build --output-on-failure`。

**规格：** `docs/superpowers/specs/2026-08-14-ligature-rendering-design.md`（已批准）。

---

## 勘察结论（落点已逐一确认）

1. **drawCharacters 默认整段路径**（`lib/src/display/TerminalDisplay.cpp:930-1100`）结构：
   - `#33` 修复分支在 :1012（`if (_fix_quardCRT_issue33 && font_width != width)`），注意其条件含宽度不一致判定——ASCII 运算符串宽度恒一致，**仅靠分支结构挡不住 #33 开关**，互斥守卫必须在连字条件里显式写 `!_fix_quardCRT_issue33`。
   - 默认路径：:1068 `else` → :1070 `isLineCharString` → :1077 `setLayoutDirection(LTR)` → :1079 `if (_bidiEnabled)` → **:1088 `else` 即唯一插入点**（bidi 由分支结构天然互斥）。
   - 现有默认绘制（:1089-1092）：`drawRect = rect` 加高 `_drawTextAdditionHeight`，`painter.drawText(drawRect, Qt::AlignBottom, LTR_OVERRIDE_CHAR + QString::fromStdU32String(text))`；`LTR_OVERRIDE_CHAR` 定义于 :108（`const QChar LTR_OVERRIDE_CHAR(0x202D)`，文件作用域，拆分绘制可直接用）。
   - 逐格等宽前提判定：`int(text.size()) * _fontWidth == rect.width()`——宽字符/扩展字符序列/双宽行混入时 `text.size()`（字符数）≠ `rect.width()/_fontWidth`（格数），自动回退。
2. **轮 7 跨度脏区在 `updateImage()` 内**（TerminalDisplay.cpp:1409 起），**不在 drawContents**——规格 §3.5 括注需修正。确切落点：:1593-1634 的 `if (updateLine)` 块内、`if (!fullLineDirty)`（:1597）中斜体邻居整行升级（:1610-1625）之后、:1626 闭括号之前。斜体先例即在同处。滚动快路径（:1489-1527）不受影响：moved 行内容逐格验证一致、新进行整段全新比对置脏，连字序列天然整段入脏。
3. **tst_rendering.cpp 框架用法**：`initRenderEnv(emu, win, display)`（:43，24x80、系统 FixedFont、关光标/文本闪烁）→ `emu.receiveData` 喂内容 → `pumpFrame(win)`（:181，走 `notifyOutputChanged()` 生产通路，双泵就位几何）→ `renderFull(display)`（:148，首帧 warmup 吃掉 `_drawTextTestFlag`）；增量重放 `replayDirtyRegion(display, base)`（:165）与 `renderFull` 逐像素 `QCOMPARE`；脏区形状断言用 `display.lastDirtyRegion()`（:263-269 与 :274-288 两个既有模式）。
4. **tst_benchmark.cpp 场景列模式**：`testFullScreenScroll_data`（:164-175）`QTest::addColumn` + `newRow` 开关 A/B 对照；帧驱动惯例 `receiveData → notifyOutputChanged → render(脏区)`；不设硬性断言、数字人工对比记入 CHANGELOG（文件头注释 :9-12 明示）。
5. **本机连字字体勘察**（直接检索字体目录，结论可靠；工作者可用 `fc-list | grep -iE 'fira|cascadia|jetbrains|hack|monoid'` 复核）：`/usr/share/fonts` 有 DejaVu Sans Mono、Hack、Liberation Mono、Noto Mono、Ubuntu[Sans]Mono 等；`~/.local/share/fonts` 仅 LXGWWenKai 系。**无 Fira Code/Cascadia/JetBrains Mono/Monoid/Iosevka/Victor Mono**（Hack 无连字字形）。→ 测试 1（连字生效）本机走 **QSKIP** 分支（规格 §4.1 许可）；测试 2/3/4/5 均不依赖连字字体，本机实跑。
6. **命名/文件惯例**：lib/src/display/ 现仅 TerminalDisplay.{h,cpp}；同级子系统均按"类名即文件名"PascalCase 成对（KittyGraphicsParser.h/.cpp、SixelDecoder、CharWidth），`#ifndef` 头卫、中文 Doxygen。新文件 `lib/src/display/LigatureHelper.{h,cpp}` 完全贴合。字符掩码先例：`Character::isLineChar()`（util/Character.h:158）位掩码内联；`CharWidth` 静态表惯例；本项目 C++20，`constexpr std::array<bool,128>` 掩码表为最简实现。
7. **setter 惯例**（TerminalDisplay.h）：内联 + 中文 Doxygen（`setTextBatchingEnabled` :421、`setScrollOptimizationEnabled` :431、`set_fix_quardCRT_issue33` :635）；测试观测钩子区 :436-450（`scrollFastPathFrameCount`/`lastDirtyRegion`，"镜像 _drawTextTestFlag 的内部观测点惯例"措辞照抄）；成员 NSDMI 风格：`_scrollFastPathFrames = 0`（:977）、`_textBatchingEnabled = true`（:1096）、`_fix_quardCRT_issue33 = false`（:1104）。CMake：lib/CMakeLists.txt 显式源单（SOURCES :4-22、HEADERS :24-45）；tests/CMakeLists.txt `QTERMWIDGET_TESTS` 名单（:2-12）注册即得 offscreen 环境。

### ⚠ 规格偏差点（执行时必读）

- **规格 §3.5 括注"脏区代码在 drawContents 批次路径内"与实际不符**：跨度脏区在 `updateImage()`（:1593-1634）。本计划落点按实际代码修正为 updateImage 跨度块，语义与规格一致（连字序列任一格变脏 → 扩到序列边界），采用**精确扩展**（规格允许的保守整行升级之精细化：候选段边界天然给出扩展终点）。
- **测试 1 的"开 ≠ 关"断言可能不成立**：关路径同样是整段 `drawText`，Qt 整形对 Fira Code 这类字体**可能已产出连字**（规格 §1 自述此现状），此时开/关逐像素一致但连字同样真实发生。本计划把测试 1 设计为双分支：开≠关时按规格断言（差异局限于序列行带）；开==关时改用"断形基线"（同内容但 `-`/`>` 间置 RE_BLINK 样式位断片段，必无连字）证明带内像素差异。两种现状下用例都给出确定的生效证据。**本机无连字字体，该用例本地 QSKIP**；生效证据的本地确定性验证由任务 2 的拆分路径计数钩子（`ligatureSplitFragmentCount`）承担。

---

## 类型/方法名跨任务一致性自检项

- `LigatureHelper`（lib/src/display/LigatureHelper.h）：静态方法 `isCandidateChar(char32_t)` / `findCandidateSpans(const std::u32string &)` / `widthMatches(const QFontMetricsF &, const std::u32string &, int cellWidth)`；内嵌结构 `Span { int start; int length; }`。
- `TerminalDisplay` 公共内联：`setLigaturesEnabled(bool)` / `ligaturesEnabled() const`（任务 1）；观测钩子 `ligatureSplitFragmentCount() const`（任务 1 声明，任务 2 起计数）。
- `TerminalDisplay` 私有：方法 `bool drawLigatureSpans(QPainter &, const QRect &, const std::u32string &)`（任务 2）；成员 `bool _ligaturesEnabled = false;`（任务 1）、`int _ligatureSplitFragments = 0;`（任务 1）。
- 新测试方法名：`testCandidateMask`/`testFindCandidateSpans`/`testWidthMatches`/`testSwitchApi`（tst_ligature）；`testLigatureSplitPathTaken`/`testLigatureRendering`/`testLigatureStyleBoundary`/`testLigatureMutualExclusion`（tst_rendering，任务 2）；`testLigatureDirtyRegion`（tst_rendering，任务 3）；`testLigatureRefresh_data`/`testLigatureRefresh`（tst_benchmark，任务 4）。
- tst_rendering 新静态辅助：`fontFormsLigature(const QFont &, const QString &)`、`findLigatureFontFamily()`、`regionDiffers(const QImage &, const QImage &, const QRect &)`。

---

## 任务 1：开关 API + LigatureHelper 判定设施（含单测）

**文件：**
- 创建：`lib/src/display/LigatureHelper.h`、`lib/src/display/LigatureHelper.cpp`、`tests/tst_ligature.cpp`
- 修改：`lib/CMakeLists.txt`（SOURCES :20 后加 cpp、HEADERS :42 后加 h）、`tests/CMakeLists.txt`（:3 `tst_charwidth` 后加 `tst_ligature`）、`lib/src/display/TerminalDisplay.h`（公共 setter/getter + 观测钩子 + 两个成员声明）

### Step 1.1 写失败测试（新套件 tst_ligature）

- [ ] 创建 `tests/tst_ligature.cpp`：

```cpp
#include <QtTest>
#include <QFontDatabase>
#include <string_view>
#include "LigatureHelper.h"
#include "TerminalDisplay.h"

/**
 * @brief LigatureHelper 判定设施与连字开关 API 单测。
 * @note 候选字符集 / 子区间探测为纯逻辑；宽度校验用系统等宽字体
 *       （offscreen 平台由 tests/CMakeLists.txt 统一注入）。
 */
class TestLigature : public QObject
{
    Q_OBJECT
private slots:
    void testCandidateMask();
    void testFindCandidateSpans();
    void testWidthMatches();
    void testSwitchApi();
};

/**
 * @brief 可连字字符集：恰好为 ASCII 运算符区
 *        "! # $ % & * + - . / : ; < = > ? @ \ ^ _ { | } ~"；
 *        空格、字母数字、逗号、括号、引号与非 ASCII 一律排除。
 */
void TestLigature::testCandidateMask()
{
    for (char c : std::string_view("!#$%&*+-./:;<=>?@\\^_{|}~"))
        QVERIFY2(LigatureHelper::isCandidateChar(char32_t(c)),
                 qPrintable(QStringLiteral("候选字符 '%1' 未命中").arg(c)));
    for (char c : std::string_view(" abcXYZ019,()[]\"'`"))
        QVERIFY2(!LigatureHelper::isCandidateChar(char32_t(c)),
                 qPrintable(QStringLiteral("非候选字符 '%1' 误命中").arg(c)));
    QVERIFY(!LigatureHelper::isCandidateChar(0x4E2D));  // 中
    QVERIFY(!LigatureHelper::isCandidateChar(0x2500));  // ─（制表符）
    QVERIFY(!LigatureHelper::isCandidateChar(0));       // 宽字符尾格占位
}

/**
 * @brief 子区间探测：连续候选字符段长度 ≥ 2 才输出，段间/串尾收尾正确，
 *        输出按起始索引升序且互不重叠。
 */
void TestLigature::testFindCandidateSpans()
{
    using Span = LigatureHelper::Span;
    const auto spans = [](const char32_t *s) {
        return LigatureHelper::findCandidateSpans(std::u32string(s));
    };
    { // 单段居中
        const QVector<Span> r = spans(U"a->b");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].start, 1);
        QCOMPARE(r[0].length, 2);
    }
    { // 全长段、长度 3
        const QVector<Span> r = spans(U"==>");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].start, 0);
        QCOMPARE(r[0].length, 3);
    }
    // 孤立候选字符（长度 1）不构成连字序列
    QVERIFY(spans(U"-a-").isEmpty());
    { // 多段
        const QVector<Span> r = spans(U"a--b--c");
        QCOMPARE(r.size(), 2);
        QCOMPARE(r[0].start, 1);
        QCOMPARE(r[0].length, 2);
        QCOMPARE(r[1].start, 4);
        QCOMPARE(r[1].length, 2);
    }
    // 空串、无候选内容（字母/空格/括号/引号/逗号/CJK）
    QVERIFY(spans(U"").isEmpty());
    QVERIFY(spans(U"if (a, b) \"x\"").isEmpty());
    QVERIFY(spans(U"\x4E2D\x6587").isEmpty());
    { // 段尾在串尾收尾
        const QVector<Span> r = spans(U"x!=");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].start, 1);
        QCOMPARE(r[0].length, 2);
    }
}

/**
 * @brief 整形宽度校验：等宽字体下 ASCII 串整形总宽恒等于 格数 × 格宽；
 *        期望宽人为偏移 ±2px 即超出 0.5px 容差判定失败；空串不可连字。
 */
void TestLigature::testWidthMatches()
{
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QFontMetricsF fm(fixed);
    const int cellWidth = qRound(fm.horizontalAdvance(QLatin1Char('0')));
    QVERIFY(LigatureHelper::widthMatches(fm, U"->", cellWidth));
    QVERIFY(LigatureHelper::widthMatches(fm, U"==>", cellWidth));
    // 期望宽每格 +1px：2 格串差 2px，超出 ±0.5px 容差
    QVERIFY(!LigatureHelper::widthMatches(fm, U"->", cellWidth + 1));
    QVERIFY(!LigatureHelper::widthMatches(fm, U"", cellWidth));
}

/**
 * @brief 开关 API：默认关（opt-in），set/get 往返正确。
 */
void TestLigature::testSwitchApi()
{
    TerminalDisplay display;
    QVERIFY(!display.ligaturesEnabled());
    display.setLigaturesEnabled(true);
    QVERIFY(display.ligaturesEnabled());
    display.setLigaturesEnabled(false);
    QVERIFY(!display.ligaturesEnabled());
}

QTEST_MAIN(TestLigature)
#include "tst_ligature.moc"
```

- [ ] 在 `tests/CMakeLists.txt` :3 `tst_charwidth` 之后插入一行：

```cmake
    tst_ligature
```

- [ ] 运行：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64 && cmake --build build --parallel`
- [ ] 预期：**编译失败**（`LigatureHelper.h` 不存在、`ligaturesEnabled` 未声明）——红灯成立。（CMake 新增测试可执行目标需重新 configure，上述命令已含。）

### Step 1.2 实现 LigatureHelper

- [ ] 创建 `lib/src/display/LigatureHelper.h`：

```cpp
#ifndef LIGATUREHELPER_H
#define LIGATUREHELPER_H

#include <QFontMetricsF>
#include <QVector>
#include <string>

/**
 * @brief 编程连字判定设施（内部工具，不进公共头 qtermwidget.h）。
 *
 * 可连字字符集为 ASCII 运算符区字符（空格、字母数字、逗号、括号、引号不参与）；
 * 探测把样式均匀的片段拆出"可连字候选子区间"（连续候选字符、长度 ≥ 2）；
 * 整形宽度校验保证"整段绘制不破坏对齐"——等宽编程字体的连字字形恰好占 n 格宽，
 * 字体无连字字形时整形结果就是逐字宽度之和，校验恒成立（静默回退语义）。
 * 连字是否真的发生由字体决定，本设施不感知也不缓存（字体切换后自动失效）。
 */
class LigatureHelper
{
public:
    /**
     * @brief 可连字候选子区间（片段内一段连续候选字符）。
     */
    struct Span {
        int start;  ///< 起始索引（字符下标，从 0 起）
        int length; ///< 长度（字符数，≥ 2）
    };

    /**
     * @brief 判定字符是否属于可连字 ASCII 运算符集合。
     * @param ch 字符码点。
     * @return true = 候选字符（! # $ % & * + - . / : ; < = > ? @ \ ^ _ { | } ~）。
     */
    static bool isCandidateChar(char32_t ch);

    /**
     * @brief 探测片段内的全部可连字候选子区间。
     * @param text 样式均匀的片段文本。
     * @return 候选子区间列表，按起始索引升序、互不重叠；无候选时为空。
     */
    static QVector<Span> findCandidateSpans(const std::u32string &text);

    /**
     * @brief 整形宽度校验：子串整形后总宽与"格数 × 单元格宽"相等才允许整段绘制。
     * @param fm 当前绘制字体度量（须为含粗斜体调整后的 painter 字体）。
     * @param text 候选子串。
     * @param cellWidth 单元格像素宽（_fontWidth）。
     * @return true = 整形总宽与格宽之和在 ±0.5px 容差内；空串恒 false。
     * @note Qt 整形默认启用 liga/calt；校验只保证对齐不破，连字发生与否由字体决定。
     */
    static bool widthMatches(const QFontMetricsF &fm, const std::u32string &text,
                             int cellWidth);
};

#endif // LIGATUREHELPER_H
```

- [ ] 创建 `lib/src/display/LigatureHelper.cpp`：

```cpp
#include "LigatureHelper.h"

#include <QString>
#include <array>
#include <string_view>

namespace {

/**
 * @brief 构造可连字候选字符掩码（下标即码点，仅 ASCII 0..127 有效）。
 * @return 掩码表；集合：! # $ % & * + - . / : ; < = > ? @ \ ^ _ { | } ~
 */
constexpr std::array<bool, 128> buildCandidateMask()
{
    std::array<bool, 128> mask{};
    for (char c : std::string_view("!#$%&*+-./:;<=>?@\\^_{|}~"))
        mask[static_cast<unsigned char>(c)] = true;
    return mask;
}

/// 可连字候选字符掩码（编译期常量，O(1) 查表）
constexpr std::array<bool, 128> kCandidateMask = buildCandidateMask();

} // namespace

bool LigatureHelper::isCandidateChar(char32_t ch)
{
    return ch < 128 && kCandidateMask[ch];
}

QVector<LigatureHelper::Span> LigatureHelper::findCandidateSpans(const std::u32string &text)
{
    QVector<Span> spans;
    const int n = int(text.size());
    int runStart = -1; ///< 当前连续候选段起点；-1 = 不在段内
    // 边界哨兵：i == n 视为非候选，统一段收尾逻辑
    for (int i = 0; i <= n; i++) {
        const bool cand = i < n && isCandidateChar(text[i]);
        if (cand && runStart < 0) {
            runStart = i;
        } else if (!cand && runStart >= 0) {
            if (i - runStart >= 2) // 长度 ≥ 2 才是可连字序列
                spans.append({runStart, i - runStart});
            runStart = -1;
        }
    }
    return spans;
}

bool LigatureHelper::widthMatches(const QFontMetricsF &fm, const std::u32string &text,
                                  int cellWidth)
{
    if (text.empty())
        return false;
    const qreal shaped = fm.horizontalAdvance(QString::fromStdU32String(text));
    const qreal cells = qreal(text.size()) * cellWidth;
    return qAbs(shaped - cells) <= 0.5; // 整像素容差（规格 §3.2）
}
```

- [ ] 在 `lib/CMakeLists.txt` :20 `src/display/TerminalDisplay.cpp` 之后插入：

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display/LigatureHelper.cpp
```

- [ ] 在 `lib/CMakeLists.txt` :42 `src/display/TerminalDisplay.h` 之后插入：

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display/LigatureHelper.h
```

### Step 1.3 开关 API + 观测钩子（TerminalDisplay.h，全内联）

- [ ] 在 `lib/src/display/TerminalDisplay.h` :434（`isScrollOptimizationEnabled()` 之后）插入：

```cpp
    /**
     * @brief 开关编程连字渲染。
     * @param enabled true = ASCII 运算符序列在字体具备恰好等宽的连字字形时
     *        整段整形绘制；false = 现有整段路径（默认，opt-in）。
     * @note 值变化时触发全量重绘（不做局部刷新优化）。与 bidi、quardCRT #33
     *       修复两开关两两互斥：任一开启时连字不生效（规格 §3.4）。
     *       内部接口，不进公共头 qtermwidget.h。
     */
    void setLigaturesEnabled(bool enabled) {
        if (_ligaturesEnabled == enabled)
            return;
        _ligaturesEnabled = enabled;
        update();
    }

    /** @brief 查询编程连字渲染是否启用。 */
    bool ligaturesEnabled() const { return _ligaturesEnabled; }
```

- [ ] 在 :450（`lastDirtyRegion()` 之后）插入：

```cpp
    /**
     * @brief 连字拆分绘制累计执行次数。
     * @return 自组件创建起 drawLigatureSpans() 实际完成拆分绘制的片段数。
     * @note 测试观测钩子：无连字字体时拆分绘制与整段绘制逐像素一致、无可观测
     *       行为差异，以此证明拆分路径真正执行（镜像 _drawTextTestFlag 的
     *       内部观测点惯例）；内部接口，不进公共头 qtermwidget.h。
     */
    int ligatureSplitFragmentCount() const { return _ligatureSplitFragments; }
```

- [ ] 在 :977（`_scrollFastPathFrames` 之后）插入：

```cpp
    int _ligatureSplitFragments = 0; ///< 连字拆分绘制累计执行次数（测试观测钩子用）
```

- [ ] 在 :1104（`_fix_quardCRT_issue33` 之后）插入：

```cpp
    bool _ligaturesEnabled = false; ///< 编程连字渲染开关（默认关，opt-in）
```

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R ligature --output-on-failure`
- [ ] 预期：`tst_ligature` 4 用例全绿；其余 9 套件不受影响（API 纯新增、默认关）。
- [ ] 提交：`feat(display): 新增编程连字判定设施 LigatureHelper 与开关 API`

---

## 任务 2：drawCharacters 连字拆分绘制（生效/回退/样式边界/互斥测试）

**文件：**
- 测试：`tests/tst_rendering.cpp`（槽声明 :36 后追加 4 个；静态辅助 3 个；实现追加在 `testDoubleHeightPixelEquivalence` 之后、`QTEST_MAIN` 前）
- 实现：`lib/src/display/TerminalDisplay.cpp`（include 区加 LigatureHelper.h；drawCharacters :1088 else 分支改造；新增 `drawLigatureSpans` 实现，放在 drawCharacters 之后 :1100 处）、`lib/src/display/TerminalDisplay.h`（:833 drawStyledUnderline 声明后加 drawLigatureSpans 声明）

### Step 2.1 写失败测试

- [ ] 在 `tests/tst_rendering.cpp` 槽声明区（:36 `void testDoubleHeightInkGeometry();` 之前）追加：

```cpp
    void testLigatureSplitPathTaken();
    void testLigatureRendering();
    void testLigatureStyleBoundary();
    void testLigatureMutualExclusion();
```

- [ ] 在 `pumpFrame` 定义（:181-184）之后追加三个静态辅助：

```cpp
/**
 * @brief 探测字体对指定序列是否真实产生连字：整段绘制与逐格绘制逐像素比对，
 *        有差异即整形产出了连字/上下文替换字形（无连字字体的等宽字体两者恒一致）。
 */
static bool fontFormsLigature(const QFont &font, const QString &seq)
{
    const QFontMetricsF fm(font);
    const qreal cw = fm.horizontalAdvance(QLatin1Char('-'));
    const int w = qCeil(cw * seq.size()) + 4;
    const int h = qCeil(fm.height()) + 4;
    QImage whole(w, h, QImage::Format_ARGB32);
    QImage piecewise(w, h, QImage::Format_ARGB32);
    whole.fill(Qt::black);
    piecewise.fill(Qt::black);
    {
        QPainter p(&whole);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(QPointF(2, 2 + fm.ascent()), seq);
    }
    {
        QPainter p(&piecewise);
        p.setFont(font);
        p.setPen(Qt::white);
        qreal x = 2;
        for (const QChar &c : seq) {
            p.drawText(QPointF(x, 2 + fm.ascent()), QString(c));
            x += cw;
        }
    }
    return whole != piecewise;
}

/**
 * @brief 在系统字体中找一款对 "->" 真实产生连字的编程字体。
 * @return 字体族名；本机/CI 无连字字体时返回空串（调用方 QSKIP）。
 */
static QString findLigatureFontFamily()
{
    static const QStringList candidates = {
        QStringLiteral("Fira Code"),     QStringLiteral("Cascadia Code"),
        QStringLiteral("Cascadia Mono"), QStringLiteral("JetBrains Mono"),
        QStringLiteral("Iosevka Term"),  QStringLiteral("Iosevka"),
        QStringLiteral("Victor Mono"),   QStringLiteral("Hasklig"),
        QStringLiteral("Monoid"),
    };
    const QStringList available = QFontDatabase::families();
    for (const QString &name : candidates) {
        if (available.contains(name)
                && fontFormsLigature(QFont(name, 12), QStringLiteral("->")))
            return name;
    }
    return QString();
}

/**
 * @brief 判定两图在指定矩形内是否存在像素差异。
 */
static bool regionDiffers(const QImage &a, const QImage &b, const QRect &r)
{
    const QRect area = r & a.rect();
    for (int y = area.top(); y <= area.bottom(); y++)
        for (int x = area.left(); x <= area.right(); x++)
            if (a.pixel(x, y) != b.pixel(x, y))
                return true;
    return false;
}
```

- [ ] 在文件末尾（`testDoubleHeightPixelEquivalence` 实现之后、`QTEST_MAIN` 前）追加四个用例：

```cpp
/**
 * @brief 拆分路径执行证据 + 无连字字体静默回退：开关开启后含运算符序列的片段
 *        确实走拆分绘制（观测钩子计数递增），且开/关渲染逐像素一致（无连字
 *        字体下拆分绘制的逐字拼合结果与整段绘制相同，规格 §3.7 回退语义）。
 * @note 本用例不依赖连字字体，本地确定性验证"拆分路径真实执行且零行为变化"；
 *       若某环境把系统等宽字体映射到连字字体，像素一致子断言不适用（QSKIP）。
 */
void TestRendering::testLigatureSplitPathTaken()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    display.setLigaturesEnabled(true);
    const QByteArray content = "\033[?25l\033[2;1Ha->b != c ==>";
    emu.receiveData(content.constData(), int(content.size()));
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup：吃掉 _drawTextTestFlag
    const int before = display.ligatureSplitFragmentCount();
    const QImage on = renderFull(display);
    // 拆分路径确已执行（无可观测行为差异，以内部观测点为证据，
    // 镜像 _drawTextTestFlag/_scrollFastPathFrames 惯例）
    QVERIFY(display.ligatureSplitFragmentCount() > before);

    if (fontFormsLigature(display.font(), QStringLiteral("->")))
        QSKIP("系统等宽字体带连字字形，回退像素一致子断言不适用");
    display.setLigaturesEnabled(false);
    const QImage off = renderFull(display);
    if (on != off) { // 排障辅助：落盘人工比对
        on.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-lig-on.png")));
        off.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-lig-off.png")));
    }
    QCOMPARE(on, off);
}

/**
 * @brief 连字生效：连字字体下开启开关，"->" 序列渲染为连字字形。
 * @note 双分支断言（勘察已确认的规格偏差点）：关路径同样是整段 drawText，
 *       Qt 整形可能已产出连字（规格 §1 现状）——开≠关时按规格 §4.1 断言
 *       差异局限于序列行带；开==关时改用断形基线（同内容但 '-' 置 RE_BLINK
 *       样式位与 '>' 分属两片断，闪烁已关渲染无视觉差异，整形上下文在片段
 *       边界断裂必无连字）证明带内存在像素差异。两种现状都给出确定生效证据。
 * @note 本机/CI 无连字字体时 QSKIP（规格 §4.1 许可）。
 */
void TestRendering::testLigatureRendering()
{
    const QString family = findLigatureFontFamily();
    if (family.isEmpty())
        QSKIP("本机/CI 无编程连字字体（Fira Code/Cascadia Code/JetBrains Mono 等）");

    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    display.setVTFont(QFont(family, 12));
    display.resize(800, 600); // setVTFont 经 propagateSize 可能调整几何，复位对齐
    display.setLigaturesEnabled(true);
    const QByteArray content = "\033[?25l\033[2;1Ha->b != c ==>";
    emu.receiveData(content.constData(), int(content.size()));
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup
    const QImage on = renderFull(display);
    display.setLigaturesEnabled(false);
    const QImage off = renderFull(display);

    const int fh = display.fontHeight();
    const int fw = display.fontWidth();
    const int top0 = display.contentsRect().top() + display.margin();
    const int left0 = display.contentsRect().left() + display.margin();
    const QRect rowBand(0, top0 + fh, display.width(), fh);     // 行 1 整行带
    const QRect arrowCells(left0 + fw, top0 + fh, 2 * fw, fh);  // "->" 两格

    if (on != off) {
        // 规格 §4.1 主路径：开关改变渲染；序列外区域（行带以外）逐像素一致
        QImage outsideOn = on, outsideOff = off;
        {
            QPainter p(&outsideOn);
            p.fillRect(rowBand, Qt::black);
        }
        {
            QPainter p(&outsideOff);
            p.fillRect(rowBand, Qt::black);
        }
        QCOMPARE(outsideOn, outsideOff);
    } else {
        // 关路径已被 Qt 整形产出连字的现状：开/关一致。用断形基线证明连字
        // 真实发生——基线中 "->" 两字符分属两片断，任何路径都不可能连字
        Vt102Emulation emu2;
        ScreenWindow *win2 = nullptr;
        TerminalDisplay display2;
        initRenderEnv(emu2, win2, display2);
        display2.setVTFont(QFont(family, 12));
        display2.resize(800, 600);
        const QByteArray broken = "\033[?25l\033[2;1Ha-\033[5m>\033[0mb != c ==>";
        emu2.receiveData(broken.constData(), int(broken.size()));
        pumpFrame(win2);
        pumpFrame(win2);
        renderFull(display2); // warmup
        const QImage baseline = renderFull(display2);
        QVERIFY2(regionDiffers(on, baseline, arrowCells),
                 "连字字体下 \"->\" 渲染与断形基线无像素差异（连字未发生）");
    }
}

/**
 * @brief 样式边界："-" 与 ">" 分属两个样式片段时不跨片段连字——各片段长度
 *        为 1 无候选段，拆分路径不执行，开/关渲染逐像素一致。
 */
void TestRendering::testLigatureStyleBoundary()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    const QByteArray content = "\033[?25l\033[2;1Ha\033[31m-\033[32m>\033[0mb";
    emu.receiveData(content.constData(), int(content.size()));
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup
    display.setLigaturesEnabled(true);
    const int before = display.ligatureSplitFragmentCount();
    const QImage on = renderFull(display);
    display.setLigaturesEnabled(false);
    const QImage off = renderFull(display);
    QCOMPARE(on, off);
    // 拆分路径一次都不应执行（候选段长度 ≥ 2 才拆分）
    QCOMPARE(display.ligatureSplitFragmentCount(), before);
}

/**
 * @brief 互斥：bidi 开启 / quardCRT #33 修复开启时连字拆分不生效
 *        （拆分路径计数恒 0，规格 §3.4 三开关两两不叠加）。
 * @note #33 分支条件含宽度不一致判定，ASCII 串挡不住——互斥靠连字条件里
 *       显式的 !_fix_quardCRT_issue33 守卫，本用例即该守卫的回归证据。
 */
void TestRendering::testLigatureMutualExclusion()
{
    { // bidi 优先
        Vt102Emulation emu;
        ScreenWindow *win = nullptr;
        TerminalDisplay display;
        initRenderEnv(emu, win, display);
        display.setBidiEnabled(true);
        display.setLigaturesEnabled(true);
        const QByteArray content = "\033[?25l\033[2;1Ha->b != c ==>";
        emu.receiveData(content.constData(), int(content.size()));
        pumpFrame(win);
        pumpFrame(win);
        renderFull(display);
        QCOMPARE(display.ligatureSplitFragmentCount(), 0);
    }
    { // quardCRT #33 修复优先
        Vt102Emulation emu;
        ScreenWindow *win = nullptr;
        TerminalDisplay display;
        initRenderEnv(emu, win, display);
        display.set_fix_quardCRT_issue33(true);
        display.setLigaturesEnabled(true);
        const QByteArray content = "\033[?25l\033[2;1Ha->b != c ==>";
        emu.receiveData(content.constData(), int(content.size()));
        pumpFrame(win);
        pumpFrame(win);
        renderFull(display);
        QCOMPARE(display.ligatureSplitFragmentCount(), 0);
    }
}
```

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R rendering --output-on-failure`
- [ ] 预期：编译失败（`ligatureSplitFragmentCount`/`drawLigatureSpans` 未实现——计数 getter 任务 1 已加，此处实际失败点为 `testLigatureSplitPathTaken` 的 `QVERIFY(count > before)`：编译通过、运行红）。`testLigatureRendering` 本机 QSKIP；`testLigatureStyleBoundary`/`testLigatureMutualExclusion` 空转绿（锁定语义，防后续回归）。

### Step 2.2 实现连字拆分绘制

- [ ] 在 `lib/src/display/TerminalDisplay.cpp` 文件头 include 区（`#include "TerminalDisplay.h"` 之后）追加：

```cpp
#include "LigatureHelper.h"
```

- [ ] 在 `lib/src/display/TerminalDisplay.h` :833-834（`drawStyledUnderline` 声明）之后追加：

```cpp
    /**
     * @brief 连字拆分绘制：把片段拆成"连字子区间 + 普通子区间"交替序列分别绘制。
     * @param painter 画具（字体/画笔已按片段样式就位）。
     * @param rect 片段格矩形。
     * @param text 片段文本。
     * @return true = 已按拆分序列完成绘制；false = 无可拆分区间、逐格等宽前提
     *         不满足或宽度校验失败，调用方须回退现有整段绘制。
     */
    bool drawLigatureSpans(QPainter &painter, const QRect &rect,
                           const std::u32string &text);
```

- [ ] 将 `lib/src/display/TerminalDisplay.cpp` :1088-1092 的默认非 bidi else 分支替换为：

```cpp
            } else {
                // 连字拆分（opt-in）：仅默认非 bidi、非 #33 修复路径生效——
                // bidi 由分支结构互斥；#33 修复开关显式判断互斥（其分支条件含
                // 宽度不一致判定，ASCII 运算符串挡不住，规格 §3.4 三开关两两不叠加）。
                // drawLigatureSpans 返回 false 时回退现有整段绘制，逐字节同现状
                if (!(_ligaturesEnabled && !_fix_quardCRT_issue33)
                        || !drawLigatureSpans(painter, rect, text)) {
                    QRect drawRect(rect.topLeft(), rect.size());
                    drawRect.setHeight(rect.height() + _drawTextAdditionHeight);
                    painter.drawText(drawRect, Qt::AlignBottom, LTR_OVERRIDE_CHAR + QString::fromStdU32String(text));
                }
            }
```

- [ ] 在 `drawCharacters` 实现结束之后（:1100 `}` 之后、`drawStyledUnderline` 之前）插入：

```cpp
bool TerminalDisplay::drawLigatureSpans(QPainter &painter, const QRect &rect,
                                        const std::u32string &text) {
    // 逐格等宽前提：字符数 × 格宽 == 片段宽。宽字符/扩展字符序列/双宽行混入时
    // 字符数 ≠ 格数，子区间 x 坐标无法按格定位，整体回退整段绘制
    if (text.size() < 2 || int(text.size()) * _fontWidth != rect.width())
        return false;
    // 字符掩码预扫描（O(n) 查表）：无候选段即回退，无字体度量开销
    const QVector<LigatureHelper::Span> candidates =
            LigatureHelper::findCandidateSpans(text);
    if (candidates.isEmpty())
        return false;
    // 整形宽度校验：只保留"整形后总宽不变"的子区间（等宽连字字形或逐字回退
    // 均满足；非严格等宽字体误判的代价仅是整段绘制，与现状行为相同，
    // 不产生新故障模式）。结果不跨帧缓存，字体切换后自动失效
    const QFontMetricsF fm(painter.font());
    QVector<LigatureHelper::Span> spans;
    for (const LigatureHelper::Span &span : candidates) {
        if (LigatureHelper::widthMatches(
                    fm, text.substr(span.start, span.length), _fontWidth))
            spans.append(span);
    }
    if (spans.isEmpty())
        return false;

    // 交替序列绘制：普通子区间沿用现有整段调用样式（LTR 覆盖字符 + AlignBottom +
    // _drawTextAdditionHeight 加高），连字子区间不加 LTR 覆盖字符保证整形上下文纯净
    const auto drawPiece = [&](int start, int length, bool ligature) {
        const QRect pieceRect(rect.x() + start * _fontWidth, rect.y(),
                              length * _fontWidth,
                              rect.height() + _drawTextAdditionHeight);
        const QString piece =
                QString::fromStdU32String(text.substr(start, length));
        if (ligature)
            painter.drawText(pieceRect, Qt::AlignBottom, piece);
        else
            painter.drawText(pieceRect, Qt::AlignBottom, LTR_OVERRIDE_CHAR + piece);
    };
    int pos = 0;
    for (const LigatureHelper::Span &span : spans) {
        if (span.start > pos)
            drawPiece(pos, span.start - pos, false);
        drawPiece(span.start, span.length, true);
        pos = span.start + span.length;
    }
    if (pos < int(text.size()))
        drawPiece(pos, int(text.size()) - pos, false);
    ++_ligatureSplitFragments; // 测试观测钩子：拆分路径确已执行
    return true;
}
```

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R 'ligature|rendering' --output-on-failure`
- [ ] 预期：`testLigatureSplitPathTaken` 转绿；`testLigatureRendering` QSKIP；rendering 套件全部既有用例（批次/Legacy 双路径像素等价、跨度脏区、滚动快路径、下划线、双高行）保持绿——证明开关默认关时零行为变化、拆分绘制像素安全。
- [ ] 提交：`feat(display): drawCharacters 连字拆分绘制（默认关，bidi/#33 互斥）`

---

## 任务 3：跨度脏区扩展（连字序列任一格变脏 → 扩到序列边界）

**文件：**
- 测试：`tests/tst_rendering.cpp`（槽声明 `testLigatureMutualExclusion` 后追加 1 个；实现追加在 `testLigatureMutualExclusion` 之后）
- 实现：`lib/src/display/TerminalDisplay.cpp`（updateImage 跨度块 :1610-1625 斜体升级之后、:1626 之前）

### Step 3.1 写失败测试

- [ ] 在 `tests/tst_rendering.cpp` 槽声明区（`testLigatureMutualExclusion();` 之后）追加：

```cpp
    void testLigatureDirtyRegion();
```

- [ ] 在 `testLigatureMutualExclusion` 实现之后追加：

```cpp
/**
 * @brief 连字序列脏区扩展：编辑 4 格候选段 "==>>" 的中间一格，脏跨度必须从
 *        编辑格 ±1 格的 [3,5] 向左扩展到序列起点（列 2），且增量重放与全量
 *        渲染逐像素相等。
 * @note 形状断言即先失败测试：未实现扩展时脏区左缘在列 3（minX-1）。
 *       扩展逻辑只依赖候选字符掩码、与字体无关，无连字字体本地同样确定性验证；
 *       增量重放比对为兜底安全网（连字字体下漏扩必留新旧字形混杂残影）。
 */
void TestRendering::testLigatureDirtyRegion()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    display.setLigaturesEnabled(true);
    const QByteArray setup = "\033[?25l\033[1;1Hx ==>> y"; // 候选段 "==>>" 在列 2..5（0 起）
    emu.receiveData(setup.constData(), int(setup.size()));
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup：吃掉 _drawTextTestFlag
    const QImage base = renderFull(display);

    // 编辑候选段中间一格（列 4：'>' → '='，新内容 "===>" 仍为候选段）
    const QByteArray edit = "\033[1;5H=";
    emu.receiveData(edit.constData(), int(edit.size()));
    pumpFrame(win);

    // 形状断言：脏跨度左缘必须扩展到序列起点列 2
    const int fw = display.fontWidth();
    const int fh = display.fontHeight();
    const int left0 = display.contentsRect().left() + display.margin();
    const int top0 = display.contentsRect().top() + display.margin();
    const QRect band(0, top0, display.width(), fh);
    int minLeft = display.width();
    for (const QRect &r : display.lastDirtyRegion())
        if (r.intersects(band))
            minLeft = qMin(minLeft, r.left());
    QVERIFY2(minLeft <= left0 + 2 * fw,
             qPrintable(QStringLiteral("连字序列脏区左缘 %1 未扩展到序列起点 %2")
                        .arg(minLeft).arg(left0 + 2 * fw)));

    // 像素兜底：增量重放与全量渲染逐像素相等
    const QImage incremental = replayDirtyRegion(display, base);
    const QImage full = renderFull(display);
    if (incremental != full) { // 排障辅助：落盘人工比对
        incremental.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-lig-incremental.png")));
        full.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-lig-full.png")));
        base.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-lig-base.png")));
    }
    QCOMPARE(incremental, full);
}
```

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R rendering --output-on-failure`
- [ ] 预期：`testLigatureDirtyRegion` 失败于形状断言（`minLeft == left0 + 3*fw`，未扩展）；增量重放比对本机（无连字字体）即已绿，不构成失败点。其余用例绿。

### Step 3.2 实现脏跨度扩展

- [ ] 在 `lib/src/display/TerminalDisplay.cpp` :1622-1625 的斜体邻居升级块之后、:1626 `}`（`if (!fullLineDirty)` 闭括号）之前插入：

```cpp
                // 连字序列边界扩展：候选运算符的连续段（长度 ≥ 2 即连字序列）若与脏
                // 跨度相交，整段纳入脏区——连字字形横跨整段落墨，段内任一格变脏而只
                // 重绘部分格会新旧字形混杂残留（与斜体邻居升级同一动机；候选段边界
                // 天然给出扩展终点，故做精确扩展而非整行升级）。只依赖字符掩码，
                // 与字体是否真有连字字形无关（无连字字形时扩展无害，仅脏区略宽）
                if (_ligaturesEnabled) {
                    auto isCand = [&](int i) {
                        return i >= 0 && i < columnsToUpdate
                               && (newLine[i].rendition & RE_EXTENDED_CHAR) == 0
                               && LigatureHelper::isCandidateChar(newLine[i].character);
                    };
                    // 跨度端格本身是候选字符才扩展：孤立候选字符（段长 1）
                    // 邻居非候选，while 条件即刻失败，天然零扩展
                    if (isCand(spanMin))
                        while (spanMin > 0 && isCand(spanMin - 1))
                            --spanMin;
                    if (isCand(spanMax))
                        while (spanMax + 1 < columnsToUpdate && isCand(spanMax + 1))
                            ++spanMax;
                }
```

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R 'ligature|rendering' --output-on-failure`
- [ ] 预期：`testLigatureDirtyRegion` 转绿；rendering 套件全部既有用例保持绿（开关默认关时该块被 `_ligaturesEnabled` 短路，零行为变化）。
- [ ] 提交：`feat(display): 连字序列脏跨度边界扩展（段内任一格变脏整段重绘）`

---

## 任务 4：benchmark 连字场景列 + 文档收尾 + 全量验证

**文件：**
- 测试：`tests/tst_benchmark.cpp`（槽声明 :30 后追加 2 个；实现追加在 `testFullScreenScroll` 之后、`QTEST_MAIN` 前）
- 文档：`CHANGELOG`、`README.md`

### Step 4.1 benchmark 连字场景列

- [ ] 在 `tests/tst_benchmark.cpp` 槽声明区（:30 `void testFullScreenScroll();` 之后）追加：

```cpp
    void testLigatureRefresh_data();
    void testLigatureRefresh();
```

- [ ] 在 `testFullScreenScroll` 实现之后、`QTEST_MAIN` 前追加：

```cpp
void TestBenchmark::testLigatureRefresh_data()
{
    QTest::addColumn<bool>("ligatures");
    // 开关 A/B 对照：连字场景重绘耗时增量（规格 §3.6 门禁：开启增量 <10%）
    QTest::newRow("连字重负载 关") << false;
    QTest::newRow("连字重负载 开") << true;
}

/**
 * @brief 连字场景：运算符密集的代码行负载全量重绘，开关 A/B 对照。
 * @note 沿用本套件惯例不设硬性断言（机器差异大）：两行数字人工对比并记入
 *       CHANGELOG，判定口径为规格 §3.6——开启后重绘耗时增量 <10% 为通过；
 *       关闭行同时是"开关关闭零开销"的回归参照（应与既有全量重绘基线无统计学差异）。
 */
void TestBenchmark::testLigatureRefresh()
{
    QFETCH(bool, ligatures);
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initDisplayEnv(emu, win, display);
    display.setLigaturesEnabled(ligatures);
    QByteArray content;
    for (int i = 0; i < 24; i++)
        content += "if (a[i] >= b && c != d) { x += y->z; } // => ok == done\r\n";
    emu.receiveData(content.constData(), int(content.size()));
    display.updateImage();
    QImage image(display.size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    display.render(&image); // warmup：吃掉 _drawTextTestFlag 一次性度量
    QBENCHMARK {
        display.render(&image);
    }
}
```

- [ ] 运行：`cmake --build build --parallel && ./build/tests/tst_benchmark -platform offscreen`（或直接 `ctest --test-dir build -R benchmark --output-on-failure`；QBENCHMARK 数值在 Release 构建下才有参考意义）
- [ ] 预期：benchmark 可编译可运行；记录"连字重负载 开/关"两行数值，计算增量百分比（写入 Step 4.2 的 CHANGELOG 条目）。若增量 >10%，按规格 §3.6 回头评估按字体+字符串小缓存（本轮默认不做）。

### Step 4.2 文档收尾

- [ ] `CHANGELOG` 顶部（:1 之前）新增条目（`<增量%>` 替换为 Step 4.1 实测数字）：

```
ZzQTermWidget 编程连字渲染（可选开关） / 2026-08-14
=============================================
 * TerminalDisplay 新增 setLigaturesEnabled/ligaturesEnabled（默认关，opt-in；
   内部接口不进公共头）：ASCII 运算符序列（! # $ % & * + - . / : ; < = > ? @
   \ ^ _ { | } ~ 的连续段，长度 ≥ 2）在字体具备恰好等宽连字字形时整段整形
   绘制（Qt 内建 HarfBuzz 自动产出连字字形），否则静默回退现有整段路径。
 * LigatureHelper 判定设施（lib/src/display/ 内部工具）：constexpr 字符掩码
   预扫描 + 可连字子区间探测 + 整形宽度校验（整形总宽 == 格数 × 格宽，
   ±0.5px 容差）；校验不跨帧缓存，字体切换自动失效。
 * 绘制集成在 drawCharacters 默认非 bidi、非 #33 修复的整段路径：普通子区间
   沿用 LTR 覆盖字符调用样式，连字子区间无覆盖字符保证整形上下文纯净；
   三开关两两互斥；开关关闭时逐字节同现状（像素等价门禁覆盖）。
 * 跨度脏区配套：连字序列任一格变脏时脏跨度精确扩展到整个候选段（精确扩展
   而非整行升级，只依赖字符掩码、与字体无关）。
 * benchmark 连字场景列：开启后全量重绘耗时增量 <增量%>%（门禁 <10%）。
 * 已知遗留：拉丁文通用连字（fi/fl）与复杂文字整形不做（YAGNI）；光标处不拆
   连字（光标块画在连字字形之上，与 kitty 一致）；连字生效像素用例在本机/CI
   无连字字体时 QSKIP。
```

- [ ] `README.md` :33（SGR 38/48 冒号条目）之后追加一行：

```
- 支持编程连字渲染（可选开关，默认关）：ASCII 运算符序列在等宽连字字体下整段整形绘制，含整形宽度校验、样式边界不跨片段、跨度脏区配套扩展；`TerminalDisplay::setLigaturesEnabled(true)` 开启。
```

### Step 4.3 全量验证

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build --output-on-failure`
- [ ] 预期：10 套件全绿（新增 tst_ligature 4 用例 + tst_rendering 5 用例，其中 `testLigatureRendering` 本机 QSKIP 属预期）。
- [ ] 提交：`test(benchmark): 新增连字场景重绘开关 A/B 列` 与 `docs: 编程连字收尾（CHANGELOG/README）`（可分两个 commit，或合并为 `feat(display): 编程连字 benchmark 与文档收尾`——按当次改动粒度二选一）

---

## 规格 §2 范围自检（每项 → 任务）

- `setLigaturesEnabled(bool)` / `ligaturesEnabled()`，默认 false → 任务 1 Step 1.3（含单测 `testSwitchApi`）。
- 连字判定设施（子区间探测 + 整形宽度校验）→ 任务 1 Step 1.2（单测 `testCandidateMask`/`testFindCandidateSpans`/`testWidthMatches`）。
- 绘制：校验通过的子区间整段 drawText，不通过回退 → 任务 2 Step 2.2（`drawLigatureSpans`）。
- 跨度脏区配套 → 任务 3 Step 3.2。
- 测试：离屏像素双路径（既有 `testBatchingPixelEquivalence*` 全程门禁）/ 静默回退（`testLigatureSplitPathTaken`）/ 脏区扩展（`testLigatureDirtyRegion`）/ 样式边界（`testLigatureStyleBoundary`）/ 生效（`testLigatureRendering`）/ 互斥（`testLigatureMutualExclusion`）→ 任务 2/3。
- YAGNI 不做项：计划内无任何拉丁连字/HarfBuzz vendored/光标拆连字/bidi+#33 叠加代码。

## 全程红线（自检）

- 不动 `lib/include/qtermwidget.h` 与 `lib/third_party/`；新增/修改注释一律中文 Doxygen。
- 每个任务独立 commit，提交前该任务相关套件全绿；全程 TDD（各任务预期失败点已逐条写明：任务 1 编译红、任务 2 观测钩子红、任务 3 形状断言红）。
- 开关默认关：`_ligaturesEnabled = false` 时 drawCharacters 仅多一次布尔短路、updateImage 仅多一次布尔短路；`testBatchingPixelEquivalence*` 等既有用例全绿即"零行为变化"证据。
- 互斥守卫复核：连字条件 `_ligaturesEnabled && !_fix_quardCRT_issue33` 且结构位于 `_bidiEnabled` 的 else 内——三开关两两不叠加；`testLigatureMutualExclusion` 锁定。
- `drawLigatureSpans` 回退路径复核：`text.size() < 2` / 逐格等宽不满足 / 无候选段 / 宽度校验全灭，四种情况全部返回 false 走原整段调用（逐字节同现状）。
- 套件总数 9 → 10（新增 tst_ligature），AGENTS.md 未写死套件数，无需同步。
