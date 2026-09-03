#include <QtTest>
#include <QFontDatabase>
#include "Screen.h"
#include "ScreenWindow.h"
#include "Vt102Emulation.h"
#include "History.h"
#include "TerminalDisplay.h"
#include "DisplayLayout.h"
#include "qtermwidget.h"

/**
 * @brief 选一个真实存在的等宽字体（跨平台测试环境用）。
 * @return 按平台习惯优先 DejaVu Sans Mono/Menlo/Consolas/Courier New，
 *         再退任意 fixedPitch 族，最后回退系统 FixedFont。
 */
static QFont monospaceFont()
{
    static const QStringList preferred = {
        QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Menlo"),
        QStringLiteral("Consolas"),         QStringLiteral("Courier New"),
    };
    QFontDatabase db;
    const QStringList available = db.families();
    for (const QString &name : preferred)
        if (available.contains(name))
            return QFont(name);
    for (const QString &name : available)
        if (db.isFixedPitch(name))
            return QFont(name);
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

/**
 * @brief 行显示模式（软折叠/横向滚动条）回归测试。
 */
class TestLineWrap : public QObject
{
    Q_OBJECT
private slots:
    void screenLineLength();
    void screenLineSlice();
    void screenWindowForwarding();
    void foldCount();
    void foldMap();
    void displayRowOffset();
    void lineWrapModeApi();
    void softWrapComposition();
    void hscrollRangeAndVisibility();
    void hscrollAutoReset();
};

void TestLineWrap::screenLineLength()
{
    Screen screen(4, 10);
    // 空行有效长度为 0
    QCOMPARE(screen.getLineLength(0), 0);
    // 写入 3 个字符后长度为 3（不写满整行）
    screen.displayCharacter(U'a');
    screen.displayCharacter(U'b');
    screen.displayCharacter(U'c');
    QCOMPARE(screen.getLineLength(0), 3);
    // 越界行返回 0
    QCOMPARE(screen.getLineLength(99), 0);
}

void TestLineWrap::screenLineSlice()
{
    Screen screen(4, 10);
    const char *text = "abcdefgh";
    for (const char *p = text; *p; ++p)
        screen.displayCharacter(*p);

    Character dest[4];
    // 从第 2 列起取 4 个字符
    screen.getLineSlice(0, 2, 4, dest);
    QCOMPARE(dest[0].character, U'c');
    QCOMPARE(dest[3].character, U'f');
    // 起始列越过有效长度：全部补默认空格
    screen.getLineSlice(0, 8, 4, dest);
    QVERIFY(dest[0].isSpace());
    QVERIFY(dest[3].isSpace());
}

void TestLineWrap::screenWindowForwarding()
{
    Screen screen(4, 10);
    ScreenWindow window;
    window.setScreen(&screen);
    window.setWindowLines(4);
    screen.displayCharacter(U'x');
    screen.displayCharacter(U'y');

    QCOMPARE(window.windowLineLength(0), 2);

    Character dest[2];
    window.getWindowLineSlice(0, 0, 2, dest);
    QCOMPARE(dest[0].character, U'x');
    QCOMPARE(dest[1].character, U'y');
}

void TestLineWrap::foldCount()
{
    QCOMPARE(foldCountForLine(0, 10), 1);   // 空行计 1 段
    QCOMPARE(foldCountForLine(5, 10), 1);
    QCOMPARE(foldCountForLine(10, 10), 1);  // 恰好整除
    QCOMPARE(foldCountForLine(11, 10), 2);
    QCOMPARE(foldCountForLine(25, 10), 3);
    QCOMPARE(foldCountForLine(5, 0), 1);    // 退化列数防御
}

void TestLineWrap::foldMap()
{
    // 窗口 3 行：长度 25、3、0；可见显示行 5
    const QVector<int> lengths = {25, 3, 0};
    const auto rows = buildFoldMap(lengths, 10, 5);
    QCOMPARE(rows.size(), 5);
    QCOMPARE(rows[0], (DisplayRow{0, 0}));
    QCOMPARE(rows[1], (DisplayRow{0, 10}));
    QCOMPARE(rows[2], (DisplayRow{0, 20}));
    QCOMPARE(rows[3], (DisplayRow{1, 0}));
    QCOMPARE(rows[4], (DisplayRow{2, 0}));
    // maxRows 截断：只取前 2 个显示行
    QCOMPARE(buildFoldMap(lengths, 10, 2).size(), 2);
}

void TestLineWrap::displayRowOffset()
{
    // 全缓冲前缀和：行 0(25)→3 段，行 1(3)→1 段，行 2(0)→1 段
    const QVector<int> lengths = {25, 3, 0};
    QCOMPARE(displayRowOffsetOfLine(lengths, 10, 0), 0);
    QCOMPARE(displayRowOffsetOfLine(lengths, 10, 1), 3);
    QCOMPARE(displayRowOffsetOfLine(lengths, 10, 2), 4);
}

void TestLineWrap::lineWrapModeApi()
{
    QTermWidget widget;
    QCOMPARE(widget.lineWrapMode(), QTermWidget::LineWrapMode::NoWrap); // 默认
    widget.setLineWrapMode(QTermWidget::LineWrapMode::SoftWrap);
    QCOMPARE(widget.lineWrapMode(), QTermWidget::LineWrapMode::SoftWrap);
    widget.setLineWrapMode(QTermWidget::LineWrapMode::NoWrap);
    QCOMPARE(widget.lineWrapMode(), QTermWidget::LineWrapMode::NoWrap);
}

void TestLineWrap::softWrapComposition()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setHistory(HistoryTypeBuffer(100));   // 保留滚出屏幕的行，历史+屏幕共同容纳长行
    emu.setImageSize(2, 10);              // 缓冲 2 行 × 10 列
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display(nullptr);
    display.setVTFont(monospaceFont());
    display.setBlinkingCursor(false);
    display.setScreenWindow(win);
    display.setScrollBarPosition(QTermWidget::NoScrollBar);

    display.setLineWrapMode(QTermWidget::LineWrapMode::SoftWrap);
    // 显示网格固定为 10 列 × 5 行（左右/上下基础边距各 1px 计入 resize 尺寸）
    display.resize(10 * display.fontWidth() + 2, 5 * display.fontHeight() + 2);

    // 输出一条 25 字符的长行：在 10 列缓冲中折成 3 个缓冲区行
    // （"abcdefghij"/"klmnopqrst"/"uvwxy"，历史与屏幕共同容纳），
    // 软折叠视图下每个缓冲区行各占一个显示行
    const QByteArray text("abcdefghijklmnopqrstuvwxy");
    emu.receiveData(text.constData(), static_cast<int>(text.size()));
    QTest::qWait(50);   // 等待 outputChanged 驱动 updateImage

    // 合成后显示行 0/1/2 首字符应为三段切片的首字符
    QCOMPARE(display.characterAtForTest(0, 0).character, U'a');
    QCOMPARE(display.characterAtForTest(0, 1).character, U'k');
    QCOMPARE(display.characterAtForTest(0, 2).character, U'u');
}

void TestLineWrap::hscrollRangeAndVisibility()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setHistory(HistoryTypeBuffer(100));
    emu.setImageSize(2, 30);              // 缓冲 2 行 × 30 列：25 字符一行放下、不折行
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display(nullptr);
    display.setVTFont(monospaceFont());
    display.setBlinkingCursor(false);
    display.setScreenWindow(win);
    display.setScrollBarPosition(QTermWidget::NoScrollBar);

    // 显示网格固定为 10 列 × 2 行（左右/上下基础边距各 1px 计入 resize 尺寸）
    display.resize(10 * display.fontWidth() + 2, 2 * display.fontHeight() + 2);
    display.show();
    QTest::qWait(50);

    // 无超宽内容：横向滚动条隐藏
    QVERIFY(!display.hScrollBarVisibleForTest());

    // 输出 25 字符超长行（缓冲 30 列不折行，行数据保留 25 格）
    const QByteArray text("abcdefghijklmnopqrstuvwxy");
    emu.receiveData(text.constData(), static_cast<int>(text.size()));
    QTest::qWait(50);   // 等待 outputChanged 驱动 updateImage

    // range = maxLen - _columns = 25 - 10 = 15，滚动条出现
    QVERIFY(display.hScrollBarVisibleForTest());
    QCOMPARE(display.hScrollBarMaximumForTest(), 15);
}

void TestLineWrap::hscrollAutoReset()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setHistory(HistoryTypeBuffer(100));
    emu.setImageSize(2, 30);              // 缓冲 2 行 × 30 列：25 字符一行放下、不折行
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display(nullptr);
    display.setVTFont(monospaceFont());
    display.setBlinkingCursor(false);
    display.setScreenWindow(win);
    display.setScrollBarPosition(QTermWidget::NoScrollBar);

    // 显示网格固定为 10 列 × 2 行（左右/上下基础边距各 1px 计入 resize 尺寸）
    display.resize(10 * display.fontWidth() + 2, 2 * display.fontHeight() + 2);
    display.show();

    const QByteArray text("abcdefghijklmnopqrstuvwxy");
    emu.receiveData(text.constData(), static_cast<int>(text.size()));
    QTest::qWait(50);
    QVERIFY(display.hScrollBarVisibleForTest());

    // Shift+滚轮向下：水平视口右移 4 列（每格 4 列）
    const QPointF center(display.width() / 2.0, display.height() / 2.0);
    QWheelEvent wheelDown(center, center, QPoint(0, 0), QPoint(0, -120),
                          Qt::NoButton, Qt::ShiftModifier,
                          Qt::NoScrollPhase, false);
    QApplication::sendEvent(&display, &wheelDown);

    // 视口右移后，显示网格 (0,0) 处为原行第 5 个字符 'e'
    QCOMPARE(display.characterAtForTest(0, 0).character, U'e');

    // 打字即回到光标处：任意按键把水平偏移拉回 0
    QTest::keyClick(&display, Qt::Key_X);
    QCOMPARE(display.characterAtForTest(0, 0).character, U'a');

    // 偏移回零不改变超宽事实：滚动条仍在，range 不变
    QVERIFY(display.hScrollBarVisibleForTest());
    QCOMPARE(display.hScrollBarMaximumForTest(), 15);
}

QTEST_MAIN(TestLineWrap)
#include "tst_linewrap.moc"
