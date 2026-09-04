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
    void softWrapCoordinateMapping();
    void softWrapScrollRange();
    void softWrapSelectionHighlight();
    void hscrollSelectionHighlight();
    void integrationNoWrapHScroll();
    void integrationSoftWrapAfterShrink();
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

void TestLineWrap::softWrapCoordinateMapping()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setHistory(HistoryTypeBuffer(100));
    emu.setImageSize(2, 30);              // 缓冲 2 行 × 30 列：25 字符一行放下、不硬折行
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display(nullptr);
    display.setVTFont(monospaceFont());
    display.setBlinkingCursor(false);
    display.setScreenWindow(win);
    display.setScrollBarPosition(QTermWidget::NoScrollBar);

    display.setLineWrapMode(QTermWidget::LineWrapMode::SoftWrap);
    // 显示网格固定为 10 列 × 5 行：25 字符缓冲行在视图中折叠为 3 个显示段
    display.resize(10 * display.fontWidth() + 2, 5 * display.fontHeight() + 2);

    const QByteArray text("abcdefghijklmnopqrstuvwxy");
    emu.receiveData(text.constData(), static_cast<int>(text.size()));
    QTest::qWait(50);   // 等待 outputChanged 驱动 updateImage 重建 _displayRows

    // 折叠段 {缓冲行 0, 列偏移 0/10/20}：显示 (0,1) → 缓冲 (10,0)；显示 (3,2) → 缓冲 (23,0)
    QCOMPARE(display.mapDisplayToBufferForTest(0, 1), QPoint(10, 0));
    QCOMPARE(display.mapDisplayToBufferForTest(3, 2), QPoint(23, 0));
    // 首段为恒等映射
    QCOMPARE(display.mapDisplayToBufferForTest(5, 0), QPoint(5, 0));
}

void TestLineWrap::softWrapScrollRange()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setHistory(HistoryTypeBuffer(100));
    emu.setImageSize(2, 30);              // 缓冲 2 行 × 30 列
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display(nullptr);
    display.setVTFont(monospaceFont());
    display.setBlinkingCursor(false);
    display.setScreenWindow(win);
    display.setScrollBarPosition(QTermWidget::NoScrollBar);

    display.setLineWrapMode(QTermWidget::LineWrapMode::SoftWrap);
    // 显示网格固定为 10 列 × 5 行
    display.resize(10 * display.fontWidth() + 2, 5 * display.fontHeight() + 2);

    // 输出 10 条 25 字符行（行间 \r\n，末尾无换行）：历史 8 行 + 屏幕 2 行，
    // 每行折叠 3 段，全缓冲折叠后共 30 个显示行
    for (int i = 0; i < 10; ++i) {
        const QByteArray line(25, char('a' + i));
        emu.receiveData(line.constData(), static_cast<int>(line.size()));
        if (i < 9)
            emu.receiveData("\r\n", 2);
    }
    QTest::qWait(50);

    // 垂直滚动条 range 以显示行计：maximum = 30 - 5 = 25
    QCOMPARE(display.vScrollBarMaximumForTest(), 25);

    // 显示行 3 = 缓冲行 1 第 2 段起点；反推后窗口顶落在缓冲行 1
    // （段内精度丢失为 v1 已知简化）
    display.setVScrollBarValueForTest(3);
    QCOMPARE(display.screenWindow()->currentLine(), 1);
}

/**
 * @brief 显示行 row 内容区的平均灰度（选区反显断言用）。
 * @note 左右/上下基础边距各 1px；选中行前景/背景交换后平均亮度显著变化。
 */
static qint64 rowBrightness(const QImage &img, int row, int columns,
                            int fontWidth, int fontHeight)
{
    qint64 sum = 0;
    qint64 n = 0;
    for (int py = 1 + row * fontHeight; py < 1 + (row + 1) * fontHeight; ++py)
        for (int px = 1; px < 1 + columns * fontWidth; ++px) {
            sum += qGray(img.pixel(px, py));
            ++n;
        }
    return n ? sum / n : 0;
}

void TestLineWrap::softWrapSelectionHighlight()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setHistory(HistoryTypeBuffer(100));
    emu.setImageSize(2, 30);              // 缓冲 2 行 × 30 列：25 字符一行放下
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display(nullptr);
    display.setVTFont(monospaceFont());
    display.setBlinkingCursor(false);
    display.setScreenWindow(win);
    display.setScrollBarPosition(QTermWidget::NoScrollBar);

    display.setLineWrapMode(QTermWidget::LineWrapMode::SoftWrap);
    // 显示网格 10 列 × 5 行：25 字符缓冲行折叠为 3 个显示段
    display.resize(10 * display.fontWidth() + 2, 5 * display.fontHeight() + 2);

    const QByteArray text("abcdefghijklmnopqrstuvwxy");
    emu.receiveData(text.constData(), static_cast<int>(text.size()));
    QTest::qWait(50);

    // 选中缓冲行 0 的列 10..19（恰为第 2 个折叠段 = 显示行 1 整段）
    win->setSelectionStart(10, 0, false);
    win->setSelectionEnd(19, 0);
    QTest::qWait(100);  // selectionChanged → bufferedUpdate（最长 40ms）→ updateImage

    // getLineSlice 刻意不做选区反色，合成视图的高亮全靠绘制层 isSelected
    // 即时交换：显示行 1（选中段，暗底亮字）平均亮度应显著低于
    // 未选中的显示行 0（默认亮底暗字）
    const QImage img = display.grab().toImage();
    const qint64 selected = rowBrightness(img, 1, 10, display.fontWidth(),
                                          display.fontHeight());
    const qint64 unselected = rowBrightness(img, 0, 10, display.fontWidth(),
                                            display.fontHeight());
    QVERIFY2(selected + 40 < unselected,
             qPrintable(QStringLiteral("selected=%1 unselected=%2")
                                .arg(selected).arg(unselected)));
}

void TestLineWrap::hscrollSelectionHighlight()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setHistory(HistoryTypeBuffer(100));
    emu.setImageSize(2, 30);              // 缓冲 2 行 × 30 列：25 字符一行放下
    ScreenWindow *win = emu.createWindow();
    TerminalDisplay display(nullptr);
    display.setVTFont(monospaceFont());
    display.setBlinkingCursor(false);
    display.setScreenWindow(win);
    display.setScrollBarPosition(QTermWidget::NoScrollBar);

    // NoWrap，显示网格 10 列 × 3 行：横向滚动条出现后余 2 个显示行，
    // 文本行 0 + 空光标行 1 均可见（2 行夹具会被滚动条吃到只剩 1 行，
    // 下一帧 trackOutput 把窗口重锚定到空光标行，文本滚出视图——任务 6 已知行为）
    display.resize(10 * display.fontWidth() + 2, 3 * display.fontHeight() + 2);
    display.show();

    const QByteArray text("abcdefghijklmnopqrstuvwxy");
    emu.receiveData(text.constData(), static_cast<int>(text.size()));
    QTest::qWait(50);
    QVERIFY(display.hScrollBarVisibleForTest());

    // Shift+滚轮向下：水平视口右移 4 列（显示列 x 对应缓冲列 x+4）
    const QPointF center(display.width() / 2.0, display.height() / 2.0);
    QWheelEvent wheelDown(center, center, QPoint(0, 0), QPoint(0, -120),
                          Qt::NoButton, Qt::ShiftModifier,
                          Qt::NoScrollPhase, false);
    QApplication::sendEvent(&display, &wheelDown);
    QCOMPARE(display.characterAtForTest(0, 0).character, U'e');

    // 选中缓冲列 4..13（恰为当前水平视口可见的整段）
    win->setSelectionStart(4, 0, false);
    win->setSelectionEnd(13, 0);
    QTest::qWait(100);  // selectionChanged → bufferedUpdate → updateImage

    // NoWrap 偏移下 isSelected 经 mapDisplayToBuffer 换算回缓冲列：
    // 显示行 0 整行反显（暗底亮字），平均亮度显著低于未选中的空行 1
    const QImage img = display.grab().toImage();
    const qint64 selected = rowBrightness(img, 0, 10, display.fontWidth(),
                                          display.fontHeight());
    const qint64 unselected = rowBrightness(img, 1, 10, display.fontWidth(),
                                            display.fontHeight());
    QVERIFY2(selected + 40 < unselected,
             qPrintable(QStringLiteral("selected=%1 unselected=%2")
                                .arg(selected).arg(unselected)));
}

void TestLineWrap::integrationNoWrapHScroll()
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

    // 初始显示网格 30 列 × 3 行：整条 25 字符行在宽窗口下完整可见
    display.resize(30 * display.fontWidth() + 2, 3 * display.fontHeight() + 2);
    display.show();

    const QByteArray text("abcdefghijklmnopqrstuvwxy");
    emu.receiveData(text.constData(), static_cast<int>(text.size()));
    QTest::qWait(50);
    QVERIFY(!display.hScrollBarVisibleForTest());   // 行未超宽：横向条不出现
    QCOMPARE(display.characterAtForTest(24, 0).character, U'y');

    // 缩窄到 10 列：夹具未接仿真层尺寸回报，手动触发输出变更驱动 updateImage
    display.resize(10 * display.fontWidth() + 2, 3 * display.fontHeight() + 2);
    win->notifyOutputChanged();
    QTest::qWait(50);

    // 超宽行撑出横向滚动条（range = 25 - 10 = 15，条吃掉一行后余 2 个显示行）
    QVERIFY(display.hScrollBarVisibleForTest());
    QCOMPARE(display.hScrollBarMaximumForTest(), 15);

    // Shift+滚轮向下：水平视口右移 4 列，(0,0) 处变为原行第 5 个字符
    const QPointF center(display.width() / 2.0, display.height() / 2.0);
    QWheelEvent wheelDown(center, center, QPoint(0, 0), QPoint(0, -120),
                          Qt::NoButton, Qt::ShiftModifier,
                          Qt::NoScrollPhase, false);
    QApplication::sendEvent(&display, &wheelDown);
    QCOMPARE(display.characterAtForTest(0, 0).character, U'e');

    // 切到 SoftWrap：横向条隐藏，25 字符行折叠为 3 个显示段。
    // setLineWrapMode 先隐藏横向条、重算几何并立即重建视图，
    // 无新输出、无手动驱动时三个折叠段也当场全部可见（回归：曾缺失末段）
    display.setLineWrapMode(QTermWidget::LineWrapMode::SoftWrap);
    QVERIFY(!display.hScrollBarVisibleForTest());
    QCOMPARE(display.characterAtForTest(0, 0).character, U'a');
    QCOMPARE(display.characterAtForTest(0, 1).character, U'k');
    QCOMPARE(display.characterAtForTest(0, 2).character, U'u');

    // 切回 NoWrap：模式切换把水平偏移拉回 0，横向条随超宽行同步重新出现
    display.setLineWrapMode(QTermWidget::LineWrapMode::NoWrap);
    QVERIFY(display.hScrollBarVisibleForTest());
    QCOMPARE(display.characterAtForTest(0, 0).character, U'a');
}

void TestLineWrap::integrationSoftWrapAfterShrink()
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

    display.setLineWrapMode(QTermWidget::LineWrapMode::SoftWrap);
    // 初始显示网格 30 列 × 3 行：宽窗口下 25 字符行只占一个显示段
    display.resize(30 * display.fontWidth() + 2, 3 * display.fontHeight() + 2);
    display.show();

    const QByteArray text("abcdefghijklmnopqrstuvwxy");
    emu.receiveData(text.constData(), static_cast<int>(text.size()));
    QTest::qWait(50);
    QCOMPARE(display.characterAtForTest(0, 0).character, U'a');
    QCOMPARE(display.characterAtForTest(24, 0).character, U'y');
    QVERIFY(!display.hScrollBarVisibleForTest());   // SoftWrap 下横向条恒隐藏

    // 缩窄到 10 列：同一缓冲行重新折叠为 3 个显示段（首字符 a/k/u），
    // 夹具未接仿真层尺寸回报，手动触发输出变更驱动 updateImage 重建映射表
    display.resize(10 * display.fontWidth() + 2, 3 * display.fontHeight() + 2);
    win->notifyOutputChanged();
    QTest::qWait(50);
    QCOMPARE(display.characterAtForTest(0, 0).character, U'a');
    QCOMPARE(display.characterAtForTest(0, 1).character, U'k');
    QCOMPARE(display.characterAtForTest(0, 2).character, U'u');
    QVERIFY(!display.hScrollBarVisibleForTest());
}

QTEST_MAIN(TestLineWrap)
#include "tst_linewrap.moc"
