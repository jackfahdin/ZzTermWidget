#include <QtTest>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
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

QTEST_MAIN(TestRendering)
#include "tst_rendering.moc"
