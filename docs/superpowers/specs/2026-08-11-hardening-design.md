# ZzQTermWidget 第二轮强化设计规格：字符管线 char32_t 化、OSC 52 开关、缓冲区安全、hack 清理、QTest 测试设施

日期：2026-08-11
状态：已获用户批准（2026-08-11）

## 背景

第一轮整理（目录重组/清理/移植/翻译链）已完成。本规格覆盖第一轮规格"风险与权衡"中记录在案的已知问题，将组件作为独立项目持续强化。关键事实：`Emulation::receiveData()` 中 `QString::toStdWString()` 按 16 位单元逐个 widening，**代理对目前在所有平台都是断的**（>U+FFFF 字符拆成两个非法 wchar_t 单元），不止 Windows。

## 目标

1. 字符管线 wchar_t → char32_t 全链路改造，根治代理对问题，消除平台差异。
2. OSC 52 剪贴板访问加开关（默认允许，保持现状兼容）。
3. 两处缓冲区安全隐患修复。
4. 三个脆弱 hack 清理或整理。
5. 建立 QTest 测试设施，核心逻辑从此有自动化回归。

## 非目标

- 不改公共 API 既有行为（OSC 52 默认允许；hack 清理不改变可观察行为）。
- 不引入 Session/会话管理。
- `lib/third_party/` 内代码不动。
- issue33 修复逻辑不改变（仅整理注释与边界说明）。

## 1. char32_t 字符管线改造

**改动点：**

- `lib/src/util/Character.h:84`：`Character.character` 由 `wchar_t` 改为 `char32_t`；`ExtendedCharTable`（Emulation.cpp 末尾）哈希键同步。
- `lib/src/emulation/Emulation.cpp` `receiveData()`：`utf16Text.toStdWString()` 改为 `toUcs4()`（`QVector<char32_t>`），按完整码点迭代送 `receiveChar(char32_t)`。代理对在解码出口正确合成，Linux/Windows/macOS 行为一致。
- `Emulation.h` / `Vt102Emulation.{h,cpp}`：`receiveChar`/`sendChar` 签名 wchar_t → char32_t；`tokenBuffer`（`MAX_TOKEN_LENGTH` 数组）改 char32_t；`charClass[256]` 分类仅作用于 `cc < 256`，`cc >= 256` 走 CHR 路径，tokenizer 宏逻辑不变。
- `dupDisplayCharacter(wchar_t)` 与 `dupCache`（`std::vector<wchar_t>`）→ char32_t。
- `lib/src/util/TerminalCharacterDecoder.cpp`、`lib/src/display/TerminalDisplay.{cpp,h}`：fragment 聚合 `std::wstring` → `std::u32string`，绘制出口 `QString::fromUcs4`；其余 wchar_t 使用点（约 12 个文件、仿真核心 20 处 grep 命中）逐一适配。
- `lib/src/util/CharWidth.{h,cpp}`：`unicode_width(char32_t)`——utf8proc 接口本身即 int32，天然契合；`font_width` 系列同步。

**验收：** 经 `receiveData` 灌入 emoji（U+1F600）、国旗区域指示符序列、组合字符后，Screen 格内容与行属性正确；Linux 与 Windows 行为一致。

## 2. OSC 52 开关

- `QTermWidget` 新增 `setOsc52Enabled(bool)` / `osc52Enabled()`，默认 `true`（保持现状，兼容 quardCRT 等现有用户）。
- 贯通至 `Vt102Emulation::processOSC()`：命令 52 在开关关闭时吞掉不执行（不写 QClipboard）。
- 中文 Doxygen 注释说明安全语义（允许远程程序写本地剪贴板的风险）。

## 3. 缓冲区安全

- `Screen.cpp` `copyLineToStream` 的 `static Character characterBuffer[1024]` → `QVarLengthArray<Character>`（栈上默认容量、超出自动堆分配），消除非线程安全与 >1024 列仅靠 assert 的问题。
- `Vt102Emulation.cpp` `tokenBuffer` 达到 `MAX_TOKEN_LENGTH` 时 `resetTokenizer()` + `qWarning`（记录序列上下文），不再静默截断。

## 4. 脆弱 hack 清理

- **选区透明度 in-place 交换**（`TerminalDisplay::drawTextFragment`）：不再修改 `_image` 中的 `Character`，改为构造局部颜色副本交换前/背景后绘制；行为不变。
- **`repaintDisplay()`**（hide+show hack）：查明现有调用场景与根因；能消除则消除，不能则降级为 `update()` 并以中文注释说明保留理由。
- **issue33 东亚引号修复**：逻辑保留（上游无对应方案），注释整理为中文 Doxygen，边界条件（哪些字符、何时逐字绘制）写清楚。

## 5. QTest 测试设施

- 新建 `tests/` 目录，顶层选项 `ZZQTERMWIDGET_BUILD_TESTS`（默认 ON），链接 `Qt6::Test`，`enable_testing()` + `add_test`。
- 覆盖：
  - `CharWidth`：CJK 宽字符、emoji、PUA（宽度 1）、易经符号（宽度 2）、组合符（宽度 0）。
  - tokenizer/`receiveData`：常见 CSI/SGR/OSC 序列喂入后断言 `Screen` 光标与字符状态。
  - 宽字符/组合字符写屏：占位格、ExtendedCharTable 行为。
  - **char32_t 回归**：emoji 经 receiveData 后占 2 列、码点正确（本规格第 1 节的防回归测试）。
  - `HistoryScrollFile`：第一轮恢复的子系统的 add/get/isWrappedLine 基本行为。
  - OSC 52 开关：关闭时不写剪贴板，开启时写入。
- CI 三平台 workflow 加 `ctest` 步骤（构建后运行）。

## 6. 验证与提交

- 每项独立 commit（char32_t 改造可拆 2-3 个 commit：核心管线 → display/decoder 适配 → 测试）。
- 每 commit 前：全量构建 0 error 0 warning + `ctest` 全绿。
- `AGENTS.md` 补测试约定（tests/ 目录、QTest、如何运行）；README 补测试说明。

## Git 提交计划

1. `refactor: 字符管线 wchar_t → char32_t（根治代理对拆分）`
2. `feat: OSC 52 剪贴板访问开关（默认允许）`
3. `fix: copyLineToStream 静态缓冲与 tokenBuffer 溢出处理`
4. `refactor: 清理选区透明度 in-place 交换与 repaintDisplay hack`
5. `test: 引入 QTest 测试设施与核心用例`

## 风险与权衡

- char32_t 改造与上游 diff 进一步拉大——已接受（用户明确选择彻底方案）。
- `std::u32string` fragment 聚合后转 QString 有微小分配开销；绘制路径本就按 fragment 聚合，开销可控。
- OSC 52 默认允许是有意的兼容选择，安全责任在上层应用（README 注明）。
