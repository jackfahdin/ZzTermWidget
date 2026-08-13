#include <QtTest>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "History.h"
#include "TerminalDisplay.h"

/**
 * @brief 像素等价性常驻测试：批次聚合路径与逐片段 Legacy 路径双渲染逐像素比对。
 * @note 渲染性能优化（drawContents 改造）的安全网：两条路径对同一份屏幕内容的
 *       离屏渲染结果必须逐像素相等。比对在同一进程同一字体环境下进行，
 *       不依赖仓库内 PNG 基线（光栅化结果随平台/字体变化，不可移植）。
 *       设环境变量 ZZQTW_RENDER_DUMP=<目录> 可把双路径渲染结果落盘备查。
 */
class TestRendering : public QObject
{
    Q_OBJECT
private slots:
    void testBatchingPixelEquivalence();
    void testBatchingPixelEquivalenceAfterPartialUpdate();
    void testSpanDirtyPixelEquivalence_data();
    void testSpanDirtyPixelEquivalence();
    void testSpanDirtySelectionFrame();
    void testScrollFastPathPixelEquivalence();
    void testScrollMixedContentFallback();
    void testScrollImageFallback();
    void testScrollSelectionFallback();
    void testScrollOptimizationSwitchAB();
};

/**
 * @brief 构造渲染测试环境：仿真 + 窗口 + 离屏显示组件（24x80，等宽字体，关闪烁保确定性）。
 */
static void initRenderEnv(Vt102Emulation &emu, ScreenWindow *&win, TerminalDisplay &display)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    win = emu.createWindow();
    win->setWindowLines(24);
    display.setVTFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    display.setBlinkingCursor(false);
    display.setBlinkingTextEnabled(false);
    display.setScreenWindow(win);
    display.resize(800, 600);
}

/**
 * @brief 构造覆盖各绘制路径的屏幕内容：普通文本、SGR 样式（粗/斜/下划线/删除线/反显）、
 *        256 色与 RGB 色、CJK 宽字符、组合字符（ExtendedCharTable 路径）、制表符自绘、
 *        双倍宽行（世界变换路径）。
 */
static QByteArray buildRenderContent()
{
    QByteArray s;
    s += "\033[H";                                                // 光标回左上角
    s += "plain ascii text row\r\n";
    s += "\033[1;3;4;9mstyled: bold italic underline strike\033[0m\r\n";
    s += "\033[38;5;196m256color fg\033[0m \033[48;2;10;200;30mrgb bg\033[0m\r\n";
    s += "CJK 宽字符混排 abc 中文测试 123\r\n";
    s += "combining: e\xcc\x81 o\xcc\x88 a\xcc\xa7\r\n";          // e+́ o+̈ a+̧（组合字符）
    s += "box: \xe2\x94\x80\xe2\x94\x82\xe2\x94\x8c\xe2\x94\x90\xe2\x94\x94\xe2\x94\x98\r\n"; // ─│┌┐└┘
    s += "\033#6" "double width line\r\n";                        // DECDWL 双倍宽行
    s += "\033[7mreverse video\033[0m\r\n";
    return s;
}

/**
 * @brief 用指定绘制路径把显示组件离屏渲染为 QImage。
 * @param display 目标显示组件。
 * @param batching true = 批次聚合路径；false = Legacy 逐片段路径。
 */
static QImage renderDisplay(TerminalDisplay &display, bool batching)
{
    display.setTextBatchingEnabled(batching);
    display.updateImage();
    QImage image(display.size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    display.render(&image);
    const QByteArray dumpDir = qgetenv("ZZQTW_RENDER_DUMP");
    if (!dumpDir.isEmpty())
        image.save(QString::fromLocal8Bit(dumpDir) + QLatin1Char('/')
                   + (batching ? QStringLiteral("batched") : QStringLiteral("legacy"))
                   + QStringLiteral(".png"));
    return image;
}

/**
 * @brief 整屏内容：两条绘制路径的渲染结果逐像素相等。
 */
void TestRendering::testBatchingPixelEquivalence()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    const QByteArray content = buildRenderContent();
    emu.receiveData(content.constData(), int(content.size()));

    const QImage batched = renderDisplay(display, true);
    const QImage legacy = renderDisplay(display, false);
    if (batched != legacy) {
        // 排障辅助：不一致时落盘到临时目录人工比对
        batched.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-batched.png")));
        legacy.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-legacy.png")));
    }
    QCOMPARE(batched, legacy); // operator== 即整图逐像素比对
}

/**
 * @brief 局部更新后再渲染（近似 TUI 帧负载）：两条路径仍逐像素相等。
 */
void TestRendering::testBatchingPixelEquivalenceAfterPartialUpdate()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    const QByteArray content = buildRenderContent();
    emu.receiveData(content.constData(), int(content.size()));
    renderDisplay(display, true); // 首帧，让 _image 就位

    const QByteArray edit =
            "\033[5;10H\033[38;5;45mEDIT\033[0m"   // 局部改写 + 颜色
            "\033[7;1Hxy";                          // 另一处小改动
    emu.receiveData(edit.constData(), int(edit.size()));

    const QImage batched = renderDisplay(display, true);
    const QImage legacy = renderDisplay(display, false);
    if (batched != legacy) {
        batched.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-batched-partial.png")));
        legacy.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-legacy-partial.png")));
    }
    QCOMPARE(batched, legacy);
}

/**
 * @brief 全量离屏渲染：黑底全新一帧（调用方负责首帧 warmup 吃掉 _drawTextTestFlag）。
 */
static QImage renderFull(TerminalDisplay &display)
{
    QImage image(display.size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    display.render(&image);
    return image;
}

/**
 * @brief 增量重放：仅把上一次 updateImage() 的脏区渲染到上一帧图像上。
 * @param display 目标显示组件（须刚完成一帧 updateImage()）。
 * @param base 上一帧的全量渲染结果。
 * @return 重放后的图像；与 renderFull() 逐像素相等即增量路径无遗漏脏区。
 * @note QWidget::render(target, offset, region) 会把 region 包围盒内容画到 offset 处
 *       （实测：区域 y155 在 offset (0,0) 下落到图像 y0），因此 offset 必须传
 *       包围盒左上角才能原位重放。
 */
static QImage replayDirtyRegion(TerminalDisplay &display, const QImage &base)
{
    QImage image = base;
    const QRegion dirty = display.lastDirtyRegion();
    if (!dirty.isEmpty())
        display.render(&image, dirty.boundingRect().topLeft(), dirty);
    return image;
}

/**
 * @brief 驱动一帧：刷新 ScreenWindow 缓冲并经 outputChanged 信号触发 updateImage()。
 * @note 测试内无事件循环，Emulation 的 bufferedUpdate 定时器不会触发，
 *       ScreenWindow 的 _windowBuffer 只在 notifyOutputChanged() 后才会重建；
 *       直接调 updateImage() 只会拿到陈旧缓冲比对出空脏区，并把 _lastDirtyRegion
 *       冲掉。notifyOutputChanged() 走的正是生产环境 outputChanged→updateImage 通路。
 */
static void pumpFrame(ScreenWindow *win)
{
    win->notifyOutputChanged();
}

void TestRendering::testSpanDirtyPixelEquivalence_data()
{
    QTest::addColumn<QByteArray>("setup");     // 首帧内容后的追加构造（双高行/图像等）
    QTest::addColumn<QByteArray>("edit");      // 本帧编辑负载
    QTest::addColumn<int>("editRow");          // 编辑所在行（0 起）
    QTest::addColumn<int>("editCells");        // 预期脏跨度格数上限；0 = 不做形状断言
    QTest::addColumn<bool>("pixelCheck");      // false = 只做形状断言，跳过逐像素比对

    QTest::newRow("单行少格")
            << QByteArray()
            << QByteArray("\033[5;10H\033[38;5;45mEDIT\033[0m") << 4 << 6 << true;
    QTest::newRow("宽字符跨界")
            << QByteArray()
            << QByteArray("\033[4;21H\xe7\x95\xbb") << 3 << 0 << true; // CJK 行内改写宽字符"画"
    // 斜体邻居越界（强用例）：在既有内容上把 "bo" 改写成斜体 ITAL，编辑点两侧
    // 邻居均为带斜体 rendition 的非空格内容（"styled: bold italic…" 整行
    // 1;3;4;9 样式）。邻居斜体字形的右倾墨迹伸入脏跨度边缘格，若实现不把斜体
    // 邻居行升级为整行脏，该墨迹会被背景重绘抹除且邻居永不被重绘——增量重放
    // 与全量渲染必然出现像素差（审查裁定覆盖的正是此机制）
    QTest::newRow("斜体邻居越界")
            << QByteArray()
            << QByteArray("\033[2;10H\033[3mITAL\033[0m") << 1 << 0 << true;
    // 斜体空格邻居：目标区两侧为空格，排除邻居越界干扰，单独验证被编辑斜体字形
    // 自身的右倾越界被 +1 格扩展吸收
    QTest::newRow("斜体空格邻居")
            << QByteArray("\033[2;5HAB\033[2;17HCD")
            << QByteArray("\033[2;9H\033[3mITAL\033[0m") << 1 << 0 << true;
    // 双高行不做逐像素比对：DECDH 片段经 scale(1,2) 世界变换绘制，墨迹落在 2×行坐标处
    // （calculateTextArea 只对原点逆映射，top 项被放大），任何行矩形脏区都盖不住实际墨迹，
    // 属于改造前就存在的绘制几何怪癖；此处只验证"双高行整行脏"的行为不退化
    QTest::newRow("双高行整行脏")
            << QByteArray("\033[10;1H\033#3double high line\r\n\033#4double high line\r\n")
            << QByteArray("\033[10;3HZ") << 9 << -1 << false;
    QTest::newRow("含图像行")
            << QByteArray("\033[12;1H\033Pq#0;2;100;0;0#0!8~-!8~-!8~\033\\")
            << QByteArray("\033[12;20HIMG") << 11 << 0 << true;
}

/**
 * @brief 跨度脏区：编辑帧的增量重放与全量渲染逐像素相等；单行少格场景的脏带
 *        宽度不得超过（编辑格数 + 两侧各 1 格扩展 + 1 格余量）× 格宽（形状证据）。
 * @note 形状断言即本任务的先失败测试：整行脏区旧实现下脏带恒为整行宽，必然失败。
 */
void TestRendering::testSpanDirtyPixelEquivalence()
{
    QFETCH(QByteArray, setup);
    QFETCH(QByteArray, edit);
    QFETCH(int, editRow);
    QFETCH(int, editCells);
    QFETCH(bool, pixelCheck);

    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    // sixel 场景需要单元格像素尺寸（镜像 tst_sixel 的 initSixelRenderEnv）
    emu.setCellPixelSize(display.cellPixelWidth(), display.cellPixelHeight());
    const QByteArray content = buildRenderContent();
    emu.receiveData(content.constData(), int(content.size()));
    if (!setup.isEmpty())
        emu.receiveData(setup.constData(), int(setup.size()));
    // 首帧驱动两次：第一次 updateImage 才创建 _image 并把 display 行数同步给
    // ScreenWindow（updateImageSize→setWindowLines），而 updateLineProperties 在信号
    // 链中先于 updateImage 执行，单泵一次拿到的是旧几何下的行属性（双倍宽/高行
    // 会被当普通行渲染）；第二次泵入行属性与视图位置才全部就位
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup：吃掉 _drawTextTestFlag 一次性度量
    const QImage base = renderFull(display);

    emu.receiveData(edit.constData(), int(edit.size()));
    pumpFrame(win);

    if (editCells > 0) {
        const int fh = display.fontHeight();
        const int rowTop = display.contentsRect().top() + display.margin() + editRow * fh;
        const QRect band(0, rowTop, display.width(), fh);
        int maxWidth = 0;
        const auto rects = display.lastDirtyRegion();
        for (const QRect &r : rects)
            if (r.intersects(band))
                maxWidth = qMax(maxWidth, r.width());
        QVERIFY2(maxWidth > 0 && maxWidth <= (editCells + 3) * display.fontWidth(),
                 qPrintable(QStringLiteral("编辑行脏带宽 %1 超出跨度上限 %2")
                            .arg(maxWidth).arg((editCells + 3) * display.fontWidth())));
    }

    if (editCells == -1) {
        // 双高行形状断言：编辑触及双高行时，该行及其另一半都必须整行置脏
        const int fh = display.fontHeight();
        const int fullWidth = display.fontWidth() * win->windowColumns(); // 整行脏 = 窗口列数满宽
        const int top0 = display.contentsRect().top() + display.margin();
        const auto rects = display.lastDirtyRegion();
        for (const int row : {editRow, editRow + 1}) {
            const QRect band(0, top0 + row * fh, display.width(), fh);
            int maxWidth = 0;
            for (const QRect &r : rects)
                if (r.intersects(band))
                    maxWidth = qMax(maxWidth, r.width());
            QVERIFY2(maxWidth >= fullWidth,
                     qPrintable(QStringLiteral("双高行第 %1 行脏带宽 %2 非整行宽 %3")
                                .arg(row).arg(maxWidth).arg(fullWidth)));
        }
    }

    if (!pixelCheck)
        return;

    const QImage incremental = replayDirtyRegion(display, base);
    const QImage full = renderFull(display);
    if (incremental != full) { // 排障辅助：落盘人工比对
        incremental.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-span-incremental.png")));
        full.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-span-full.png")));
        base.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-span-base.png")));
    }
    QCOMPARE(incremental, full);
}

/**
 * @brief 选区高亮帧：selectAll 把选区 rendition 烤进 newimg，逐格比对捕获，
 *        增量重放与全量渲染仍逐像素相等。
 * @note 内容刻意用普通文本+SGR 颜色行：双倍宽/高行的世界变换墨迹会越出行矩形
 *       （详见 testSpanDirtyPixelEquivalence_data 的 DECDH 注释），选区帧只做
 *       像素等价安全网，不混入变换路径怪癖。
 */
void TestRendering::testSpanDirtySelectionFrame()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    // 离屏隐藏部件的 resize 不触发 resizeEvent，_lines/_columns 要等首帧 updateImage
    // 才重算；先泵一帧让几何就位再对齐 Screen 尺寸。selectAll 按 display 行列数取
    // 选区文本，Screen 尺寸不齐会越界读 screenLines（initRenderEnv 的 24 行 <
    // display 42 行）
    pumpFrame(win);
    emu.setImageSize(display.lines(), display.columns());
    QByteArray content;
    content += "\033[H";
    content += "plain ascii text row\r\n";
    content += "\033[1;4mstyled: bold underline\033[0m\r\n";
    content += "\033[38;5;196m256color fg\033[0m \033[48;2;10;200;30mrgb bg\033[0m\r\n";
    content += "CJK 宽字符混排 abc 中文测试 123\r\n";
    content += "last line\r\n";
    emu.receiveData(content.constData(), int(content.size()));
    pumpFrame(win);
    pumpFrame(win); // 双泵就位行属性/视图几何（同上）
    renderFull(display); // warmup
    const QImage base = renderFull(display);

    display.selectAll();
    pumpFrame(win);

    const QImage incremental = replayDirtyRegion(display, base);
    const QImage full = renderFull(display);
    if (incremental != full) { // 排障辅助：落盘人工比对
        incremental.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-sel-incremental.png")));
        full.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-sel-full.png")));
        base.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-sel-base.png")));
    }
    QCOMPARE(incremental, full);
}

/**
 * @brief 构造整屏滚动负载：持续行输出（行号递增）使视图匀速上滚。
 */
static QByteArray buildScrollPayload(int from, int count)
{
    QByteArray payload;
    for (int i = from; i < from + count; i++)
        payload += "scroll payload line " + QByteArray::number(i)
                 + " mixed 文本 abc 123\r\n";
    return payload;
}

/**
 * @brief 驱动一帧滚动：刷新 ScreenWindow 缓冲并经 outputChanged 信号触发 updateImage()，
 *        随后复位 Screen 的滚动/丢行计数。
 * @note 无事件循环时 Emulation::showBulk()（先发 outputChanged、再 resetScrolledLines）
 *       不会执行，Screen::scrolledLines 会跨帧累积，导致 scrollCount() 拿到的是
 *       历史累计值而非本帧滚动量；此处镜像 showBulk 的"消费后复位"帧节奏。
 */
static void pumpScrollFrame(ScreenWindow *win)
{
    win->notifyOutputChanged();
    win->screen()->resetScrolledLines();
    win->screen()->resetDroppedLines();
}

/**
 * @brief 把显示组件推进滚动态：首帧几何就位后对齐 Screen/窗口/显示三者行列并开启
 *        内存历史（生产环境经 imageSizeChanged 保持三者一致；不一致时新进内容落在
 *        窗口底部空白尾区上方，不存在纯整屏滚动形态），填满屏幕并多输出 10 行使
 *        视图匀速上滚，完成 warmup 后返回首帧全量渲染结果。
 */
static QImage enterScrollingState(Vt102Emulation &emu, ScreenWindow *win, TerminalDisplay &display)
{
    // 首帧：updateImageSize 重算 _lines/_columns 并同步给 ScreenWindow
    pumpFrame(win);
    // 对齐三者行列（显示组件格数由像素尺寸与字体决定，此处反向对齐 Screen）
    emu.setImageSize(display.lines(), display.columns());
    // 默认 HistoryScrollNone 视图不移动（无历史可回滚、图像放置索引不随内容迁移），
    // 开启内存历史镜像生产环境
    win->screen()->setScroll(HistoryTypeBuffer(1000));
    const QByteArray content = buildScrollPayload(0, display.lines() + 10);
    emu.receiveData(content.constData(), int(content.size()));
    pumpScrollFrame(win);
    renderFull(display); // warmup：吃掉 _drawTextTestFlag
    return renderFull(display);
}

/**
 * @brief 滚动帧增量重放：先在 base 上镜像 scrollImage 的像素搬迁（QWidget::scroll
 *        作用于部件本身，对测试侧的离屏 QImage 无效，须在此建模），再重放脏区。
 * @param n 本帧上滚行数（>0）。
 * @note 全屏对齐几何下有效滚动区为 [0, L-1)：Screen::lastScrolledRegion 高为
 *       bottomMargin-topMargin（L-1 行），scrollImage 再把区域下沿钳到 _lines-2；
 *       像素搬迁行数 linesToMove = L-1-n，与 updateImage() 快路径的验证区间同源。
 */
static QImage replayScrollFrame(TerminalDisplay &display, const QImage &base, int n)
{
    const int fh = display.fontHeight();
    const int rowTop = display.contentsRect().top() + display.margin();
    const int linesToMove = display.lines() - 1 - n;
    QImage image = base;
    {
        QPainter p(&image);
        p.drawImage(0, rowTop, base, 0, rowTop + n * fh, base.width(), linesToMove * fh);
    }
    const QRegion dirty = display.lastDirtyRegion();
    if (!dirty.isEmpty())
        display.render(&image, dirty.boundingRect().topLeft(), dirty);
    return image;
}

/**
 * @brief 滚动帧像素比对：允许逐通道 ≤32 的抗锯齿/亚像素边缘抖动，内容级差异硬失败。
 * @note scrollImage 的像素搬迁是位搬移而非重绘：同一字形在不同绝对 y 的抗锯齿
 *       边缘可有 ±1~2 的灰度抖动（实测 CJK 字形顶部越界行）；跨度脏区硬裁剪边界
 *       上的字形亚像素（LCD）边缘也可与全量重绘差 ~22 灰度（实测编辑点左邻居格
 *       1px）。两者在真实部件上同样存在——逐像素恒等对滚动帧在原理上不可达。
 *       漏脏/错位产生的是墨迹与背景量级（百级灰度）的差异，阈值 32 足以区分。
 */
static void verifyScrollFrameEqual(const QImage &actual, const QImage &expected)
{
    QCOMPARE(actual.size(), expected.size());
    QCOMPARE(actual.format(), expected.format());
    int badPixels = 0;
    int maxDiff = 0;
    QPoint firstBad;
    for (int y = 0; y < actual.height(); y++)
        for (int x = 0; x < actual.width(); x++) {
            const QColor a = actual.pixelColor(x, y);
            const QColor e = expected.pixelColor(x, y);
            const int d = qMax(qAbs(a.red() - e.red()),
                               qMax(qAbs(a.green() - e.green()),
                                    qMax(qAbs(a.blue() - e.blue()), qAbs(a.alpha() - e.alpha()))));
            maxDiff = qMax(maxDiff, d);
            if (d > 32) {
                if (badPixels == 0)
                    firstBad = QPoint(x, y);
                badPixels++;
            }
        }
    if (badPixels > 0) { // 排障辅助：落盘人工比对
        actual.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-scroll-incremental.png")));
        expected.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-scroll-full.png")));
    }
    QVERIFY2(badPixels == 0,
             qPrintable(QStringLiteral("滚动帧像素差异 %1 处（最大通道差 %2，首处 (%3,%4)）")
                        .arg(badPixels).arg(maxDiff).arg(firstBad.x()).arg(firstBad.y())));
}

/**
 * @brief 纯滚动帧（1 行与 N 行）：镜像像素搬迁后的增量重放与全量渲染逐像素相等，
 *        且脏区仅覆盖新进 N 行 + 1 行边界陈旧行行带（远小于全屏）。
 * @note 快路径正确性安全网：若 moved 行被错误跳过（漏脏），重放结果将与全量渲染不符。
 *       脏带上限取 (n+2) 行高：新进 N 行 + scrollImage 区域钳制遗留的 1 行边界
 *       陈旧行（_image 该行未被 memmove、与移位后内容不符而置脏）+ 纵向余量。
 */
void TestRendering::testScrollFastPathPixelEquivalence()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    QImage base = enterScrollingState(emu, win, display);
    // 三者行列对齐是纯整屏滚动形态的前提（enterScrollingState 已对齐）
    QCOMPARE(win->windowLines(), display.lines());
    QCOMPARE(win->screen()->getLines(), display.lines());

    int lineNo = display.lines() + 10;
    int expectedFastPathFrames = 0;
    const int scrollSteps[] = {1, 4, 3}; // 连续滚动 1 行与 N 行
    for (const int n : scrollSteps) {
        const QByteArray out = buildScrollPayload(lineNo, n);
        lineNo += n;
        emu.receiveData(out.constData(), int(out.size()));
        pumpScrollFrame(win);

        // 快路径确已接管本帧（纯滚动无可观测行为差异，以命中计数为证据）
        ++expectedFastPathFrames;
        QCOMPARE(display.scrollFastPathFrameCount(), expectedFastPathFrames);

        const int band = display.lastDirtyRegion().boundingRect().height();
        QVERIFY2(band > 0 && band <= (n + 2) * display.fontHeight(),
                 qPrintable(QStringLiteral("滚动 %1 行脏区高度 %2 超出新进 N 行行带")
                            .arg(n).arg(band)));

        const QImage incremental = replayScrollFrame(display, base, n);
        const QImage full = renderFull(display);
        verifyScrollFrameEqual(incremental, full);
        base = full;
    }
}

/**
 * @brief 滚动+局部修改混合帧：滚动外修改必须触发错位回退，修改行进入脏区，
 *        增量重放与全量渲染逐像素相等。
 */
void TestRendering::testScrollMixedContentFallback()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    const QImage base = enterScrollingState(emu, win, display);

    // 一帧内：先输出 3 行（滚 3 行），再绝对定位回 moved 区改两格
    QByteArray frame = buildScrollPayload(display.lines() + 10, 3);
    frame += "\033[10;5H\033[38;5;45mXX\033[0m";
    emu.receiveData(frame.constData(), int(frame.size()));
    pumpScrollFrame(win);

    // 回退证据一：错位检测命中，快路径未接管本帧
    QCOMPARE(display.scrollFastPathFrameCount(), 0);
    // 回退证据二：视图第 10 行（0 起 9）行带必须在脏区内
    const int fh = display.fontHeight();
    const int rowTop = display.contentsRect().top() + display.margin();
    const QRect band9(0, rowTop + 9 * fh, display.width(), fh);
    QVERIFY2(display.lastDirtyRegion().intersects(band9),
             "滚动外修改行未进入脏区（错位回退失效）");

    const QImage incremental = replayScrollFrame(display, base, 3);
    const QImage full = renderFull(display);
    verifyScrollFrameEqual(incremental, full);
}

/**
 * @brief 含图滚动帧：hasImages() 回退条件命中，滚动前后两个视图的含图行都在脏区内，
 *        增量重放与全量渲染逐像素相等（行带断言镜像 tst_sixel 模式）。
 */
void TestRendering::testScrollImageFallback()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    emu.setCellPixelSize(display.cellPixelWidth(), display.cellPixelHeight());

    // 填满进入滚动态后，在屏幕中下部锚定 8x18 sixel 图（约占 2 个网格行）
    QImage base = enterScrollingState(emu, win, display);
    emu.receiveData("\033[40;1H", 6); // 光标上移到视图第 40 行，避免图像越出底边
    const QByteArray seq = QByteArray("\033Pq#0;2;100;0;0#0!8~-!8~-!8~\033\\");
    emu.receiveData(seq.constData(), int(seq.size()));
    pumpScrollFrame(win);
    base = renderFull(display);

    // 滚动前视图的含图行（图像随内容上滚，旧位置残留必须被抹除）
    Screen *scr = win->screen();
    const int oldTop = win->currentLine();
    QList<int> oldImageRows;
    for (int y = 0; y < win->windowLines(); y++)
        if (!scr->imagePlacements(oldTop + y).isEmpty())
            oldImageRows.append(y);
    QVERIFY(!oldImageRows.isEmpty()); // 防空断言：sixel 确已锚定

    // 滚 2 行：图像随内容上滚，hasImages() 使快路径回退
    const QByteArray out = buildScrollPayload(display.lines() + 10, 2);
    emu.receiveData(out.constData(), int(out.size()));
    pumpScrollFrame(win);

    // 回退证据一：含图帧快路径未接管
    QCOMPARE(display.scrollFastPathFrameCount(), 0);
    // 回退证据二：新视图含图行（补画）与滚动前视图含图行（抹除残留）都在脏区内
    const int fh = display.fontHeight();
    const int rowTop = display.contentsRect().top() + display.margin();
    const int newTop = win->currentLine();
    int checkedRows = 0;
    for (int y = 0; y < win->windowLines(); y++) {
        const bool inNewView = !scr->imagePlacements(newTop + y).isEmpty();
        const bool inOldView = oldImageRows.contains(y);
        if (!inNewView && !inOldView)
            continue;
        checkedRows++;
        const QRect band(0, rowTop + fh * y, display.width(), fh);
        QVERIFY2(display.lastDirtyRegion().intersects(band),
                 qPrintable(QStringLiteral("含图视图行 %1 未进入脏区").arg(y)));
    }
    QVERIFY(checkedRows >= 1);

    const QImage incremental = replayScrollFrame(display, base, 2);
    const QImage full = renderFull(display);
    verifyScrollFrameEqual(incremental, full);
}

/**
 * @brief 选区回退帧：selectAll 选区在滚动中被内容推移清除（moveImage 语义），
 *        反转 rendition 消退使 moved 行大面积进脏区（错位回退同理会命中），
 *        增量重放与全量渲染逐像素相等。
 */
void TestRendering::testScrollSelectionFallback()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    QImage base = enterScrollingState(emu, win, display);

    display.selectAll();
    pumpScrollFrame(win); // 选区高亮帧落账
    base = renderFull(display);

    const QByteArray out = buildScrollPayload(display.lines() + 10, 2);
    emu.receiveData(out.constData(), int(out.size()));
    pumpScrollFrame(win);

    // 回退证据一：选区帧快路径未接管（选区被滚动清除前 isClearSelection 不成立，
    // 清除后 moved 行反转 rendition 消退、错位检测同样命中）
    QCOMPARE(display.scrollFastPathFrameCount(), 0);
    // 回退证据二：选区反转消退，脏区远大于新进 2 行行带
    QVERIFY(display.lastDirtyRegion().boundingRect().height() >= 20 * display.fontHeight());

    const QImage incremental = replayScrollFrame(display, base, 2);
    const QImage full = renderFull(display);
    verifyScrollFrameEqual(incremental, full);
}

/**
 * @brief 开关 A/B：两套独立环境喂相同滚动负载，开/关快路径的增量重放结果
 *        逐像素相等且都与全量渲染一致。
 */
void TestRendering::testScrollOptimizationSwitchAB()
{
    Vt102Emulation emuA, emuB;
    ScreenWindow *winA = nullptr, *winB = nullptr;
    TerminalDisplay displayA, displayB;
    initRenderEnv(emuA, winA, displayA);
    initRenderEnv(emuB, winB, displayB);
    displayB.setScrollOptimizationEnabled(false);
    QVERIFY(displayA.isScrollOptimizationEnabled());
    QVERIFY(!displayB.isScrollOptimizationEnabled());

    QImage baseA = enterScrollingState(emuA, winA, displayA);
    QImage baseB = enterScrollingState(emuB, winB, displayB);

    for (int round = 0; round < 3; round++) {
        const QByteArray out = buildScrollPayload(displayA.lines() + 10 + round * 3, 3);
        emuA.receiveData(out.constData(), int(out.size()));
        emuB.receiveData(out.constData(), int(out.size()));
        pumpScrollFrame(winA);
        pumpScrollFrame(winB);

        const QImage incrementalA = replayScrollFrame(displayA, baseA, 3);
        const QImage incrementalB = replayScrollFrame(displayB, baseB, 3);
        const QImage full = renderFull(displayA);
        verifyScrollFrameEqual(incrementalA, full);
        verifyScrollFrameEqual(incrementalB, full);
        QCOMPARE(incrementalA, incrementalB); // 同负载同模型，两条路径应逐像素一致
        baseA = full;
        baseB = renderFull(displayB);
    }
    // 快路径在 A 上逐帧接管、在 B 上被开关关闭（无可观测行为差异，以命中计数为证据）
    QCOMPARE(displayA.scrollFastPathFrameCount(), 3);
    QCOMPARE(displayB.scrollFastPathFrameCount(), 0);
}

QTEST_MAIN(TestRendering)
#include "tst_rendering.moc"
