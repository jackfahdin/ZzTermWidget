#include <QtTest>
#include "History.h"
#include "Character.h"
#include "Screen.h"
#include "TerminalCharacterDecoder.h"

/**
 * @brief 历史读回注入（前插）的缓冲层回归测试。
 * @note 前插语义：外部提供者把更老的历史行注入滚动缓冲头部（旧→新顺序）；
 *       前插区独立于环形区，容量上限同为 _maxLineCount（内存有界 ≤2×）。
 */
class TestHistoryReadback : public QObject
{
    Q_OBJECT
private slots:
    void testPrependBufferBasic();
    void testPrependUnsupportedScrollTypes();
    void testPrependCapacityCap();
    void testAppendAfterPrependKeepsOrder();
    void testScreenPrependReadbackOrder();
    void testHistoryBaseLine();
    void testPrependKeepsHyperlinkAlignment();
};

/**
 * @brief 前插后行序、行长、折行标志、单元格内容均可按新行号原样读回。
 */
void TestHistoryReadback::testPrependBufferBasic()
{
    HistoryScrollBuffer buf(100);
    buf.addCellsVector({ Character(U'a') });
    buf.addLine(false);
    buf.addCellsVector({ Character(U'b') });
    buf.addLine(true);
    QCOMPARE(buf.getLines(), 2);

    const QVector<QVector<Character>> older = {
        { Character(U'x'), Character(U'y') },
        { Character(U'z') }
    };
    const QVector<bool> wrapped = { true, false };
    QCOMPARE(buf.prependLines(older, wrapped), 2);

    QCOMPARE(buf.getLines(), 4);
    QCOMPARE(buf.getLineLen(0), 2);
    QCOMPARE(buf.getLineLen(1), 1);
    QVERIFY(buf.isWrappedLine(0));
    QVERIFY(!buf.isWrappedLine(1));
    QVERIFY(!buf.isWrappedLine(2));
    QVERIFY(buf.isWrappedLine(3)); // 原环形区第 2 行（'b'）的折行标志随索引平移

    QVector<Character> out(2);
    buf.getCells(0, 0, 2, out.data());
    QCOMPARE(out[0].character, char32_t(U'x'));
    QCOMPARE(out[1].character, char32_t(U'y'));
    buf.getCells(3, 0, 1, out.data());
    QCOMPARE(out[0].character, char32_t(U'b')); // 环形区内容不受前插影响
}

/**
 * @brief 无历史/文件历史不支持前插，返回 0（文件型为无限历史，无需读回）。
 */
void TestHistoryReadback::testPrependUnsupportedScrollTypes()
{
    HistoryScrollNone none;
    HistoryScrollFile file{QString()};
    const QVector<QVector<Character>> lines = { { Character(U'x') } };
    const QVector<bool> wrapped = { false };
    QCOMPARE(none.prependLines(lines, wrapped), 0);
    QCOMPARE(file.prependLines(lines, wrapped), 0);
    QCOMPARE(file.getLines(), 0); // 不支持即无副作用
}

/**
 * @brief 前插区容量独立于环形区、上限相同；超量输入保留较新的行（无索引空洞）。
 */
void TestHistoryReadback::testPrependCapacityCap()
{
    HistoryScrollBuffer buf(10);
    for (int i = 0; i < 10; i++) {
        buf.addCellsVector({ Character(char32_t(U'a' + i)) });
        buf.addLine(false);
    }
    QCOMPARE(buf.getLines(), 10); // 环形区已满

    QVector<QVector<Character>> older;
    QVector<bool> wrapped;
    for (int i = 0; i < 15; i++) {
        older.append({ Character(char32_t(U'A' + i)) });
        wrapped.append(false);
    }
    QCOMPARE(buf.prependLines(older, wrapped), 10); // 前插区上限 10，最老的 'A'..'E' 丢弃
    QCOMPARE(buf.getLines(), 20);

    QVector<Character> out(1);
    buf.getCells(0, 0, 1, out.data());
    QCOMPARE(out[0].character, char32_t(U'F'));  // 保留输入中较新的 10 行
    buf.getCells(9, 0, 1, out.data());
    QCOMPARE(out[0].character, char32_t(U'O'));
    buf.getCells(10, 0, 1, out.data());
    QCOMPARE(out[0].character, char32_t(U'a'));  // 环形区首行未受影响
}

/**
 * @brief 前插区存在时继续尾部追加：环形区满员覆盖的是环形区最老行，行序保持连续。
 */
void TestHistoryReadback::testAppendAfterPrependKeepsOrder()
{
    HistoryScrollBuffer buf(3);
    for (char32_t c : { U'a', U'b', U'c' }) {
        buf.addCellsVector({ Character(c) });
        buf.addLine(false);
    }
    const QVector<QVector<Character>> older = { { Character(U'X') }, { Character(U'Y') } };
    const QVector<bool> wrapped = { false, false };
    QCOMPARE(buf.prependLines(older, wrapped), 2);
    QCOMPARE(buf.getLines(), 5);

    buf.addCellsVector({ Character(U'd') }); // 环形区满员，覆盖环形区最老行 'a'
    buf.addLine(false);

    QCOMPARE(buf.getLines(), 5);
    const char32_t expected[5] = { U'X', U'Y', U'b', U'c', U'd' };
    for (int i = 0; i < 5; i++) {
        QVector<Character> out(1);
        buf.getCells(i, 0, 1, out.data());
        QCOMPARE(out[0].character, expected[i]);
    }
}

/**
 * @brief 向 Screen 喂数据产生历史行前插更老的行经解码流按 旧→新 顺序读回。
 */
void TestHistoryReadback::testScreenPrependReadbackOrder()
{
    Screen screen(2, 80);
    screen.setScroll(HistoryTypeBuffer(100));
    // LNM 模式使 newLine 兼作回车（newLine 本体仅是换行，不回列），
    // 保证逐行喂入的文本都从第 0 列开始
    screen.setMode(MODE_NewLine);
    // 喂 5 行：L0..L3 滚入历史，L4 留在屏幕
    for (int i = 0; i < 5; i++) {
        for (const char32_t c : QString("L%1").arg(i).toUcs4())
            screen.displayCharacter(c);
        screen.newLine();
    }
    QCOMPARE(screen.getHistLines(), 4);

    const QVector<QVector<Character>> older = {
        { Character(U'o'), Character(U'1') },
        { Character(U'o'), Character(U'2') }
    };
    const QVector<bool> wrapped = { false, false };
    QCOMPARE(screen.prependHistoryLines(older, wrapped), 2);
    QCOMPARE(screen.getHistLines(), 6);

    PlainTextDecoder decoder;
    QString text;
    QTextStream stream(&text);
    decoder.begin(&stream);
    screen.writeLinesToStream(&decoder, 0, screen.getHistLines() - 1);
    decoder.end();
    const QStringList outLines = text.split(QLatin1Char('\n'));
    QCOMPARE(outLines.size(), 7); // 6 行 + 末行后补换行产生的空串
    QCOMPARE(outLines[0], QStringLiteral("o1"));
    QCOMPARE(outLines[1], QStringLiteral("o2"));
    QCOMPARE(outLines[2], QStringLiteral("L0"));
    QCOMPARE(outLines[5], QStringLiteral("L3"));
}

/**
 * @brief historyBaseLine 口径：满员丢行后 base 前进，前插注入后 base 等量回退。
 */
void TestHistoryReadback::testHistoryBaseLine()
{
    Screen screen(2, 80);
    screen.setScroll(HistoryTypeBuffer(10));
    // LNM 模式使 newLine 兼作回车（newLine 本体仅是换行，不回列），
    // 保证逐行喂入的文本都从第 0 列开始
    screen.setMode(MODE_NewLine);
    // 喂 15 行：14 次滚入历史（首行 newLine 时光标未触底不滚动），环形区封顶 10
    for (int i = 0; i < 15; i++) {
        for (const char32_t c : QString("L%1").arg(i).toUcs4())
            screen.displayCharacter(c);
        screen.newLine();
    }
    QCOMPARE(screen.getHistLines(), 10);
    QCOMPARE(screen.historyBaseLine(), qint64(14 - 10)); // 4 行已离开内存

    const QVector<QVector<Character>> older = {
        { Character(U'p') }, { Character(U'q') }
    };
    const QVector<bool> wrapped = { false, false };
    QCOMPARE(screen.prependHistoryLines(older, wrapped), 2);
    QCOMPARE(screen.getHistLines(), 12);
    QCOMPARE(screen.historyBaseLine(), qint64(2)); // base 回退 2
}

/**
 * @brief 前插后 OSC 8 链接段平行表同步平移：原历史行的链接在新索引处可查，
 *        前插入的空平行行不产生链接。
 */
void TestHistoryReadback::testPrependKeepsHyperlinkAlignment()
{
    Screen screen(2, 80);
    screen.setScroll(HistoryTypeBuffer(100));
    // LNM 模式使 newLine 兼作回车（newLine 本体仅是换行，不回列），
    // 保证逐行喂入的文本都从第 0 列开始
    screen.setMode(MODE_NewLine);
    screen.setCurrentHyperlink(QStringLiteral("https://example.com"), QString());
    for (const char32_t c : QStringLiteral("L0").toUcs4())
        screen.displayCharacter(c);
    screen.setCurrentHyperlink(QString(), QString());
    screen.newLine();
    for (const char32_t c : QStringLiteral("L1").toUcs4())
        screen.displayCharacter(c);
    screen.newLine(); // L0 滚入历史行 0
    QCOMPARE(screen.getHistLines(), 1);
    QCOMPARE(screen.hyperlinkAt(0, 0), QStringLiteral("https://example.com"));

    const QVector<QVector<Character>> older = {
        { Character(U'a') }, { Character(U'b') }, { Character(U'c') }
    };
    const QVector<bool> wrapped = { false, false, false };
    QCOMPARE(screen.prependHistoryLines(older, wrapped), 3);
    QCOMPARE(screen.getHistLines(), 4);

    // 原历史行 0 的链接随索引平移到行 3；前插入的空行无链接
    QCOMPARE(screen.hyperlinkAt(3, 0), QStringLiteral("https://example.com"));
    QVERIFY(screen.hyperlinkAt(0, 0).isEmpty());
}

QTEST_MAIN(TestHistoryReadback)
#include "tst_historyreadback.moc"
