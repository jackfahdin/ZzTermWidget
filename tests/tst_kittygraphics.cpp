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

QTEST_MAIN(TestKittyGraphics)
#include "tst_kittygraphics.moc"
