#include <QtTest>

#include <QFontDatabase>

#include "SixelDecoder.h"
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "Screen.h"
#include "TerminalDisplay.h"

/**
 * @brief Sixel 解码器的纯逻辑单测（无终端状态，直接喂 data 段断言 QImage）。
 * @note 绘制层测试（需 QApplication 与 TerminalDisplay）也放本文件，故用 QTEST_MAIN。
 */
class TestSixel : public QObject
{
    Q_OBJECT
private slots:
    void testMinimalOnePixel();
    void testRepeatRle();
    void testPaletteRgb();
    void testPaletteHls();
    void testTransparentBackground();
    void testBackgroundFillP2();
    void testNewlineAndCarriageReturn();
    void testRasterAttributes();
    void testDimensionCapRejects();
    void testInvalidDataRejectedOrIgnored();
    void testDisplayPaintsImageUnderText();
};

/**
 * @brief 最小 1x1 图：#0 定义纯红（RGB 百分比），'@'（值 1）在 (0,0) 写 1 像素。
 */
void TestSixel::testMinimalOnePixel()
{
    const auto result = SixelDecoder::decode("#0;2;100;0;0#0@", 1);
    QVERIFY(result.has_value());
    QCOMPARE(result->image.size(), QSize(1, 1));
    QCOMPARE(result->image.pixelColor(0, 0), QColor(255, 0, 0));
    QVERIFY(result->transparentBackground); // P2=1：透明底
}

/**
 * @brief RLE：!5@ 即 '@' 重复 5 次，得 5x1 横条。
 */
void TestSixel::testRepeatRle()
{
    const auto result = SixelDecoder::decode("#0;2;100;0;0#0!5@", 1);
    QVERIFY(result.has_value());
    QCOMPARE(result->image.size(), QSize(5, 1));
    for (int x = 0; x < 5; x++)
        QCOMPARE(result->image.pixelColor(x, 0), QColor(255, 0, 0));
}

/**
 * @brief RGB 百分比取整：50% → 127（50*255/100）。
 */
void TestSixel::testPaletteRgb()
{
    const auto result = SixelDecoder::decode("#1;2;50;100;0#1@", 1);
    QVERIFY(result.has_value());
    QCOMPARE(result->image.pixelColor(0, 0), QColor(127, 255, 0));
}

/**
 * @brief HLS 色彩空间：色相 0（红）、亮度 50%、饱和度 100% → 纯红。
 */
void TestSixel::testPaletteHls()
{
    const auto result = SixelDecoder::decode("#2;1;0;50;100#2@", 1);
    QVERIFY(result.has_value());
    QCOMPARE(result->image.pixelColor(0, 0), QColor(255, 0, 0));
}

/**
 * @brief 透明底（P2=1）：光栅声明 2x1，未覆盖的 (1,0) 保持全透明。
 */
void TestSixel::testTransparentBackground()
{
    const auto result = SixelDecoder::decode("\"1;1;2;1#0;2;100;0;0#0@", 1);
    QVERIFY(result.has_value());
    QCOMPARE(result->image.size(), QSize(2, 1));
    QCOMPARE(result->image.pixelColor(1, 0).alpha(), 0);
}

/**
 * @brief P2=2 且 0 号色已定义：透明区域以 0 号色填底。
 */
void TestSixel::testBackgroundFillP2()
{
    // #0 蓝（填底色）、#1 红（数据色）；'@' 用当前色 1 写 (0,0)，(1,0) 由填底覆盖
    const auto result = SixelDecoder::decode("\"1;1;2;1#0;2;0;0;100#1;2;100;0;0#1@", 2);
    QVERIFY(result.has_value());
    QVERIFY(!result->transparentBackground);
    QCOMPARE(result->image.pixelColor(0, 0), QColor(255, 0, 0)); // 数据写入
    QCOMPARE(result->image.pixelColor(1, 0), QColor(0, 0, 255)); // 0 号色填底
}

/**
 * @brief '-' 换带（y+=6）与 '$' 回车（x 归 0）。
 */
void TestSixel::testNewlineAndCarriageReturn()
{
    const auto nl = SixelDecoder::decode("#0;2;100;0;0#0@-@", 1);
    QVERIFY(nl.has_value());
    QCOMPARE(nl->image.size(), QSize(1, 7)); // 第二像素在 y=6，高度按最高置位位精确计算
    QCOMPARE(nl->image.pixelColor(0, 0), QColor(255, 0, 0));
    QCOMPARE(nl->image.pixelColor(0, 6), QColor(255, 0, 0));
    QCOMPARE(nl->image.pixelColor(0, 3).alpha(), 0); // 带间未写区域透明

    const auto cr = SixelDecoder::decode("#0;2;100;0;0#0@$~", 1);
    QVERIFY(cr.has_value());
    QCOMPARE(cr->image.size(), QSize(1, 6)); // '$' 归 x=0，'~'（63）覆盖同列 6 行
    QCOMPARE(cr->image.pixelColor(0, 5), QColor(255, 0, 0));
}

/**
 * @brief 光栅属性 "Pan;Pad;Ph;Pv：无像素数据时按声明尺寸产出全透明图。
 */
void TestSixel::testRasterAttributes()
{
    const auto result = SixelDecoder::decode("\"1;1;10;20", 1);
    QVERIFY(result.has_value());
    QCOMPARE(result->image.size(), QSize(10, 20));
    QCOMPARE(result->image.pixelColor(9, 19).alpha(), 0);
}

/**
 * @brief 资源上限：宽/高超 4096 解码失败（调用方静默丢弃）；边界值可通过。
 */
void TestSixel::testDimensionCapRejects()
{
    QVERIFY(!SixelDecoder::decode("\"1;1;4097;10", 1).has_value());
    QVERIFY(!SixelDecoder::decode("\"1;1;10;4097", 1).has_value());
    QVERIFY(SixelDecoder::decode("\"1;1;4096;4096", 1).has_value());
}

/**
 * @brief 非法输入：空数据/无尺寸无像素 → 失败；混杂垃圾字节被忽略，有效部分照常解析。
 */
void TestSixel::testInvalidDataRejectedOrIgnored()
{
    QVERIFY(!SixelDecoder::decode("", 1).has_value());
    QVERIFY(!SixelDecoder::decode("###;;", 1).has_value());
    const auto result = SixelDecoder::decode("\x01\x02#0;2;100;0;0\x7F#0@", 1);
    QVERIFY(result.has_value());
    QCOMPARE(result->image.size(), QSize(1, 1));
}

/**
 * @brief 构造 sixel 渲染测试环境（镜像 tst_rendering 的 initRenderEnv，另同步单元格像素尺寸）。
 */
static void initSixelRenderEnv(Vt102Emulation &emu, ScreenWindow *&win, TerminalDisplay &display)
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
    emu.setCellPixelSize(display.cellPixelWidth(), display.cellPixelHeight());
}

/**
 * @brief 绘制层：8x18 纯红 sixel 图渲染进显示组件（1:1 设备像素），清行后消失。
 * @note 只抽查红色像素包围盒尺寸（8x18 确定），不逐像素锁整张，避免字体环境敏感。
 */
void TestSixel::testDisplayPaintsImageUnderText()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initSixelRenderEnv(emu, win, display);

    // 3 个位带各 '!8~'：8 列 x 18 行纯红（'~'=63 置满 6 行，'-' 换带）
    const QByteArray seq = QByteArray("\033Pq#0;2;100;0;0#0!8~-!8~-!8~\033\\");
    emu.receiveData(seq.constData(), int(seq.size()));
    display.updateImage(); // 同步触发脏区计算（绕过 bufferedUpdate 定时器）

    QImage frame(display.size(), QImage::Format_ARGB32);
    frame.fill(Qt::black);
    display.render(&frame);

    // 提取红色像素包围盒：应为 8x18（1:1 设备像素映射；位置随边距浮动，不锁定）
    const auto isRed = [](const QColor &c) {
        return c.red() > 200 && c.green() < 60 && c.blue() < 60;
    };
    int minX = frame.width(), minY = frame.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < frame.height(); y++)
        for (int x = 0; x < frame.width(); x++)
            if (isRed(frame.pixelColor(x, y))) {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
                minY = qMin(minY, y);
                maxY = qMax(maxY, y);
            }
    QVERIFY(maxX >= 0); // 存在红色区域
    QCOMPARE(maxX - minX + 1, 8);
    QCOMPARE(maxY - minY + 1, 18);

    // 清行销毁：图像各行引用随 CSI 2 K 释放，重绘后红色消失
    const QByteArray clearSeq = QByteArray("\033[H\033[2K\033[B\033[2K\033[B\033[2K\033[B\033[2K");
    emu.receiveData(clearSeq.constData(), int(clearSeq.size()));
    QVERIFY(win->screen()->graphicsDirty()); // 清行销毁引用必须置位图形脏标志，否则实机残留图像
    display.updateImage();
    QVERIFY(!win->screen()->graphicsDirty()); // updateImage 消费并清除标志
    QImage cleared(display.size(), QImage::Format_ARGB32);
    cleared.fill(Qt::black);
    display.render(&cleared);
    for (int y = 0; y < cleared.height(); y++)
        for (int x = 0; x < cleared.width(); x++)
            QVERIFY(!isRed(cleared.pixelColor(x, y)));
}

QTEST_MAIN(TestSixel)
#include "tst_sixel.moc"
