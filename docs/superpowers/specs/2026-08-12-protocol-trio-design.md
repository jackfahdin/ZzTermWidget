# 协议三件套设计：OSC 8 超链接 / 同步输出（CSI ? 2026）/ Kitty 键盘协议

日期：2026-08-12
状态：已批准（头脑风暴结论）
范围：ZzQTermWidget 第三轮强化。对标 MobaXterm/WindTerm 差距清单中投入产出比最高的三项；Sixel/图形协议与渲染性能优化留待后续轮次。

## 背景与目标

ZzQTermWidget 是 lxqt/qtermwidget 的 Qt6/C++20 深度 fork。经过前两轮整理（lib/ 重组、char32_t 管线、OSC 52、QTest 设施），代码库已具备安全网。本轮目标：

1. **OSC 8 超链接**——应用显式标注的真链接，可点击、可复制。
2. **同步输出 CSI ? 2026**——TUI 应用（neovim、lazygit 等）批量重绘时防闪屏。
3. **Kitty 键盘协议（级别 1+2）**——消除转义码歧义（Ctrl+I=Tab 等），支持按键释放事件上报。

三项均为终端与应用的协议协商特性，无新构建依赖、无第三方库、公共 API 面基本不动。

## 关键决策记录

| 决策点 | 结论 | 理由 |
|--------|------|------|
| 本轮范围 | OSC 8 + 同步输出 + Kitty 键盘 | 投入产出比最高；Sixel/性能优化量级大，单独成轮 |
| Kitty 支持级别 | 1+2（消歧义 + 事件类型） | 覆盖 neovim/helix/yazi 等实际应用；级别 4/8/16 用户极少，YAGNI |
| OSC 8 默认行为 | 主流行为，默认启用 | Ctrl+点击打开、右键复制链接；不加开关 |
| 实现路径 | 最小侵入直接集成（方案 A） | 特性落在现有管线自然位置；不建抽象注册层 |
| 链接存储 | 行级稀疏段表 + 屏级链接哈希表 | 不塞进 ExtendedCharTable（长 URI 会让哈希格膨胀变慢） |

## 架构

改动落在三个现有层：

- `lib/src/emulation/` — Vt102Emulation：OSC 8 解析、CSI ? 2026 模式、kitty 键盘协商序列
- `lib/src/emulation/` — Screen/ScreenWindow：链接存储（行级段表）
- `lib/src/display/` — TerminalDisplay：同步输出攒帧、kitty 按键编码、链接交互
- 链接交互复用 `lib/src/emulation/Filter.cpp` 现有 Filter/HotSpot 管线

## 1. OSC 8 超链接

### 解析

- Vt102Emulation 的 OSC 分发新增 `8` 分支，格式：`OSC 8 ; params ; URI ST`。
- `params` 以 `:` 分隔的键值对，识别 `id=<value>`（同 id 的分段视为同一链接）；未知键忽略。
- 空 URI 表示当前链接结束（后续文本不再属于链接）。
- 超长/异常 OSC 复用现有 `tokenDiscard` 机制吞吃到终止符，不崩溃。

### 存储

- Screen 新增屏级哈希表 `linkId → Hyperlink{ uri, id }`（quint32 自增分配，链接结束或清屏时回收）。
- 每行新增可选的稀疏段表 `QVector<{startCol, endCol, linkId}>`，仅当行内含链接时分配；无链接的行零开销。
- 行进入 scrollback 时链接段随行走；行被复用/清行时段表清除。

### 交互

- OSC 8 热点实现为 `Filter::HotSpot` 子类（`Osc8HotSpot`），由解析路径直接创建并注册，不走正则扫描。天然复用现有悬停高亮、下划线渲染、`activated` 信号与右键菜单。
- 命中单元格同时存在 OSC 8 链接与正则 urlFilter 链接时，OSC 8 优先。
- Ctrl+点击：`QDesktopServices::openUrl`；右键菜单增加"复制链接地址"。
- 默认启用，无新开关。

## 2. 同步输出（CSI ? 2026）

- Vt102Emulation 识别私有模式 `?2026` 的 set（h）/reset（l），经信号通知显示层。
- TerminalDisplay 在模式置位期间挂起重绘（输入照常解析进 Screen，仅攒帧不刷），复位时一次性 `update()`。
- 安全兜底（kitty/iTerm2 同款策略）：模式持续超过 1000ms 强制 flush；收到用户键盘输入时也立即 flush。防止应用崩溃把终端锁成黑屏。
- DECRQM 查询如实应答 2026 状态。
- 无公共 API。

## 3. Kitty 键盘协议（级别 1+2）

### 协商序列

| 序列 | 语义 |
|------|------|
| `CSI > flags u` | push 当前 flags 并入栈新值 |
| `CSI < u` / `CSI < count u` | pop（可指定弹出层数） |
| `CSI = flags ; mode u` | set（mode：1=置位 2=复位 3=重置） |
| `CSI ? flags u` | 查询，应答 `CSI ? flags u`，flags 如实上报仅支持 1+2 |

- flags 栈深度上限 64，超出拒绝，防恶意输入撑爆内存。
- 默认全关，纯应用协商。

### 按键编码

- TerminalDisplay 按键路径新增 kitty 编码分支：
  - 级别 1（消歧义）：对歧义键（Tab/Enter/Esc/Backspace 及带修饰键组合）输出 `CSI codepoint ; modifiers u` 形式；无歧义键维持现有编码。
  - 级别 2（事件类型）：附加 `:1`（按下）/`:2`（重复）/`:3`（释放），释放事件接 `keyReleaseEvent`。
- 修饰键编码按 kitty 规范（shift=1 alt=2 ctrl=4 super=8，值=修饰和+1）。
- 与现有 applicationCursorKeys 等传统模式的优先级按 kitty 规范：kitty flags 生效时优先于传统模式。
- 级别 4/8/16 不实现。

## 错误处理

- OSC 8 空 URI / 非法 params：安全忽略，不产生热点；`openUrl` 失败静默不崩溃。
- CSI ? 2026 嵌套 set：幂等（重复 set 不叠加状态）。
- kitty 非法参数序列：按未知 CSI 忽略，不影响后续解析。
- 所有新解析路径复用现有容错骨架（tokenDiscard、状态机默认分支）。

## 测试

扩展 `tests/tst_emulation.cpp`（现有 QTest 设施），三组用例：

1. **OSC 8**：基本解析、id 参数合并、空 URI 结束链接、行级段表生成、行进 scrollback 后链接保留、清行清除段表。
2. **CSI ? 2026**：set/reset 信号、置位期间不触发重绘、复位一次性重绘、1000ms 超时强制 flush、键盘输入触发 flush。
3. **kitty 键盘**：push/set/pop/query 应答、栈深上限 64、Ctrl+I 与 Tab 消歧编码、修饰键编码值、release 事件（级别 2）。

HotSpot 交互逻辑中能进 QTest 的部分进测试；纯 GUI 点击行为（Ctrl+点击、右键菜单）手动验证。

## 明确不做

- Sixel / Kitty 图形协议（后续轮次）。
- 渲染性能优化（后续轮次，需先建 benchmark 基线）。
- kitty 键盘级别 4/8/16。
- OSC 8 开关项、协议特性注册抽象层（YAGNI）。
