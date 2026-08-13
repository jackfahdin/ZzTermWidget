#include <QtTest>
#include <QImage>
#include <cstring>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "Screen.h"
#include "TerminalCharacterDecoder.h"

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
    void testOversizedToken();
    void testFullLineCopyKeepsNewLine();
    void testOsc8BasicLink();
    void testOsc8IdMergesSegments();
    void testOsc8ScrollbackKeepsLink();
    void testOsc8ClearLineDropsSegments();
    void testOsc8StTerminator();
    void testOsc8ClearLineResetsCurrentLink();
    void testSyncOutputModeSignalAndDecrqm();
    void testKittyPushQueryPop();
    void testKittySetModes();
    void testKittyStackLimit();
    void testKittyInvalidSequenceIgnored();
    void testKittyDisambiguateEncoding();
    void testKittyEventTypes();
    void testKittyReleaseIgnoredWhenDisabled();
    void testKittyUnhandledKeyReleaseSwallowed();
    void testSixelAnchorStoresAndMovesCursor();
    void testSixelScrollbackKeepsImage();
    void testSixelClearLineDestroysImage();
    void testSixelResetClearsImages();
    void testSixelAlternateScreenIndependent();
    void testSixelPixelBudgetDrops();
    void testSixelDcsAnchorsImage();
    void testSixelAnchorAtCursorPosition();
    void testSixelP2FillsBackground();
    void testSixelAbortOnCanSub();
    void testSixelTextContinuesBelowImage();
    void testSixelInvalidDataSilentlyDropped();
    void testSixelAbortOnEsc();
    void testSixelOversizedStreamDropped();
    void testNonSixelDcsUnaffected();
    void testCharacterUnderlineEquality();
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

/**
 * @brief 为仿真器设置 UTF-8 解码器与屏幕尺寸。
 * @note 显式 setCodec 与真实应用路径一致（默认解码器虽已为 UTF-8，仍保持显式设置）。
 */
static void initEmu(Vt102Emulation &emu, int lines, int columns)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(lines, columns);
}

void TestEmulation::testPlainText()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.receiveData("hello", 5);
    QVERIFY(firstLineText(emu, 80).startsWith(QStringLiteral("hello")));
}

void TestEmulation::testCsiCursorMove()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    // CSI 5 C：光标右移 5 列后写 X，X 应在第 6 列（索引 5）
    emu.receiveData("\033[5CX", 6);
    const QString line = firstLineText(emu, 80);
    QCOMPARE(line.indexOf(QLatin1Char('X')), 5);
}

void TestEmulation::testWideChar()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    // "中文" 两宽字符占 4 列，随后 'x' 在索引 4
    emu.receiveData("\xE4\xB8\xAD\xE6\x96\x87x", 7);
    const QString line = firstLineText(emu, 80);
    QCOMPARE(line.indexOf(QLatin1Char('x')), 4);
}

void TestEmulation::testEmojiSurrogatePair()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    // "a😀b"：😀(U+1F600) UTF-8 = F0 9F 98 80，占 2 列；b 应在索引 3
    // 改造前（代理对拆分）b 会落在错误位置或 emoji 显示为乱码
    // 注意：必须用字面量拼接终止 \x80 的十六进制转义，否则 'b' 会被吞进转义序列
    emu.receiveData("a\xF0\x9F\x98\x80" "b", 6);
    const QString line = firstLineText(emu, 80);
    const char32_t emoji = U'😀';
    QVERIFY(line.contains(QString::fromUcs4(&emoji, 1)));
    QCOMPARE(line.indexOf(QLatin1Char('b')), 3);
}

/**
 * @brief 超长 OSC token：解析器应丢弃该序列并恢复，不崩溃不越界。
 * @note 超长窗口标题等场景曾导致 tokenBuffer 静默截断，产生错误语义。
 */
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

/**
 * @brief 满行复制回归：恰好写满整行的文本经复制路径导出时，行尾换行不得被丢弃。
 * @note 走 Screen::writeLinesToStream → copyLineToStream 真实路径（PlainTextDecoder）。
 *       80 个字符写满第 0 行后以 "\r\n" 显式换行，行不会被标记 LINE_WRAPPED，
 *       因此 preserveLineBreaks 下应输出换行；修复前 count + 1 < worstCase 恒假导致
 *       '\n' 被静默丢弃，两行文本粘连（边界 off-by-one）。
 */
void TestEmulation::testFullLineCopyKeepsNewLine()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    QByteArray firstLine(80, 'A');
    firstLine.append("\r\n");
    emu.receiveData(firstLine.constData(), firstLine.size());
    emu.receiveData("next", 4);

    QString output;
    QTextStream stream(&output);
    PlainTextDecoder decoder;
    decoder.begin(&stream);
    emu.createWindow()->screen()->writeLinesToStream(&decoder, 0, 1);
    decoder.end();

    // 末尾 '\n' 来自 writeToStream 的“选区超出末行末尾则补换行”既有逻辑，与本次修复无关
    QCOMPARE(output, QString(80, QLatin1Char('A')) + QStringLiteral("\nnext\n"));
}

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

/**
 * @brief OSC 8 以 ST（ESC \\）终止：URI 无残留字符，链接段正确。
 */
void TestEmulation::testOsc8StTerminator()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const char *seq = "\033]8;;https://example.com\033\\link\033]8;;\033\\ tail";
    emu.receiveData(seq, int(std::strlen(seq)));
    Screen *scr = emu.createWindow()->screen();
    QCOMPARE(scr->hyperlinkAt(0, 0), QStringLiteral("https://example.com"));
    QCOMPARE(scr->hyperlinkAt(0, 3), QStringLiteral("https://example.com"));
    QVERIFY(scr->hyperlinkAt(0, 4).isEmpty()); // 空 URI（ST 终止）已结束链接
    const auto segs = scr->linkSegments(0);
    QCOMPARE(segs.size(), 1);
    QCOMPARE(segs[0].startCol, 0);
    QCOMPARE(segs[0].endCol, 3);
}

/**
 * @brief 清行回收活动链接后复位 _currentHyperlinkId：未重发 OSC 8 继续写入的
 *        字符不得携带已失效的 linkId（zombie 链接回归）。
 * @note 复现序列：开启链接写 "ab"（未发空 URI 关闭）→ CSI 2 K 清行使引用计数归零
 *       → 继续写 "cd"。修复前 "cd" 的段指向已回收的 URI，链接静默丢失。
 */
void TestEmulation::testOsc8ClearLineResetsCurrentLink()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const char *seq = "\033]8;;https://a.com\007ab\033[2Kcd";
    emu.receiveData(seq, int(std::strlen(seq)));
    Screen *scr = emu.createWindow()->screen();
    // 清行后活动链接已随段表回收并复位，"cd" 不产生链接段
    QVERIFY(scr->linkSegments(0).isEmpty());
    QVERIFY(scr->hyperlinkAt(0, 2).isEmpty());
    QVERIFY(scr->hyperlinkAt(0, 3).isEmpty());
    // 链接开启状态已复位：重发同一 URI 可正常建立新链接
    const char *seq2 = "\033]8;;https://a.com\007ef";
    emu.receiveData(seq2, int(std::strlen(seq2)));
    QCOMPARE(scr->hyperlinkAt(0, 4), QStringLiteral("https://a.com"));
}

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

/**
 * @brief kitty 生效时未覆盖功能键（codepoint==0）的释放事件吞掉：
 *        按下回落传统编码，释放不重发按下序列（回归：方向键释放双发 \033[A）。
 */
void TestEmulation::testKittyUnhandledKeyReleaseSwallowed()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setKeyBindings(QString());
    QByteArray sent;
    QObject::connect(&emu, &Emulation::sendData,
                     [&](const char *d, int len) { sent.append(d, len); });

    emu.receiveData("\033[>1u", 5); // push 级别 1（消歧义）

    QKeyEvent upPress = makeKeyEvent(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    emu.sendKeyEvent(&upPress, false);
    QCOMPARE(sent, QByteArray("\033[A")); // 按下：回落传统编码
    sent.clear();

    QKeyEvent upRelease = makeKeyEvent(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier);
    emu.sendKeyEvent(&upRelease, false);
    QCOMPARE(sent, QByteArray()); // 释放：吞掉，不双发 \033[A
    sent.clear();

    QKeyEvent f5Release = makeKeyEvent(QEvent::KeyRelease, Qt::Key_F5, Qt::NoModifier);
    emu.sendKeyEvent(&f5Release, false);
    QCOMPARE(sent, QByteArray()); // F5 释放同样吞掉
}

/**
 * @brief 构造纯色 ARGB32 测试图。
 */
static QImage solidImage(const QSize &size, const QColor &color)
{
    QImage img(size, QImage::Format_ARGB32);
    img.fill(color);
    return img;
}

/**
 * @brief 锚定：行级引用逐行放置（含行偏移），文本光标移到图像最后一行之下。
 */
void TestEmulation::testSixelAnchorStoresAndMovesCursor()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    Screen *scr = emu.createWindow()->screen();
    scr->anchorImage(solidImage({16, 33}, Qt::red), true); // 33px 高 / 16px 行高 → 3 网格行
    const auto row0 = scr->imagePlacements(0);
    QCOMPARE(row0.size(), 1);
    QCOMPARE(row0[0].startCol, 0);
    QCOMPARE(row0[0].rowOffset, 0);
    QCOMPARE(scr->imagePlacements(1)[0].rowOffset, 1);
    QCOMPARE(scr->imagePlacements(2)[0].rowOffset, 2);
    QVERIFY(scr->imagePlacements(3).isEmpty());
    QCOMPARE(scr->getCursorY(), 3); // 光标在图最后一行之下
    const ScreenImage *stored = scr->image(row0[0].imageId);
    QVERIFY(stored != nullptr);
    QCOMPARE(stored->image.size(), QSize(16, 33));
    QVERIFY(stored->transparentBackground);
    QVERIFY(scr->graphicsDirty()); // 通知显示层补刷
}

/**
 * @brief 锚定触底触发滚动：图像引用随行走入历史，绝对行坐标下保持完整。
 */
void TestEmulation::testSixelScrollbackKeepsImage()
{
    Vt102Emulation emu;
    initEmu(emu, 3, 80);
    emu.setCellPixelSize(8, 16);
    emu.setHistory(HistoryTypeBuffer(100));
    Screen *scr = emu.createWindow()->screen();
    scr->anchorImage(solidImage({16, 48}, Qt::red), false); // 恰 3 网格行，第三次 index() 触底滚动
    QVERIFY(scr->getHistLines() >= 1);
    QCOMPARE(scr->imagePlacements(0).size(), 1); // 绝对行 0 = 最早历史行
    QCOMPARE(scr->imagePlacements(0)[0].rowOffset, 0);
    QCOMPARE(scr->imagePlacements(1)[0].rowOffset, 1);
    QCOMPARE(scr->imagePlacements(2)[0].rowOffset, 2);
    QCOMPARE(scr->getCursorY(), 2); // 光标停留末行
}

/**
 * @brief 清行（CSI 2 K）销毁该行图像引用，计数归零后像素数据回收。
 */
void TestEmulation::testSixelClearLineDestroysImage()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    Screen *scr = emu.createWindow()->screen();
    scr->anchorImage(solidImage({16, 16}, Qt::red), false); // 1 网格行
    const quint32 id = scr->imagePlacements(0)[0].imageId;
    QVERIFY(scr->image(id) != nullptr);
    emu.receiveData("\033[H\033[2K", 7); // 回第 0 行清整行
    QVERIFY(scr->imagePlacements(0).isEmpty());
    QVERIFY(scr->image(id) == nullptr);
}

/**
 * @brief RIS（ESC c）复位：全部图像与引用清空。
 */
void TestEmulation::testSixelResetClearsImages()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    Screen *scr = emu.createWindow()->screen();
    scr->anchorImage(solidImage({16, 16}, Qt::red), false);
    emu.receiveData("\033c", 2);
    QVERIFY(scr->imagePlacements(0).isEmpty());
}

/**
 * @brief 主/备屏图像各自独立：切备选屏后无图，切回主屏图像仍在。
 */
void TestEmulation::testSixelAlternateScreenIndependent()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    Screen *mainScr = emu.createWindow()->screen();
    mainScr->anchorImage(solidImage({16, 16}, Qt::red), false);
    emu.receiveData("\033[?1049h", 8); // 切备选屏
    Screen *altScr = emu.createWindow()->screen();
    QVERIFY(altScr->imagePlacements(0).isEmpty());
    emu.receiveData("\033[?1049l", 8); // 切回主屏
    QCOMPARE(mainScr->imagePlacements(0).size(), 1);
}

/**
 * @brief 累计像素预算 256MB：4 张 4096x4096（各 64MB）共存，第 5 张超限静默丢弃。
 */
void TestEmulation::testSixelPixelBudgetDrops()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 4096); // 每张 4096px 高恰占 1 网格行，4 张同屏存活
    Screen *scr = emu.createWindow()->screen();
    const QImage big = solidImage({4096, 4096}, Qt::red); // 隐式共享，实际内存仅一份
    for (int i = 0; i < 4; i++)
        scr->anchorImage(big, false);
    for (int i = 0; i < 4; i++)
        QCOMPARE(scr->imagePlacements(i).size(), 1);
    scr->anchorImage(big, false); // 第 5 张：超 256MB 预算丢弃，光标不下移
    QVERIFY(scr->imagePlacements(4).isEmpty());
    QCOMPARE(scr->getCursorY(), 4);
}

/**
 * @brief 构造最小 sixel 流：1x1 纯红图（#0 定义红色，'@' 写 1 像素）。
 */
static QByteArray minimalSixelStream()
{
    return QByteArray("\033Pq#0;2;100;0;0#0@\033\\");
}

/**
 * @brief DCS q 分流：sixel 流解码锚定到当前光标位，文本光标移到图下。
 */
void TestEmulation::testSixelDcsAnchorsImage()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    const QByteArray seq = minimalSixelStream();
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = emu.createWindow()->screen();
    const auto placements = scr->imagePlacements(0);
    QCOMPARE(placements.size(), 1);
    QCOMPARE(placements[0].startCol, 0);
    const ScreenImage *img = scr->image(placements[0].imageId);
    QVERIFY(img != nullptr);
    QCOMPARE(img->image.size(), QSize(1, 1));
    QCOMPARE(img->image.pixelColor(0, 0), QColor(255, 0, 0));
    QCOMPARE(scr->getCursorY(), 1); // 光标移到图像之下
}

/**
 * @brief 锚定位置跟随文本光标：CSI 定位后锚定到光标所在行列。
 * @note 本用例的 sixel 流为 `\033Pq`（无 DCS 参数），P2 填底语义见 testSixelP2FillsBackground()。
 */
void TestEmulation::testSixelAnchorAtCursorPosition()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    // CSI 5;10 H 定位后锚定：应在第 5 行（索引 4）第 10 列（索引 9）
    const QByteArray seq = QByteArray("\033[5;10H") + minimalSixelStream();
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = emu.createWindow()->screen();
    const auto placements = scr->imagePlacements(4);
    QCOMPARE(placements.size(), 1);
    QCOMPARE(placements[0].startCol, 9);
    QCOMPARE(scr->getCursorY(), 5);
}

/**
 * @brief DCS 参数 P2=2：非透明底，未着色像素以 0 号色填底。
 * @note P2 是 DCS 头部的第 2 个参数（`\033P1;2q`），覆盖 parts[1].toInt() 解析分支；
 *       `\033P2q` 的 "2" 是 P1（宽高比，忽略），P2 缺省仍为透明底——一并钉死该语义。
 */
void TestEmulation::testSixelP2FillsBackground()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    // 2x6 图：'@' 只写 (0,0)，'~' 写满第 1 列；(0,1..5) 未着色，P2=2 时应被 0 号红色填底
    const QByteArray seq = QByteArray("\033P1;2q#0;2;100;0;0#0@~\033\\") // P2=2：填底
                           + QByteArray("\033Pq#0;2;100;0;0#0@~\033\\")   // 无 P2：透明底对照
                           + QByteArray("\033P2q#0;2;100;0;0#0@~\033\\"); // 仅 P1=2：仍透明底
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = emu.createWindow()->screen();
    const auto fillPlacements = scr->imagePlacements(0);
    QCOMPARE(fillPlacements.size(), 1);
    const ScreenImage *fillImg = scr->image(fillPlacements[0].imageId);
    QVERIFY(fillImg != nullptr);
    QCOMPARE(fillImg->image.size(), QSize(2, 6));
    QCOMPARE(fillImg->transparentBackground, false);
    QCOMPARE(fillImg->image.pixelColor(0, 5), QColor(255, 0, 0)); // 未着色区填入 0 号色
    for (int row = 1; row <= 2; row++) { // 两个透明底对照组
        const auto placements = scr->imagePlacements(row);
        QCOMPARE(placements.size(), 1);
        const ScreenImage *img = scr->image(placements[0].imageId);
        QVERIFY(img != nullptr);
        QCOMPARE(img->transparentBackground, true);
        QCOMPARE(img->image.pixelColor(0, 5).alpha(), 0); // 未着色区保持透明
    }
}

/**
 * @brief 数据段中途 CAN(0x18)/SUB(0x1A)：中止该图不锚定，后续 sixel 流照常解析锚定。
 */
void TestEmulation::testSixelAbortOnCanSub()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    // 第 0 行：CAN 中止第一张图，随后完整流正常锚定
    const QByteArray canSeq = QByteArray("\033Pq#0;2;100;0;0#0@@\x18") + minimalSixelStream();
    emu.receiveData(canSeq.constData(), int(canSeq.size()));
    Screen *scr = emu.createWindow()->screen();
    const auto canPlacements = scr->imagePlacements(0);
    QCOMPARE(canPlacements.size(), 1); // 仅后续完整流的图
    const ScreenImage *canImg = scr->image(canPlacements[0].imageId);
    QVERIFY(canImg != nullptr);
    QCOMPARE(canImg->image.pixelColor(0, 0), QColor(255, 0, 0)); // 后续流解码正确
    QCOMPARE(scr->getCursorY(), 1);
    // 第 1 行：SUB 中止，同样语义
    const QByteArray subSeq = QByteArray("\033Pq#0;2;100;0;0#0@@\x1a") + minimalSixelStream();
    emu.receiveData(subSeq.constData(), int(subSeq.size()));
    QCOMPARE(scr->imagePlacements(1).size(), 1);
    QCOMPARE(scr->getCursorY(), 2);
}

/**
 * @brief 图像之后的文本从图下方继续输出。
 */
void TestEmulation::testSixelTextContinuesBelowImage()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    // 光栅声明 1x33（3 网格行）后输出文字：落在第 4 行（索引 3）
    const QByteArray seq = QByteArray("\033Pq\"1;1;1;33#0;2;100;0;0#0@\033\\") + "after";
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = emu.createWindow()->screen();
    QCOMPARE(scr->getCursorY(), 3);
    QCOMPARE(scr->getCursorX(), 5);
    QVERIFY(scr->getScreenText(3, 0, 3, 4, 1).startsWith(QStringLiteral("after")));
}

/**
 * @brief 空数据段解码失败：静默丢弃，后续文本不受影响。
 */
void TestEmulation::testSixelInvalidDataSilentlyDropped()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const QByteArray seq = QByteArray("\033Pq\033\\") + "OK";
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = emu.createWindow()->screen();
    QVERIFY(scr->imagePlacements(0).isEmpty());
    QVERIFY(firstLineText(emu, 80).startsWith(QStringLiteral("OK")));
}

/**
 * @brief 数据段中途 ESC（非 ST）：中止该图；ESC 起始的后续序列照常解析。
 */
void TestEmulation::testSixelAbortOnEsc()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    const QByteArray seq = QByteArray("\033Pq#0;2;100;0;0#0@@@\033[2JOK");
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = emu.createWindow()->screen();
    for (int i = 0; i < 3; i++)
        QVERIFY(scr->imagePlacements(i).isEmpty());
    QVERIFY(firstLineText(emu, 80).startsWith(QStringLiteral("OK")));
}

/**
 * @brief 数据流超 32MB 上限：吞到 ST 丢弃，后续字节流正常。
 */
void TestEmulation::testSixelOversizedStreamDropped()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    emu.setCellPixelSize(8, 16);
    QByteArray seq = "\033Pq";
    seq.append(QByteArray(33 * 1024 * 1024, '~')); // 超 32MB 上限
    seq.append("\033\\OK");
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = emu.createWindow()->screen();
    QVERIFY(scr->imagePlacements(0).isEmpty());
    QVERIFY(firstLineText(emu, 80).startsWith(QStringLiteral("OK")));
}

/**
 * @brief 非 sixel DCS（DECRQSS 形态 DCS $ q）：不进入 sixel 通道，整条吞吃，后续正常。
 */
void TestEmulation::testNonSixelDcsUnaffected()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const QByteArray seq = QByteArray("\033P$qm\033\\OK");
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = emu.createWindow()->screen();
    QVERIFY(scr->imagePlacements(0).isEmpty());
    QVERIFY(firstLineText(emu, 80).startsWith(QStringLiteral("OK")));
}

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

QTEST_GUILESS_MAIN(TestEmulation)
#include "tst_emulation.moc"
