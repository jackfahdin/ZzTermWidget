#include <QtTest>

#include <QBuffer>

#include "KittyGraphicsParser.h"
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "Screen.h"
#include "TerminalDisplay.h"

/**
 * @brief kitty 图形协议测试辅助：构造一条 APC 序列（ESC _ G keys ; payload ESC \）。
 */
static QByteArray kittySeq(const QByteArray &keys, const QByteArray &payload = {})
{
    QByteArray s = "\033_G";
    s += keys;
    if (!payload.isEmpty()) {
        s += ';';
        s += payload;
    }
    s += "\033\\";
    return s;
}

/** @brief 单个 RGBA 像素字节串。 */
static QByteArray rgbaPixel(quint8 r, quint8 g, quint8 b, quint8 a = 255)
{
    const char px[] = {char(r), char(g), char(b), char(a)};
    return QByteArray(px, 4);
}

/** @brief RFC 1950 zlib 流（去掉 qCompress 的 4 字节长度前缀）。 */
static QByteArray zlibStream(const QByteArray &raw)
{
    return qCompress(raw).mid(4);
}

/** @brief 生成最小 PNG（2x1，左红右蓝）。 */
static QByteArray tinyPng()
{
    QImage img(2, 1, QImage::Format_ARGB32);
    img.setPixelColor(0, 0, QColor(255, 0, 0));
    img.setPixelColor(1, 0, QColor(0, 0, 255));
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return png;
}

/**
 * @brief Kitty 图形协议测试套件：解析器纯逻辑、字节流执行、生命周期、离屏渲染。
 */
class TestKittyGraphics : public QObject
{
    Q_OBJECT
private slots:
    // 解析器（任务 1）
    void testParseKeysAndDefaults();
    void testUnknownKeysIgnored();
    void testParseRgbaPixels();
    void testParseRgbPixels();
    void testParsePng();
    void testZlibInflate();
    void testPngCompressedMissingS();
    void testChunkedTransfer();
    void testChunkInterruptedByNewCommand();
    void testMalformedInputs();
    void testDimensionCap();
    // Screen 级生命周期（任务 2）
    void testScreenPlaceDefaultsAndRefs();
    void testScreenPlacementSurvivesScrollIntoHistory();
    void testScreenClearLineDestroysPlacement();
    void testScreenResizeCropsPlacements();
    void testScreenResetClearsAll();
    void testScreenDeleteVariants();
    void testScreenReplaceSameImageAndPlacement();
    void testScreenAnonymousImageFreedWithPlacement();
};

void TestKittyGraphics::testParseKeysAndDefaults()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    // 无负载的 a=p 命令：键值全量解析 + 缺省值
    QCOMPARE(parser.feed("a=p,i=42,p=3,x=1,y=2,w=10,h=20,X=1,Y=2,c=4,r=5,z=-3,C=1,q=1",
                         256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Ready);
    const KittyCommand &c = r.command;
    QCOMPARE(c.action, 'p');
    QCOMPARE(c.imageId, 42u);
    QCOMPARE(c.placementId, 3u);
    QCOMPARE(c.srcX, 1); QCOMPARE(c.srcY, 2);
    QCOMPARE(c.srcW, 10); QCOMPARE(c.srcH, 20);
    QCOMPARE(c.cellXOff, 1); QCOMPARE(c.cellYOff, 2);
    QCOMPARE(c.cols, 4); QCOMPARE(c.rows, 5);
    QCOMPARE(c.zIndex, -3);
    QVERIFY(c.cursorNoMove);
    QCOMPARE(c.quiet, 1);
    // 缺省值
    QCOMPARE(c.medium, 'd');
    QCOMPARE(c.format, 32);
    QVERIFY(!c.compressed);
    QVERIFY(c.image.isNull()); // a=p 不解码负载
}

void TestKittyGraphics::testUnknownKeysIgnored()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    // U=（Unicode 占位符）、I=（图像编号）、P/Q/H/V（相对放置）等一律静默忽略
    const QByteArray payload = rgbaPixel(255, 0, 0).toBase64();
    QCOMPARE(parser.feed("a=T,f=32,s=1,v=1,i=1,U=1,I=99,P=1,Q=2,H=3,V=4,zz=abc;" + payload,
                         256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Ready);
    QCOMPARE(r.command.image.size(), QSize(1, 1));
}

void TestKittyGraphics::testParseRgbaPixels()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    const QByteArray raw = rgbaPixel(255, 0, 0) + rgbaPixel(0, 255, 0, 128);
    QCOMPARE(parser.feed("a=T,f=32,s=2,v=1,i=7;" + raw.toBase64(), 256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Ready);
    QCOMPARE(r.command.imageId, 7u);
    QCOMPARE(r.command.image.size(), QSize(2, 1));
    QCOMPARE(r.command.image.pixelColor(0, 0), QColor(255, 0, 0, 255));
    QCOMPARE(r.command.image.pixelColor(1, 0), QColor(0, 255, 0, 128));
}

void TestKittyGraphics::testParseRgbPixels()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    const QByteArray raw = rgbaPixel(255, 0, 0).left(3) + rgbaPixel(0, 0, 255).left(3);
    QCOMPARE(parser.feed("a=T,f=24,s=2,v=1,i=8;" + raw.toBase64(), 256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Ready);
    QCOMPARE(r.command.image.size(), QSize(2, 1));
    QCOMPARE(r.command.image.pixelColor(0, 0), QColor(255, 0, 0));
    QCOMPARE(r.command.image.pixelColor(1, 0), QColor(0, 0, 255));
}

void TestKittyGraphics::testParsePng()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    QCOMPARE(parser.feed("a=T,f=100,i=9;" + tinyPng().toBase64(), 256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Ready);
    QCOMPARE(r.command.image.size(), QSize(2, 1));
    QCOMPARE(r.command.image.pixelColor(0, 0), QColor(255, 0, 0));
    QCOMPARE(r.command.image.pixelColor(1, 0), QColor(0, 0, 255));
}

void TestKittyGraphics::testZlibInflate()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    const QByteArray raw = rgbaPixel(1, 2, 3, 4) + rgbaPixel(5, 6, 7, 8);
    QCOMPARE(parser.feed("a=T,f=32,s=2,v=1,i=10,o=z;" + zlibStream(raw).toBase64(),
                         256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Ready);
    QCOMPARE(r.command.image.pixelColor(0, 0), QColor(1, 2, 3, 4));
    QCOMPARE(r.command.image.pixelColor(1, 0), QColor(5, 6, 7, 8));
}

void TestKittyGraphics::testPngCompressedMissingS()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    // f=100 + o=z 缺 S 键：EINVAL（上游规定压缩 PNG 必须提供 S）
    QCOMPARE(parser.feed("a=T,f=100,i=11,o=z;" + zlibStream(tinyPng()).toBase64(),
                         256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Error);
    QCOMPARE(r.errorCode, QByteArray("EINVAL"));
    QCOMPARE(r.imageId, 11u); // 能定位 i= 时必须带回，供应答
    // 提供 S 后成功
    KittyGraphicsParser parser2;
    KittyGraphicsParser::Result r2;
    const QByteArray keys = QByteArray("a=T,f=100,i=11,o=z,S=")
                            + QByteArray::number(tinyPng().size());
    QCOMPARE(parser2.feed(keys + ";" + zlibStream(tinyPng()).toBase64(),
                          256LL * 1024 * 1024, r2),
             KittyGraphicsParser::Status::Ready);
    QCOMPARE(r2.command.image.size(), QSize(2, 1));
}

void TestKittyGraphics::testChunkedTransfer()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    const QByteArray raw = rgbaPixel(9, 9, 9) + rgbaPixel(8, 8, 8);
    const QByteArray b64 = raw.toBase64();
    const int half = b64.size() / 2 * 2; // 保持 4 的倍数切分（本例 16 字节，半长 8）
    // 首块：全部控制键 + m=1
    QCOMPARE(parser.feed("a=T,f=32,s=2,v=1,i=12,m=1;" + b64.left(half),
                         256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::NeedMore);
    QVERIFY(parser.midChunk());
    // 续块：仅 m/q 与负载
    QCOMPARE(parser.feed("m=0;" + b64.mid(half), 256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Ready);
    QVERIFY(!parser.midChunk());
    QCOMPARE(r.command.imageId, 12u);
    QCOMPARE(r.command.image.pixelColor(0, 0), QColor(9, 9, 9));
    QCOMPARE(r.command.image.pixelColor(1, 0), QColor(8, 8, 8));
}

void TestKittyGraphics::testChunkInterruptedByNewCommand()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    const QByteArray b64 = rgbaPixel(1, 1, 1).toBase64();
    QCOMPARE(parser.feed("a=T,f=32,s=1,v=1,i=13,m=1;" + b64.left(4),
                         256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::NeedMore);
    // 分块流被带控制键的新命令打断：丢弃半成品，按新命令处理
    QCOMPARE(parser.feed("a=T,f=32,s=1,v=1,i=14;" + rgbaPixel(2, 2, 2).toBase64(),
                         256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Ready);
    QCOMPARE(r.command.imageId, 14u);
    QCOMPARE(r.command.image.pixelColor(0, 0), QColor(2, 2, 2));
}

void TestKittyGraphics::testMalformedInputs()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    // f=32 缺 s/v：EINVAL
    QCOMPARE(parser.feed("a=T,f=32,i=15;" + rgbaPixel(0, 0, 0).toBase64(),
                         256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Error);
    QCOMPARE(r.errorCode, QByteArray("EINVAL"));
    QCOMPARE(r.errorMessage, QByteArray("missing size"));
    // 负载长度与 s*v*bpp 不符：EINVAL
    KittyGraphicsParser p2;
    KittyGraphicsParser::Result r2;
    QCOMPARE(p2.feed("a=T,f=32,s=3,v=3,i=16;" + rgbaPixel(0, 0, 0).toBase64(),
                     256LL * 1024 * 1024, r2),
             KittyGraphicsParser::Status::Error);
    QCOMPARE(r2.errorCode, QByteArray("EINVAL"));
    // 损坏的 PNG：EINVAL decode failed
    KittyGraphicsParser p3;
    KittyGraphicsParser::Result r3;
    QCOMPARE(p3.feed("a=T,f=100,i=17;" + QByteArray("not-a-png").toBase64(),
                     256LL * 1024 * 1024, r3),
             KittyGraphicsParser::Status::Error);
    QCOMPARE(r3.errorCode, QByteArray("EINVAL"));
    QCOMPARE(r3.errorMessage, QByteArray("decode failed"));
    // 预算不足：ENOSPC（解码前预检，不分配大缓冲）
    KittyGraphicsParser p4;
    KittyGraphicsParser::Result r4;
    QCOMPARE(p4.feed("a=T,f=32,s=100,v=100,i=18;" + QByteArray(100 * 100 * 4, 0).toBase64(),
                     100, r4),
             KittyGraphicsParser::Status::Error);
    QCOMPARE(r4.errorCode, QByteArray("ENOSPC"));
    // 无 i= 的畸形命令：Error 且 imageId 为 0（调用方静默忽略）
    KittyGraphicsParser p5;
    KittyGraphicsParser::Result r5;
    QCOMPARE(p5.feed("a=T,f=32;" + rgbaPixel(0, 0, 0).toBase64(),
                     256LL * 1024 * 1024, r5),
             KittyGraphicsParser::Status::Error);
    QCOMPARE(r5.imageId, 0u);
}

void TestKittyGraphics::testDimensionCap()
{
    KittyGraphicsParser parser;
    KittyGraphicsParser::Result r;
    // 宽超 10000：EINVAL image too large（负载长度不符会在尺寸检查之后，先报尺寸错）
    QCOMPARE(parser.feed("a=T,f=32,s=10001,v=1,i=19;" + QByteArray(4, 0).toBase64(),
                         256LL * 1024 * 1024, r),
             KittyGraphicsParser::Status::Error);
    QCOMPARE(r.errorMessage, QByteArray("image too large"));
}

/** @brief 构造纯色 ARGB32 测试图。 */
static QImage solidImage(int w, int h, const QColor &color)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(color);
    return img;
}

void TestKittyGraphics::testScreenPlaceDefaultsAndRefs()
{
    Screen scr(24, 80);
    scr.setScroll(HistoryTypeBuffer(1000));
    QVERIFY(scr.kittyStoreImage(solidImage(16, 32, Qt::red), 42)); // 恰好 2x2 单元格（默认 8x16）
    const quint32 handle = scr.kittyImageHandle(42);
    QVERIFY(handle != 0);
    quint32 ph = 0;
    int cols = 0, rows = 0;
    QCOMPARE(scr.kittyPlace(handle, 42, KittyPlacementParams{}, &ph, &cols, &rows),
             KittyPlaceError::Ok);
    QCOMPARE(cols, 2); // 16px / 8px
    QCOMPARE(rows, 2); // 32px / 16px
    QVERIFY(ph != 0);
    // 行级引用挂在放置覆盖的每一行（锚定行 rowOffset=0）
    QCOMPARE(scr.kittyRefs(0).size(), 1);
    QCOMPARE(scr.kittyRefs(1).size(), 1);
    QCOMPARE(scr.kittyRefs(0).at(0).rowOffset, 0);
    QCOMPARE(scr.kittyRefs(1).at(0).rowOffset, 1);
    QCOMPARE(scr.kittyRefs(2).size(), 0);
    const KittyPlacement *pl = scr.kittyPlacement(ph);
    QVERIFY(pl != nullptr);
    QCOMPARE(pl->imageId, 42u);
    QCOMPARE(pl->anchorLine, 0);
    QCOMPARE(pl->col, 0);
    QVERIFY(scr.hasImages());
}

void TestKittyGraphics::testScreenPlacementSurvivesScrollIntoHistory()
{
    Screen scr(24, 80);
    scr.setScroll(HistoryTypeBuffer(1000));
    QVERIFY(scr.kittyStoreImage(solidImage(8, 16, Qt::red), 1));
    quint32 ph = 0;
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(1), 1, KittyPlacementParams{}, &ph),
             KittyPlaceError::Ok);
    // 30 次 index()：首行滚入历史，放置引用随行迁移（绝对行坐标不变仍为 0）
    for (int i = 0; i < 30; i++)
        scr.index();
    QVERIFY(scr.getHistLines() > 0);
    QCOMPARE(scr.kittyRefs(0).size(), 1);
    QCOMPARE(scr.kittyPlacement(ph), scr.kittyPlacement(ph)); // 放置存活
    QVERIFY(scr.kittyPlacement(ph) != nullptr);
    // 废弃历史（clearHistory 路径）：历史中的引用销毁，放置死亡；图像数据仍在
    scr.setScroll(HistoryTypeNone(), false);
    QCOMPARE(scr.kittyRefs(0).size(), 0);
    QVERIFY(scr.kittyPlacement(ph) == nullptr);
    QVERIFY(scr.hasKittyImage(1)); // a=t/T 落库图像不因放置消失而释放
}

void TestKittyGraphics::testScreenClearLineDestroysPlacement()
{
    Screen scr(24, 80);
    QVERIFY(scr.kittyStoreImage(solidImage(8, 16, Qt::red), 2));
    quint32 ph = 0;
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(2), 2, KittyPlacementParams{}, &ph),
             KittyPlaceError::Ok);
    scr.clearEntireLine(); // 光标在第 0 行：清行连带销毁该行引用
    QCOMPARE(scr.kittyRefs(0).size(), 0);
    QVERIFY(scr.kittyPlacement(ph) == nullptr); // 全部行引用销毁 → 放置回收
    QVERIFY(scr.graphicsDirty());               // 引用销毁必须置图形脏标志
}

void TestKittyGraphics::testScreenResizeCropsPlacements()
{
    Screen scr(24, 80);
    scr.setCursorYX(21, 0); // 放置在第 20 行（setCursorYX 为 1 基坐标）
    QVERIFY(scr.kittyStoreImage(solidImage(8, 16, Qt::red), 3));
    quint32 ph = 0;
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(3), 3, KittyPlacementParams{}, &ph),
             KittyPlaceError::Ok);
    QCOMPARE(scr.kittyRefs(20).size(), 1);
    scr.setCursorYX(1, 0); // 光标移离放置行，避免 resize 保焦路径把该行滚回可见区
    scr.resizeImage(10, 80); // 收缩：第 20 行被裁，其引用直接销毁（无历史可入）
    QVERIFY(scr.kittyPlacement(ph) == nullptr);
}

void TestKittyGraphics::testScreenResetClearsAll()
{
    Screen scr(24, 80);
    QVERIFY(scr.kittyStoreImage(solidImage(8, 16, Qt::red), 4));
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(4), 4, KittyPlacementParams{}),
             KittyPlaceError::Ok);
    scr.reset();
    QVERIFY(!scr.hasKittyImage(4));
    QVERIFY(scr.kittyRefs(0).isEmpty());
    QVERIFY(!scr.hasImages());
}

void TestKittyGraphics::testScreenDeleteVariants()
{
    Screen scr(24, 80);
    QVERIFY(scr.kittyStoreImage(solidImage(8, 16, Qt::red), 10));
    QVERIFY(scr.kittyStoreImage(solidImage(8, 16, Qt::blue), 11));
    // 图像 10 两个放置（行 0 与行 5），图像 11 一个放置（行 10）
    KittyPlacementParams p;
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(10), 10, p), KittyPlaceError::Ok);
    scr.setCursorYX(6, 0); // 第 5 行（setCursorYX 为 1 基坐标）
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(10), 10, p), KittyPlaceError::Ok);
    scr.setCursorYX(11, 0); // 第 10 行
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(11), 11, p), KittyPlaceError::Ok);

    // d=i + p：只删指定 (i,p) 放置
    KittyPlacementParams withPid;
    withPid.placementId = 7;
    scr.setCursorYX(13, 0); // 第 12 行
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(11), 11, withPid), KittyPlaceError::Ok);
    scr.kittyDeleteByImage(11, 7, false);
    QCOMPARE(scr.kittyRefs(12).size(), 0); // (11,7) 放置已删
    QCOMPARE(scr.kittyRefs(10).size(), 1); // 同图像匿名放置保留
    QVERIFY(scr.hasKittyImage(11));

    // d=i（小写）：删图像 10 全部放置，数据保留
    scr.kittyDeleteByImage(10, 0, false);
    QCOMPARE(scr.kittyRefs(0).size(), 0);
    QCOMPARE(scr.kittyRefs(5).size(), 0);
    QVERIFY(scr.hasKittyImage(10));

    // d=I（大写）：连同释放无引用图像数据
    scr.kittyDeleteByImage(10, 0, true);
    QVERIFY(!scr.hasKittyImage(10));

    // d=c：删除与光标单元格相交的放置
    scr.setCursorYX(11, 0); // 光标置于第 10 行
    scr.kittyDeleteAtCursor(false);
    QCOMPARE(scr.kittyRefs(10).size(), 0);
    QVERIFY(scr.hasKittyImage(11)); // 小写不释放数据

    // d=A：删除全部可见放置并释放无引用图像
    scr.setCursorYX(16, 0); // 第 15 行
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(11), 11, p), KittyPlaceError::Ok);
    scr.kittyDeleteAll(true);
    QVERIFY(!scr.hasKittyImage(11));
    QVERIFY(!scr.hasImages());
}

void TestKittyGraphics::testScreenReplaceSameImageAndPlacement()
{
    Screen scr(24, 80);
    QVERIFY(scr.kittyStoreImage(solidImage(8, 16, Qt::red), 20));
    KittyPlacementParams p;
    p.placementId = 3;
    quint32 ph1 = 0;
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(20), 20, p, &ph1), KittyPlaceError::Ok);
    // 同 (i=20, p=3) 在新位置重复放置 = 替换：旧放置消失
    scr.setCursorYX(9, 5); // 第 8 行第 4 列（setCursorYX 为 1 基坐标）
    quint32 ph2 = 0;
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(20), 20, p, &ph2), KittyPlaceError::Ok);
    QVERIFY(ph2 != ph1);
    QVERIFY(scr.kittyPlacement(ph1) == nullptr);
    QCOMPARE(scr.kittyRefs(0).size(), 0);
    QCOMPARE(scr.kittyRefs(8).size(), 1);
    QCOMPARE(scr.kittyPlacement(ph2)->col, 4);
}

void TestKittyGraphics::testScreenAnonymousImageFreedWithPlacement()
{
    Screen scr(24, 80);
    quint32 handle = 0;
    QVERIFY(scr.kittyStoreImage(solidImage(8, 16, Qt::red), 0, &handle)); // i=0 匿名
    QVERIFY(handle != 0);
    QCOMPARE(scr.kittyImageHandle(0), 0u); // 匿名不占 id 命名空间
    QCOMPARE(scr.kittyPlace(handle, 0, KittyPlacementParams{}), KittyPlaceError::Ok);
    scr.clearEntireLine(); // 销毁唯一放置 → 匿名图像数据随之释放
    QVERIFY(scr.image(handle) == nullptr);
    QVERIFY(!scr.hasImages());
}

QTEST_MAIN(TestKittyGraphics)
#include "tst_kittygraphics.moc"
