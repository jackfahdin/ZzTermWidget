#include <QtTest>
#include "History.h"
#include "Character.h"

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

QTEST_MAIN(TestHistoryReadback)
#include "tst_historyreadback.moc"
