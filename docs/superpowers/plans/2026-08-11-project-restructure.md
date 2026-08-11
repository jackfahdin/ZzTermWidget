# ZzQTermWidget 项目整理实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 将 ZzQTermWidget（lxqt/qtermwidget 的深度 fork）整理为分层清晰的 C++20 静态库：目录重组、删除无用内容、修复 4 个已确认 bug、选择性移植上游改进、修复翻译链、升级 utf8proc。

**架构：** 依据 `docs/superpowers/specs/2026-08-11-project-restructure-design.md`。lib/ 重组为 `include/`（公共头）+ `src/{emulation,display,widget,util}` + `third_party/{utf8proc,ptyqt}` + `resources/`。所有源码 `#include "Xxx.h"` 不带路径，重组只需改 CMake include 目录。qrc 前缀 `:/lib/qtermwidget` 保持不变。

**技术栈：** CMake（本机 4.3.3）、Qt 6.11.1（`/home/zz/Qt/6.11.1/gcc_64`）、C++20、git。上游仓库已 fetch 为 `upstream/master`（57b2539）。

**通用验证命令（每个任务都用，下称"构建验证"）：**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64
cmake --build build --parallel
```

预期：`[100%] Built target qtermwidget_example`，无 error。（基线构建已验证通过，构建目录 `build-baseline/` 可留作对照。）

**注意：** 不要 push；commit 信息按约定式提交（本项目惯例中文/英文混用，沿用中文描述）。

---

## 文件结构（整理后）

- `lib/include/qtermwidget.h`、`lib/include/qtermwidget_version.h` — 对外公共头
- `lib/src/emulation/` — Emulation、Vt102Emulation、Screen、ScreenWindow（.h/.cpp 共 8 个）
- `lib/src/display/` — TerminalDisplay（.h/.cpp）
- `lib/src/widget/` — qtermwidget.cpp
- `lib/src/util/` — 现 lib/util 全部 17 个文件（含 SearchBar.ui）
- `lib/third_party/utf8proc/` — utf8proc.c/.h/utf8proc_data.c（任务 6 升级到 2.11.3 并补 LICENSE）
- `lib/third_party/ptyqt/` — 现 lib/ptyqt 原样迁移（独立子库，内部不动）
- `lib/resources/` — color-schemes/、kb-layouts/、translations/、res.qrc
- `example/main.cpp` — 现 examples_main.cpp

---

## 任务 1：目录重组

**文件：**
- 移动：`lib/*` → 上述新结构（git mv）
- 修改：`lib/CMakeLists.txt`（全部重写路径）、`CMakeLists.txt:47`（示例源路径）

- [ ] **步骤 1：git mv 建立新目录结构**

```bash
cd /home/zz/Jackfahdin/gitcode/qt/ZzQTermWidget
mkdir -p lib/include lib/src/emulation lib/src/display lib/src/widget \
         lib/third_party lib/resources example
git mv lib/qtermwidget.h lib/qtermwidget_version.h lib/include/
git mv lib/Emulation.h lib/Emulation.cpp lib/Vt102Emulation.h lib/Vt102Emulation.cpp \
       lib/Screen.h lib/Screen.cpp lib/ScreenWindow.h lib/ScreenWindow.cpp lib/src/emulation/
git mv lib/TerminalDisplay.h lib/TerminalDisplay.cpp lib/src/display/
git mv lib/qtermwidget.cpp lib/src/widget/
git mv lib/util lib/src/util
git mv lib/utf8proc lib/third_party/utf8proc
git mv lib/ptyqt lib/third_party/ptyqt
git mv lib/color-schemes lib/kb-layouts lib/translations lib/resources/
git mv lib/res.qrc lib/resources/res.qrc
git mv examples_main.cpp example/main.cpp
```

注意：`lib/resources/res.qrc` 内的条目是 `./color-schemes/...`、`./kb-layouts/...` 相对路径，与 qrc 一起移动后**内容不需要改**。

- [ ] **步骤 2：重写 `lib/CMakeLists.txt` 的路径部分**

将 `QTERMWIDGET_SOURCES` / `QTERMWIDGET_HEADERS` / `QTERMWIDGET_FORMS` / `QTERMWIDGET_RESOURCES` 四段替换为：

```cmake
add_subdirectory(third_party/ptyqt)

# utf8proc.c already #includes utf8proc_data.c, so only utf8proc.c is compiled here.
set(QTERMWIDGET_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/utf8proc/utf8proc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/CharWidth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/ColorScheme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/Filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/History.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/HistorySearch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/KeyboardTranslator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/SearchBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/TerminalCharacterDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Emulation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Vt102Emulation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Screen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/ScreenWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display/TerminalDisplay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/widget/qtermwidget.cpp
)

set(QTERMWIDGET_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/utf8proc/utf8proc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/CharWidth.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/CharacterColor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/Character.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/ColorScheme.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/Filter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/History.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/HistorySearch.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/KeyboardTranslator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/SearchBar.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/TerminalCharacterDecoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Emulation.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Vt102Emulation.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Screen.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/ScreenWindow.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display/TerminalDisplay.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qtermwidget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qtermwidget_version.h
)

set(QTERMWIDGET_FORMS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/SearchBar.ui
)

set(QTERMWIDGET_RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/res.qrc
)
```

其余部分改动两处：

```cmake
set_target_properties(qtermwidget PROPERTIES
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src/util
)

target_include_directories(qtermwidget PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/emulation>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/display>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/widget>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/util>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/utf8proc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ZzQTermWidget>
)
```

（`add_library`、alias、警告选项、link、安装规则保持原样。）

- [ ] **步骤 3：修改顶层 `CMakeLists.txt` 示例源路径**

`CMakeLists.txt:47`：`add_executable(qtermwidget_example examples_main.cpp)` → `add_executable(qtermwidget_example example/main.cpp)`

- [ ] **步骤 4：构建验证**

运行通用构建验证。预期编译链接全通过（源文件 include 均不带路径，无需改源码）。

- [ ] **步骤 5：Commit**

```bash
git add -A
git commit -m "refactor: 重组 lib 目录结构为 include/src/third_party/resources 分层"
```

---

## 任务 2：删除无用内容

**文件：**
- 删除：`tools/sed/`（整个目录）
- 修改：`lib/include/qtermwidget.h`、`lib/src/widget/qtermwidget.cpp`、`lib/src/emulation/Emulation.cpp`、`.github/ISSUE_TEMPLATE.md`

- [ ] **步骤 1：删除孤儿二进制**

```bash
git rm -r tools/sed
```

（全仓库无任何引用，已用 `grep -r "tools/sed" .` 确认。）

- [ ] **步骤 2：清理 qtermwidget.h 死代码**

`lib/include/qtermwidget.h` 中删除：
- `:22-23` 的 `#include <QTranslator>` 和 `#include <QLocale>`（翻译链在任务 5 由 .cpp 自行 include，公共头不暴露）
- `:32` 的 `class Session;` 前向声明（Session 已不存在）
- `:272` 信号 `void finished();`
- `:345` 槽 `void sessionFinished();`

同时把 `:115` 注释笔误 `lies < 0` 改为 `lines < 0`。

- [ ] **步骤 3：清理 qtermwidget.cpp 死代码**

`lib/src/widget/qtermwidget.cpp` 中：
- 删除 `sessionFinished()` 实现（`:387-389`）：

```cpp
void QTermWidget::sessionFinished() {
    emit finished();
}
```

- 析构函数（约 `:167`）中的 `emit destroyed();` 删除（`destroyed` 由 QObject 自动发射，手动 emit 无意义）。
- 检查 `:387` 附近及构造函数中是否有对 `sessionFinished` 的 connect，有则一并删除（预期没有——该信号是死的）。

- [ ] **步骤 4：删除 Emulation.cpp 的无用 include**

`lib/src/emulation/Emulation.cpp:39`：删除 `#include "TerminalDisplay.h"`（已确认全文无任何 TerminalDisplay 符号使用）。

- [ ] **步骤 5：更新 ISSUE_TEMPLATE.md**

`.github/ISSUE_TEMPLATE.md` 中提及 `lxqt-build-tools` 的条目改为询问 Qt 版本与编译器版本（本项目已无 lxqt-build-tools 依赖）。

- [ ] **步骤 6：构建验证 + Commit**

```bash
# 构建验证后：
git add -A
git commit -m "chore: 删除 tools/sed 孤儿二进制与 Session 遗留死代码"
```

注意：example/main.cpp 连接了 `finished` 信号（约 `:99` 附近 `connect(console, &QTermWidget::finished, ...)`)——**删除信号前必须先检查并删除此 connect**，否则编译失败。预期处理：示例中 shell 退出感知改由 `localShell->notifier()->aboutToClose` 已覆盖（示例已有此连接），直接删掉 finished 的 connect 行。

---

## 任务 3：Bug 修复（4 个）

**文件：**
- 修改：`lib/src/widget/qtermwidget.cpp`、`lib/include/qtermwidget.h`、`.github/workflows/{linux,macos,windows}.yml`

- [ ] **步骤 1：修复 setUrlFilterEnabled 状态不更新**

`lib/src/widget/qtermwidget.cpp`（现 `:865-874`）：

```cpp
void QTermWidget::setUrlFilterEnabled(bool enable) {
    if(m_UrlFilterEnable == enable) {
        return;
    }
    m_UrlFilterEnable = enable;
    if(enable) {
        m_terminalDisplay->filterChain()->addFilter(m_urlFilter);
    } else {
        m_terminalDisplay->filterChain()->removeFilter(m_urlFilter);
    }
}
```

- [ ] **步骤 2：修复 setHistorySize 负数语义**

`lib/src/widget/qtermwidget.cpp`（现 `:345-350`）改为（对齐上游与头文件注释）：

```cpp
void QTermWidget::setHistorySize(int lines) {
    if (lines < 0)
        m_emulation->setHistory(HistoryTypeFile());
    else if (lines == 0)
        m_emulation->setHistory(HistoryTypeNone());
    else
        m_emulation->setHistory(HistoryTypeBuffer(lines));
}
```

- [ ] **步骤 3：修复 sizeHint 硬编码高度**

`lib/src/widget/qtermwidget.cpp`（现 `:234-238`）改为：

```cpp
QSize QTermWidget::sizeHint() const {
    const QSize size = m_terminalDisplay->sizeHint();
    // TerminalDisplay 未给出有效高度时回退到历史默认值 150
    return (size.isValid() && size.height() > 0) ? size : QSize(size.width(), 150);
}
```

- [ ] **步骤 4：修复 CI 触发分支**

三个 workflow 文件中 `branches: [ master ]` 全部改为 `branches: [ main ]`（linux.yml:5,9、macos.yml:5,9、windows.yml:5,9）。

```bash
sed -i 's/branches: \[ master \]/branches: [ main ]/' .github/workflows/linux.yml .github/workflows/macos.yml .github/workflows/windows.yml
grep -n 'branches' .github/workflows/*.yml   # 预期 6 行均为 [ main ]
```

- [ ] **步骤 5：构建验证 + Commit**

```bash
git add -A
git commit -m "fix: 修复 URL 过滤器开关失效、历史大小负数语义、sizeHint 硬编码、CI 分支名"
```

---

## 任务 4：移植上游 Vt102Emulation 改进（DCS/APC/SOS/PM + DECRQM）

**文件：**
- 修改：`lib/src/emulation/Vt102Emulation.cpp`、`lib/src/emulation/Vt102Emulation.h`

参考：上游 commit `adf8cf2`（控制串吞吃）、`d5cffe1`（DECRQM/DECRPM）。行号基于当前 `lib/Vt102Emulation.cpp`（重组后内容不变）。可用 `git show upstream/master:lib/Vt102Emulation.cpp` 对照。

- [ ] **步骤 1：GRP 字符集补充（initTokenizer，约 `:206`）**

```cpp
  for(s = (quint8*)"()+*#[]%_^PX"; *s; ++s)
    charClass[*s] |= GRP;
```

（原字符串 `"()+*#[]%"` 追加 `_^PX`。）

- [ ] **步骤 2：tokenizer 宏替换（约 `:243-245`）**

将 `Xpe`/`Xte`/`ces` 三个宏整体替换为：

```cpp
#define Cse        (tokenBufferPos >= 2 && (tokenBuffer[1] == ']' || tokenBuffer[1] == 'P' || tokenBuffer[1] == '_' || tokenBuffer[1] == '^' || tokenBuffer[1] == 'X'))
#define Cte        (Cse      && ((tokenBuffer[1] == ']' && cc == 7) || (prevCC == 27 && cc == 92) )) // 27, 92 => "\e\\" (ST); BEL only for OSC
#define ces(C)     (cc < 256 && (charClass[cc] & (C)) == (C) && !Cte)
```

- [ ] **步骤 3：receiveChar 的 CTL 分支（约 `:258-264`）**

`if (Xpe)` 改为 `if (Cse)`，注释同步更新为 ECMA-48 控制串说明（参照上游 :276-289）：

```cpp
  if (ces(CTL))
  {
    // ignore control characters in the text part of Cse escape sequences, aka: OSC "ESC]", DCS
    // "ESCP", APC "ESC_", SOS "ESCX", and PM  "ESC^".
    if (Cse) {
        // Store in prevCC so Cte can detect the ST terminator (prevCC == 27 && cc == 92 => ESC \).
        prevCC = cc;
        return;
    }
```

- [ ] **步骤 4：receiveChar 的终止分支（约 `:295-303`）**

改为（**保留本地函数名 `processOSC()`**，不改回上游的 `processWindowAttributeChange()`）：

```cpp
    if (Cte         ) {
        if (tokenBufferPos >= 2 && tokenBuffer[1] == ']')
            processOSC();
        resetTokenizer();
        return;
    }
    if (Cse         ) { prevCC = cc; return; }
```

- [ ] **步骤 5：DECRQM 的 `$` 中间字节吞吃（`esp()` 检查之后，约 `:346` 后插入）**

```cpp
    // DECRQM: CSI Pd $ p — absorb '$' intermediate byte and dispatch on 'p'
    if (eec('$')) { return; } // absorb '$' and wait for final byte
```

- [ ] **步骤 6：processToken 新增 DECRQM case（`TY_CSI_PE('p')` case 之后，约 `:1498` 后）**

```cpp
    // DECRQM — Request Mode (Host To Terminal)
    // ANSI mode queries: CSI Pd $ p  →  TY_CSI_PS('p', Pd)
    // NOTE: Screen-owned modes must be queried via _currentScreen->getMode()
    case TY_CSI_PS('p',   2) : reportAnsiMode( 2, 2); break; // KAM - Not supported
    case TY_CSI_PS('p',   4) : reportAnsiMode( 4, _currentScreen->getMode(MODE_Insert) ? 1 : 2); break; // IRM
    case TY_CSI_PS('p',  10) : reportAnsiMode(10, 4); break; // HEM - Permanently reset
    case TY_CSI_PS('p',  20) : reportAnsiMode(20, getMode(MODE_NewLine) ? 1 : 2); break; // LNM

    // DEC private mode queries: CSI ? Pd $ p  →  TY_CSI_PR('p', Pd)
    case TY_CSI_PR('p',   1) : reportDecMode(  1, getMode(MODE_AppCuKeys) ? 1 : 2); break; // DECCKM
    case TY_CSI_PR('p',   2) : reportDecMode(  2, getMode(MODE_Ansi) ? 1 : 2);      break; // DECANM
    case TY_CSI_PR('p',   3) : reportDecMode(  3, getMode(MODE_132Columns) ? 1 : 2); break; // DECCOLM
    case TY_CSI_PR('p',   4) : reportDecMode(  4, 4); break; // DECSCLM - Permanently reset
    case TY_CSI_PR('p',   5) : reportDecMode(  5, _currentScreen->getMode(MODE_Screen) ? 1 : 2); break; // DECSCNM
    case TY_CSI_PR('p',   6) : reportDecMode(  6, _currentScreen->getMode(MODE_Origin) ? 1 : 2); break; // DECOM
    case TY_CSI_PR('p',   7) : reportDecMode(  7, _currentScreen->getMode(MODE_Wrap) ? 1 : 2);   break; // DECAWM
    case TY_CSI_PR('p',   8) : reportDecMode(  8, 4); break; // DECARM - Permanently reset
    case TY_CSI_PR('p',   9) : reportDecMode(  9, 4); break; // DECINLM - Permanently reset
    case TY_CSI_PR('p',  10) : reportDecMode( 10, 4); break; // DECEDM - Permanently reset
    case TY_CSI_PR('p',  25) : reportDecMode( 25, _currentScreen->getMode(MODE_Cursor) ? 1 : 2); break; // DECTCEM
    case TY_CSI_PR('p',  47) : reportDecMode( 47, getMode(MODE_AppScreen) ? 1 : 2);            break; // Alt screen
    case TY_CSI_PR('p', 1000) : reportDecMode(1000, getMode(MODE_Mouse1000) ? 1 : 2);          break; // VT200 mouse
    case TY_CSI_PR('p', 1002) : reportDecMode(1002, getMode(MODE_Mouse1002) ? 1 : 2);          break; // Cell motion mouse
    case TY_CSI_PR('p', 1003) : reportDecMode(1003, getMode(MODE_Mouse1003) ? 1 : 2);          break; // All motion mouse
    case TY_CSI_PR('p', 1004) : reportDecMode(1004, _reportFocusEvents ? 1 : 2);               break; // Focus events
    case TY_CSI_PR('p', 1005) : reportDecMode(1005, getMode(MODE_Mouse1005) ? 1 : 2);          break; // UTF-8 mouse
    case TY_CSI_PR('p', 1006) : reportDecMode(1006, getMode(MODE_Mouse1006) ? 1 : 2);          break; // SGR mouse
    case TY_CSI_PR('p', 1015) : reportDecMode(1015, getMode(MODE_Mouse1015) ? 1 : 2);          break; // URXVT mouse
    case TY_CSI_PR('p', 1047) : reportDecMode(1047, getMode(MODE_AppScreen) ? 1 : 2);          break; // Alt screen (xterm)
    case TY_CSI_PR('p', 1049) : reportDecMode(1049, getMode(MODE_AppScreen) ? 1 : 2);          break; // Alt screen + cursor
    case TY_CSI_PR('p', 2004) : reportDecMode(2004, getMode(MODE_BracketedPaste) ? 1 : 2);     break; // Bracketed paste
```

- [ ] **步骤 7：DECRPM 应答函数（`reportStatus()` 之后，约 `:1623` 附近）**

```cpp
// DECRPM — Report Mode (Terminal To Host), response to DECRQM
// Responds to an ANSI mode query (CSI Pd $ p) with: CSI Pd ; Pm $ y
void Vt102Emulation::reportAnsiMode(int mode, int status)
{
    const size_t sz = 32;
    char tmp[sz];
    const size_t r = snprintf(tmp, sz, "\033[%d;%d$y", mode, status);
    if (sz <= r)
        qWarning("Vt102Emulation::reportAnsiMode: Buffer too small\n");
    sendString(tmp);
}

// DECRPM — Report Mode (Terminal To Host), response to DECRQM
// Responds to a DEC private mode query (CSI ? Pd $ p) with: CSI ? Pd ; Pm $ y
void Vt102Emulation::reportDecMode(int mode, int status)
{
    const size_t sz = 32;
    char tmp[sz];
    const size_t r = snprintf(tmp, sz, "\033[?%d;%d$y", mode, status);
    if (sz <= r)
        qWarning("Vt102Emulation::reportDecMode: Buffer too small\n");
    sendString(tmp);
}
```

- [ ] **步骤 8：头文件声明（Vt102Emulation.h，`reportTerminalParms` 声明之后，约 `:169` 后）**

```cpp
  // DECRPM responses to DECRQM queries
  void reportAnsiMode(int mode, int status);
  void reportDecMode(int mode, int status);
```

- [ ] **步骤 9：构建验证 + Commit**

额外冒烟：用 `printf '\033P1!|payload\033\\'` 构造 DCS 串经 `recvData` 灌入不应产生乱码（可在 example 里手工验证或加临时代码，验证后不保留）。

```bash
git add -A
git commit -m "feat: 移植上游 DCS/APC/SOS/PM 控制串吞吃与 DECRQM 模式查询应答"
```

---

## 任务 5：移植上游 TerminalDisplay 修复系列

**文件：**
- 修改：`lib/src/display/TerminalDisplay.cpp`、`lib/src/display/TerminalDisplay.h`、`lib/src/emulation/ScreenWindow.cpp`

参考：上游 commit `62b80fa`、`e5db34a`、`7d535ef`、`dfc282e`（选择区修复系列）、`f22e286`（Shift+click）、`e10d447`（双宽/双高行）、`d58c390`（Fill 背景模式）、`57b2539`（删 debug）。用 `git show upstream/master:lib/TerminalDisplay.cpp` 和 `git show <commit>` 对照。

**约束（移植时必须保留的 fork 特有逻辑）：**
- 选区透明度 in-place 颜色交换绘制（`drawTextFragment` 中 `_selectedTextOpacity` 段）
- `BackgroundMode::Tile` 枚举值（上游没有）
- `_fix_quardCRT_issue33` 东亚引号对齐修复
- fork 新增的 Shift 点击扩展选区 `shiftSelectionStartX/Y` —— 与上游 `f22e286` 功能重叠，移植时**对比两者**：若上游实现更完整则替换 fork 版本，否则保留 fork 版本并跳过 `f22e286`（在实施时判断并在 commit message 中记录选择及理由）
- 多行粘贴确认 `MultilineConfirmationMessageBox`、`setLocked` 遮罩、背景动图/视频

- [ ] **步骤 1：选择区修复系列（约 ±280 行）**

按 upstream/master 最终状态移植以下函数（不要逐 commit 套用，直接对照终态）：
- `loc()`：宏（当前 `TerminalDisplay.cpp:58`）→ 带边界钳制的函数
- `extendSelection()`、`findWordStart`/`findWordEnd`、新增 `findLineStart`/`findLineEnd`
- `ScreenWindow.cpp`：删除 `setSelectionStart`/`setSelectionEnd` 中旧的 `qMin(..., endWindowLine())` 钳制（约 `:112/:119/:126`），`isSelected` 的钳制保留
- `TerminalDisplay.h`：函数声明同步（`findLineStart`/`findLineEnd` 新增、签名变化）

逐函数对照上游终态移植，每移完一个函数确认周边 fork 特有调用点仍一致。

- [ ] **步骤 2：双宽/双高行绘制修复（e10d447）**

`calculateTextArea()` 增加 `const QTransform &textScale` 参数，只对原点做逆变换；替换调用点（当前 `:1895` 旧签名、`:2032-2039` 的 `moveTopLeft(textScale.inverted().map(...))` 块）。头文件签名同步。

- [ ] **步骤 3：Fill 背景图模式（d58c390）**

`BackgroundMode` 枚举在 `Center` 后插入 `Fill`（保留 fork 的 `Tile`）；`paintEvent` 背景绘制分支按上游补充 Fill（等比缩放填满、裁剪溢出）。`qtermwidget.h:83` 按 int 透传，无需改 API。

- [ ] **步骤 4：删 dropEvent 调试输出（57b2539，1 行）**

- [ ] **步骤 5：构建验证 + Commit**

```bash
git add -A
git commit -m "feat: 移植上游选择区修复系列、双宽高行绘制修复、Fill 背景模式"
```

---

## 任务 6：翻译链修复

**文件：**
- 修改：顶层 `CMakeLists.txt`、`lib/CMakeLists.txt`、`lib/src/widget/qtermwidget.cpp`

- [ ] **步骤 1：顶层 CMakeLists.txt 增加 LinguistTools 组件**

`find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ...)` 列表中追加 `LinguistTools`。

- [ ] **步骤 2：lib/CMakeLists.txt 编译并内嵌翻译**

在 `add_library` 之后追加（带 guard，环境无 linguist 时静默跳过保证可构建）：

```cmake
# Translations: compile .ts -> .qm and embed under :/lib/qtermwidget/translations
if(TARGET Qt6::lrelease)
    file(GLOB QTERMWIDGET_TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/resources/translations/*.ts)
    qt_add_lrelease(qtermwidget
        TS_FILES ${QTERMWIDGET_TS_FILES}
        QM_FILES_OUTPUT_VARIABLE QTERMWIDGET_QM_FILES
    )
    qt_add_resources(qtermwidget "translations"
        PREFIX "/lib/qtermwidget/translations"
        FILES ${QTERMWIDGET_QM_FILES}
    )
endif()
```

- [ ] **步骤 3：QTermWidget 构造函数安装 translator**

`lib/src/widget/qtermwidget.cpp` 顶部 include `<QTranslator>`、`<QLocale>`、`<QCoreApplication>`，并加文件级辅助函数：

```cpp
// 按系统 locale 从 qrc 加载库自身翻译；QTranslator 有意不释放，生命周期与 QCoreApplication 一致。
static void installQTermWidgetTranslator()
{
    static QTranslator *translator = [] {
        auto *t = new QTranslator(QCoreApplication::instance());
        if (t->load(QLocale::system(), QStringLiteral("qtermwidget"), QStringLiteral("_"),
                    QStringLiteral(":/lib/qtermwidget/translations"))) {
            QCoreApplication::installTranslator(t);
        } else {
            delete t;
            t = nullptr;
        }
        return t;
    }();
    Q_UNUSED(translator);
}
```

在 `QTermWidget` 构造函数首行调用 `installQTermWidgetTranslator();`。

- [ ] **步骤 4：构建验证 + Commit**

验证 qm 已内嵌：构建后用 `strings build/lib/libqtermwidget.a | grep -c "qtermwidget_zh_CN"` 应 > 0（或运行示例确认搜索栏占位符随 LANG=zh_CN.UTF-8 显示中文）。

```bash
git add -A
git commit -m "feat: 修复翻译链（lrelease 编译 + qrc 内嵌 + 运行时按 locale 加载）"
```

---

## 任务 7：升级 utf8proc 2.9.0 → 2.11.3

**文件：**
- 替换：`lib/third_party/utf8proc/utf8proc.c`、`utf8proc.h`、`utf8proc_data.c`
- 创建：`lib/third_party/utf8proc/LICENSE.md`

- [ ] **步骤 1：下载并替换**

```bash
cd /tmp
curl -L -o utf8proc-2.11.3.tar.gz https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.11.3.tar.gz
tar xzf utf8proc-2.11.3.tar.gz
cp utf8proc-2.11.3/utf8proc.c utf8proc-2.11.3/utf8proc.h utf8proc-2.11.3/utf8proc_data.c \
   /home/zz/Jackfahdin/gitcode/qt/ZzQTermWidget/lib/third_party/utf8proc/
cp utf8proc-2.11.3/LICENSE.md /home/zz/Jackfahdin/gitcode/qt/ZzQTermWidget/lib/third_party/utf8proc/
```

（utf8proc 为 MIT 许可，原目录缺 LICENSE 文件，一并补上。）

- [ ] **步骤 2：构建验证**

`CharWidth.cpp` 使用的 `utf8proc_charwidth()`、`utf8proc_category()` 在 2.x 系列签名未变，预期零源码改动。若编译报错，对照 `utf8proc.h` 调整调用。

- [ ] **步骤 3：Commit**

```bash
git add -A
git commit -m "chore: 升级 vendored utf8proc 2.9.0 → 2.11.3（Unicode 17 数据）"
```

---

## 任务 8：收尾（README + 全量验证）

**文件：**
- 修改：`README.md`

- [ ] **步骤 1：更新 README.md**

- 目录结构说明改为新分层（include/src/third_party/resources + example/）。
- 构建段落补充：翻译已内嵌自动加载；utf8proc 版本 2.11.3。
- "主要修改"列表追加：DCS/APC/SOS/PM 控制串支持、DECRQM 模式查询应答、选择区修复系列、Fill 背景模式、翻译链修复。

- [ ] **步骤 2：全量验证**

```bash
rm -rf build build-baseline
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64
cmake --build build --parallel
```

预期全通过。有条件可 `QT_QPA_PLATFORM=offscreen ./build/qtermwidget_example &` 冒烟 3 秒后 kill（offscreen 平台下能起进程即算通过）。

- [ ] **步骤 3：Commit**

```bash
git add -A
git commit -m "docs: 更新 README 反映新目录结构与本次整理内容"
```
