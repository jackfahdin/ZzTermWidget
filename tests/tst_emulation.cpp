#include <QtTest>
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

QTEST_GUILESS_MAIN(TestEmulation)
#include "tst_emulation.moc"
