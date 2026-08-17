#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScrollBar>
#include <QSysInfo>
#include "qtermwidget.h"
#include "History.h"
#include "Character.h"
#include "Screen.h"
#include "TerminalCharacterDecoder.h"
#include "Vt102Emulation.h"

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
    void testPrependDropPopsParallelTableAtRightIndex();
    void testShrinkAfterReadbackSyncsParallelTables();
    void testPrependWrappedFlagsLengthMismatch();
    void testEmulationPrependForward();
    void testWidgetFetchOlderOnScrollTop();
    void testProviderEmptyMarksExhausted();
    void testPrepend100kLinesPerf();
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

/**
 * @brief 前插区非空时环形区满员丢行：平行表须在被丢行的整体索引处弹出，
 *        被丢行的链接归属不得平移错配到前插行上（pop_front 弹错位置的回归）。
 */
void TestHistoryReadback::testPrependDropPopsParallelTableAtRightIndex()
{
    Screen screen(2, 80);
    screen.setScroll(HistoryTypeBuffer(4));
    // LNM 模式使 newLine 兼作回车（newLine 本体仅是换行，不回列），
    // 保证逐行喂入的文本都从第 0 列开始
    screen.setMode(MODE_NewLine);

    // H0 带链接，L1..L4 普通；H0..L3 滚入环形区（4 行满），L4 留屏
    screen.setCurrentHyperlink(QStringLiteral("https://example.com"), QString());
    for (const char32_t c : QStringLiteral("H0").toUcs4())
        screen.displayCharacter(c);
    screen.setCurrentHyperlink(QString(), QString());
    screen.newLine();
    for (int i = 1; i <= 4; i++) {
        for (const char32_t c : QString("L%1").arg(i).toUcs4())
            screen.displayCharacter(c);
        screen.newLine();
    }
    QCOMPARE(screen.getHistLines(), 4);
    QCOMPARE(screen.hyperlinkAt(0, 0), QStringLiteral("https://example.com"));

    // 前插 2 行：H0 链接行整体上移到索引 2
    const QVector<QVector<Character>> older = {
        { Character(U'p') }, { Character(U'q') }
    };
    const QVector<bool> wrapped = { false, false };
    QCOMPARE(screen.prependHistoryLines(older, wrapped), 2);
    QCOMPARE(screen.getHistLines(), 6);
    QCOMPARE(screen.hyperlinkAt(2, 0), QStringLiteral("https://example.com"));

    // 再喂一行：环形区满员覆盖最老环形行 H0（整体索引 2），L4 滚入历史
    for (const char32_t c : QStringLiteral("L5").toUcs4())
        screen.displayCharacter(c);
    screen.newLine();
    QCOMPARE(screen.getHistLines(), 6);

    // 被丢行的链接必须随之销毁：任何历史行都不得再查到该 URI
    // （若平行表 pop_front 弹错位置，链接会错配到前插行 q 所在的索引 1）
    for (int i = 0; i < screen.getHistLines(); i++)
        QVERIFY2(screen.hyperlinkAt(i, 0).isEmpty(),
                 qPrintable(QStringLiteral("history line %1 仍持有已丢弃行的链接").arg(i)));
}

/**
 * @brief 读回后运行时缩容（setScroll copyPreviousScroll → setMaxNbLines）：
 *        前插区被弹出的行必须从三张平行表前端同步弹出（带引用释放）、
 *        绝对行号基线随前端丢弃量前进、尾部被环形区截断的行从平行表尾端弹出；
 *        缩容前不弹表则链接归属错配、基线停滞导致提供者重复回传（回归）。
 */
void TestHistoryReadback::testShrinkAfterReadbackSyncsParallelTables()
{
    Screen screen(2, 80);
    screen.setScroll(HistoryTypeBuffer(10));
    // LNM 模式使 newLine 兼作回车（newLine 本体仅是换行，不回列），
    // 保证逐行喂入的文本都从第 0 列开始
    screen.setMode(MODE_NewLine);

    // 喂 15 行：L0..L13 滚入历史（环形区封顶 10，保留 L4..L13），L14 留屏；
    // L4（缩容后幸存的最老环形行）带链接 A，L13（缩容时被尾部截断）带链接 B
    const QString uriA = QStringLiteral("https://example.com/a");
    const QString uriB = QStringLiteral("https://example.com/b");
    for (int i = 0; i < 15; i++) {
        if (i == 4)
            screen.setCurrentHyperlink(uriA, QString());
        else if (i == 13)
            screen.setCurrentHyperlink(uriB, QString());
        for (const char32_t c : QString("L%1").arg(i).toUcs4())
            screen.displayCharacter(c);
        screen.setCurrentHyperlink(QString(), QString());
        screen.newLine();
    }
    QCOMPARE(screen.getHistLines(), 10);
    QCOMPARE(screen.historyBaseLine(), qint64(4));
    QCOMPARE(screen.hyperlinkAt(0, 0), uriA); // L4 位于历史行 0
    QCOMPARE(screen.hyperlinkAt(9, 0), uriB); // L13 位于历史行 9

    // 读回前插 2 行（绝对行号 2、3）：基线回退，链接行索引平移
    const QVector<QVector<Character>> older = {
        { Character(U'p') }, { Character(U'q') }
    };
    const QVector<bool> wrapped = { false, false };
    QCOMPARE(screen.prependHistoryLines(older, wrapped), 2);
    QCOMPARE(screen.getHistLines(), 12);
    QCOMPARE(screen.historyBaseLine(), qint64(2));
    QCOMPARE(screen.hyperlinkAt(2, 0), uriA);
    QCOMPARE(screen.hyperlinkAt(11, 0), uriB);

    // 运行时缩容到 1 行：前插区弹最老的 p（前端丢弃 1 行），
    // 环形区截断到最老的 L4（L5..L13 从尾部离开）；幸存历史为 [q, L4]
    screen.setScroll(HistoryTypeBuffer(1));
    QCOMPARE(screen.getHistLines(), 2);
    // 基线只随前端丢弃量（p，1 行）前进；尾部截断不影响最老在内存行的绝对行号
    QCOMPARE(screen.historyBaseLine(), qint64(3));
    // 平行表同步弹出后：q 行（索引 0）无链接，L4 的链接 A 平移到索引 1
    QVERIFY(screen.hyperlinkAt(0, 0).isEmpty());
    QCOMPARE(screen.hyperlinkAt(1, 0), uriA);

    // 继续喂 2 行：环形区满员覆盖最老环形行（整体索引 1），
    // L4 与 M 依次被丢弃，链接 A 必须随之销毁且不残留错配
    for (const char32_t c : QStringLiteral("M").toUcs4())
        screen.displayCharacter(c);
    screen.newLine();
    for (const char32_t c : QStringLiteral("N").toUcs4())
        screen.displayCharacter(c);
    screen.newLine();
    QCOMPARE(screen.getHistLines(), 2);
    QCOMPARE(screen.historyBaseLine(), qint64(3)); // 前插区非空，基线不动
    for (int i = 0; i < screen.getHistLines(); i++)
        QVERIFY2(screen.hyperlinkAt(i, 0).isEmpty(),
                 qPrintable(QStringLiteral("history line %1 仍持有已丢弃行的链接").arg(i)));
}

/**
 * @brief wrappedFlags 与 lines 长度不一致时按较短者截断（防御，不越界不触发断言）。
 */
void TestHistoryReadback::testPrependWrappedFlagsLengthMismatch()
{
    Screen screen(2, 80);
    screen.setScroll(HistoryTypeBuffer(100));
    const QVector<QVector<Character>> lines = {
        { Character(U'a') }, { Character(U'b') }, { Character(U'c') }
    };
    const QVector<bool> wrapped = { true }; // 故意短于 lines
    QCOMPARE(screen.prependHistoryLines(lines, wrapped), 1);
    QCOMPARE(screen.getHistLines(), 1);
}

/**
 * @brief Emulation 转发层：前插作用于主屏，行总数与绝对行号口径同步变化。
 */
void TestHistoryReadback::testEmulationPrependForward()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setHistory(HistoryTypeBuffer(50));
    emu.setImageSize(2, 80);
    QByteArray payload;
    for (int i = 0; i < 60; i++)
        payload += "x\r\n";
    emu.receiveData(payload.constData(), int(payload.size()));
    // 2 行屏幕：首个换行不滚动，其后 59 次换行各滚入 1 行；环形区封顶 50
    QCOMPARE(emu.lineCount(), 52);
    QCOMPARE(emu.historyBaseLine(), qint64(59 - 50)); // 9 行已离开内存

    QVector<QVector<Character>> older;
    QVector<bool> wrapped;
    for (int i = 0; i < 9; i++) {
        older.append({ Character(char32_t(U'0' + i)) });
        wrapped.append(false);
    }
    QCOMPARE(emu.prependHistoryLines(older, wrapped), 9);
    QCOMPARE(emu.lineCount(), 61);
    QCOMPARE(emu.historyBaseLine(), qint64(0)); // 注入量恰等于丢行量，base 归零
}

/**
 * @brief 部件级端到端：滚动条越顶触发提供者回调，读回行前插入历史，视图保持稳定。
 */
void TestHistoryReadback::testWidgetFetchOlderOnScrollTop()
{
    QTermWidget term(nullptr, nullptr);
    term.setHistorySize(50);
    QStringList allLines;
    QByteArray payload;
    for (int i = 0; i < 200; i++) {
        allLines << QString("line %1").arg(i);
        payload += "line " + QByteArray::number(i) + "\r\n";
    }
    term.recvData(payload.constData(), int(payload.size()));
    // 输出变更经攒帧定时器刷新，无事件循环需显式等待
    QTest::qWait(60);
    QCoreApplication::processEvents();
    QCOMPARE(term.historyLinesCount(), 50); // 内存历史封顶

    QVector<qint64> requestedBefore;
    term.setHistoryProvider([&](qint64 beforeLine, int maxLines) -> QStringList {
        requestedBefore << beforeLine;
        QStringList out;
        for (qint64 id = qMax<qint64>(0, beforeLine - maxLines); id < beforeLine; id++)
            out << allLines.at(int(id));
        return out;
    });

    QScrollBar *bar = term.findChild<QScrollBar *>();
    QVERIFY(bar);
    QVERIFY(bar->maximum() > 0); // 攒帧刷新后滚动条范围就位

    bar->setValue(0); // 越顶触发读回
    QCOMPARE(requestedBefore.size(), 1);
    QVERIFY(requestedBefore[0] > 0);
    // 前插区容量同内存历史上限（50）：合计 100；视图稳定 = 当前行下移 n
    QCOMPARE(term.historyLinesCount(), 100);
    QCOMPARE(bar->value(), 50);
    QCOMPARE(bar->maximum(), 100);

    // 再次越顶：前插区已满，注入 0 行并标记耗尽；beforeLine 已随首次前插回退 50
    bar->setValue(0);
    QCOMPARE(requestedBefore.size(), 2);
    QCOMPARE(requestedBefore[1], requestedBefore[0] - 50);
    QCOMPARE(term.historyLinesCount(), 100);

    // 耗尽后不再打扰提供者
    bar->setValue(50);
    bar->setValue(0);
    QCOMPARE(requestedBefore.size(), 2);
}

/**
 * @brief 提供者返回空列表即标记耗尽，后续越顶不再回调。
 */
void TestHistoryReadback::testProviderEmptyMarksExhausted()
{
    QTermWidget term(nullptr, nullptr);
    term.setHistorySize(10);
    QByteArray payload;
    for (int i = 0; i < 50; i++)
        payload += "line " + QByteArray::number(i) + "\r\n";
    term.recvData(payload.constData(), int(payload.size()));
    QTest::qWait(60);
    QCoreApplication::processEvents();

    int calls = 0;
    term.setHistoryProvider([&](qint64, int) -> QStringList {
        calls++;
        return {};
    });

    QScrollBar *bar = term.findChild<QScrollBar *>();
    QVERIFY(bar);
    bar->setValue(0);
    QCOMPARE(calls, 1);
    bar->setValue(5);
    bar->setValue(0);
    QCOMPARE(calls, 1); // 已耗尽，不再回调
}

/**
 * @brief 读取物理内存总量（MB），无法获取返回 -1。
 * @note 与 ZzClawTerm ZzPerfRecorder 同风格；本仓库自包含，不依赖应用仓库基建。
 */
static qint64 totalMemoryMB()
{
#ifdef Q_OS_LINUX
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) {
        const QByteArray content = meminfo.readAll();
        const int begin = content.indexOf("MemTotal:");
        if (begin >= 0) {
            const int end = content.indexOf('\n', begin);
            const QByteArray line = content.mid(begin, end - begin);
            // 格式：MemTotal:       16384000 kB（标签与数值之间有多个空格）
            return QString::fromLatin1(line).split(' ', Qt::SkipEmptyParts)
                       .value(1).toLongLong() / 1024;
        }
    }
#endif
    return -1; // Windows/macOS 暂无采集实现，记录为 -1
}

/**
 * @brief 性能记录落盘（规格 §9.1，统一 schema）：写入
 *        tests/perf/records/YYYY-MM-DD-<功能名>.json（日期按 UTC）。
 * @param name 功能名（文件名与 testName 组成部分）。
 * @param thresholdMs 通过阈值（毫秒）。
 * @param elapsedMs 实测耗时（毫秒）。
 * @param passed 是否通过。
 * @param details 负载形态等补充说明（可选）。
 */
static void writePerfRecord(const QString &name, qint64 thresholdMs, qint64 elapsedMs,
                            bool passed, const QJsonObject &details)
{
    QString compiler;
#if defined(Q_CC_CLANG)
    compiler = QStringLiteral("clang %1.%2").arg(__clang_major__).arg(__clang_minor__);
#elif defined(Q_CC_GNU)
    compiler = QStringLiteral("gcc %1.%2").arg(__GNUC__).arg(__GNUC_MINOR__);
#elif defined(Q_CC_MSVC)
    compiler = QStringLiteral("msvc %1").arg(_MSC_VER);
#else
    compiler = QStringLiteral("unknown");
#endif

    QString commit;
    QProcess git;
    git.start(QStringLiteral("git"),
              { QStringLiteral("-C"), QStringLiteral(ZZ_TERM_SOURCE_DIR),
                QStringLiteral("rev-parse"), QStringLiteral("HEAD") });
    if (git.waitForFinished(5000))
        commit = QString::fromUtf8(git.readAllStandardOutput()).trimmed();

    QJsonObject env {
        { QStringLiteral("cpu"), QSysInfo::currentCpuArchitecture() },
        { QStringLiteral("memory_mb"), double(totalMemoryMB()) },
        { QStringLiteral("os"), QSysInfo::prettyProductName() },
        { QStringLiteral("kernel"), QSysInfo::kernelVersion() },
        { QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()) },
        { QStringLiteral("compiler"), compiler },
#ifdef QT_DEBUG
        { QStringLiteral("buildType"), QStringLiteral("Debug") },
#else
        { QStringLiteral("buildType"), QStringLiteral("Release") },
#endif
        { QStringLiteral("gitCommit"), commit },
    };
    // 扁平结构 + environment/details/timestamp，与 ZzClawTerm 统一 schema 对齐（规格 §9.1 可比性）
    QJsonObject record {
        { QStringLiteral("testName"), name },
        { QStringLiteral("threshold"), double(thresholdMs) },
        { QStringLiteral("unit"), QStringLiteral("ms") },
        { QStringLiteral("measured"), double(elapsedMs) },
        { QStringLiteral("passed"), passed },
        { QStringLiteral("environment"), env },
        { QStringLiteral("details"), details },
        { QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
    };

    QDir dir(QStringLiteral(ZZ_TERM_SOURCE_DIR) + QStringLiteral("/tests/perf/records"));
    QVERIFY2(dir.mkpath(QStringLiteral(".")), "创建性能记录目录失败");
    const QString fileName =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd"))
        + QStringLiteral("-") + name + QStringLiteral(".json");
    QFile file(dir.filePath(fileName));
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate),
             qPrintable(file.errorString()));
    file.write(QJsonDocument(record).toJson(QJsonDocument::Indented));
}

/**
 * @brief 性能门控：前插注入 10 万行总耗时 ≤2000ms（仅 Release 门控，规格 §9.1）。
 */
void TestHistoryReadback::testPrepend100kLinesPerf()
{
#ifdef QT_DEBUG
    QSKIP("性能阈值仅在 Release 构建下门控（规格 §9.1）");
#endif
    Screen screen(24, 80);
    screen.setScroll(HistoryTypeBuffer(100000));

    // 预造一批 500 行（40 列）带默认属性的行，模拟提供者分批读回的同形态负载
    QVector<QVector<Character>> batch;
    QVector<bool> wrapped;
    batch.reserve(500);
    wrapped.reserve(500);
    for (int i = 0; i < 500; i++) {
        QVector<Character> line;
        line.reserve(40);
        for (int j = 0; j < 40; j++)
            line.append(Character(char32_t(U'a' + (j % 26))));
        batch.append(std::move(line));
        wrapped.append(false);
    }

    QElapsedTimer timer;
    timer.start();
    for (int b = 0; b < 200; b++)
        QCOMPARE(screen.prependHistoryLines(batch, wrapped), 500);
    const qint64 elapsed = timer.elapsed();
    QCOMPARE(screen.getHistLines(), 100000);

    const bool passed = elapsed <= 2000;
    const QJsonObject details {
        { QStringLiteral("description"),
          QStringLiteral("Screen::prependHistoryLines 前插注入 100000 行"
                         "（200 批 × 500 行，40 列，与生产越顶取数同形态）") },
        { QStringLiteral("batches"), 200 },
        { QStringLiteral("linesPerBatch"), 500 },
        { QStringLiteral("columns"), 40 },
        { QStringLiteral("totalLines"), 100000 },
    };
    writePerfRecord(QStringLiteral("zztermwidget-history-prepend"), 2000, elapsed, passed, details);
    QVERIFY2(passed, qPrintable(QStringLiteral("前插 10 万行耗时 %1ms，阈值 2000ms").arg(elapsed)));
}

QTEST_MAIN(TestHistoryReadback)
#include "tst_historyreadback.moc"
