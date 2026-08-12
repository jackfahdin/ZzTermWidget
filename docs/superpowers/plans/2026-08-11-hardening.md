# ZzQTermWidget 第二轮强化实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 字符管线 wchar_t → char32_t 根治代理对问题、OSC 52 开关、缓冲区安全、脆弱 hack 清理、建立 QTest 测试设施。

**架构：** 依据 `docs/superpowers/specs/2026-08-11-hardening-design.md`。解码出口 `toUcs4()` 按完整码点送入解析器；内部表示全链路 char32_t。测试先行（TDD）：任务 1 建测试设施，char32_t 的 emoji 回归测试在改造前写成 RED。

**技术栈：** CMake 4.3.3、Qt 6.11.1（`/home/zz/Qt/6.11.1/gcc_64`，含 Qt6::Test）、C++20。

**通用构建命令（下称"构建"）：**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64
cmake --build build --parallel
```

**通用测试命令（下称"ctest"）：** `ctest --test-dir build --output-on-failure`（测试属性已设 `QT_QPA_PLATFORM=offscreen`）。

**注释约定（强制）：** 新增/修改注释用简体中文 Doxygen 风格；移植遗留的英文注释可保留。`lib/third_party/` 不动。

**注意：** 在 main 分支直接提交（用户已授权）；commit 用约定式提交；不要 push。

**关于规格提交顺序的调整：** 规格把测试列在最后一条 commit；按 TDD 原则测试设施（任务 1）与各项测试先行落地，规格的覆盖范围不变。

---

## 文件结构

- 创建 `tests/CMakeLists.txt`、`tests/tst_charwidth.cpp`、`tests/tst_emulation.cpp`、`tests/tst_history.cpp`、`tests/tst_osc52.cpp`
- 修改：`lib/src/util/Character.h`、`lib/src/util/CharWidth.{h,cpp}`、`lib/src/emulation/{Emulation,Vt102Emulation,Screen,ScreenWindow}.{h,cpp}`（按需）、`lib/src/display/TerminalDisplay.{h,cpp}`、`lib/src/util/TerminalCharacterDecoder.{h,cpp}`、`lib/src/widget/qtermwidget.{cpp}`、`lib/include/qtermwidget.h`、顶层 `CMakeLists.txt`、`.github/workflows/*.yml`、`README.md`、`AGENTS.md`

**改造时一并修复的现存截断 bug**（勘察确认，属 char32_t 范围内的自然配套）：
- `Character.h:69` 构造形参 `quint16` → `char32_t`（BMP 外字符构造即截断）
- `Character.h` `rendition` 成员 `quint8` → `quint16`（`RE_STRIKEOUT (1<<8)` 起已被截断），构造形参同步
- `Emulation.h:523` `dupCache` 为 `QByteArray` 存 wchar_t（隐式窄化）→ `std::vector<char32_t>`
- `TerminalDisplay.cpp:1994` `quint8 currentRendition` → `quint16`

---

## 任务 1：QTest 测试设施 + CharWidth 基线测试

**文件：**
- 修改：`CMakeLists.txt`（选项段、find_package 段、文件尾部）
- 创建：`tests/CMakeLists.txt`、`tests/tst_charwidth.cpp`

- [ ] **步骤 1：顶层 CMakeLists.txt 接入测试**

选项段（`ZZQTERMWIDGET_BUILD_EXAMPLE` 附近）加：

```cmake
option(ZZQTERMWIDGET_BUILD_TESTS "Build the QTest-based test suite" ON)
```

find_package 段（LinguistTools QUIET 行之后）加：

```cmake
if(ZZQTERMWIDGET_BUILD_TESTS)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
    enable_testing()
endif()
```

文件尾部（example 块之后）加：

```cmake
if(ZZQTERMWIDGET_BUILD_TESTS)
    add_subdirectory(tests)
endif()
```

- [ ] **步骤 2：创建 tests/CMakeLists.txt**

```cmake
# 测试目标统一注册：链接静态库与 Qt6::Test，offscreen 平台运行
set(QTERMWIDGET_TESTS
    tst_charwidth
)

foreach(test_name IN LISTS QTERMWIDGET_TESTS)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE
        ZzQTermWidget::qtermwidget
        Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
    # 无显示环境下运行（CI）；QClipboard 在 offscreen 下为进程内实现，可用
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
```

- [ ] **步骤 3：创建 tests/tst_charwidth.cpp（现有行为基线）**

```cpp
#include <QtTest>
#include "CharWidth.h"

/**
 * @brief CharWidth 字符宽度判定的基线回归测试。
 *
 * 覆盖 utf8proc 宽度判定与 fork 的特殊规则（PUA=1、易经符号=2）。
 */
class TestCharWidth : public QObject
{
    Q_OBJECT
private slots:
    void testAscii();
    void testCjk();
    void testPrivateUse();
    void testYiJing();
    void testCombining();
    void testEmoji();
};

void TestCharWidth::testAscii()
{
    QCOMPARE(CharWidth::unicode_width(U'a'), 1);
}

void TestCharWidth::testCjk()
{
    QCOMPARE(CharWidth::unicode_width(U'中'), 2);
}

void TestCharWidth::testPrivateUse()
{
    // PUA（私用区）强制宽度 1（对齐 tmux/glibc 行为）
    QCOMPARE(CharWidth::unicode_width(char32_t(0xE000)), 1);
}

void TestCharWidth::testYiJing()
{
    // 易经六十四卦符号强制宽度 2
    QCOMPARE(CharWidth::unicode_width(char32_t(0x4DC0)), 2);
    QCOMPARE(CharWidth::unicode_width(char32_t(0x4DFF)), 2);
}

void TestCharWidth::testCombining()
{
    // 组合用变音符（U+0301）宽度 0
    QCOMPARE(CharWidth::unicode_width(char32_t(0x0301)), 0);
}

void TestCharWidth::testEmoji()
{
    // BMP 外字符：当前 wchar_t 接口在 Windows 会截断；char32_t 改造后必须稳定为 2
    QCOMPARE(CharWidth::unicode_width(char32_t(0x1F600)), 2);
}

QTEST_APPLESS_MAIN(TestCharWidth)
#include "tst_charwidth.moc"
```

注意：当前 `CharWidth::unicode_width` 签名是 `wchar_t`，Linux 下 wchar_t=32bit 测试可编译通过；任务 2 改签名后测试无需改（char32_t 实参兼容）。

- [ ] **步骤 4：构建 + ctest，预期全绿**

```bash
# 构建（见头部命令）后：
ctest --test-dir build --output-on-failure
```

预期：`1/1 Test #1: tst_charwidth ... Passed`。

- [ ] **步骤 5：Commit**

```bash
git add CMakeLists.txt tests/
git commit -m "test: 引入 QTest 测试设施与 CharWidth 基线用例"
```

---

## 任务 2：char32_t 核心管线（Character/CharWidth/Emulation/Vt102Emulation/Screen）

**文件：**
- 修改：`lib/src/util/Character.h`、`lib/src/util/CharWidth.{h,cpp}`、`lib/src/emulation/Emulation.{h,cpp}`、`lib/src/emulation/Vt102Emulation.{h,cpp}`、`lib/src/emulation/Screen.{h,cpp}`、`lib/src/emulation/ScreenWindow.{h,cpp}`（如有 wchar_t 命中）、`lib/src/util/TerminalCharacterDecoder.{h,cpp}`（本任务只改编译必需处，绘制相关在任务 3）
- 测试：`tests/tst_emulation.cpp`（新建，先 RED）

**说明：** 本任务与任务 3 之间代码处于"display 层未完成适配"状态，可能编译不过——任务 2 的验证以核心仿真层单测为准，全量构建在任务 3 完成后恢复。若实现中发现 display 层耦合导致无法单独编译，把任务 2/3 作为一个提交完成也可以（在报告中说明）。

- [ ] **步骤 1：先写 RED 测试 tests/tst_emulation.cpp**

```cpp
#include <QtTest>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "Screen.h"

/**
 * @brief 终端仿真核心的回归测试（经 receiveData 喂字节流，断言 Screen 状态）。
 */
class TestEmulation : public QObject
{
    Q_OBJECT
private slots:
    void testPlainText();
    void testCsiCursorMove();
    void testWideChar();
    void testEmojiSurrogatePair();
};

/**
 * @brief 取当前屏幕第 0 行文本（Screen::getScreenText，mode 1 拼接）。
 * @note 若 getScreenText 语义与本假设不符，以实现时核实的等效解码路径替换。
 */
static QString firstLineText(Vt102Emulation &emu, int columns)
{
    ScreenWindow *win = emu.createWindow();
    return win->screen()->getScreenText(0, 0, 0, columns - 1, 1);
}

void TestEmulation::testPlainText()
{
    Vt102Emulation emu;
    emu.setImageSize(24, 80);
    emu.receiveData("hello", 5);
    QVERIFY(firstLineText(emu, 80).startsWith(QStringLiteral("hello")));
}

void TestEmulation::testCsiCursorMove()
{
    Vt102Emulation emu;
    emu.setImageSize(24, 80);
    // CSI 5 C：光标右移 5 列后写 X，X 应在第 6 列（索引 5）
    emu.receiveData("\033[5CX", 6);
    const QString line = firstLineText(emu, 80);
    QCOMPARE(line.indexOf(QLatin1Char('X')), 5);
}

void TestEmulation::testWideChar()
{
    Vt102Emulation emu;
    emu.setImageSize(24, 80);
    // "中文" 两宽字符占 4 列，随后 'x' 在索引 4
    emu.receiveData("\xE4\xB8\xAD\xE6\x96\x87x", 7);
    const QString line = firstLineText(emu, 80);
    QCOMPARE(line.indexOf(QLatin1Char('x')), 4);
}

void TestEmulation::testEmojiSurrogatePair()
{
    Vt102Emulation emu;
    emu.setImageSize(24, 80);
    // "a😀b"：😀(U+1F600) UTF-8 = F0 9F 98 80，占 2 列；b 应在索引 3
    // 改造前（代理对拆分）b 会落在错误位置或 emoji 显示为乱码
    emu.receiveData("a\xF0\x9F\x98\x80b", 6);
    const QString line = firstLineText(emu, 80);
    QVERIFY(line.contains(QString::fromUcs4(U'😀')));
    QCOMPARE(line.indexOf(QLatin1Char('b')), 3);
}

QTEST_GUILESS_MAIN(TestEmulation)
#include "tst_emulation.moc"
```

注册：把 `tst_emulation` 加入 `tests/CMakeLists.txt` 的 `QTERMWIDGET_TESTS` 列表。

- [ ] **步骤 2：运行确认 RED**

`ctest --test-dir build -R tst_emulation --output-on-failure`。预期：`testEmojiSurrogatePair` FAIL（代理对被拆分，emoji 不匹配或 b 位置错误）。其余用例应 PASS（记录基线）。

- [ ] **步骤 3：Character.h 改造**

- 构造形参 `quint16 _c = ' '` → `char32_t _c = U' '`；`quint8 _r` → `quint16 _r`
- `wchar_t character;` → `char32_t character;`（注释同步为中文 Doxygen 简注）
- `quint8 rendition;` → `quint16 rendition;`
- `isSpace()` 中 `QChar(character).isSpace()` → `QChar::isSpace(character)`（静态重载接受 char32_t）
- `isLineChar()` 逻辑不变（位运算对 char32_t 有效）

- [ ] **步骤 4：CharWidth 改造**

`CharWidth.h` 签名（保留 QChar 重载）：

```cpp
    int font_width(char32_t ucs);
    int font_width(const QChar & c);
    int string_font_width( const std::u32string & wstr );
    int string_font_width( const QString & str );

    static int unicode_width(char32_t ucs, bool fix_width = true);
    static int unicode_width(const QChar & c, bool fix_width = true);
    static int string_unicode_width(const std::u32string & wstr, bool fix_width = true);
    static int string_unicode_width(const QString & str, bool fix_width = true);
```

`CharWidth.cpp` 同步实现（形参 wchar_t→char32_t、`std::wstring`→`std::u32string`，逻辑不变；utf8proc 接口本就是 int32）。顺手删除 `CharWidth.h:8` 的自包含 `#include "CharWidth.h"`。

- [ ] **步骤 5：Emulation 改造**

- `Emulation.h:279`：`void dupDisplayCharacter(char32_t cc);`
- `Emulation.h:460`：`virtual void receiveChar(char32_t ch);`
- `Emulation.h:523`：`QByteArray dupCache;` → `std::vector<char32_t> dupCache;`（加 `#include <vector>`）
- `Emulation.cpp` `receiveData`（:225-257）解码段替换为：

```cpp
    // 以 UCS-4 码点迭代：代理对在解码出口合成，全平台行为一致
    const QString utf16Text = _toUtf16(QByteArray::fromRawData(text, length));
    const QVector<char32_t> ucs4Text = utf16Text.toUcs4();

    // send characters to terminal emulator
    for (char32_t c : ucs4Text)
        receiveChar(c);
```

（删除旧 XXX 注释块与 toStdWString 两行；zmodem 扫描段不动。）

- `dupDisplayCharacter`（:259-279）适配 char32_t：`cc == L'\n'` → `cc == U'\n'`，`dupCache.append` → `push_back`，`dupCache.at(j)` → `dupCache[j]`，其余逻辑不变
- ExtendedCharTable（`Emulation.cpp:341-449`）：内部已是 `uint*` 存储，无需改类型；确认编译即可

- [ ] **步骤 6：Vt102Emulation 改造**

- `Vt102Emulation.h`：`receiveChar(char32_t cc) override`、`addToCurrentToken(char32_t cc)`、`char32_t tokenBuffer[MAX_TOKEN_LENGTH];`、`applyCharset(char32_t c)`、`processToken(int code, char32_t p, int q)`；删除 `:142` 的 `//FIXME: overflow?`（任务 5 处理溢出本体）
- `Vt102Emulation.cpp`：字面量 `L'...'` → `U'...'`（全局替换语境检查，仅在 char32_t 语境）；`processOSC()` 中 `QString::fromWCharArray(tokenBuffer, ...)` → `QString::fromUcs4(...)`（3 处），并删除 :461 未使用的 `token` 变量
- **charClass 越界审计**：grep `charClass\[` 全部使用点，凡无 `cc < 256` 前置保护的补保护（`cc >= 256` 一律按普通字符 CHR 处理）。这是既有隐患（旧代码 wchar_t 也可能 >255）
- `receiveChar` 入口的 `dupDisplayCharacter(cc)` 调用适配

- [ ] **步骤 7：Screen 改造**

- `Screen.h:356`：`void displayCharacter(char32_t c);`
- `Screen.cpp` 内 wchar_t 命中点逐一适配（displayCharacter 实现、lastDrawnChar 等，以编译器错误清单为准逐个处理）
- `ScreenWindow.{h,cpp}` 同理（如有命中）

- [ ] **步骤 8：编译核心层并跑测试**

反复构建直至 lib 编译通过（TerminalDisplay/decoder 适配若阻塞 lib 编译，本步允许临时改通但完整适配留给任务 3）。`ctest -R "tst_charwidth|tst_emulation"`。预期：全绿（emoji 用例转 GREEN）。

- [ ] **步骤 9：Commit**

```bash
git add -A
git commit -m "refactor: 字符管线核心 wchar_t → char32_t（根治代理对拆分）"
```

---

## 任务 3：char32_t display/decoder 适配

**文件：**
- 修改：`lib/src/display/TerminalDisplay.{h,cpp}`、`lib/src/util/TerminalCharacterDecoder.{h,cpp}`

- [ ] **步骤 1：TerminalDisplay.h 签名**

- `drawTextFragment(..., const std::u32string &text, const Character *style, ...)`（style 顺带改 const，为任务 6 铺垫）
- `drawCharacters(..., const std::u32string &text, ...)`
- 两个 `drawLineCharString` 重载与 `isLineCharString`：`std::wstring`→`std::u32string`、`wchar_t ch`→`char32_t ch`
- `InputMethodData.preeditString` → `std::u32string`

- [ ] **步骤 2：TerminalDisplay.cpp 适配要点**

- `:223` `isLineCharString`：逻辑不变（位运算有效）
- `:1325` 旧绘制路径 `wchar_t *disstrU = new wchar_t[columnsToUpdate]` → `char32_t *disstrU = new char32_t[columnsToUpdate]`；`:1406` `std::wstring unistr` → `std::u32string`
- **`:1988` 与 `:2005` 的 reinterpret_cast hack 替换**：

```cpp
// 原：int charWidth = fm.horizontalAdvance(QString::fromWCharArray((wchar_t *)&c, 1));
int charWidth = fm.horizontalAdvance(QChar::fromUcs4(c));
// 原：(const wchar_t *)(&nxtC) 同理
nxtCharWidth = fm.horizontalAdvance(QChar::fromUcs4(nxtC));
```

- `:1994` `quint8 currentRendition` → `quint16 currentRendition`
- drawContents 的 `std::wstring unistr` → `std::u32string unistr`（聚合逻辑不变，char32_t push 兼容 uint）
- drawCharacters 的 issue33 hack 段：`QList<uint16_t>` 三张表 → `QList<char32_t>`；`QString::fromWCharArray(&line_char, 1)` → `QChar::fromUcs4(line_char)`；`QString::fromStdWString(text)` → `QString::fromStdU32String(text)`
- 输入法 preedit 赋值点（`event->preeditString().toStdWString()`）→ `.toStdU32String()`；`:1770` preedit 绘制调用适配
- `:3118-3135` charClass 的 extended char 组装：`std::wstring str` → `std::u32string`，`QString::fromStdWString` → `fromStdU32String`

- [ ] **步骤 3：TerminalCharacterDecoder 适配**

- `PlainTextDecoder::decodeLine`：`std::wstring plainText` → `std::u32string`；`QString::fromStdWString(plainText)` → `fromStdU32String`
- `HTMLDecoder`：`openSpan/closeSpan` 的 `std::wstring&` → `std::u32string&`（.h 声明同步）；`L"&lt;"` 等字面量 → `U"&lt;"`；`.toStdWString()` → `.toStdU32String()`；`fromStdWString` → `fromStdU32String`
- `wchar_t ch(characters[i].character)` → `char32_t ch(...)`

- [ ] **步骤 4：全量构建 + ctest**

预期 0 error 0 warning，tst_charwidth/tst_emulation 全绿。offscreen 冒烟：`QT_QPA_PLATFORM=offscreen timeout 3 ./build/qtermwidget_example || true`（进程能起即可）。

- [ ] **步骤 5：Commit**

```bash
git add -A
git commit -m "refactor: display/decoder 层 char32_t 适配，清除 reinterpret_cast hack"
```

---

## 任务 4：OSC 52 剪贴板访问开关

**文件：**
- 修改：`lib/src/emulation/Emulation.{h,cpp}`、`lib/src/emulation/Vt102Emulation.cpp`、`lib/include/qtermwidget.h`、`lib/src/widget/qtermwidget.cpp`
- 测试：`tests/tst_osc52.cpp`（新建，先 RED）

- [ ] **步骤 1：先写 RED 测试 tests/tst_osc52.cpp**

```cpp
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include "Vt102Emulation.h"

/**
 * @brief OSC 52 剪贴板访问开关的行为测试。
 */
class TestOsc52 : public QObject
{
    Q_OBJECT
private slots:
    void testDisabledBySwitch();
    void testEnabledByDefault();
};

void TestOsc52::testDisabledBySwitch()
{
    Vt102Emulation emu;
    emu.setImageSize(24, 80);
    emu.setOsc52Enabled(false);
    QApplication::clipboard()->clear();
    // OSC 52 ; c ; aGVsbG8= (base64 "hello") BEL
    emu.receiveData("\033]52;c;aGVsbG8=\007", 15);
    QVERIFY(QApplication::clipboard()->text().isEmpty());
}

void TestOsc52::testEnabledByDefault()
{
    Vt102Emulation emu;
    emu.setImageSize(24, 80);
    QApplication::clipboard()->clear();
    emu.receiveData("\033]52;c;aGVsbG8=\007", 15);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello"));
}

QTEST_MAIN(TestOsc52)
#include "tst_osc52.moc"
```

注册到 `tests/CMakeLists.txt` 的 `QTERMWIDGET_TESTS`。运行确认 `testDisabledBySwitch` RED（`setOsc52Enabled` 不存在，编译失败即 RED；先注释该行验证行为失败亦可，以编译失败为准即可）。

- [ ] **步骤 2：Emulation 增加开关**

`Emulation.h` public 段加（附中文 Doxygen 注释说明安全语义）：

```cpp
    /**
     * @brief 设置是否允许 OSC 52 序列访问本地剪贴板。
     * @param enabled true 允许（默认）；false 时 OSC 52 被吞掉不执行。
     * @note 允许时远程程序可读写本地剪贴板，有安全风险，上层应用可按需关闭。
     */
    void setOsc52Enabled(bool enabled);
    bool osc52Enabled() const;
```

private 段加成员 `bool _osc52Enabled = true;`。`Emulation.cpp` 实现两个访问器（各一行）。

- [ ] **步骤 3：Vt102Emulation::processOSC 加门控**

`Vt102Emulation.cpp` 的 `case 52:` 块开头加：

```cpp
        // 开关关闭时吞掉整个 OSC 52（见 Emulation::setOsc52Enabled 的安全说明）
        if (!_osc52Enabled)
            break;
```

- [ ] **步骤 4：QTermWidget 公共 API**

`lib/include/qtermwidget.h` public 段加（中文 Doxygen）：

```cpp
    /**
     * @brief 设置是否允许 OSC 52 序列访问本地剪贴板（默认允许）。
     * @see Emulation::setOsc52Enabled
     */
    void setOsc52Enabled(bool enabled);
    bool osc52Enabled() const;
```

`lib/src/widget/qtermwidget.cpp` 实现转发：

```cpp
void QTermWidget::setOsc52Enabled(bool enabled) {
    m_emulation->setOsc52Enabled(enabled);
}

bool QTermWidget::osc52Enabled() const {
    return m_emulation->osc52Enabled();
}
```

- [ ] **步骤 5：构建 + ctest，确认转 GREEN；Commit**

```bash
git add -A
git commit -m "feat: OSC 52 剪贴板访问开关（默认允许，保持兼容）"
```

---

## 任务 5：缓冲区安全（copyLineToStream + tokenBuffer）

**文件：**
- 修改：`lib/src/emulation/Screen.cpp`（copyLineToStream，:1211-1288）、`lib/src/emulation/Vt102Emulation.cpp`（addToCurrentToken，:173-176）
- 测试：`tests/tst_emulation.cpp` 追加 tokenBuffer 溢出用例

- [ ] **步骤 1：先写 RED/基线测试（tests/tst_emulation.cpp 追加）**

```cpp
void TestEmulation::testOversizedToken()
{
    Vt102Emulation emu;
    emu.setImageSize(24, 80);
    // 超长 OSC 标题（超过 MAX_TOKEN_LENGTH=100000）：解析器应丢弃该序列并恢复，不崩溃
    QByteArray payload = "\033]0;";
    payload.append(QByteArray(100005, 'A'));
    payload.append('\007');
    QTest::ignoreMessage(QtWarningMsg, "Vt102Emulation: token exceeds MAX_TOKEN_LENGTH, sequence discarded");
    emu.receiveData(payload.constData(), payload.size());
    // 恢复验证：后续正常文本仍可正确显示
    emu.receiveData("OK", 2);
    QVERIFY(firstLineText(emu, 80).startsWith(QStringLiteral("OK")));
}
```

（在 private slots 声明 `void testOversizedToken();`。）运行：改造前 token 被静默截断且标题内容可能部分生效——测试的恢复断言可能即通过也可能失败，以观察为准记录基线；核心价值是防回归。

- [ ] **步骤 2：addToCurrentToken 溢出处理**

```cpp
void Vt102Emulation::addToCurrentToken(char32_t cc) {
    if (tokenBufferPos >= MAX_TOKEN_LENGTH - 1) {
        // token 超长（如超长窗口标题）：丢弃整个序列并复位解析器，避免静默截断产生错误语义
        qWarning("Vt102Emulation: token exceeds MAX_TOKEN_LENGTH, sequence discarded");
        resetTokenizer();
        return;
    }
    tokenBuffer[tokenBufferPos++] = cc;
}
```

- [ ] **步骤 3：copyLineToStream 静态缓冲消除**

`Screen.cpp:1211-1288` 中：

- 删除 `static const int MAX_CHARS = 1024;` 与 `static Character characterBuffer[MAX_CHARS];` 及 `Q_ASSERT(count < MAX_CHARS);`
- 函数开头改为：

```cpp
    // 行字符缓冲：栈上小容量、超出自动堆分配（替代原静态 1024 上限，消除线程安全与越界隐患）
    const int worstCase = qMax(columns, line < history->getLines() ? history->getLineLen(line) : 0) + 1;
    QVarLengthArray<Character> characterBuffer(worstCase);
```

- 使用点适配：`history->getCells(line, start, count, characterBuffer.data())`；`characterBuffer[i - start]` 不变；换行追加的边界 `count + 1 < MAX_CHARS` → `count + 1 < worstCase`；结尾 `decoder->decodeLine(characterBuffer.data(), count, currentLineProperties)`（去掉 C 强转）
- 文件头确认 `#include <QVarLengthArray>`

- [ ] **步骤 4：补 HistoryScrollFile 回归测试（规格第 5 节要求）**

创建 `tests/tst_history.cpp`：

```cpp
#include <QtTest>
#include "History.h"
#include "Character.h"

/**
 * @brief HistoryScrollFile（文件回滚缓冲）的基本行为回归测试。
 */
class TestHistory : public QObject
{
    Q_OBJECT
private slots:
    void testAddAndGet();
    void testWrappedFlag();
};

void TestHistory::testAddAndGet()
{
    HistoryScrollFile file(QString());
    QVector<Character> line = { Character(U'h'), Character(U'i') };
    file.addCellsVector(line);
    file.addLine(false);
    QCOMPARE(file.getLines(), 1);
    QVector<Character> out(2);
    file.getCells(0, 0, 2, out.data());
    QCOMPARE(out[0].character, char32_t(U'h'));
    QCOMPARE(out[1].character, char32_t(U'i'));
}

void TestHistory::testWrappedFlag()
{
    HistoryScrollFile file(QString());
    QVector<Character> line = { Character(U'x') };
    file.addCellsVector(line);
    file.addLine(true);
    QVERIFY(file.isWrappedLine(0));
}

QTEST_APPLESS_MAIN(TestHistory)
#include "tst_history.moc"
```

（`HistoryScrollFile` 的方法名以 `History.h` 实际接口为准微调；`addCellsVector`/`addLine`/`getCells` 为 Konsole 系标准名。）注册到 `QTERMWIDGET_TESTS`。

- [ ] **步骤 5：构建 + ctest；Commit**

```bash
git add -A
git commit -m "fix: copyLineToStream 静态缓冲与 tokenBuffer 溢出安全处理"
```

---

## 任务 6：脆弱 hack 清理

**文件：**
- 修改：`lib/src/display/TerminalDisplay.{h,cpp}`（drawTextFragment、repaintDisplay、issue33 注释）

- [ ] **步骤 1：选区透明度去 in-place 交换**

`TerminalDisplay.cpp` `drawTextFragment`（:1051-1106）改为局部副本方案（任务 3 已把 style 形参改为 const）：

```cpp
void TerminalDisplay::drawTextFragment(QPainter &painter, const QRect &rect,
                                       const std::u32string &text,
                                       const Character *style,
                                       bool tooWide,
                                       bool isSelection) {
    painter.save();

    // 选区透明度 < 1 时以交换前/背景色的局部副本绘制文字，不修改屏幕图像
    Character localStyle;
    if (_selectedTextOpacity < 1.0 && isSelection) {
        localStyle = *style;
        std::swap(localStyle.foregroundColor, localStyle.backgroundColor);
        style = &localStyle;
    }

    // setup painter（以下原逻辑不变，style 一律只读）
    ...
```

尾部（:1091-1105）只保留半透明填充，**删除交换回去的代码**：

```cpp
    if (_selectedTextOpacity < 1.0 && isSelection) {
        painter.save();
        painter.setOpacity(_selectedTextOpacity);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(rect, CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR)
                                                   .color(_colorTable));
        painter.restore();
    }
```

（若任务 3 未改 const，本步把 .h/.cpp 的 `Character *style` 形参改为 `const Character *style`；调用点传非 const 指针可隐式转换。）

- [ ] **步骤 2：repaintDisplay 降级**

`TerminalDisplay.h:572-579` 的内联实现替换为：

```cpp
    /**
     * @brief 请求重绘整个终端显示区。
     * @note 历史实现用 hide()+show() 强制重绘（Linux 下的粗暴 hack），
     *       现已降级为标准的 update() 调度重绘；如特定场景仍有残留绘制问题再评估。
     */
    void repaintDisplay() {
        update();
    }
```

公共 API 保留（`QTermWidget::repaintDisplay` 转发不动）。

- [ ] **步骤 3：issue33 hack 注释整理**

`TerminalDisplay.cpp` drawCharacters 中 `_fix_quardCRT_issue33` 段（:958-1048）：把说明性注释整理为中文 Doxygen 风格段落，写清：修复什么问题（东亚引号等"字体宽度≠Unicode 宽度"字符的对齐）、来源（quardCRT issue #33）、边界（仅 `_fix_quardCRT_issue33` 开启且命中三张字符表时逐字绘制）。**逻辑一行不改**。

- [ ] **步骤 4：构建 + ctest + Commit**

```bash
git add -A
git commit -m "refactor: 清理选区透明度 in-place 交换与 repaintDisplay hack"
```

---

## 任务 7：CI 接入 + 文档收尾

**文件：**
- 修改：`.github/workflows/{linux,macos,windows}.yml`、`AGENTS.md`、`README.md`

- [ ] **步骤 1：CI 加 ctest 步骤**

三个 workflow 的 Build 步骤之后各加：

```yaml
      - name: Test
        run: ctest --test-dir build --output-on-failure
```

（windows.yml 若分 mingw/msvc 两 job 则各加一次；ctest 跨平台可用，测试属性已内置 `QT_QPA_PLATFORM=offscreen`。）

- [ ] **步骤 2：AGENTS.md 补测试约定**

在"构建"节后追加：

```markdown
## 测试

- 测试位于 `tests/`，使用 Qt 官方 QTest 框架（`Qt6::Test`），无第三方依赖。
- 选项 `-DZZQTERMWIDGET_BUILD_TESTS=ON`（默认开）；运行：`ctest --test-dir build --output-on-failure`。
- 新增核心逻辑（解析器、屏幕缓冲、宽度判定等）必须附带回归测试。
```

- [ ] **步骤 3：README 补充**

"一些注意"前补一段：测试设施说明（同 AGENTS.md 简述）；"主要修改"列表追加：char32_t 字符管线（修复 BMP 外字符代理对拆分）、OSC 52 剪贴板开关、缓冲区安全修复。

- [ ] **步骤 4：全量验证 + Commit**

```bash
rm -rf build
# 构建（头部命令）+ ctest 全绿后：
git add -A
git commit -m "test: CI 三平台接入 ctest，文档补充测试约定"
```
