#include <QtTest>
#include "History.h"
#include "Character.h"

/**
 * @brief HistoryScrollFile（文件回滚缓冲）的基本行为回归测试。
 */
class TestHistory : public QObject
{
    Q_OBJECT
private slots:
    void testAddAndGet();
    void testWrappedFlag();
};

/**
 * @brief 写入一行字符后可按行号/列号原样取回（char32_t 码点不丢失）。
 */
void TestHistory::testAddAndGet()
{
    HistoryScrollFile file{QString()};
    QVector<Character> line = { Character(U'h'), Character(U'i') };
    file.addCellsVector(line);
    file.addLine(false);
    QCOMPARE(file.getLines(), 1);
    QVector<Character> out(2);
    file.getCells(0, 0, 2, out.data());
    QCOMPARE(out[0].character, char32_t(U'h'));
    QCOMPARE(out[1].character, char32_t(U'i'));
}

/**
 * @brief addLine 的 previousWrapped 标志可通过 isWrappedLine 正确读回。
 */
void TestHistory::testWrappedFlag()
{
    HistoryScrollFile file{QString()};
    QVector<Character> line = { Character(U'x') };
    file.addCellsVector(line);
    file.addLine(true);
    QVERIFY(file.isWrappedLine(0));
}

QTEST_APPLESS_MAIN(TestHistory)
#include "tst_history.moc"
