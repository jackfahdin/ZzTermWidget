# ZzQTermWidget 项目整理设计规格

日期：2026-08-11
状态：已获用户批准（2026-08-11）

## 背景

本项目 fork 自 lxqt/qtermwidget（经 QQxiaoming 中间 fork），对方整理较粗糙：核心源码、第三方代码、资源、孤儿二进制混在 `lib/` 下。本项目将作为用户自研终端工具（C++20）的核心组件，需要一次彻底整理，为后续优化打好基础。

## 目标

1. 目录结构清晰分层：公共头 / 核心源码（按职责细分）/ 第三方 / 资源。
2. 删除确认无用的文件与死代码。
3. 修复已确认的 bug。
4. 选择性移植上游 lxqt/qtermwidget 的有价值改进。
5. 修复断裂的翻译链。
6. 全程保持可构建、示例可运行，git 语义化分 commit。

## 非目标

- 不重构 `TerminalDisplay` 中已知的"明知故犯"hack（选区透明度 in-place 颜色交换、`repaintDisplay()` hide/show、quardCRT issue #33 东亚引号修复）——属行为优化，后续专项处理。
- 不引入 Session/SessionManager（fork 的去 Session 化架构保持不变）。
- 不改 ptyqt 子库的内部结构与 CMake（仅随目录迁移）。
- 不改公共 API 形态（`QTermWidget` 类接口保持兼容；仅删除死代码信号）。
- 不动 LICENSE 系列文件的内容。

## 1. 目录重组

目标结构：

```
lib/
├── include/              # qtermwidget.h、qtermwidget_version.h（对外公共头）
├── src/
│   ├── emulation/        # Emulation、Vt102Emulation、Screen、ScreenWindow (.h/.cpp)
│   ├── display/          # TerminalDisplay (.h/.cpp)
│   ├── widget/           # qtermwidget.cpp（门面实现）
│   └── util/             # 现 lib/util 全部内容（含 SearchBar.ui）
├── third_party/
│   ├── utf8proc/         # 现 lib/utf8proc
│   └── ptyqt/            # 现 lib/ptyqt（独立子库，内部不动）
├── resources/
│   ├── color-schemes/    # 现 lib/color-schemes
│   ├── kb-layouts/       # 现 lib/kb-layouts
│   ├── translations/     # 现 lib/translations
│   └── res.qrc           # 现 lib/res.qrc，更新内部相对路径
├── CMakeLists.txt
└── ZzQTermWidgetConfig.cmake.in
example/
└── main.cpp              # 现 examples_main.cpp
```

实现要点：

- 所有源文件 `#include "Xxx.h"` 不带目录前缀，移动后只需调整 `lib/CMakeLists.txt` 的 BUILD_INTERFACE include 目录（`src/emulation`、`src/display`、`src/widget`、`src/util`、`third_party/utf8proc`）；源文件本身原则上零改动。
- qrc 前缀 `:/lib/qtermwidget` 保持不变，`ColorScheme.cpp` / `KeyboardTranslator.cpp` 中的资源路径常量不用改。
- `AUTOUIC_SEARCH_PATHS` 更新指向 `src/util`。
- 安装规则保持不变：全部头文件平铺安装到 `include/ZzQTermWidget`；`find_package(ZzQTermWidget)` 下游用法不受影响。
- 顶层 `CMakeLists.txt` 中示例目标源文件路径更新为 `example/main.cpp`。

## 2. 删除清单

- `tools/sed/` 整个目录（GnuWin32 孤儿二进制，全仓库无引用）。
- `qtermwidget.h/.cpp` 死代码：`finished()` 信号、`sessionFinished()` 槽、`#include <QTranslator>`/`<QLocale>` 残留（第 5 节会重新引入正确的翻译代码，此处指清掉无用的旧残渣）、析构中 `emit destroyed()` 调用。
- `Emulation.cpp` 中直接 `#include "TerminalDisplay.h"` 的层间耦合：检查实际用途，能解耦则解耦（改前向声明或移动依赖代码），确实需要则保留并加注释说明。
- `.github/ISSUE_TEMPLATE.md` 中 lxqt-build-tools 相关过时条目更新为当前构建方式。

保留：`CHANGELOG`、`AUTHORS`、所有 LICENSE 文件原样不动。

## 3. Bug 修复清单

| 问题 | 位置 | 修复 |
|------|------|------|
| `setUrlFilterEnabled()` 从不更新 `m_UrlFilterEnable`，关闭后无法重开 | lib qtermwidget.cpp :865-874 | 补上状态赋值 |
| `setHistorySize()` 负数语义与注释/上游相反（`<=0` 无历史；注释与上游为负数=无限） | lib qtermwidget.cpp :346 | 对齐上游：`<0` → `HistoryTypeFile()`，`==0` → 无历史 |
| CI 触发分支 `master` 与仓库默认分支 `main` 不匹配 | .github/workflows/*.yml | 改为 `main` |
| `sizeHint()` 高度硬编码 150 | lib qtermwidget.cpp :234-238 | 按行高估算合理默认值 |

## 4. 上游选择性移植（来自 lxqt/qtermwidget master）

移植：

- DCS（`ESCP`）/ APC（`ESC_`）/ SOS（`ESCX`）/ PM（`ESC^`）控制串的吞吃支持（`Cse/Cte` 宏、GRP 字符集补 `_^PX`），修复此类序列当前被误解析的问题。
- DECRQM/DECRPM（`CSI Ps $ p` 模式查询与应答），修复终端程序模式查询被静默吞掉的问题。
- 上游近期 TerminalDisplay/Screen 渲染与字体相关的修复（逐个核对上游 commit，只取与本 fork 无冲突的）。

不移植：

- Session/SessionManager 回归、shared 库导出宏、磁盘资源安装路径、lxqt-build-tools 依赖等方向性差异。

## 5. 翻译链修复

- `lib/CMakeLists.txt` 增加 `qt_add_translations`（或 lrelease 自定义目标）将 38 个 `.ts` 编译为 `.qm`，通过 qrc 内嵌。
- `QTermWidget` 构造函数恢复 translator 安装：按 `QLocale::system()` 从 qrc 加载对应 `.qm`，`reTranslateUi` 接口保留可用。
- CI 各平台安装 Qt 时补充 linguist 组件。

## 第三方库决策

- **utf8proc**：保留。该领域事实标准（Julia 语言同款），单文件零依赖；自研 = 自维护 Unicode 数据表，不划算；ICU 过重。升级到最新稳定版 **2.11.3**（当前 vendored 2.9.0 为 2023 年版），获得 Unicode 17 数据与更准确的宽度表，直接改善 CJK/emoji 宽度判定。
- **ptyqt**：保留。已是深度定制的跨平台 PTY 抽象（内嵌 ConPTY 实现 + jthread），市面无更好替代。

## 6. 验证方式

- 每个 commit 前本机构建验证：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel`。
- 示例程序 `qtermwidget_example` 编译通过并能起 shell 冒烟验证。
- CI（Linux/Windows-mingw/Windows-msvc/macOS）三平台兜底。

## Git 提交计划

语义化分 commit，顺序：

1. `refactor: 重组 lib 目录结构（include/src/third_party/resources 分层）`
2. `chore: 删除 tools/sed 孤儿二进制与死代码`
3. `fix: 修复 URL 过滤器开关、历史大小语义、sizeHint、CI 分支名`
4. `feat: 移植上游 DCS/APC/SOS/PM 控制串与 DECRQM 模式查询支持`
5. `feat: 修复翻译链（lrelease 编译 + qrc 内嵌 + 运行时加载）`
6. `chore: 升级 vendored utf8proc 2.9.0 → 2.11.3`

## 风险与权衡

- 重组后与上游文件级 diff 进一步脱钩，日后 cherry-pick 需按 commit hash 手工对照——已接受。
- wchar_t 管线的代理对问题、`copyLineToStream` 静态缓冲、OSC 52 无开关等已知风险点记录在案，本次不改。
