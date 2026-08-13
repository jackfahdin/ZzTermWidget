#include <QtTest>

#include "SixelDecoder.h"

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

QTEST_MAIN(TestSixel)
#include "tst_sixel.moc"
