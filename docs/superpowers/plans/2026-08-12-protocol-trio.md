# 协议三件套实现计划：OSC 8 超链接 / 同步输出 CSI ? 2026 / Kitty 键盘协议

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 依据已批准规格实现三项协议协商特性——OSC 8 显式超链接（可点击、可复制）、同步输出 CSI ? 2026（TUI 批量重绘防闪屏）、Kitty 键盘协议级别 1+2（消歧义 + 事件类型上报）。

**架构：** 依据 `docs/superpowers/specs/2026-08-12-protocol-trio-design.md`。改动落在三个现有层：`lib/src/emulation/`（Vt102Emulation 解析、Screen 链接存储）、`lib/src/util/Filter.{h,cpp}`（Osc8Filter 热点）、`lib/src/display/TerminalDisplay.{h,cpp}`（攒帧、按键编码、热点接入）。测试先行（TDD）：每个任务先写失败测试再实现。

**技术栈：** CMake 4.3.3、Qt 6.11.1（`/home/zz/Qt/6.11.1/gcc_64`，含 Qt6::Test）、C++20。

**通用构建命令（下称"构建"）：**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64
cmake --build build --parallel
```

**通用测试命令（下称"ctest"）：** `ctest --test-dir build --output-on-failure`（测试属性已设 `QT_QPA_PLATFORM=offscreen`）。

**注释约定（强制）：** 新增/修改注释用简体中文 Doxygen 风格；移植遗留的英文注释可保留。`lib/third_party/` 不动。

**注意：** 在 main 分支直接提交（用户已授权）；commit 用约定式提交（中文描述）；不要 push。

**勘察结论（实现依据，均已逐行核实）：**

- `Vt102Emulation::processOSC()`（`lib/src/emulation/Vt102Emulation.cpp:481`）命令解析支持 1-2 位数字，OSC 8 落入 `case 2:` 分支（`command = 8`）；内容提取模板：`QString::fromUcs4(tokenBuffer + 4, tokenBufferPos - 5)`（BEL 与 ST 两种终止符均已验证该长度正确）。超长 OSC 已由既有 `tokenDiscard` 机制吞吃（`receiveChar` :267）。
- `receiveChar` 已有 `elt()`/`eeq()`（CSI `<`/`=`）吞吃分支并填充 `argv`，对应 token `TY_CSI_PL`/`TY_CSI_PQ`；`epp()`（`?`）/`egt()`（`>`）走末尾 for 循环逐参数分发——kitty 的 `CSI ? u` 与 `CSI > flags u` 必须在 for 循环前整体拦截，避免重复应答/重复压栈。
- DECRQM 应答骨架已存在（`reportDecMode`，:1700；私有模式 `case TY_CSI_PR('p', Pd)` 系列，:1538），2026 直接补一行。
- `Screen` 行存储为 `ImageLine* screenLines`（`QVector<Character>` 数组，lines+1 行）+ 平行 `lineProperties`；链接段表按同模式加平行数组 `_linkLines`。scrollback 交接点：`addHistLine()`（:1314，`newHistLines == oldHistLines` 表示历史满丢弃最旧行或 None 无滚动）、`moveImage()`（:954，双向拷贝循环，目标行被覆盖）、`clearImage()`（:915，逐行清理）、`resizeImage()`（:285）、`setScroll()`（:1368，`copyPreviousScroll=false` 即清历史）。
- `Screen::displayCharacter` 正常写入路径在 `notcombine:` 标签（:703）之后：折行处理（:704-711）→ 写入（:727-732）→ `cuX = newCursorX`（:752）。链接段挂钩点在 :752 之后，写入起点在折行处理后捕获。
- Filter 管线（`lib/src/util/Filter.{h,cpp}`）：`Filter::HotSpot` 有 `clickAction()/clickActionToolTip()/hasClickAction()/actions()` 虚接口；`FilterChain::hotSpotAt`（:97）**按注册顺序返回首个命中过滤器的热点**——OSC 8 优先 = Osc8Filter 先于 UrlFilter 注册。`TerminalDisplay` 构造（:345）创建 `_filterChain`，`QTermWidget` 构造（:147）才 addFilter UrlFilter，故在 TerminalDisplay 构造体内注册 Osc8Filter 天然优先。
- TerminalDisplay 重绘管线：`ScreenWindow::outputChanged` → `updateImage()`（:1296，结尾 `update(dirtyRegion)`）；Ctrl+点击（:2326-2333）与右键动作（`filterActions` :2352）均走 `_filterChain->hotSpotAt` → Osc8HotSpot 接入后零改动复用；悬停下划线由 `paintFilters`（:1789）对 Link 类型热点统一绘制。无 `keyReleaseEvent`，`keyPressEvent`（:3305）仅转发 `keyPressedSignal` → `Emulation::sendKeyEvent`。
- 按键编码核心在 `Vt102Emulation::sendKeyEvent`（:1807），kitty 分支加在函数开头；`_keyTranslator` 为 null 时走错误路径，测试须先 `emu.setKeyBindings(QString())`（落到 defaultTranslator，与 `QTermWidget` :119 同路径）。
- Emulation 有缓冲刷新：receiveData → `bufferedUpdate()`（10/40ms 定时器）→ `outputChanged`。显示层测试需 `QSignalSpy::wait()`。
- 测试设施：`tests/tst_emulation.cpp` 用 `QTEST_GUILESS_MAIN`（无剪贴板），`tst_osc52.cpp` 用 `QTEST_MAIN`（QApplication，剪贴板可用）。lib 的 include 目录（`lib/CMakeLists.txt:103-109`）PUBLIC 导出 emulation/display/util，测试可直接包含 `TerminalDisplay.h`、`Filter.h`。

**对规格的两处设计调整（勘察驱动）：**

1. 规格说链接"链接结束或清屏时回收"——链接结束时其已写字符仍在屏上，立即回收会让现存单元格悬空。改为**段引用计数**：段随行清除/滚出/丢弃而销毁，计数归零才回收 URI 与 id 映射。
2. kitty 规范要求主/备屏各维护独立 flags 栈；规格未提。本轮实现单栈（neovim 等实际应用 push/pop 配对使用，单栈行为一致），在代码注释中说明。

---

## 文件结构

- 修改：`lib/src/emulation/Screen.{h,cpp}`、`lib/src/emulation/Vt102Emulation.{h,cpp}`、`lib/src/emulation/Emulation.h`、`lib/src/util/Filter.{h,cpp}`、`lib/src/display/TerminalDisplay.{h,cpp}`、`lib/src/widget/qtermwidget.cpp`、`tests/tst_emulation.cpp`、`tests/CMakeLists.txt`、`CHANGELOG`、`README.md`
- 创建：`tests/tst_protocols.cpp`

---

## 任务 1：OSC 8 解析 + Screen 链接存储

**文件：**
- 修改：`lib/src/emulation/Screen.{h,cpp}`、`lib/src/emulation/Vt102Emulation.cpp`
- 测试：`tests/tst_emulation.cpp`（追加，先 RED）

- [ ] **步骤 1：先写 RED 测试（tests/tst_emulation.cpp 追加）**

在 `private slots:` 追加声明：

```cpp
    void testOsc8BasicLink();
    void testOsc8IdMergesSegments();
    void testOsc8ScrollbackKeepsLink();
    void testOsc8ClearLineDropsSegments();
```

文件头部追加 `#include <cstring>`，类外追加实现：

```cpp
/**
 * @brief OSC 8 基本解析：链接文本落入段表，空 URI 结束链接后文本不再属于链接。
 */
void TestEmulation::testOsc8BasicLink()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const char *seq = "\033]8;;https://example.com\007link\033]8;;\007 tail";
    emu.receiveData(seq, int(std::strlen(seq)));
    Screen *scr = emu.createWindow()->screen();
    QCOMPARE(scr->hyperlinkAt(0, 0), QStringLiteral("https://example.com"));
    QCOMPARE(scr->hyperlinkAt(0, 3), QStringLiteral("https://example.com"));
    QVERIFY(scr->hyperlinkAt(0, 4).isEmpty()); // 空 URI 已结束链接
    QVERIFY(scr->hyperlinkAt(0, 6).isEmpty());
}

/**
 * @brief OSC 8 id 参数：相同 id 的多次开启视为同一链接（复用同一 linkId）。
 */
void TestEmulation::testOsc8IdMergesSegments()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const char *seq = "\033]8;id=x;https://a.com\007ab\033]8;;\007 \033]8;id=x;https://a.com\007cd\033]8;;\007";
    emu.receiveData(seq, int(std::strlen(seq)));
    Screen *scr = emu.createWindow()->screen();
    const auto segs = scr->linkSegments(0);
    QCOMPARE(segs.size(), 2);
    QCOMPARE(segs[0].linkId, segs[1].linkId); // 同 id 合并为同一链接
    QCOMPARE(segs[0].startCol, 0);
    QCOMPARE(segs[0].endCol, 1);
    QCOMPARE(segs[1].startCol, 3);
    QCOMPARE(segs[1].endCol, 4);
}

/**
 * @brief 链接行进 scrollback 后段表随行走：绝对行坐标下仍可查到 URI。
 */
void TestEmulation::testOsc8ScrollbackKeepsLink()
{
    Vt102Emulation emu;
    initEmu(emu, 3, 80);
    emu.setHistory(HistoryTypeBuffer(100));
    const char *seq = "\033]8;;https://example.com\007lnk\033]8;;\007\r\n1\r\n2\r\n3\r\n4";
    emu.receiveData(seq, int(std::strlen(seq)));
    Screen *scr = emu.createWindow()->screen();
    QVERIFY(scr->getHistLines() >= 1); // 链接行已滚入历史
    QCOMPARE(scr->hyperlinkAt(0, 0), QStringLiteral("https://example.com")); // 绝对行 0 = 最早历史行
}

/**
 * @brief 清行（CSI 2 K）清除该行链接段表，链接 URI 引用计数归零回收。
 */
void TestEmulation::testOsc8ClearLineDropsSegments()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const char *seq = "\033]8;;https://example.com\007lnk\033]8;;\007";
    emu.receiveData(seq, int(std::strlen(seq)));
    Screen *scr = emu.createWindow()->screen();
    QCOMPARE(scr->linkSegments(0).size(), 1);
    emu.receiveData("\033[2K", 4); // 光标仍在第 0 行，清整行
    QVERIFY(scr->linkSegments(0).isEmpty());
    QVERIFY(scr->hyperlinkAt(0, 0).isEmpty());
}
```

- [ ] **步骤 2：运行确认 RED**

`ctest --test-dir build -R tst_emulation --output-on-failure`。预期：编译失败（`hyperlinkAt`/`linkSegments` 不存在）即 RED。

- [ ] **步骤 3：Screen.h 增加超链接存储与查询接口**

头部 includes 追加 `#include <deque>`、`#include <QHash>`。`class Screen` 前追加：

```cpp
/**
 * @brief OSC 8 超链接在一行内覆盖的列区间段。
 */
struct HyperlinkSegment {
    int startCol;   ///< 起始列（含）
    int endCol;     ///< 结束列（含）
    quint32 linkId; ///< 链接标识，经 Screen::hyperlinkUri() 换取 URI
};
```

`Screen` public 段（`getScreenText` 声明附近）追加：

```cpp
    /**
     * @brief 设置/结束当前 OSC 8 超链接上下文。
     * @param uri 链接目标；空串表示结束当前链接（后续文本不再属于链接）。
     * @param osc8Id OSC 8 params 中的 id 参数（可为空）；相同 id 且 URI 未变的分段复用同一 linkId。
     */
    void setCurrentHyperlink(const QString &uri, const QString &osc8Id);

    /**
     * @brief 返回绝对行 @p absoluteLine（历史行 + 屏幕行统一编号）上的超链接段表。
     * @return 段表副本；该行无链接或行号越界时为空。
     */
    QVector<HyperlinkSegment> linkSegments(int absoluteLine) const;

    /**
     * @brief 返回 @p linkId 对应的 URI；无效 id 返回空串。
     */
    QString hyperlinkUri(quint32 linkId) const;

    /**
     * @brief 返回绝对行 @p absoluteLine、列 @p column 处的超链接 URI；无链接返回空串。
     */
    QString hyperlinkAt(int absoluteLine, int column) const;
```

private 段（`static Character defaultChar;` 前）追加：

```cpp
    // OSC 8 超链接 ----------------
    // 行级稀疏段表：无链接的行为空 QVector（零额外堆分配）；链接 URI 用段引用计数管理，
    // 段随行清除/滚出/丢弃而销毁，计数归零时回收 URI 与 id 映射
    typedef QVector<HyperlinkSegment> HyperlinkLine;
    HyperlinkLine *_linkLines;               // [lines + 1]，与 screenLines 平行
    std::deque<HyperlinkLine> _historyLinks; // 与 history 行一一对应
    QHash<quint32, QString> _hyperlinkUris;  // linkId → URI
    QHash<quint32, int> _hyperlinkRefs;      // linkId → 段引用计数
    QHash<QString, quint32> _hyperlinkIds;   // OSC 8 id 参数 → linkId
    quint32 _currentHyperlinkId = 0;         // 0 = 无活动链接
    quint32 _nextHyperlinkId = 1;

    void addHyperlinkSegment(int y, int startCol, int endCol);
    void releaseHyperlinkLine(HyperlinkLine &row);
    void clearAllHyperlinks();
```

- [ ] **步骤 4：Screen.cpp 实现存储与生命周期挂钩**

构造初始化列表 `screenLines(new ImageLine[lines + 1])` 后追加 `_linkLines(new HyperlinkLine[lines + 1]),`；析构 `delete[] screenLines;` 后追加 `delete[] _linkLines;`。

新增方法实现（放 `getHistLines()` 附近）：

```cpp
void Screen::setCurrentHyperlink(const QString &uri, const QString &osc8Id) {
    if (uri.isEmpty()) {
        _currentHyperlinkId = 0; // 空 URI：结束当前链接
        return;
    }
    if (!osc8Id.isEmpty()) {
        // 相同 id 且 URI 未变：复用 linkId（分段属于同一链接）
        auto it = _hyperlinkIds.find(osc8Id);
        if (it != _hyperlinkIds.end() && _hyperlinkUris.value(it.value()) == uri) {
            _currentHyperlinkId = it.value();
            return;
        }
    }
    const quint32 id = _nextHyperlinkId++;
    _hyperlinkUris.insert(id, uri);
    _hyperlinkRefs.insert(id, 0);
    if (!osc8Id.isEmpty())
        _hyperlinkIds.insert(osc8Id, id);
    _currentHyperlinkId = id;
}

QVector<HyperlinkSegment> Screen::linkSegments(int absoluteLine) const {
    const int histLines = history->getLines();
    if (absoluteLine < 0 || absoluteLine >= histLines + lines)
        return {};
    if (absoluteLine < histLines) {
        if (absoluteLine < static_cast<int>(_historyLinks.size()))
            return _historyLinks[absoluteLine];
        return {};
    }
    return _linkLines[absoluteLine - histLines];
}

QString Screen::hyperlinkUri(quint32 linkId) const {
    return _hyperlinkUris.value(linkId);
}

QString Screen::hyperlinkAt(int absoluteLine, int column) const {
    const auto segments = linkSegments(absoluteLine);
    for (const HyperlinkSegment &seg : segments) {
        if (column >= seg.startCol && column <= seg.endCol)
            return _hyperlinkUris.value(seg.linkId);
    }
    return {};
}

void Screen::addHyperlinkSegment(int y, int startCol, int endCol) {
    HyperlinkLine &row = _linkLines[y];
    // 与行尾相邻的同 id 段合并，避免逐字符产生碎段
    if (!row.isEmpty() && row.last().linkId == _currentHyperlinkId
            && row.last().endCol >= startCol - 1) {
        row.last().endCol = qMax(row.last().endCol, endCol);
        return;
    }
    row.append({startCol, endCol, _currentHyperlinkId});
    _hyperlinkRefs[_currentHyperlinkId]++;
}

void Screen::releaseHyperlinkLine(HyperlinkLine &row) {
    for (const HyperlinkSegment &seg : row) {
        auto it = _hyperlinkRefs.find(seg.linkId);
        if (it != _hyperlinkRefs.end() && --it.value() == 0) {
            _hyperlinkRefs.erase(it);
            _hyperlinkUris.remove(seg.linkId);
            // id 参数映射若仍指向被回收的 linkId，一并移除（映射表很小，线性扫可接受）
            for (auto keyIt = _hyperlinkIds.begin(); keyIt != _hyperlinkIds.end();) {
                if (keyIt.value() == seg.linkId)
                    keyIt = _hyperlinkIds.erase(keyIt);
                else
                    ++keyIt;
            }
        }
    }
    row.clear();
}

void Screen::clearAllHyperlinks() {
    // 整体丢弃（reset 路径），无需逐个维护引用计数
    for (int i = 0; i < lines + 1; i++)
        _linkLines[i].clear();
    _historyLinks.clear();
    _hyperlinkUris.clear();
    _hyperlinkRefs.clear();
    _hyperlinkIds.clear();
    _currentHyperlinkId = 0;
}
```

生命周期挂钩（5 处）：

1. `displayCharacter`（:703 `notcombine:` 路径）：在 `lastPos = loc(cuX, cuY);`（:722）前插入 `const int writeStartX = cuX;`（此时 cuX/cuY 已是折行后的最终写入位置）；在尾部 `cuX = newCursorX;`（:752）后追加：

```cpp
    // OSC 8：活动链接期间写入的字符计入当前行的链接段表
    if (_currentHyperlinkId != 0)
        addHyperlinkSegment(cuY, writeStartX, newCursorX - 1);
```

（已知简化：MODE_Insert 插入模式下既有段表不随字符右移，段与单元格可能错位；该组合场景罕见，注释说明即可。）

2. `clearImage`（:933 循环内 `lineProperties[y] = 0;` 旁）追加：

```cpp
        releaseHyperlinkLine(_linkLines[y]); // 清行连带清除链接段表
```

3. `moveImage` 两个方向的拷贝循环：在 `screenLines[...] = screenLines[...];` 赋值前各插入：

```cpp
            releaseHyperlinkLine(_linkLines[(dest / columns) + i]); // 目标行被覆盖，旧段表回收
```
并在 `lineProperties[...] = lineProperties[...];` 后各插入：

```cpp
            _linkLines[(dest / columns) + i] =
                    std::move(_linkLines[(sourceBegin / columns) + i]); // 段表随行走（move 防止引用计数双降）
```

4. `addHistLine`（:1318 `if (hasScroll()) {` 块内，`history->addLine(...)` 与 `_droppedLines` 处理之间）插入：

```cpp
        // OSC 8：链接段随行进入 scrollback；历史满丢弃最旧行时同步丢弃其段表
        if (newHistLines > oldHistLines) {
            _historyLinks.push_back(std::move(_linkLines[0]));
        } else if (oldHistLines > 0) {
            releaseHyperlinkLine(_historyLinks.front());
            _historyLinks.pop_front();
            _historyLinks.push_back(std::move(_linkLines[0]));
        } else {
            releaseHyperlinkLine(_linkLines[0]); // 防御：无滚动存储时直接丢弃
        }
        _linkLines[0].clear();
```

5. `resizeImage`（:299 `newScreenLines` 分配块后）追加：

```cpp
    HyperlinkLine *newLinkLines = new HyperlinkLine[new_lines + 1];
    for (int i = 0; i < qMin(lines, new_lines + 1); i++)
        newLinkLines[i] = std::move(_linkLines[i]);
    for (int i = qMin(lines, new_lines + 1); i < lines + 1; i++)
        releaseHyperlinkLine(_linkLines[i]); // 收缩时被裁行的段表回收
    delete[] _linkLines;
    _linkLines = newLinkLines;
```
（加在 `delete[] screenLines; screenLines = newScreenLines;` 附近，注意 `_linkLines` 的 delete/move 顺序：`std::move` 到 newLinkLines 之后才 `delete[] _linkLines`。）

6. `reset(bool clearScreen)`（:500）末尾（`if (clearScreen) clear();` 前）追加 `clearAllHyperlinks();`；`setScroll`（:1368）`else` 分支 `delete oldScroll;` 后追加：

```cpp
        // 历史整体废弃（clearHistory）：同步丢弃历史链接段表
        for (HyperlinkLine &row : _historyLinks)
            releaseHyperlinkLine(row);
        _historyLinks.clear();
```

- [ ] **步骤 5：Vt102Emulation::processOSC 增加 case 8**

`case 52:` 块之后、`default:` 之前插入：

```cpp
    //  Ps = 8 → Hyperlink（OSC 8）：ESC ] 8 ; params ; URI ST。params 为 ':' 分隔的
    //  键值对，仅识别 id=<value>（相同 id 的分段视为同一链接），未知键忽略；
    //  空 URI 表示当前链接结束。非法格式安全忽略，不产生热点。
    case 8: {
        const QString content = QString::fromUcs4(tokenBuffer + 4, tokenBufferPos - 5);
        const int sep = content.indexOf(QLatin1Char(';'));
        if (sep < 0)
            break; // 缺 URI 段：非法序列，忽略
        const QString params = content.left(sep);
        const QString uri = content.mid(sep + 1);
        QString id;
        if (!params.isEmpty()) {
            const auto pairs = params.split(QLatin1Char(':'), Qt::SkipEmptyParts);
            for (const QString &kv : pairs) {
                if (kv.startsWith(QLatin1String("id=")))
                    id = kv.mid(3);
            }
        }
        _currentScreen->setCurrentHyperlink(uri, id);
        break;
    }
```

- [ ] **步骤 6：构建 + ctest，确认转 GREEN；Commit**

```bash
# 构建（见头部命令）后：
ctest --test-dir build --output-on-failure
git add -A
git commit -m "feat: OSC 8 超链接解析与 Screen 行级段表存储"
```

---

## 任务 2：OSC 8 HotSpot 交互（Osc8Filter + TerminalDisplay 接入）

**文件：**
- 修改：`lib/src/util/Filter.{h,cpp}`、`lib/src/display/TerminalDisplay.{h,cpp}`
- 测试：`tests/tst_protocols.cpp`（新建，QTEST_MAIN）、`tests/CMakeLists.txt`（注册）

**说明：** Ctrl+点击（`TerminalDisplay.cpp:2326-2333`）与右键动作（`filterActions` :2352）已走 `_filterChain->hotSpotAt` 通用路径，Osc8Filter 接入后无需改动鼠标处理；悬停下划线由 `paintFilters` 对 Link 类型热点统一绘制。OSC 8 优先 = Osc8Filter 在 TerminalDisplay 构造体内注册（早于 QTermWidget 注册 UrlFilter）。纯 GUI 点击行为手动验证。

- [ ] **步骤 1：先写 RED 测试 tests/tst_protocols.cpp**

```cpp
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "Screen.h"
#include "Filter.h"
#include "TerminalDisplay.h"

/**
 * @brief 协议三件套的显示层/交互层测试（OSC 8 热点、同步输出攒帧、kitty 释放事件路由）。
 * @note 需要 QApplication（剪贴板与 QWidget），故独立于 tst_emulation.cpp。
 */
class TestProtocols : public QObject
{
    Q_OBJECT
private slots:
    void testOsc8HotSpotHit();
    void testOsc8CopyLinkAction();
    void testFilterChainOrderPriority();
};

/**
 * @brief 测试用过滤器：开放受保护的 addHotSpot 以便手工构造热点。
 */
class TestSpotFilter : public Filter
{
public:
    using Filter::addHotSpot;
    void process() override {}
};

/**
 * @brief 构造带 OSC 8 链接 "ab" 的仿真环境（链接位于第 0 行 0-1 列）。
 */
static void initOsc8Emu(Vt102Emulation &emu, ScreenWindow *&win)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    win = emu.createWindow();
    win->setWindowLines(24);
    const char *seq = "\033]8;;https://example.com\007ab\033]8;;\007";
    emu.receiveData(seq, int(strlen(seq)));
}

void TestProtocols::testOsc8HotSpotHit()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    initOsc8Emu(emu, win);

    Osc8Filter filter;
    filter.setScreenWindow(win);
    filter.process();

    Filter::HotSpot *spot = filter.hotSpotAt(0, 1);
    QVERIFY(spot != nullptr);
    QCOMPARE(spot->type(), Filter::HotSpot::Link);
    auto *osc8Spot = dynamic_cast<Osc8Filter::HotSpot *>(spot);
    QVERIFY(osc8Spot != nullptr);
    QCOMPARE(osc8Spot->uri(), QStringLiteral("https://example.com"));
    QVERIFY(filter.hotSpotAt(0, 10) == nullptr); // 链接区域外无热点
}

void TestProtocols::testOsc8CopyLinkAction()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    initOsc8Emu(emu, win);

    Osc8Filter filter;
    filter.setScreenWindow(win);
    filter.process();

    Filter::HotSpot *spot = filter.hotSpotAt(0, 0);
    QVERIFY(spot != nullptr);
    const QList<QAction *> acts = spot->actions();
    QVERIFY(!acts.isEmpty());
    QApplication::clipboard()->clear();
    acts.first()->trigger(); // "复制链接地址"
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("https://example.com"));
}

void TestProtocols::testFilterChainOrderPriority()
{
    // OSC 8 优先机制：FilterChain::hotSpotAt 按注册顺序返回首个命中，
    // Osc8Filter 在 TerminalDisplay 构造体内先于 UrlFilter 注册
    FilterChain chain;
    TestSpotFilter first, second;
    first.addHotSpot(new Osc8Filter::HotSpot(0, 0, 0, 5, QStringLiteral("https://a/")));
    second.addHotSpot(new Osc8Filter::HotSpot(0, 0, 0, 5, QStringLiteral("https://b/")));
    chain.addFilter(&first);
    chain.addFilter(&second);
    auto *spot = dynamic_cast<Osc8Filter::HotSpot *>(chain.hotSpotAt(0, 2));
    QVERIFY(spot != nullptr);
    QCOMPARE(spot->uri(), QStringLiteral("https://a/")); // 先注册者优先
}

QTEST_MAIN(TestProtocols)
#include "tst_protocols.moc"
```

`tests/CMakeLists.txt` 的 `QTERMWIDGET_TESTS` 列表追加 `tst_protocols`。

- [ ] **步骤 2：运行确认 RED**

编译失败（`Osc8Filter` 不存在）即 RED。

- [ ] **步骤 3：Filter.h 声明 Osc8Filter**

头部追加 `class ScreenWindow;` 前置声明。文件尾部（`TerminalImageFilterChain` 类之后、`#endif` 之前）追加：

```cpp
/**
 * @brief OSC 8 显式超链接过滤器。
 *
 * 与正则扫描的 UrlFilter 不同，本过滤器不做文本匹配：process() 直接读取
 * ScreenWindow 可见各行在 Screen 中登记的 OSC 8 链接段表，逐段创建热点。
 * 在 FilterChain 中须先于 UrlFilter 注册，使命中同一单元格时 OSC 8 链接优先。
 */
class Osc8Filter : public Filter
{
    Q_OBJECT
public:
    /**
     * @brief OSC 8 超链接热点：Ctrl+点击经 QDesktopServices::openUrl 打开，
     *        右键动作提供"复制链接地址"。
     */
    class HotSpot : public Filter::HotSpot {
    public:
        HotSpot(int startLine, int startColumn, int endLine, int endColumn,
                const QString &uri, QObject *actionParent = nullptr);
        void clickAction() override;
        QString clickActionToolTip() override;
        bool hasClickAction() override;
        QList<QAction *> actions() override;
        /** @brief 返回热点对应的链接 URI。 */
        QString uri() const { return _uri; }
    private:
        QString _uri;
        QObject *_actionParent; ///< 右键动作的对象树父节点（不拥有语义外的生命周期责任）
    };

    Osc8Filter();
    /** @brief 绑定数据源窗口；process() 时从该窗口的 Screen 读取链接段表。 */
    void setScreenWindow(ScreenWindow *window);
    void process() override;
private:
    ScreenWindow *_screenWindow = nullptr;
};
```

- [ ] **步骤 4：Filter.cpp 实现 Osc8Filter**

头部追加 `#include "ScreenWindow.h"`、`#include "Screen.h"`。文件尾部追加：

```cpp
Osc8Filter::Osc8Filter() : Filter() {
}

void Osc8Filter::setScreenWindow(ScreenWindow *window) { _screenWindow = window; }

void Osc8Filter::process() {
    if (!_screenWindow)
        return;
    Screen *screen = _screenWindow->screen();
    if (!screen)
        return;
    // 热点行号以窗口可见区为坐标系：窗口第 i 行 ↔ 绝对行 currentLine()+i
    const int topLine = _screenWindow->currentLine();
    const int windowLines = _screenWindow->windowLines();
    for (int i = 0; i < windowLines; i++) {
        const auto segments = screen->linkSegments(topLine + i);
        for (const HyperlinkSegment &seg : segments) {
            const QString uri = screen->hyperlinkUri(seg.linkId);
            if (uri.isEmpty())
                continue;
            addHotSpot(new HotSpot(i, seg.startCol, i, seg.endCol, uri, this));
        }
    }
}

Osc8Filter::HotSpot::HotSpot(int startLine, int startColumn, int endLine,
                             int endColumn, const QString &uri, QObject *actionParent)
    : Filter::HotSpot(startLine, startColumn, endLine, endColumn),
      _uri(uri), _actionParent(actionParent) {
    setType(Link);
}

bool Osc8Filter::HotSpot::hasClickAction() { return true; }

QString Osc8Filter::HotSpot::clickActionToolTip() {
    return tr("Follow link (ctrl + click)");
}

void Osc8Filter::HotSpot::clickAction() {
    // openUrl 失败（无 handler 等）静默返回 false，不崩溃
    QDesktopServices::openUrl(QUrl(_uri));
}

QList<QAction *> Osc8Filter::HotSpot::actions() {
    // URI 按值捕获：热点可能随过滤器 reset 被删除，动作不得悬空引用
    QAction *copyLinkAction = new QAction(QObject::tr("Copy Link Address"), _actionParent);
    const QString uri = _uri;
    QObject::connect(copyLinkAction, &QAction::triggered, copyLinkAction, [uri]() {
        QApplication::clipboard()->setText(uri);
    });
    return {copyLinkAction};
}
```

- [ ] **步骤 5：TerminalDisplay 接入 Osc8Filter**

`TerminalDisplay.h`：`_filterChain` 成员声明（:943）后追加：

```cpp
    // OSC 8 显式超链接过滤器：构造时先于 UrlFilter 注册进 _filterChain，
    // 命中同一单元格时 OSC 8 链接优先于正则匹配链接
    Osc8Filter *_osc8Filter;
```
（Filter.h 已被 TerminalDisplay.h 包含——确认头部已有 `#include "Filter.h"`，无则补。）

`TerminalDisplay.cpp` 构造初始化列表 `_filterChain(new TerminalImageFilterChain()),`（:345）后追加 `_osc8Filter(new Osc8Filter()),`；构造函数体内（`_scrollBar` 设置之前即可）追加：

```cpp
    // OSC 8 显式超链接热点：先于链中其他过滤器注册，保证命中优先级
    _filterChain->addFilter(_osc8Filter);
```

`setScreenWindow`（:141）`_screenWindow = window;` 之后追加：

```cpp
    _osc8Filter->setScreenWindow(window);
```

析构（:454 `delete _filterChain;` 前）追加：

```cpp
    _filterChain->removeFilter(_osc8Filter);
    delete _osc8Filter;
```

- [ ] **步骤 6：构建 + ctest，确认转 GREEN；Commit**

```bash
ctest --test-dir build --output-on-failure
git add -A
git commit -m "feat: OSC 8 热点交互（Osc8Filter，Ctrl+点击打开/右键复制，优先于 urlFilter）"
```

- [ ] **步骤 7：手动验证（报告中记录结果）**

example 程序内执行 `printf '\e]8;;https://www.qt.io\aQt 官网\e]8;;\a\n'`：悬停出下划线与 tooltip、Ctrl+点击打开浏览器、右键菜单含"Copy Link Address"且复制正确；与正则 urlFilter 同时命中时 OSC 8 生效。

---

## 任务 3：同步输出 CSI ? 2026

**文件：**
- 修改：`lib/src/emulation/Vt102Emulation.{h,cpp}`、`lib/src/emulation/Emulation.h`、`lib/src/display/TerminalDisplay.{h,cpp}`、`lib/src/widget/qtermwidget.cpp`
- 测试：`tests/tst_emulation.cpp`（信号 + DECRQM）、`tests/tst_protocols.cpp`（显示层攒帧/超时/输入 flush）

- [ ] **步骤 1：先写 RED 测试（tests/tst_emulation.cpp 追加）**

slots 声明追加 `void testSyncOutputModeSignalAndDecrqm();`，实现：

```cpp
/**
 * @brief CSI ? 2026 同步输出：set/reset 信号、嵌套 set 幂等、DECRQM 如实应答。
 */
void TestEmulation::testSyncOutputModeSignalAndDecrqm()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    QSignalSpy spy(&emu, &Emulation::synchronizedOutputModeChanged);
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    emu.receiveData("\033[?2026h", 8); // BSU
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    emu.receiveData("\033[?2026h", 8); // 嵌套 set 幂等：不再发信号
    QCOMPARE(spy.count(), 0);

    emu.receiveData("\033[?2026$p", 9); // DECRQM：置位应答 1（set）
    QCOMPARE(sent, QByteArray("\033[?2026;1$y"));
    sent.clear();

    emu.receiveData("\033[?2026l", 8); // ESU
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);

    emu.receiveData("\033[?2026$p", 9); // DECRQM：复位应答 2（reset）
    QCOMPARE(sent, QByteArray("\033[?2026;2$y"));
}
```

- [ ] **步骤 2：先写 RED 测试（tests/tst_protocols.cpp 追加）**

slots 声明追加：

```cpp
    void testSyncOutputDefersRepaint();
    void testSyncOutputTimeoutFlush();
    void testSyncOutputKeypressFlush();
```

实现：

```cpp
/**
 * @brief 构造同步输出测试环境：仿真 + 显示组件 + 模式信号接线。
 */
static void initSyncEnv(Vt102Emulation &emu, ScreenWindow *&win, TerminalDisplay &display)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    win = emu.createWindow();
    display.setScreenWindow(win);
    display.resize(800, 600);
    QObject::connect(&emu, &Emulation::synchronizedOutputModeChanged,
                     &display, &TerminalDisplay::setSynchronizedOutputMode);
}

void TestProtocols::testSyncOutputDefersRepaint()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initSyncEnv(emu, win, display);

    emu.receiveData("\033[?2026h", 8);
    QVERIFY(display.synchronizedOutputActive());

    emu.receiveData("x", 1);
    QSignalSpy spy(win, &ScreenWindow::outputChanged);
    QVERIFY(spy.wait(500)); // 输入照常解析，outputChanged 仍触发
    QVERIFY(display.synchronizedOutputPending()); // 但重绘被攒帧

    emu.receiveData("\033[?2026l", 8);
    QVERIFY(!display.synchronizedOutputActive());
    QVERIFY(!display.synchronizedOutputPending()); // 复位时一次性补刷
}

void TestProtocols::testSyncOutputTimeoutFlush()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initSyncEnv(emu, win, display);

    emu.receiveData("\033[?2026h", 8);
    emu.receiveData("x", 1);
    QSignalSpy spy(win, &ScreenWindow::outputChanged);
    QVERIFY(spy.wait(500));
    QVERIFY(display.synchronizedOutputPending());

    QTest::qWait(1200); // 超过 1000ms 兜底阈值
    QVERIFY(!display.synchronizedOutputPending()); // 已强制 flush
    QVERIFY(display.synchronizedOutputActive()); // 模式仍生效，继续等 ESU

    emu.receiveData("\033[?2026l", 8); // 清理
}

void TestProtocols::testSyncOutputKeypressFlush()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initSyncEnv(emu, win, display);
    emu.setKeyBindings(QString()); // 避免 sendKeyEvent 走无键位表错误路径干扰

    emu.receiveData("\033[?2026h", 8);
    emu.receiveData("x", 1);
    QSignalSpy spy(win, &ScreenWindow::outputChanged);
    QVERIFY(spy.wait(500));
    QVERIFY(display.synchronizedOutputPending());

    QTest::keyClick(&display, Qt::Key_A); // 键盘输入立即触发 flush
    QVERIFY(!display.synchronizedOutputPending());
    QVERIFY(display.synchronizedOutputActive());

    emu.receiveData("\033[?2026l", 8); // 清理
}
```

- [ ] **步骤 3：运行确认 RED**

编译失败（信号/槽/ getter 不存在）即 RED。

- [ ] **步骤 4：Vt102Emulation 模式位 + 信号 + DECRQM**

`Vt102Emulation.h`（:48-49）改为：

```cpp
#define MODE_BracketedPaste  (MODES_SCREEN+13)  // Xterm-style bracketed paste mode
#define MODE_SynchronizedOutput (MODES_SCREEN+14) // 同步输出（BSU/ESU，CSI ? 2026）
#define MODE_total           (MODES_SCREEN+15)
```

`Vt102Emulation.cpp` `processToken` 在 2004 组（:1512-1523）之后插入：

```cpp
    case TY_CSI_PR('h', 2026):
        if (!getMode(MODE_SynchronizedOutput))
            setMode(MODE_SynchronizedOutput); // 幂等：嵌套 set 不重复发信号
        break; // BSU：开始批量更新
    case TY_CSI_PR('l', 2026):
        if (getMode(MODE_SynchronizedOutput))
            resetMode(MODE_SynchronizedOutput);
        break; // ESU：结束批量更新
```

DECRQM 段（:1559 `case TY_CSI_PR('p', 2004)` 之后）追加：

```cpp
    case TY_CSI_PR('p', 2026) : reportDecMode(2026, getMode(MODE_SynchronizedOutput) ? 1 : 2); break; // Synchronized output
```

`setMode` 的 switch（:2089）追加：

```cpp
    case MODE_SynchronizedOutput:
        emit synchronizedOutputModeChanged(true);
        break;
```

`resetMode` 的 switch（:2120）追加：

```cpp
    case MODE_SynchronizedOutput:
        emit synchronizedOutputModeChanged(false);
        break;
```

`resetModes()`（:2054，`resetMode(MODE_BracketedPaste); saveMode(MODE_BracketedPaste);` 后）追加：

```cpp
    resetMode(MODE_SynchronizedOutput);
    saveMode(MODE_SynchronizedOutput);
```

`Emulation.h` signals 段（`programBracketedPasteModeChanged` :360 之后）追加：

```cpp
    /**
     * @brief 终端程序通过 CSI ? 2026 h/l 切换同步输出（批量重绘）模式时发出。
     * @param enabled true = 开始攒帧（BSU）；false = 结束并一次性重绘（ESU）。
     */
    void synchronizedOutputModeChanged(bool enabled);
```

- [ ] **步骤 5：TerminalDisplay 攒帧与兜底**

`TerminalDisplay.h` public slots 段（`setBracketedPasteMode` :534 之后）追加：

```cpp
    /**
     * @brief 设置同步输出模式（CSI ? 2026）。
     * @param enabled true 时挂起屏幕重绘仅攒帧；false 时立即补刷一帧。
     * @note 安全兜底：模式持续超过 1000ms 或收到键盘输入时强制 flush，防应用崩溃锁黑屏。
     */
    void setSynchronizedOutputMode(bool enabled);
    /** @brief 查询同步输出模式是否生效（主要供测试与调试）。 */
    bool synchronizedOutputActive() const { return _syncOutputActive; }
    /** @brief 查询是否存在被攒帧推迟的重绘（主要供测试与调试）。 */
    bool synchronizedOutputPending() const { return _syncUpdatePending; }
```

private 段追加声明：

```cpp
    /** @brief 同步输出攒帧期间存在被推迟的重绘时，立即补刷一帧。 */
    void flushSynchronizedOutput();
```

private 成员（`_filterChain` 附近）追加：

```cpp
    // 同步输出（CSI ? 2026）攒帧状态
    bool _syncOutputActive = false;
    bool _syncUpdatePending = false;
    QTimer *_syncOutputTimer = nullptr;
```

`TerminalDisplay.cpp` 构造函数体（`_blinkCursorTimer` 连接之后）追加：

```cpp
    // 同步输出兜底定时器：模式持续过久（如应用崩溃未发 ESU）时强制 flush
    _syncOutputTimer = new QTimer(this);
    _syncOutputTimer->setSingleShot(true);
    _syncOutputTimer->setInterval(1000);
    connect(_syncOutputTimer, &QTimer::timeout, this, [this]() {
        flushSynchronizedOutput();
        if (_syncOutputActive)
            _syncOutputTimer->start(); // 模式仍未复位：继续下一轮兜底
    });
```

`updateImage()`（:1296）`if (!_screenWindow) return;` 之后插入：

```cpp
    // 同步输出模式：输入照常解析进 Screen，此处仅攒帧不刷；复位/超时/键盘输入时一次性补刷
    if (_syncOutputActive) {
        _syncUpdatePending = true;
        return;
    }
```

新增实现（放 `setBracketedPasteMode` 实现附近）：

```cpp
void TerminalDisplay::setSynchronizedOutputMode(bool enabled) {
    if (_syncOutputActive == enabled)
        return; // 嵌套 set 幂等
    _syncOutputActive = enabled;
    if (enabled) {
        _syncOutputTimer->start();
    } else {
        _syncOutputTimer->stop();
        flushSynchronizedOutput();
    }
}

void TerminalDisplay::flushSynchronizedOutput() {
    if (!_syncUpdatePending)
        return;
    _syncUpdatePending = false;
    updateImage();
}
```

`keyPressEvent`（:3305）`_actSel = 0;` 之后插入：

```cpp
    // 同步输出兜底：用户键盘输入说明交互在进行，立即补刷避免画面停滞
    if (_syncOutputActive)
        flushSynchronizedOutput();
```

- [ ] **步骤 6：QTermWidget 接线**

`lib/src/widget/qtermwidget.cpp`（:105 bracketedPaste 连接之后）追加：

```cpp
    connect(m_emulation, &Emulation::synchronizedOutputModeChanged, m_terminalDisplay, &TerminalDisplay::setSynchronizedOutputMode);
```

- [ ] **步骤 7：构建 + ctest，确认转 GREEN；Commit**

```bash
ctest --test-dir build --output-on-failure
git add -A
git commit -m "feat: 同步输出 CSI ? 2026（显示层攒帧 + 1000ms/键盘输入兜底 flush + DECRQM 应答）"
```

---

## 任务 4：kitty 键盘协商序列（CSI >/=/?/< u）

**文件：**
- 修改：`lib/src/emulation/Vt102Emulation.{h,cpp}`
- 测试：`tests/tst_emulation.cpp`（追加，先 RED）

**说明：** kitty 规范要求主/备屏独立 flags 栈；本轮按规格实现单栈（实际应用 push/pop 配对使用，行为一致），代码注释说明。flags 仅实现 1+2，高位在入口处掩掉。栈满拒绝（规格决策；kitty 原文是逐出最旧，此处从严）。

- [ ] **步骤 1：先写 RED 测试（tests/tst_emulation.cpp 追加）**

slots 声明追加：

```cpp
    void testKittyPushQueryPop();
    void testKittySetModes();
    void testKittyStackLimit();
    void testKittyInvalidSequenceIgnored();
```

实现：

```cpp
/**
 * @brief kitty 协商：push/query/pop 与 flags 高位掩码（仅支持级别 1+2）。
 */
void TestEmulation::testKittyPushQueryPop()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    emu.receiveData("\033[?u", 4); // 查询：默认全关
    QCOMPARE(sent, QByteArray("\033[?0u"));
    sent.clear();

    emu.receiveData("\033[>1u", 5); // push 级别 1
    emu.receiveData("\033[?u", 4);
    QCOMPARE(sent, QByteArray("\033[?1u"));
    sent.clear();

    emu.receiveData("\033[>31u", 6); // 高位掩掉，仅保留 1+2
    emu.receiveData("\033[?u", 4);
    QCOMPARE(sent, QByteArray("\033[?3u"));
    sent.clear();

    emu.receiveData("\033[<u", 4); // pop → 回到 1
    emu.receiveData("\033[?u", 4);
    QCOMPARE(sent, QByteArray("\033[?1u"));
    sent.clear();

    emu.receiveData("\033[<5u", 5); // 弹空：flags 复位 0
    emu.receiveData("\033[?u", 4);
    QCOMPARE(sent, QByteArray("\033[?0u"));
}

/**
 * @brief kitty 协商：CSI = flags ; mode u 三种应用方式。
 */
void TestEmulation::testKittySetModes()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    emu.receiveData("\033[=1;1u", 7); // mode 1 整体设置 → 1
    emu.receiveData("\033[?u", 4);
    QCOMPARE(sent, QByteArray("\033[?1u"));
    sent.clear();

    emu.receiveData("\033[=2;2u", 7); // mode 2 置位 → 3
    emu.receiveData("\033[?u", 4);
    QCOMPARE(sent, QByteArray("\033[?3u"));
    sent.clear();

    emu.receiveData("\033[=1;3u", 7); // mode 3 复位指定位 → 2
    emu.receiveData("\033[?u", 4);
    QCOMPARE(sent, QByteArray("\033[?2u"));
    sent.clear();

    emu.receiveData("\033[=3u", 5); // mode 省略默认 1 → 3
    emu.receiveData("\033[?u", 4);
    QCOMPARE(sent, QByteArray("\033[?3u"));
}

/**
 * @brief kitty flags 栈深度上限 64：第 65 次压栈被拒绝，不影响已有栈内容。
 */
void TestEmulation::testKittyStackLimit()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    QTest::ignoreMessage(QtWarningMsg,
                         "Vt102Emulation: kitty keyboard flags stack full, push rejected");
    for (int i = 0; i < 65; i++)
        emu.receiveData("\033[>1u", 5); // 第 65 次压栈被拒

    emu.receiveData("\033[<64u", 6); // 弹出全部 64 层
    emu.receiveData("\033[?u", 4);
    // 若第 65 次未被拒绝，此处会残留 flags=1
    QCOMPARE(sent, QByteArray("\033[?0u"));
}

/**
 * @brief kitty 非法参数序列：按未知 CSI 忽略，不影响后续解析。
 */
void TestEmulation::testKittyInvalidSequenceIgnored()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    emu.receiveData("\033[=1;1u", 7);
    emu.receiveData("\033[=2;99u", 8); // 非法 mode：忽略
    emu.receiveData("\033[?u", 4);
    QCOMPARE(sent, QByteArray("\033[?1u")); // flags 未被破坏

    emu.receiveData("ok", 2); // 解析器恢复正常
    QVERIFY(firstLineText(emu, 80).startsWith(QStringLiteral("ok")));
}
```

- [ ] **步骤 2：运行确认 RED**

编译失败即 RED。

- [ ] **步骤 3：Vt102Emulation.h 声明协商接口**

private 段（`processOSC` 声明附近）追加：

```cpp
  // kitty 键盘协议（级别 1+2）协商状态
  // 注：kitty 规范要求主/备屏独立 flags 栈；本轮按规格实现单栈，
  //     实际应用（neovim 等）push/pop 配对使用，行为一致
  static constexpr quint32 KITTY_FLAGS_SUPPORTED = 0b11; // 仅实现消歧义（1）与事件类型（2）
  static constexpr int KITTY_FLAGS_STACK_MAX = 64; // flags 栈深度上限，防恶意输入撑爆内存
  quint32 _kittyFlags = 0;               // 当前生效 flags（默认全关，纯应用协商）
  QVector<quint32> _kittyFlagsStack;     // CSI > u 压入的历史 flags

  void kittyFlagsPush(quint32 flags);
  void kittyFlagsPop(int count);
  void kittyFlagsSet(quint32 flags, int mode);
  void reportKittyKeyboardFlags();
  bool encodeKittyKeyEvent(QKeyEvent *event, QByteArray &out);
```

- [ ] **步骤 4：Vt102Emulation.cpp 实现协商**

`reset()`（:54）`tokenDiscard = false;` 后追加：

```cpp
    _kittyFlags = 0;
    _kittyFlagsStack.clear();
```

`receiveChar` 的 ANSI 分支中，`if (p >= 4 && cc >= 0x3C && cc <= 0x3F) { return; }`（:431-433）之后、`for (int i = 0; i <= argc; i++)` 循环之前插入：

```cpp
        // kitty 键盘协议：CSI ? u（查询）与 CSI > flags u（压栈）在通用参数分发前整体拦截，
        // 避免下方 for 循环按参数逐个触发导致重复应答/重复压栈
        if (epp() && cc == U'u') {
            reportKittyKeyboardFlags();
            resetTokenizer();
            return;
        }
        if (egt() && cc == U'u') {
            kittyFlagsPush(argv[0]);
            resetTokenizer();
            return;
        }
```

`processToken` 的 `default:` 之前插入：

```cpp
    // kitty 键盘协议：CSI < [count] u（弹栈）/ CSI = flags ; mode u（设置）
    case TY_CSI_PL('u'):
        kittyFlagsPop(qMax(1, argv[0]));
        break;
    case TY_CSI_PQ('u'):
        kittyFlagsSet(argv[0], argc >= 1 ? argv[1] : 1);
        break;
```

文件尾部（`reportDecodingError` 前）追加实现：

```cpp
void Vt102Emulation::kittyFlagsPush(quint32 flags) {
    if (_kittyFlagsStack.size() >= KITTY_FLAGS_STACK_MAX) {
        // 栈满拒绝（防 DoS）：当前 flags 与栈内容均不变
        qWarning("Vt102Emulation: kitty keyboard flags stack full, push rejected");
        return;
    }
    _kittyFlagsStack.append(_kittyFlags);
    _kittyFlags = flags & KITTY_FLAGS_SUPPORTED; // 未实现的 4/8/16 位直接掩掉
}

void Vt102Emulation::kittyFlagsPop(int count) {
    for (int i = 0; i < count; i++) {
        if (_kittyFlagsStack.isEmpty()) {
            _kittyFlags = 0; // 弹空：所有 flags 复位
            break;
        }
        _kittyFlags = _kittyFlagsStack.takeLast();
    }
}

void Vt102Emulation::kittyFlagsSet(quint32 flags, int mode) {
    flags &= KITTY_FLAGS_SUPPORTED;
    switch (mode) {
    case 1: _kittyFlags = flags; break;   // 整体设置（默认）
    case 2: _kittyFlags |= flags; break;  // 置位指定位
    case 3: _kittyFlags &= ~flags; break; // 复位指定位
    default: break;                       // 非法 mode：忽略
    }
}

void Vt102Emulation::reportKittyKeyboardFlags() {
    // 应答 CSI ? flags u：flags 如实上报（仅含已实现的级别 1+2）
    char tmp[16];
    const int r = snprintf(tmp, sizeof(tmp), "\033[?%uu", _kittyFlags);
    if (r <= 0 || r >= static_cast<int>(sizeof(tmp)))
        return;
    sendString(tmp);
}
```

- [ ] **步骤 5：构建 + ctest，确认转 GREEN；Commit**

```bash
ctest --test-dir build --output-on-failure
git add -A
git commit -m "feat: kitty 键盘协议协商序列（push/pop/set/query，flags 栈上限 64）"
```

---

## 任务 5：kitty 按键编码（级别 1 消歧义 + 级别 2 事件类型）

**文件：**
- 修改：`lib/src/emulation/Vt102Emulation.cpp`、`lib/src/display/TerminalDisplay.{h,cpp}`、`lib/src/widget/qtermwidget.cpp`
- 测试：`tests/tst_emulation.cpp`（编码）、`tests/tst_protocols.cpp`（keyReleaseEvent 路由）

**编码规则（kitty 规范子集，与规格范围一致）：**
- 修饰键参数 = 位和 + 1（shift=1 alt=2 ctrl=4 super=8）。
- 级别 1：Esc（任意修饰）、Enter/Tab/Backspace（带任意修饰）、可打印键带 ctrl/alt/super → `CSI codepoint[;modifiers]u`；裸 Enter/Tab/Backspace 维持传统字节（kitty 规范例外，保证崩溃后 shell 可用）；普通文本键维持现有编码。
- 级别 2：CSI u 形式附加 `:1`（按下）/`:2`（重复）/`:3`（释放）；文本键与裸 Enter/Tab/Backspace 的释放不上报（kitty 规范：需级别 8，本轮不实现）。
- 码点取未 shift 形态（字母一律小写）。
- kitty flags 生效时优先于 applicationCursorKeys 等传统模式（分支在传统查表之前）。

- [ ] **步骤 1：先写 RED 测试（tests/tst_emulation.cpp 追加）**

slots 声明追加：

```cpp
    void testKittyDisambiguateEncoding();
    void testKittyEventTypes();
    void testKittyReleaseIgnoredWhenDisabled();
```

类外追加辅助函数与实现：

```cpp
/**
 * @brief 构造按键事件（kitty 编码测试用）。
 */
static QKeyEvent makeKeyEvent(QEvent::Type type, int key, Qt::KeyboardModifiers mods,
                              const QString &text = QString(), bool autorepeat = false)
{
    return QKeyEvent(type, key, mods, text, autorepeat);
}

/**
 * @brief kitty 级别 1 消歧义：Ctrl+I 与 Tab 分离、Esc/带修饰键 CSI u 化、裸键维持传统编码。
 */
void TestEmulation::testKittyDisambiguateEncoding()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setKeyBindings(QString()); // 默认键位表（与 QTermWidget 一致的路径）
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    // 未协商：裸 Tab 传统字节 0x09
    QKeyEvent tabPress = makeKeyEvent(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
    emu.sendKeyEvent(&tabPress, false);
    QCOMPARE(sent, QByteArray("\x09"));
    sent.clear();

    emu.receiveData("\033[>1u", 5); // push 级别 1（消歧义）

    QKeyEvent ctrlI = makeKeyEvent(QEvent::KeyPress, Qt::Key_I, Qt::ControlModifier);
    emu.sendKeyEvent(&ctrlI, false);
    QCOMPARE(sent, QByteArray("\033[105;5u")); // Ctrl+I：i=105，ctrl → 4+1=5
    sent.clear();

    emu.sendKeyEvent(&tabPress, false);
    QCOMPARE(sent, QByteArray("\x09")); // 裸 Tab 维持传统字节（kitty 规范例外）
    sent.clear();

    QKeyEvent shiftTab = makeKeyEvent(QEvent::KeyPress, Qt::Key_Tab, Qt::ShiftModifier);
    emu.sendKeyEvent(&shiftTab, false);
    QCOMPARE(sent, QByteArray("\033[9;2u")); // Shift+Tab：shift → 1+1=2
    sent.clear();

    QKeyEvent esc = makeKeyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, QStringLiteral("\x1b"));
    emu.sendKeyEvent(&esc, false);
    QCOMPARE(sent, QByteArray("\033[27u")); // Esc 消歧
    sent.clear();

    QKeyEvent plainA = makeKeyEvent(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    emu.sendKeyEvent(&plainA, false);
    QCOMPARE(sent, QByteArray("a")); // 普通文本键维持现有编码
}

/**
 * @brief kitty 级别 2 事件类型：按下 :1 / 重复 :2 / 释放 :3；文本键释放不上报。
 */
void TestEmulation::testKittyEventTypes()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setKeyBindings(QString());
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    emu.receiveData("\033[>3u", 5); // push 级别 1+2

    QKeyEvent ctrlShiftI = makeKeyEvent(QEvent::KeyPress, Qt::Key_I,
                                        Qt::ControlModifier | Qt::ShiftModifier);
    emu.sendKeyEvent(&ctrlShiftI, false);
    QCOMPARE(sent, QByteArray("\033[105;6:1u")); // ctrl+shift → 1+5=6，按下 :1
    sent.clear();

    QKeyEvent ctrlIRelease = makeKeyEvent(QEvent::KeyRelease, Qt::Key_I, Qt::ControlModifier);
    emu.sendKeyEvent(&ctrlIRelease, false);
    QCOMPARE(sent, QByteArray("\033[105;5:3u")); // 释放 :3
    sent.clear();

    QKeyEvent ctrlIRepeat = makeKeyEvent(QEvent::KeyPress, Qt::Key_I, Qt::ControlModifier,
                                         QString(), true);
    emu.sendKeyEvent(&ctrlIRepeat, false);
    QCOMPARE(sent, QByteArray("\033[105;5:2u")); // 自动重复 :2
    sent.clear();

    QKeyEvent aRelease = makeKeyEvent(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
    emu.sendKeyEvent(&aRelease, false);
    QCOMPARE(sent, QByteArray()); // 文本键释放不上报（kitty 规范：需级别 8）
    sent.clear();

    QKeyEvent enterRelease = makeKeyEvent(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
    emu.sendKeyEvent(&enterRelease, false);
    QCOMPARE(sent, QByteArray()); // 裸 Enter 释放不上报（同上）
}

/**
 * @brief 未协商 kitty 时：释放事件不发送任何字节（回归保护）。
 */
void TestEmulation::testKittyReleaseIgnoredWhenDisabled()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setKeyBindings(QString());
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    QKeyEvent aRelease = makeKeyEvent(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    emu.sendKeyEvent(&aRelease, false);
    QCOMPARE(sent, QByteArray());
}
```

- [ ] **步骤 2：先写 RED 测试（tests/tst_protocols.cpp 追加）**

slots 声明追加 `void testKittyReleaseEventRouting();`，实现：

```cpp
/**
 * @brief keyReleaseEvent 路由：TerminalDisplay 释放事件经 keyReleasedSignal 抵达仿真层编码。
 */
void TestProtocols::testKittyReleaseEventRouting()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    emu.setKeyBindings(QString());
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display;
    display.setScreenWindow(win);
    display.resize(800, 600);
    QObject::connect(&display, &TerminalDisplay::keyReleasedSignal, &emu,
                     [&](QKeyEvent *e) { emu.sendKeyEvent(e, false); });
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    emu.receiveData("\033[>3u", 5); // 级别 1+2
    QTest::keyRelease(&display, Qt::Key_I, Qt::ControlModifier);
    QCOMPARE(sent, QByteArray("\033[105;5:3u"));
}
```

- [ ] **步骤 3：运行确认 RED**

编译失败（`encodeKittyKeyEvent`/`keyReleasedSignal` 不存在）即 RED。

- [ ] **步骤 4：Vt102Emulation.cpp 实现编码器**

`sendKeyEvent`（:1807）函数体开头（`Qt::KeyboardModifiers modifiers = ...` 之前）插入：

```cpp
    // kitty 键盘协议（级别 1+2）：协商 flags 生效时优先于传统编码；
    // 粘贴文本（fromPaste）与无键码事件（sendText 合成的 key()==0）不参与
    if (_kittyFlags != 0 && !fromPaste && event->key() != 0) {
        QByteArray encoded;
        if (encodeKittyKeyEvent(event, encoded)) {
            if (!encoded.isEmpty()) {
                if (event->type() != QEvent::KeyRelease)
                    emit outputFromKeypressEvent();
                emit sendData(encoded.constData(), encoded.length());
            }
            return;
        }
        // 未命中 kitty 编码的无歧义键：回落下方传统编码
    } else if (event->type() == QEvent::KeyRelease) {
        return; // 未协商 kitty：释放事件无传统编码，直接忽略
    }
```

文件尾部追加编码器实现：

```cpp
/**
 * @brief 计算 kitty 键盘协议的修饰键参数。
 * @param modifiers Qt 修饰键。
 * @return 编码值 = 位和 + 1（shift=1 alt=2 ctrl=4 super=8）。
 */
static int kittyModifierParam(Qt::KeyboardModifiers modifiers) {
    int bits = 0;
    if (modifiers & Qt::ShiftModifier)   bits |= 1;
    if (modifiers & Qt::AltModifier)     bits |= 2;
    if (modifiers & Qt::ControlModifier) bits |= 4;
    if (modifiers & Qt::MetaModifier)    bits |= 8; // super
    return bits + 1;
}

bool Vt102Emulation::encodeKittyKeyEvent(QKeyEvent *event, QByteArray &out) {
    out.clear();

    const int key = event->key();
    // 纯修饰键本身不产生事件（级别 8 未实现）：吞掉，不回落传统路径
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt
            || key == Qt::Key_Meta)
        return true;

    // 键码：特殊键取 kitty 功能键码；可打印键取未 shift 形态码点（字母一律小写）
    int codepoint = 0;
    const bool isEnterTabBackspace =
            (key == Qt::Key_Return || key == Qt::Key_Enter
             || key == Qt::Key_Tab || key == Qt::Key_Backspace);
    switch (key) {
    case Qt::Key_Escape:    codepoint = 27;  break;
    case Qt::Key_Return:
    case Qt::Key_Enter:     codepoint = 13;  break;
    case Qt::Key_Tab:       codepoint = 9;   break;
    case Qt::Key_Backspace: codepoint = 127; break;
    default:
        if (key >= 0x20 && key <= 0x10FFFF) {
            codepoint = key;
            if (codepoint >= 'A' && codepoint <= 'Z')
                codepoint += 32; // kitty 要求未 shift（小写）码点
        }
        break;
    }
    if (codepoint == 0)
        return false; // 未覆盖的功能键（方向键等本已无歧义）：回落传统编码

    const int modBits = kittyModifierParam(event->modifiers()) - 1;
    const bool hasCtrlAltSuper = (modBits & (2 | 4 | 8)) != 0;

    // 事件类型：级别 2 才上报重复（2）与释放（3）；按下恒为 1
    int eventType = 1;
    if (_kittyFlags & 2) {
        if (event->type() == QEvent::KeyRelease)
            eventType = 3;
        else if (event->isAutoRepeat())
            eventType = 2;
    } else if (event->type() == QEvent::KeyRelease) {
        return true; // 未协商事件类型：吞掉释放事件
    }

    // 级别 1（消歧义）下需要 CSI u 编码的键：
    // Esc（任意修饰）；Enter/Tab/Backspace 带修饰；可打印键带 ctrl/alt/super
    bool useKittyForm = false;
    if (_kittyFlags & 1) {
        if (key == Qt::Key_Escape)
            useKittyForm = true;
        else if (isEnterTabBackspace)
            useKittyForm = (modBits != 0);
        else
            useKittyForm = hasCtrlAltSuper;
    }

    if (!useKittyForm) {
        // 无歧义键：按下/重复回落传统编码；释放事件按 kitty 规范不上报（级别 8 未实现）
        return event->type() == QEvent::KeyRelease;
    }

    out = "\033[" + QByteArray::number(codepoint);
    const int modParam = modBits + 1;
    if (_kittyFlags & 2)
        out += ";" + QByteArray::number(modParam) + ":" + QByteArray::number(eventType);
    else if (modParam != 1)
        out += ";" + QByteArray::number(modParam);
    out += "u";
    return true;
}
```

- [ ] **步骤 5：TerminalDisplay keyReleaseEvent + QTermWidget 接线**

`TerminalDisplay.h` protected overrides 区（`keyPressEvent` :650 之后）追加：

```cpp
    void keyReleaseEvent(QKeyEvent* event) override;
```

signals 区（`keyPressedSignal` 声明附近）追加：

```cpp
    /**
     * @brief 用户在终端组件内释放按键时发出（kitty 键盘协议级别 2 的释放事件上报通道）。
     */
    void keyReleasedSignal(QKeyEvent* event);
```

`TerminalDisplay.cpp`（`keyPressEvent` 实现之后）追加：

```cpp
void TerminalDisplay::keyReleaseEvent(QKeyEvent *event) {
    // 同步输出兜底同 keyPressEvent：任何键盘输入都立即补刷
    if (_syncOutputActive)
        flushSynchronizedOutput();
    emit keyReleasedSignal(event);
    event->accept();
}
```

`lib/src/widget/qtermwidget.cpp`（:91 keyPressedSignal 连接之后）追加：

```cpp
    connect(m_terminalDisplay, &TerminalDisplay::keyReleasedSignal, this, [this](QKeyEvent *e) {
        m_emulation->sendKeyEvent(e, false);
    });
```

- [ ] **步骤 6：构建 + ctest，确认转 GREEN；Commit**

```bash
ctest --test-dir build --output-on-failure
git add -A
git commit -m "feat: kitty 键盘按键编码（级别 1 消歧义 + 级别 2 事件类型，keyReleaseEvent 路由）"
```

- [ ] **步骤 7：手动验证（报告中记录结果）**

example 程序内运行 `printf '\e[>3u'` 后用 `showkey -a`/`cat -v` 之类观察：Ctrl+I 输出 `^[[105;5:1u`、释放输出 `^[[105;5:3u`、裸 Tab 仍为 `^I`；退出前 `printf '\e[<u'` 弹栈恢复。

---

## 任务 6：文档收尾 + 全量验证

**文件：**
- 修改：`CHANGELOG`、`README.md`

- [ ] **步骤 1：CHANGELOG 顶部追加条目**

```text
ZzQTermWidget 协议三件套 / 2026-08-12
===============================
 * OSC 8 显式超链接：应用标注的真链接可 Ctrl+点击打开、右键复制地址，优先于正则 URL 匹配。
 * 同步输出 CSI ? 2026：TUI 应用批量重绘期间攒帧防闪屏，1000ms 超时与键盘输入兜底强制 flush。
 * Kitty 键盘协议级别 1+2：消歧义转义码（Ctrl+I 与 Tab 分离等）与按键重复/释放事件上报。
```

- [ ] **步骤 2：README.md "主要修改"列表追加三条**（:26 之后）

```markdown
- 支持 OSC 8 显式超链接（Ctrl+点击打开、右键复制链接地址，优先于正则 URL 匹配）。
- 支持同步输出模式 CSI ? 2026（TUI 批量重绘防闪屏，带超时/输入兜底强制刷新）。
- 支持 kitty 键盘协议级别 1+2（按键消歧义与重复/释放事件上报）。
```

- [ ] **步骤 3：全量验证**

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64
cmake --build build --parallel
ctest --test-dir build --output-on-failure
QT_QPA_PLATFORM=offscreen timeout 3 ./build/qtermwidget_example || true
```

预期：0 error 0 warning，全部测试通过，example 进程能起。

- [ ] **步骤 4：Commit**

```bash
git add -A
git commit -m "docs: 协议三件套（OSC 8 / CSI ? 2026 / kitty 键盘）CHANGELOG 与 README 收尾"
```
