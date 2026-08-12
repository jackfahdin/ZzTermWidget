#include <QtTest>
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

QTEST_GUILESS_MAIN(TestEmulation)
#include "tst_emulation.moc"
