# 花样下划线与独立下划线色（SGR 4:x / 58 / 59）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 全量支持 SGR `4:0/4:1..4:5`（关/单/双/波浪/点/虚）+ SGR 58 独立下划线色（256 色板与真彩，分号/冒号两种形式）+ SGR 59 复位；修正冒号子参数解析使 `4;3`（下划线+斜体）与 `4:3`（波浪）严格区分；默认单线无独立色保持 QFont underline 现状零回归。

**架构：** CSI 逐参数前导分隔符记录（仅 SGR 消费）→ SGR 处理把样式位（rendition 位 11-13）与 `underlineColor` 落到 Screen 当前格式 → Character（16→20 字节，相等性比较全覆盖，updateImage 脏区天然捕获）→ drawCharacters 尾部手绘样式线（批次/Legacy 单点覆盖）。

**技术栈：** Qt 6.11.1（前缀 `/home/zz/Qt/6.11.1/gcc_64`）、C++20、CMake、QTest（offscreen）。

---

## 关键勘察结论（实现的精确锚点）

- 分隔符记录：`Vt102Emulation.h:148-152` 加 `char argSeparators[MAXARGS]`（首参数记 0）；`addArgument()` → `addArgument(char sep)`；三处调用点 `Vt102Emulation.cpp:486/:499/:525-527` 传 `static_cast<char>(cc)`（`eec` 宏 `:248` 即 `cc == C`，cc 就是分隔符）。截断语义与 argv 一致（`qMin(argc+1, MAXARGS-1)` 后写入）。
- SGR 分发：`receiveChar` 分发循环 `:553-573`。新分支顺序：**4:x 冒号 → 58 冒号 → 58 分号定长 → 既有 38/48 → 通用 else**。58 冒号分支必须在 `58;2` 分号分支之前（`58:2::r:g:b` 拍平后满足 `argv[i]==58 && argv[i+1]==2`）。
- `processToken` switch：`case TY_CSI_PS('m', 4)`（`:1288-1290`）改为置位 + `setUnderlineStyle(p)`；`case 24`（`:1322-1324`）改为 `resetRendition(RE_UNDERLINE | RE_UNDERLINE_STYLE_MASK)`；在 `case 49`（`:1403`）后插 `case 58 / case 59`。
- Character 落点：`displayCharacter` 三处赋值（`Screen.cpp:706-710`、`:766-768`、`:782-784`）各补一行 `underlineColor = effectiveUnderlineColor;`；`updateEffectiveRendition:404-416` 顶部补 `effectiveUnderlineColor = currentUnderlineColor;`（不参与 RE_REVERSE 交换）。
- 绘制：`drawCharacters`（`TerminalDisplay.cpp:929-1090`）：`:945-946` 的 `useUnderline` 加 `!styledUnderline` 门控，函数尾部（`:1090` 前）插手绘调用，`:967-969` 已解析的 `color` 作 DEFAULT 回落色。blink/conceal 提前返回（`:935-940`）天然跳过手绘。
- 聚合循环：批次 `:2305-2307` 加 `currentUnderlineColor` 局部量、while 条件 `:2313` 后加比较；Legacy 同改 `:2459-2461`、`:2467`。
- 无 `sizeof(Character)==16` 假设；History 文件滚动条按字节存取，布局变化构建内透明。
- `clearImage`（`Screen.cpp:968`）**不改**：clearCh 的 `DEFAULT_RENDITION` 本就不含 RE_UNDERLINE，underlineColor 留 NSDMI 默认值即可。

## 跨任务一致的标识符（自检项）

| 标识符 | 定义处 | 含义 |
|---|---|---|
| `RE_UNDERLINE_STYLE_MASK` | `Character.h` | `(7 << 11)`，位 11-13 |
| `UNDERLINE_SINGLE/DOUBLE/CURLY/DOTTED/DASHED` | `Character.h` | 0/1/2/3/4（= SGR 4:n 的 n-1） |
| `Character::underlineColor` | `Character.h` | 公有成员，NSDMI 默认 `CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR)` |
| `Character::underlineStyle()` / `hasCustomUnderlineColor()` | `Character.h` | 内联辅助 |
| `Screen::setUnderlineStyle(int)` / `setUnderlineColor(int,int)` | `Screen.h/.cpp` | 当前格式接口 |
| `Screen::currentUnderlineColor` / `effectiveUnderlineColor` | `Screen.h` 私有 | 当前/生效格式 |
| `Vt102Emulation::argSeparators` / `addArgument(char)` | `Vt102Emulation.h/.cpp` | 分隔符记录 |
| `TerminalDisplay::drawStyledUnderline(QPainter&, const QRect&, const Character*, const QColor&)` | `TerminalDisplay.h/.cpp` | 手绘样式线 |

---

## 任务 1：Character 扩展（存储层 + 相等性回归测试）

**文件：**
- 修改：`lib/src/util/Character.h`
- 测试：`tests/tst_emulation.cpp`

- [ ] **步骤 1：写失败测试**

在 `tests/tst_emulation.cpp` 顶部 `#include "Screen.h"`（已有）之后无需新头文件（Character.h 经 Screen.h 传递包含）。在 slots 列表（`:51` `testNonSixelDcsUnaffected();` 后）追加声明：

```cpp
    void testCharacterUnderlineEquality();
```

文件末尾追加实现：

```cpp
/**
 * @brief Character 相等性必须纳入下划线样式位与 underlineColor。
 * @note 防脏区漏检回归：updateImage 逐格比对走 operator!=，片段合并走逐字段比较；
 *       漏掉 underlineColor 会导致"仅改下划线色"的帧不重绘（显示事故）。
 */
void TestEmulation::testCharacterUnderlineEquality()
{
    Character a, b;
    QVERIFY(a == b);
    QVERIFY(a.equalsFormat(b));
    QCOMPARE(a.underlineStyle(), UNDERLINE_SINGLE);
    QVERIFY(!a.hasCustomUnderlineColor());

    // 仅样式位不同（波浪下划线）
    b.rendition |= RE_UNDERLINE | (UNDERLINE_CURLY << 11);
    QVERIFY(a != b);
    QVERIFY(!a.equalsFormat(b));
    QCOMPARE(b.underlineStyle(), UNDERLINE_CURLY);

    // 仅下划线色不同
    Character c;
    c.underlineColor = CharacterColor(COLOR_SPACE_RGB, (1 << 16) | (2 << 8) | 3);
    QVERIFY(a != c);
    QVERIFY(!a.equalsFormat(c));
    QVERIFY(c.hasCustomUnderlineColor());
}
```

运行（预期**编译失败**——`underlineColor` 等尚不存在，即为 Red）：

```bash
cmake --build build --parallel 2>&1 | grep -c "error:"   # 预期：>0
```

- [ ] **步骤 2：实现 Character.h 扩展**

`lib/src/util/Character.h` 在 `#define RE_OVERLINE (1 << 10)`（`:49`）后追加：

```cpp
/**
 * @brief 下划线样式掩码：rendition 位 11-13 存 UNDERLINE_* 取值，位 14-15 保留。
 * @note RE_UNDERLINE（位 2）保留为"有下划线"汇总位：任何非关样式都置位。
 */
#define RE_UNDERLINE_STYLE_MASK (7 << 11)

/** @brief 下划线样式取值（SGR 4:n 子参数映射 n-1，存于 rendition 位 11-13）。 */
static const int UNDERLINE_SINGLE = 0; ///< 单线（默认）
static const int UNDERLINE_DOUBLE = 1; ///< 双线
static const int UNDERLINE_CURLY  = 2; ///< 波浪线
static const int UNDERLINE_DOTTED = 3; ///< 点线
static const int UNDERLINE_DASHED = 4; ///< 虚线
```

`Character` 类内（`backgroundColor` 成员 `:93` 之后）追加：

```cpp
    /**
     * @brief 独立下划线颜色（SGR 58）；COLOR_SPACE_DEFAULT 表示跟随前景色（SGR 59 复位态）。
     * @note 不参与 RE_REVERSE 前景/背景交换；绘制时 DEFAULT 回落片段实际文本色。
     */
    CharacterColor underlineColor = CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);

    /**
     * @brief 返回下划线样式（rendition 位 11-13）。
     * @return UNDERLINE_* 取值之一；仅当 RE_UNDERLINE 置位时有意义。
     */
    inline int underlineStyle() const {
        return (rendition >> 11) & 0x7;
    }

    /**
     * @brief 返回 true 表示设置了独立下划线色（SGR 58）；false = 跟随前景色。
     */
    inline bool hasCustomUnderlineColor() const {
        return underlineColor.isValid() && underlineColor._colorSpace != COLOR_SPACE_DEFAULT;
    }
```

三个比较函数各补一个条件（`operator==` `:133-138`、`operator!=` `:140-145`、`equalsFormat` `:154-158`）：

```cpp
inline bool operator == (const Character& a, const Character& b) {
    return a.character == b.character &&
           a.rendition == b.rendition &&
           a.foregroundColor == b.foregroundColor &&
           a.backgroundColor == b.backgroundColor &&
           a.underlineColor == b.underlineColor;
}

inline bool operator != (const Character& a, const Character& b) {
    return a.character != b.character ||
           a.rendition != b.rendition ||
           a.foregroundColor != b.foregroundColor ||
           a.backgroundColor != b.backgroundColor ||
           a.underlineColor != b.underlineColor;
}
```

`equalsFormat` 的 return 末尾加 `&& underlineColor==other.underlineColor`。

构造函数**不**加参数（NSDMI 兜底，`Character()` 与 `Screen::defaultChar` 语义不变）。

- [ ] **步骤 3：构建 + 跑测试**

```bash
cmake --build build --parallel
ctest --test-dir build -R tst_emulation --output-on-failure
```

预期：`100% tests passed, 0 tests failed out of 1`。再跑全量确认无回归：

```bash
ctest --test-dir build --output-on-failure   # 预期：100% tests passed, 0 tests failed out of 9
```

- [ ] **步骤 4：提交**

```bash
git add lib/src/util/Character.h tests/tst_emulation.cpp
git commit -m "feat(terminal): Character 增加下划线样式位与独立下划线色字段

- rendition 位 11-13 存 UNDERLINE_* 样式（RE_UNDERLINE 保留为汇总位）
- 新增 underlineColor 成员（16→20 字节），operator==/!=/equalsFormat 全覆盖
- 新增 underlineStyle()/hasCustomUnderlineColor() 内联辅助"
```

---

## 任务 2：CSI 分隔符记录 + SGR 4:x/58/59 解析 + Screen 存储落点

**文件：**
- 修改：`lib/src/emulation/Vt102Emulation.h`、`lib/src/emulation/Vt102Emulation.cpp`
- 修改：`lib/src/emulation/Screen.h`、`lib/src/emulation/Screen.cpp`
- 测试：`tests/tst_emulation.cpp`

- [ ] **步骤 1：写失败测试**

slots 列表追加：

```cpp
    void testSgrUnderlineStyles_data();
    void testSgrUnderlineStyles();
    void testSgrUnderlineSemicolonVsColon();
    void testSgrUnderlineColor();
```

`firstLineText` 辅助（`:58-62`）后追加读格辅助：

```cpp
/**
 * @brief 读取当前屏幕第 0 行前 columns 格的 Character（经 Screen::getImage 真实路径）。
 */
static QVector<Character> firstLineChars(Vt102Emulation &emu, int columns)
{
    Screen *scr = emu.createWindow()->screen();
    QVector<Character> buf(columns);
    scr->getImage(buf.data(), columns, 0, 0);
    return buf;
}
```

文件末尾追加：

```cpp
void TestEmulation::testSgrUnderlineStyles_data()
{
    QTest::addColumn<QByteArray>("seq");
    QTest::addColumn<bool>("underlined");
    QTest::addColumn<int>("style");
    QTest::newRow("SGR 4 单线")    << QByteArray("\033[4m")          << true  << UNDERLINE_SINGLE;
    QTest::newRow("SGR 4:0 关")    << QByteArray("\033[4:0m")        << false << UNDERLINE_SINGLE;
    QTest::newRow("SGR 4:1 单线")  << QByteArray("\033[4:1m")        << true  << UNDERLINE_SINGLE;
    QTest::newRow("SGR 4:2 双线")  << QByteArray("\033[4:2m")        << true  << UNDERLINE_DOUBLE;
    QTest::newRow("SGR 4:3 波浪")  << QByteArray("\033[4:3m")        << true  << UNDERLINE_CURLY;
    QTest::newRow("SGR 4:4 点线")  << QByteArray("\033[4:4m")        << true  << UNDERLINE_DOTTED;
    QTest::newRow("SGR 4:5 虚线")  << QByteArray("\033[4:5m")        << true  << UNDERLINE_DASHED;
    QTest::newRow("SGR 4:6 非法")  << QByteArray("\033[4:6m")        << false << UNDERLINE_SINGLE;
    QTest::newRow("SGR 4:99 非法") << QByteArray("\033[4:99m")       << false << UNDERLINE_SINGLE;
    QTest::newRow("SGR 24 关")     << QByteArray("\033[4:3m\033[24m") << false << UNDERLINE_SINGLE;
}

/**
 * @brief SGR 4:n 冒口子参数：样式落入 rendition 位 11-13，RE_UNDERLINE 作汇总位。
 * @note 4:0/24 关下划线同时清样式位；非法样式（>=6）整体忽略。
 */
void TestEmulation::testSgrUnderlineStyles()
{
    QFETCH(QByteArray, seq);
    QFETCH(bool, underlined);
    QFETCH(int, style);
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.receiveData(seq.constData(), seq.size());
    emu.receiveData("X", 1);
    const QVector<Character> line = firstLineChars(emu, 80);
    QCOMPARE(bool(line[0].rendition & RE_UNDERLINE), underlined);
    QCOMPARE(line[0].underlineStyle(), style);
}

/**
 * @brief 核心回归：4;3（分号）= 下划线+斜体，4:3（冒号）= 波浪下划线，严格区分。
 * @note 改造前 ':' 被拍平为参数分隔，4:3 被误判为下划线+斜体。
 */
void TestEmulation::testSgrUnderlineSemicolonVsColon()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const char *seq = "\033[4;3mX\033[0m\033[4:3mY";
    emu.receiveData(seq, int(std::strlen(seq)));
    const QVector<Character> line = firstLineChars(emu, 80);
    // X：分号语义 —— 下划线 + 斜体，样式位保持单线
    QVERIFY(line[0].rendition & RE_UNDERLINE);
    QVERIFY(line[0].rendition & RE_ITALIC);
    QCOMPARE(line[0].underlineStyle(), UNDERLINE_SINGLE);
    // Y：冒口子参数 —— 波浪下划线，无斜体
    QVERIFY(line[1].rendition & RE_UNDERLINE);
    QVERIFY(!(line[1].rendition & RE_ITALIC));
    QCOMPARE(line[1].underlineStyle(), UNDERLINE_CURLY);
}

/**
 * @brief SGR 58 独立下划线色（分号/冒号两种形式）与 SGR 59 复位。
 * @note 58;2 按定长消费 5 槽：58;2;1;2;3;1 的尾巴 1 必须解释为独立 SGR（粗体）。
 */
void TestEmulation::testSgrUnderlineColor()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const char *seq =
            "\033[58;5;196mA"          // 分号 256 色
            "\033[59mB"                // 复位为跟随前景
            "\033[58;2;10;20;30mC"     // 分号真彩
            "\033[58:5:42mD"           // 冒号 256 色
            "\033[58:2::100:150:200mE" // 冒号真彩（容忍色彩空间空位）
            "\033[58;2;1;2;3;1mF";     // 定长消费：尾巴 ;1 = 粗体
    emu.receiveData(seq, int(std::strlen(seq)));
    const QVector<Character> line = firstLineChars(emu, 80);
    QVERIFY(line[0].underlineColor == CharacterColor(COLOR_SPACE_256, 196));
    QVERIFY(line[1].underlineColor == CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR));
    QVERIFY(line[2].underlineColor == CharacterColor(COLOR_SPACE_RGB, (10 << 16) | (20 << 8) | 30));
    QVERIFY(line[3].underlineColor == CharacterColor(COLOR_SPACE_256, 42));
    QVERIFY(line[4].underlineColor == CharacterColor(COLOR_SPACE_RGB, (100 << 16) | (150 << 8) | 200));
    QVERIFY(line[5].underlineColor == CharacterColor(COLOR_SPACE_RGB, (1 << 16) | (2 << 8) | 3));
    QVERIFY(line[5].rendition & RE_BOLD); // 定长消费回归：尾巴未被吞
}
```

构建 + 运行（预期：编译通过，新测试**失败**——`4:3` 现状误判为下划线+斜体，underlineColor 全默认）：

```bash
cmake --build build --parallel
ctest --test-dir build -R tst_emulation --output-on-failure   # 预期：新用例 FAIL
```

- [ ] **步骤 2：分隔符记录（Vt102Emulation.h/.cpp）**

`Vt102Emulation.h` 把 `:148-152` 段改为：

```cpp
#define MAXARGS 15
  void addDigit(int dig);
  void addArgument(char sep);
  int argv[MAXARGS];
  /**
   * @brief 逐参数前导分隔符记录：argSeparators[i] 为引入第 i 个参数的分隔符
   *        （';' 或 ':'），首参数记 0。仅 SGR（最终字符 'm'）分发消费该信息，
   *        其余 CSI 序列只记录不解释，行为零变化；截断语义与 argv 一致。
   */
  char argSeparators[MAXARGS];
  int argc;
```

`Vt102Emulation.cpp` 的 `resetTokenizer`（`:163-169`）补两行；`addArgument`（`:176-179`）改签名：

```cpp
void Vt102Emulation::resetTokenizer() {
    tokenBufferPos = 0;
    argc = 0;
    argv[0] = 0;
    argv[1] = 0;
    argSeparators[0] = 0;
    argSeparators[1] = 0;
    prevCC = 0;
}

/**
 * @brief 结束当前参数并开始下一个参数。
 * @param sep 引入新参数的前导分隔符（';' 或 ':'），记录到 argSeparators 供 SGR 消费。
 */
void Vt102Emulation::addArgument(char sep) {
    argc = qMin(argc + 1, MAXARGS - 1);
    argv[argc] = 0;
    argSeparators[argc] = sep;
}
```

三处调用点（`:486`、`:499`、`:525-527`）统一改为（`eec` 宏保证 `cc` 就是分隔符）：

```cpp
            if (eec(';') || eec(':')) { addArgument(static_cast<char>(cc)); return; }
```

- [ ] **步骤 3：SGR 分发分支（Vt102Emulation.cpp 分发循环）**

把 `:553-573` 的 for 循环体改为（新增 4:x、58 三组分支，置于 38/48 分支之前；顺序即匹配优先级）：

```cpp
        for (int i = 0; i <= argc; i++) {
            if (epp())
                processToken(TY_CSI_PR(cc, argv[i]), 0, 0);
            else if (egt())
                processToken(TY_CSI_PG(cc), 0, 0); // spec. case for ESC]>0c or ESC]>c
            else if (cc == 'm' && argv[i] == 4 && i + 1 <= argc && argSeparators[i + 1] == ':') {
                // SGR 4:n 冒口子参数：下划线样式（ECMA-48 子参数；与 4;n 分号语义严格区分）
                const int sub = argv[++i]; // 消费子参数
                if (sub == 0)
                    processToken(TY_CSI_PS(cc, 24), 0, 0);        // 4:0 = 关下划线
                else if (sub >= 1 && sub <= 5)
                    processToken(TY_CSI_PS(cc, 4), sub - 1, 0);   // p 携带样式（0=单线…4=虚线）
                // 非法样式（>=6）：连同子参数整体忽略，不影响其余 SGR 参数
            } else if (cc == 'm' && argv[i] == 58 && i + 1 <= argc && argSeparators[i + 1] == ':') {
                // SGR 58 冒号形式：58:5:n / 58:2[:色彩空间空位]:r:g:b（必须先于 58 分号分支：
                // 58:2::r:g:b 拍平后同样满足 argv[i]==58 && argv[i+1]==2）
                if (argv[i + 1] == 5 && i + 2 <= argc) {
                    processToken(TY_CSI_PS(cc, 58), COLOR_SPACE_256, argv[i + 2]);
                    i += 2;
                } else if (argv[i + 1] == 2) {
                    int j = i + 2;
                    // 容忍色彩空间空位：58:2::r:g:b 拍平后该槽为 0
                    // （要求其后仍够 r/g/b 三槽才跳过；已知歧义：r=0 的真彩写法被当空位，整体忽略）
                    if (j <= argc && argv[j] == 0 && argc - j >= 3)
                        j++;
                    if (argc - j >= 2) {
                        processToken(TY_CSI_PS(cc, 58), COLOR_SPACE_RGB,
                                             (argv[j] << 16) | (argv[j + 1] << 8) | argv[j + 2]);
                        i = j + 2;
                    } else {
                        i += 1; // 参数不足：忽略 58 与模式槽，其余参数按独立 SGR 解释
                    }
                } else {
                    i += 1; // 未知 58 模式：忽略 58 与模式槽
                }
            } else if (cc == 'm' && argc - i >= 4 && argv[i] == 58 && argv[i + 1] == 2) {
                // SGR 58 分号真彩：58;2;r;g;b（镜像 38/48 写法，定长消费 5 槽，尾巴不吞）
                i += 2;
                processToken(TY_CSI_PS(cc, 58), COLOR_SPACE_RGB,
                                         (argv[i] << 16) | (argv[i + 1] << 8) | argv[i + 2]);
                i += 2;
            } else if (cc == 'm' && argc - i >= 2 && argv[i] == 58 && argv[i + 1] == 5) {
                // SGR 58 分号 256 色：58;5;n
                i += 2;
                processToken(TY_CSI_PS(cc, 58), COLOR_SPACE_256, argv[i]);
            } else if (cc == 'm' && argv[i] == 58) {
                // 参数不足的 58：忽略该参数，不吞后续独立 SGR
            } else if (cc == 'm' && argc - i >= 4 && (argv[i] == 38 || argv[i] == 48) &&
```

（38/48 两个既有分支与通用 else 原样保留。）

- [ ] **步骤 4：processToken 的 SGR case 改造**

`case 'm', 4`（`:1288-1290`）改为：

```cpp
    case TY_CSI_PS('m', 4):
        _currentScreen->setRendition(RE_UNDERLINE);
        _currentScreen->setUnderlineStyle(int(p)); // p：冒口子参数样式（0=单线…4=虚线）；分号形式恒 0
        break; // VT100
```

`case 'm', 24`（`:1322-1324`）改为：

```cpp
    case TY_CSI_PS('m', 24):
        _currentScreen->resetRendition(RE_UNDERLINE | RE_UNDERLINE_STYLE_MASK); // 关下划线并清样式位
        break;
```

`case 'm', 49`（`:1403-1405`）之后插入：

```cpp
    case TY_CSI_PS('m', 58):
        _currentScreen->setUnderlineColor(p, q);
        break;

    case TY_CSI_PS('m', 59):
        _currentScreen->setUnderlineColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
        break;
```

- [ ] **步骤 5：Screen 当前格式与落点**

`Screen.h` 公有区（`setBackColor` 声明 `:392` 后）追加：

```cpp
    /**
     * @brief 设置当前下划线样式（SGR 4:n 冒口子参数）。
     * @param style UNDERLINE_* 取值（0=单线…4=虚线），写入 rendition 位 11-13。
     * @note 仅写样式位；RE_UNDERLINE 汇总位由调用方（SGR 4 分支）负责置位。
     */
    void setUnderlineStyle(int style);
    /**
     * @brief 设置当前独立下划线颜色（SGR 58）。
     * @param space 颜色空间（COLOR_SPACE_*）；COLOR_SPACE_DEFAULT 表示跟随前景色（SGR 59）。
     * @param color 颜色值，语义同 setForeColor()；非法值回退为跟随前景。
     */
    void setUnderlineColor(int space, int color);
```

`Screen.h` 私有成员（`:865-867` 区与 `:888-890` 区）追加：

```cpp
    CharacterColor currentForeground;
    CharacterColor currentBackground;
    /** @brief 当前独立下划线颜色；默认 COLOR_SPACE_DEFAULT（跟随前景）。 */
    CharacterColor currentUnderlineColor = CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
    quint16 currentRendition;
```

```cpp
    CharacterColor effectiveForeground; // These are derived from
    CharacterColor effectiveBackground; // the cu_* variables above
    CharacterColor effectiveUnderlineColor = CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
    quint16 effectiveRendition;         // to speed up operation
```

`SavedState`（`:892` 起，`foreground`/`background` 成员后）加：

```cpp
        /** @brief 保存的下划线色；DECSC/DECRC 随前景/背景一同保存恢复（镜像 konsole 语义）。 */
        CharacterColor underlineColor = CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
```

`Screen.cpp`：

`updateEffectiveRendition`（`:404-416`）函数体顶部加一行：

```cpp
void Screen::updateEffectiveRendition() {
    effectiveRendition = currentRendition;
    // 下划线色不参与 RE_REVERSE 前景/背景交换；DEFAULT 由绘制层回落片段实际文本色
    effectiveUnderlineColor = currentUnderlineColor;
```

`setDefaultRendition`（`:1131-1136`）加一行：

```cpp
void Screen::setDefaultRendition() {
    setForeColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
    setBackColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR);
    setUnderlineColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR); // SGR 0 连带复位下划线色
    currentRendition = DEFAULT_RENDITION;
    updateEffectiveRendition();
}
```

`setBackColor`（`:1154`）之后追加两个新方法：

```cpp
void Screen::setUnderlineStyle(int style) {
    currentRendition = static_cast<quint16>(
            (currentRendition & ~RE_UNDERLINE_STYLE_MASK)
            | ((style << 11) & RE_UNDERLINE_STYLE_MASK));
    updateEffectiveRendition();
}

void Screen::setUnderlineColor(int space, int color) {
    currentUnderlineColor = CharacterColor(space, color);

    if (currentUnderlineColor.isValid())
        updateEffectiveRendition();
    else
        // 非法颜色：复位为跟随前景（镜像 setForeColor 的回退语义）
        setUnderlineColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
}
```

`saveCursor`（`:273-279`）加 `savedState.underlineColor = currentUnderlineColor;`；`restoreCursor`（`:281-288`）在 `updateEffectiveRendition()` 前加 `currentUnderlineColor = savedState.underlineColor;`。

`displayCharacter` 三处落点各补一行（`:708-710` 组合字符补格块、`:766-768` 主字符、`:782-784` 宽字符填充格）：

```cpp
    ch.underlineColor = effectiveUnderlineColor;        // :710 后（ch 块）
    currentChar.underlineColor = effectiveUnderlineColor; // :768 后
    ch.underlineColor = effectiveUnderlineColor;        // :784 后（宽字符填充块）
```

- [ ] **步骤 6：构建 + 测试 + 全量回归**

```bash
cmake --build build --parallel
ctest --test-dir build -R tst_emulation --output-on-failure   # 预期：Passed
ctest --test-dir build --output-on-failure                    # 预期：9/9 全绿
```

- [ ] **步骤 7：提交**

```bash
git add lib/src/emulation/Vt102Emulation.h lib/src/emulation/Vt102Emulation.cpp \
        lib/src/emulation/Screen.h lib/src/emulation/Screen.cpp tests/tst_emulation.cpp
git commit -m "feat(emulation): CSI 逐参数分隔符记录与 SGR 4:x/58/59 解析

- argSeparators[MAXARGS] 记录逐参数前导分隔符，仅 SGR 消费，其余序列行为不变
- 4:0/4:1..4:5 下划线样式落 rendition 位 11-13；4;3 与 4:3 严格区分
- 58 分号/冒号两种形式（58;5;n、58;2;r;g;b、58:5:n、58:2::r:g:b），59 复位
- 58 分号形式定长消费不吞尾巴；非法样式与参数不足整体忽略
- Screen 增加 currentUnderlineColor/effectiveUnderlineColor，DECSC 随游标保存"
```

---

## 任务 3：手绘样式线绘制（双/点/虚/波浪 + 独立色，批次/Legacy 一致）

**文件：**
- 修改：`lib/src/display/TerminalDisplay.h`、`lib/src/display/TerminalDisplay.cpp`
- 测试：`tests/tst_rendering.cpp`

- [ ] **步骤 1：写失败测试**

`tests/tst_rendering.cpp` slots 列表（`:31` 后）追加：

```cpp
    void testStyledUnderlinePixelEquivalence();
    void testStyledUnderlinePixels();
    void testStyledUnderlineDirtyRegion();
```

文件末尾追加（辅助 `initRenderEnv`/`renderFull`/`replayDirtyRegion`/`pumpFrame` 复用既有实现）：

```cpp
/**
 * @brief 花样下划线与独立下划线色：批次聚合路径与 Legacy 逐片段路径逐像素相等。
 * @note 覆盖全部手绘样式（双/波浪/点/虚）、58 分号/冒号、59 复位与宽字符混排。
 */
void TestRendering::testStyledUnderlinePixelEquivalence()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    QByteArray s;
    s += "\033[H";
    s += "\033[4:1msingle styled\033[0m\r\n";
    s += "\033[4:2mdouble underline\033[0m\r\n";
    s += "\033[4:3mcurly underline\033[0m\r\n";
    s += "\033[4:4mdotted underline\033[0m\r\n";
    s += "\033[4:5mdashed underline\033[0m\r\n";
    s += "\033[4m\033[58;5;196m256color underline\033[0m\r\n";
    s += "\033[4:3m\033[58;2;0;255;0mrgb curly\033[0m\r\n";
    s += "\033[4:2m\033[58:2::255:128:0mcolon rgb double\033[0m\r\n";
    s += "\033[4:3m\xE4\xB8\xAD\xE6\x96\x87 wide \xE4\xB8\xAD\xE6\x96\x87\033[0m\r\n"; // 中文 wide 中文
    s += "\033[58;5;42m\033[4mcolored then \033[59m\033[4mreset\033[0m\r\n";
    emu.receiveData(s.constData(), int(s.size()));

    const QImage batched = renderDisplay(display, true);
    const QImage legacy = renderDisplay(display, false);
    if (batched != legacy) { // 排障辅助：落盘人工比对
        batched.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-ul-batched.png")));
        legacy.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-ul-legacy.png")));
    }
    QCOMPARE(batched, legacy);
}

/**
 * @brief 手绘下划线像素证据：独立绿色下划线可被逐行检出；波浪线纵向覆盖行数多于单线
 *        且波谷低于单线最底行；双线存在中间无墨间隙。
 * @note 用 58;2;0;255;0 纯绿独立色 + 红色文本，颜色隔离文本抗锯齿像素；
 *       波浪线开抗锯齿，边缘像素为混合色，故用"偏绿"阈值而非精确等值。
 */
void TestRendering::testStyledUnderlinePixels()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    QByteArray s;
    s += "\033[H";
    s += "\033[31m\033[4:1m\033[58;2;0;255;0mAAAA\033[0m\r\n"; // 行 0：红字 + 绿色单线
    s += "\033[4:3m\033[58;2;0;255;0mAAAA\033[0m\r\n";          // 行 1：绿色波浪
    s += "\033[4:2m\033[58;2;0;255;0mAAAA\033[0m\r\n";          // 行 2：绿色双线
    emu.receiveData(s.constData(), int(s.size()));
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup：吃掉 _drawTextTestFlag 一次性度量
    const QImage img = renderFull(display);

    const int fw = display.fontWidth();
    const int fh = display.fontHeight();
    const int top0 = display.contentsRect().top() + display.margin();
    const int left0 = display.contentsRect().left() + display.margin();
    const auto greenish = [](QRgb px) {
        return qGreen(px) > 200 && qRed(px) < 80 && qBlue(px) < 80;
    };
    // 统计第 row 行、行内 y 偏移 dy 处、前 4 格宽内的绿色像素数
    const auto greenCountAt = [&](int row, int dy) {
        int n = 0;
        const int y = top0 + row * fh + dy;
        for (int x = left0; x < left0 + 4 * fw; ++x)
            if (greenish(img.pixel(x, y)))
                ++n;
        return n;
    };
    const auto inkDys = [&](int row) {
        QList<int> dys;
        for (int dy = fh / 2; dy < fh; ++dy)
            if (greenCountAt(row, dy) > 0)
                dys.append(dy);
        return dys;
    };

    // 单线：绿色像素集中在 1~2 条相邻水平线上
    const QList<int> single = inkDys(0);
    QVERIFY2(single.size() >= 1 && single.size() <= 2,
             qPrintable(QStringLiteral("单线下划线墨行数 %1 异常").arg(single.size())));

    // 波浪：振幅使墨行多于单线，且波谷有像素低于单线最底行
    const QList<int> curly = inkDys(1);
    QVERIFY2(curly.size() > single.size(),
             qPrintable(QStringLiteral("波浪墨行数 %1 未多于单线 %2")
                        .arg(curly.size()).arg(single.size())));
    QVERIFY2(curly.last() > single.last(),
             qPrintable(QStringLiteral("波浪波谷 %1 未低于单线底 %2")
                        .arg(curly.last()).arg(single.last())));

    // 双线：两条墨带之间存在无墨间隙行
    const QList<int> dbl = inkDys(2);
    QVERIFY(dbl.size() >= 2);
    bool hasGap = false;
    for (int dy = dbl.first() + 1; dy < dbl.last(); ++dy)
        if (!dbl.contains(dy))
            hasGap = true;
    QVERIFY2(hasGap, "双线下划线两条墨带间无间隙");
}
```

先跑渲染等价（此时手绘未实现，`4:x` 已被任务 2 解析但绘制仍走字体下划线开关——所有样式渲染相同，双路径仍相等，此用例**未必先红**，属安全网）；像素证据用例必然先红（波浪/双线墨行断言不成立）。脏区用例：

```cpp
/**
 * @brief 样式/下划线色变更格必脏：仅改下划线样式或仅改 underlineColor 重写同文本，
 *        该行脏区非空且增量重放与全量渲染逐像素相等。
 * @note 后者是 operator!=/equalsFormat 纳入 underlineColor 的端到端证据（防脏区漏检）。
 */
void TestRendering::testStyledUnderlineDirtyRegion()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    emu.receiveData("\033[1;1HAB", 8);
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup
    const QImage base = renderFull(display);

    const int fh = display.fontHeight();
    const QRect band(0, display.contentsRect().top() + display.margin(), display.width(), fh);

    // 仅改下划线样式（A B 文本不变）：样式位变化 → 必脏
    const char *edit1 = "\033[1;1H\033[4:3mAB";
    emu.receiveData(edit1, int(std::strlen(edit1)));
    pumpFrame(win);
    QVERIFY2(display.lastDirtyRegion().intersects(band), "下划线样式变更未置脏");
    const QImage inc1 = replayDirtyRegion(display, base);
    const QImage full1 = renderFull(display);
    QCOMPARE(inc1, full1);

    // 仅改下划线色（文本与样式位均不变）：underlineColor 参与相等性 → 必脏
    const char *edit2 = "\033[1;1H\033[4m\033[58;5;196mAB";
    emu.receiveData(edit2, int(std::strlen(edit2)));
    pumpFrame(win);
    QVERIFY2(display.lastDirtyRegion().intersects(band), "下划线色变更未置脏（equalsFormat 漏检）");
    const QImage inc2 = replayDirtyRegion(display, full1);
    const QImage full2 = renderFull(display);
    QCOMPARE(inc2, full2);
}
```

注：`tst_rendering.cpp` 需 `#include <cstring>`（`std::strlen`）——若无则加。

```bash
cmake --build build --parallel
ctest --test-dir build -R tst_rendering --output-on-failure   # 预期：Pixels/DirtyRegion 用例 FAIL
```

- [ ] **步骤 2：drawCharacters 门控 + 手绘插入点**

`TerminalDisplay.cpp` 的 `drawCharacters`（`:943-950`）改为：

```cpp
    // setup bold and underline
    bool useBold =
            ((style->rendition & RE_BOLD) && _boldIntense) || font().bold();
    // 花样下划线（非单线样式）或独立下划线色走手绘路径：关闭字体下划线，
    // 文本绘制后在函数尾部按样式手绘；默认单线无独立色保持 QFont underline 现状
    const bool styledUnderline = (style->rendition & RE_UNDERLINE) != 0
            && (style->underlineStyle() != UNDERLINE_SINGLE || style->hasCustomUnderlineColor());
    const bool useUnderline = !styledUnderline
            && (style->rendition & RE_UNDERLINE || font().underline());
```

`drawCharacters` 函数尾部（`:1089` else 块结束后、`:1090` 闭括号前）插入：

```cpp
    // 手绘样式下划线：blink/conceal 已在函数头部提前返回，此处无需再判；
    // color 为 :967-969 解析的片段实际文本色（含光标反色），作 DEFAULT 下划线色的回落
    if (styledUnderline)
        drawStyledUnderline(painter, rect, style, color);
```

- [ ] **步骤 3：drawStyledUnderline 实现**

`TerminalDisplay.h` 在 `drawCharacters` 声明（`:822-824`）后追加：

```cpp
    /**
     * @brief 手绘样式下划线（SGR 4:2..4:5 双/波浪/点/虚，或带独立下划线色的单线）。
     * @param painter 目标画笔（字体与世界变换已按片段设置，DECDH 缩放同样生效）。
     * @param rect 片段绘制区域（宽字符片段跨格连续）。
     * @param style 片段首格样式（rendition 位 11-13 + underlineColor）。
     * @param fallbackColor underlineColor 为"跟随前景"时使用的颜色（片段实际文本色）。
     */
    void drawStyledUnderline(QPainter &painter, const QRect &rect, const Character *style,
                             const QColor &fallbackColor);
```

`TerminalDisplay.cpp` 在 `drawCharacters` 定义结束后（`:1090` 后）追加：

```cpp
void TerminalDisplay::drawStyledUnderline(QPainter &painter, const QRect &rect,
                                          const Character *style,
                                          const QColor &fallbackColor) {
    const QFontMetrics fm = painter.fontMetrics();
    // 几何与字体下划线同源：基线与文本绘制（rect.y()+_fontAscent+_lineSpacing）一致
    const qreal y0 = rect.y() + _fontAscent + _lineSpacing + fm.underlinePos();
    const qreal lineWidth = qMax(1, fm.lineWidth()); // 下限 1px
    const qreal left = rect.left();
    const qreal right = rect.right();
    // DEFAULT 下划线色回落片段实际文本色（含光标反色/选区交换后的颜色）
    const QColor lineColor = style->hasCustomUnderlineColor()
            ? style->underlineColor.color(_colorTable)
            : fallbackColor;

    painter.save();
    QPen pen(lineColor, lineWidth);
    pen.setCapStyle(Qt::FlatCap);
    switch (style->underlineStyle()) {
    case UNDERLINE_DOUBLE: {
        // 双线：第二条线置于首线下方，线间距 = 线宽（下限 1px）
        painter.setPen(pen);
        const qreal y2 = y0 + lineWidth + qMax<qreal>(1.0, lineWidth);
        painter.drawLine(QPointF(left, y0), QPointF(right, y0));
        painter.drawLine(QPointF(left, y2), QPointF(right, y2));
        break;
    }
    case UNDERLINE_CURLY: {
        /**
         * 波浪线：正弦波用逐半波二次贝塞尔段近似（quadTo 控制点取 ±2A 时波峰恰为 A）。
         * 波形参数（随字体度量，不设配置项）：
         *   振幅  A  = max(1px, fm.underlinePos() / 2)
         *   半波长 hλ = max(3px, fm.horizontalAdvance('~') / 4)
         * 起始相位向上（首半波波峰在 y0-A），末端截断到片段右缘。
         */
        const qreal amplitude = qMax<qreal>(1.0, fm.underlinePos() / 2.0);
        const qreal halfWave = qMax<qreal>(3.0, fm.horizontalAdvance(QLatin1Char('~')) / 4.0);
        painter.setRenderHint(QPainter::Antialiasing, true); // 仅波浪开抗锯齿
        painter.setPen(pen);
        QPainterPath path;
        path.moveTo(left, y0);
        bool up = true;
        for (qreal x = left; x < right; x += halfWave, up = !up) {
            const qreal xEnd = qMin(x + halfWave, right);
            path.quadTo(x + (xEnd - x) / 2.0, up ? y0 - 2 * amplitude : y0 + 2 * amplitude,
                        xEnd, y0);
        }
        painter.drawPath(path);
        break;
    }
    case UNDERLINE_DOTTED:
        pen.setStyle(Qt::DotStyle);
        painter.setPen(pen);
        painter.drawLine(QPointF(left, y0), QPointF(right, y0));
        break;
    case UNDERLINE_DASHED:
        pen.setStyle(Qt::DashStyle);
        painter.setPen(pen);
        painter.drawLine(QPointF(left, y0), QPointF(right, y0));
        break;
    default: // UNDERLINE_SINGLE + 独立下划线色：手绘单色实线（颜色脱离字体下划线）
        painter.setPen(pen);
        painter.drawLine(QPointF(left, y0), QPointF(right, y0));
        break;
    }
    painter.restore();
}
```

- [ ] **步骤 4：批次/Legacy 聚合条件补 underlineColor**

批次路径（`:2305-2307`）加局部量并在 while 条件（`:2313` 起）加比较：

```cpp
            CharacterColor currentForeground = _image[loc(x, y)].foregroundColor;
            CharacterColor currentBackground = _image[loc(x, y)].backgroundColor;
            CharacterColor currentUnderlineColor = _image[loc(x, y)].underlineColor; // 下划线色不同不得并入同一片段
            quint16 currentRendition = _image[loc(x, y)].rendition;
```

while 条件第一个合取项后插入一行：

```cpp
            while (x + len <= rlx &&
                        _image[loc(x + len, y)].foregroundColor == currentForeground &&
                        _image[loc(x + len, y)].backgroundColor == currentBackground &&
                        _image[loc(x + len, y)].underlineColor == currentUnderlineColor &&
                        _image[loc(x + len, y)].rendition == currentRendition &&
```

Legacy 路径同改（`:2459-2461` 局部量、`:2467` 后插比较行）。

注：样式位已在 rendition 整体比较内，天然分裂片段；underlineColor 是唯一新增维度。

- [ ] **步骤 5：构建 + 测试 + 全量回归**

```bash
cmake --build build --parallel
ctest --test-dir build -R tst_rendering --output-on-failure   # 预期：Passed
ctest --test-dir build --output-on-failure                    # 预期：9/9 全绿
```

已知调试点（先跑再调，不预设结论）：
- 若 `testStyledUnderlinePixels` 单线墨行数断言失败：offscreen 固定字体的 `fm.lineWidth()` 可能 >1，放宽到 `<= 3` 并相应校准波浪断言；
- 波浪 `underlinePos()/2` 在 underlinePos=1 时 A=1px，波谷仅低 1 行——断言用的是 `>` 非 `>=`，实测若字体退化（underlinePos=1、lineWidth=1）导致 `curly.last() == single.last()`，把起始相位截断误差计入后仍应成立；若不成立，将振幅公式上调为 `qMax(1.5, ...)` 并在注释中记录实测依据。

- [ ] **步骤 6：提交**

```bash
git add lib/src/display/TerminalDisplay.h lib/src/display/TerminalDisplay.cpp tests/tst_rendering.cpp
git commit -m "feat(display): 花样下划线与独立下划线色手绘绘制

- 默认单线无独立色保持 QFont underline 现状路径，零回归
- 双/点/虚走 QPen（penStyle 区分），波浪走 QPainterPath 正弦段
  （振幅 max(1px, underlinePos/2)，半波长 max(3px, '~'字宽/4)）
- y = baseline + underlinePos，线宽 max(1, lineWidth)，DEFAULT 色回落片段文本色
- drawCharacters 尾部单点插入，批次/Legacy 双路径一致；聚合条件补 underlineColor"
```

---

## 任务 4：文档收尾 + 全量验证

**文件：**
- 修改：`CHANGELOG`

- [ ] **步骤 1：CHANGELOG 条目**

在 `CHANGELOG` 文件顶部（`:1` 之前）插入：

```
ZzQTermWidget 花样下划线与独立下划线色（SGR 4:x / 58 / 59） / 2026-08-14
=============================================
 * CSI 逐参数前导分隔符记录（;/:），仅 SGR 消费：4;3（下划线+斜体）与 4:3（波浪）严格区分，
   其余 CSI 序列冒号维持拍平行为不变。
 * SGR 4:0/4:1..4:5 关/单/双/波浪/点/虚；样式存 rendition 位 11-13，RE_UNDERLINE 保留汇总位。
 * SGR 58 独立下划线色（58;5;n、58;2;r;g;b、58:5:n、58:2::r:g:b 容忍空位），SGR 59 复位；
   分号形式定长消费不吞后续独立 SGR，非法样式/参数不足整体忽略。
 * Character 16→20 字节（新增 underlineColor），operator==/!=/equalsFormat 全覆盖，
   样式/颜色变更格脏区比对天然捕获；DECSC/DECRC 随游标保存下划线色。
 * 绘制：默认单线无独立色保持 QFont underline 现状；其余手绘（双/点/虚 QPen，
   波浪 QPainterPath 正弦段），DEFAULT 色回落片段实际文本色；批次/Legacy 双路径像素一致。
 * 已知遗留：DECRQSS 不回报下划线样式状态（与上游多数终端一致）；波浪波形参数按字体
   度量经验取值，不做配置项。
```

- [ ] **步骤 2：全量验证（验收标准逐条核对）**

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

预期输出末行：`100% tests passed, 0 tests failed out of 9`。

验收核对清单：
- [ ] 9 套件全绿（含新增用例：tst_emulation +4、tst_rendering +3）
- [ ] `4;3`/`4:3` 区分专测（testSgrUnderlineSemicolonVsColon）绿
- [ ] Character 相等性纳入新字段专测（testCharacterUnderlineEquality）绿
- [ ] `lib/include/qtermwidget.h` 与 `lib/third_party/` 未动：`git diff --stat` 确认
- [ ] 新增/修改注释均为中文 Doxygen
- [ ] 抽查性能无意外劣化（可选）：`ctest --test-dir build -R tst_benchmark --output-on-failure`，与 `build/benchmark-baseline.txt` 同口径对比，劣化超噪声则记录原因

- [ ] **步骤 3：提交**

```bash
git add CHANGELOG
git commit -m "docs: CHANGELOG 记录花样下划线与独立下划线色（SGR 4:x / 58 / 59）"
```

---

## 自检清单（执行中逐任务核对）

- [ ] 标识符跨任务一致：`RE_UNDERLINE_STYLE_MASK` / `UNDERLINE_*` / `underlineColor` / `underlineStyle()` / `hasCustomUnderlineColor()` / `setUnderlineStyle` / `setUnderlineColor` / `argSeparators` / `drawStyledUnderline` —— 与"标识符表"逐字一致。
- [ ] 冒号 58 分支在分号 `58;2` 分支**之前**（顺序错误会被拍平误吞，测试 testSgrUnderlineColor 的冒号真彩行能抓到）。
- [ ] 批次与 Legacy 的聚合 while 条件**都**补了 `underlineColor` 比较（漏一边则像素等价测试抓到）。
- [ ] `displayCharacter` 三处落点全部补齐（漏宽字符填充格则宽字符片段右半下划线色错误）。
- [ ] `drawCharacters` 手绘调用在函数尾部、`styledUnderline` 门控在 `:945`；preedit 调用点（`:1895`）样式为默认值，不受影响。
- [ ] 每任务结束 9 套件全绿后才 commit。
