# Kitty 图形协议（核心子集）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 为 ZzQTermWidget 接入 Kitty 图形协议核心子集（APC 通道、5 种动作、3 种像素格式、zlib 压缩、放置/删除/应答/重传语义、z-index 双层叠加绘制），与 Sixel 共享统一图形锚定层和 256MB 像素预算。

**架构：**
```
APC 字节流 → Vt102Emulation APC 累积通道（新增，镜像 DCS sixel 通道）
           → KittyGraphicsParser（纯逻辑：键值/分块重组/base64/zlib/像素解码）
           → Screen 统一图形锚定层（泛化图像表 + kitty 放置表 + 共享生命周期挂钩）
           → TerminalDisplay 双层叠加（文本下层：sixel + z<0；文本上层：z≥0，光标复绘）
```

**技术栈：** Qt 6.11.1（Core/Gui/Widgets/Test，qUncompress 无新依赖）、C++20、CMake、QTest。

**规格依据：** `docs/superpowers/specs/2026-08-13-kitty-graphics-design.md`（已批准）。本计划不引入规格之外的特性。

**通用约定：**
- 所有新代码注释为中文 Doxygen 风格；`lib/third_party/` 不动；`lib/include/qtermwidget.h` 不动。
- 构建：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64 && cmake --build build --parallel`
- 测试：`ctest --test-dir build --output-on-failure`
- 提交：约定式提交，中文描述。git 操作需用户确认后执行。

---

## 命名总表（跨任务一致性自检基准）

实现时所有任务必须严格使用以下名字；完成后逐项核对。

**新文件：**
- `lib/src/emulation/KittyGraphicsParser.h` / `lib/src/emulation/KittyGraphicsParser.cpp`
- `tests/tst_kittygraphics.cpp`

**解析器（任务 1）：**
- `struct KittyCommand`：字段 `action(char, 缺省 'T')`、`medium(char, 缺省 'd')`、`format(int, 缺省 32)`、`compressed(bool)`、`imageId(quint32)`、`placementId(quint32)`、`deleteWhat(char, 缺省 0)`、`quiet(int)`、`width/height(int, s=/v=)`、`pngSize(qint64, S=)`、`srcX/srcY/srcW/srcH(int)`、`cellXOff/cellYOff(int, X=/Y=)`、`cols/rows(int, c=/r=)`、`zIndex(qint32)`、`cursorNoMove(bool, C=1)`、`image(QImage)`。
- `class KittyGraphicsParser`：`enum class Status { NeedMore, Ready, Error }`；`struct Result { KittyCommand command; QByteArray errorCode; QByteArray errorMessage; quint32 imageId = 0; quint32 placementId = 0; int quiet = 0; }`；`static constexpr int MAX_DIMENSION = 10000;`；`Status feed(const QByteArray &chunk, qint64 budgetRemaining, Result &out);`；`void reset();`；`bool midChunk() const;`

**Screen 泛化改名（任务 2）：**
- `SixelImage` → `ScreenImage`（字段 `image`、`transparentBackground` 不变）
- `Screen::sixelImage(quint32)` → `Screen::image(quint32)`
- `_sixelImages` → `_images`；`_nextImageId` → `_nextImageHandle`
- 保留不改名：`ImagePlacement`、`ImageRefLine`、`_imageLines`、`_historyImages`、`_imageRefs`、`_imageBytes`、`MAX_IMAGE_BYTES`、`anchorImage()`、`imagePlacements()`、`releaseImageLine()`、`clearAllImages()`

**Screen kitty 新增（任务 2）：**
- `struct KittyPlacementParams { quint32 placementId = 0; int srcX=0,srcY=0,srcW=0,srcH=0; int cellXOff=0,cellYOff=0; int cols=0,rows=0; qint32 zIndex=0; }`
- `struct KittyPlacement { quint32 imageHandle; quint32 imageId; quint32 placementId; int anchorLine; int col; int cols,rows; int srcX,srcY,srcW,srcH; int cellXOff,cellYOff; qint32 zIndex; quint64 serial; }`
- `struct KittyPlacementRef { quint32 placementHandle; int rowOffset; }`，`typedef QVector<KittyPlacementRef> KittyRefLine;`
- `enum class KittyPlaceError { Ok, NoSuchImage, InvalidArgument, BudgetExceeded };`
- 方法：`qint64 imageBytesRemaining() const`、`bool kittyStoreImage(const QImage&, quint32 clientId, quint32 *handleOut=nullptr)`、`quint32 kittyImageHandle(quint32 clientId) const`、`bool hasKittyImage(quint32 clientId) const`、`KittyPlaceError kittyPlace(quint32 imageHandle, quint32 clientId, const KittyPlacementParams&, quint32 *placementHandleOut=nullptr, int *colsUsed=nullptr, int *rowsUsed=nullptr)`、`void kittyDeleteAll(bool freeData)`、`void kittyDeleteByImage(quint32 clientId, quint32 placementId, bool freeData)`、`void kittyDeleteAtCursor(bool freeData)`、`QVector<KittyPlacementRef> kittyRefs(int absoluteLine) const`、`const KittyPlacement *kittyPlacement(quint32 placementHandle) const`
- 私有：`_kittyLines`（`KittyRefLine*`，[lines+1]）、`_historyKittyRefs`（`std::deque<KittyRefLine>`）、`_kittyPlacements`（`QHash<quint32,KittyPlacement>`）、`_kittyPlacementRefs`（`QHash<quint32,int>`）、`_kittyPlacementKeys`（`QHash<quint64,quint32>`，key=`(quint64(clientId)<<32)|placementId`）、`_kittyImageHandles`（`QHash<quint32,quint32>`，clientId→imageHandle）、`_kittyAnonymous`（`QSet<quint32>`）、`_kittyEvictionOrder`（`QList<quint32>`）、`_nextKittyPlacementHandle=1`、`_nextKittySerial=1`、`DEFAULT_CELL_PIXEL_WIDTH=8`
- 私有方法：`releaseKittyRefLine(KittyRefLine&)`、`removeKittyPlacement(quint32)`、`removeKittyImage(quint32 imageHandle)`、`kittyImageInUse(quint32 imageHandle) const`、`evictUnreferencedKittyImages(qint64 bytesNeeded)`

**Vt102Emulation（任务 3）：**
- `static constexpr qint64 MAX_APC_DATA_LENGTH = 350LL*1024*1024;`
- `_apcActive`、`_apcOverflow`、`_apcEscPending`（bool）、`_apcData`（QByteArray）、`_kittyParser`（KittyGraphicsParser 成员）
- `finishApc()`、`abortApc()`、`executeKittyCommand(const KittyGraphicsParser::Result&)`、`sendKittyResponse(quint32 imageId, quint32 placementId, bool includePlacement, bool ok, const QByteArray &error)`

**TerminalDisplay（任务 4）：**
- `drawSixelImages` → `drawImagesBelowText`（泛化：sixel 切片 + kitty z<0）
- 新增 `drawImagesAboveText(QPainter&, const QRect&)`、`drawKittyPlacements(QPainter&, const QRect&, bool aboveText)`（私有实现）、`redrawCursorOverImages(QPainter&)`

**应答字节格式（精确串，测试按字节断言）：**
- 成功：`\x1b_Gi=<id>;OK\x1b\\`；带放置 id：`\x1b_Gi=<id>,p=<pid>;OK\x1b\\`
- 失败：`\x1b_Gi=<id>;<code>:<msg>\x1b\\`（不带 p=）
- 错误串全集：`ENOENT:no such image`、`EINVAL:unsupported medium`、`EINVAL:unsupported action`、`EINVAL:missing size`、`EINVAL:missing S`、`EINVAL:image too large`、`EINVAL:decode failed`、`EINVAL:bad placement`、`ENOSPC:pixel budget exceeded`

---

## 任务 1：KittyGraphicsParser 纯逻辑解析器 + 单测

**文件：**
- 新建 `lib/src/emulation/KittyGraphicsParser.h`
- 新建 `lib/src/emulation/KittyGraphicsParser.cpp`
- 新建 `tests/tst_kittygraphics.cpp`（本轮先含解析器测试，后续任务往同一文件追加）
- 修改 `lib/CMakeLists.txt`、`tests/CMakeLists.txt`

- [ ] **步骤 1.1：写失败测试（解析器全部用例）**

创建 `tests/tst_kittygraphics.cpp`，完整内容如下（本任务只跑其中解析器部分，后续任务在此文件追加）：

```cpp
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
```

在 `tests/CMakeLists.txt` 的 `QTERMWIDGET_TESTS` 列表（`tst_sixel` 之后）追加一行：

```cmake
    tst_kittygraphics
```

运行（预期编译失败：KittyGraphicsParser.h 不存在）：

```bash
cmake --build build --parallel 2>&1 | tail -5
```

预期输出含：`fatal error: KittyGraphicsParser.h: No such file or directory`

- [ ] **步骤 1.2：实现 KittyGraphicsParser.h**

新建 `lib/src/emulation/KittyGraphicsParser.h`，完整内容：

```cpp
#ifndef KITTYGRAPHICSPARSER_H
#define KITTYGRAPHICSPARSER_H

#include <QByteArray>
#include <QImage>

/**
 * @brief 单条 kitty 图形命令（APC "ESC _ G keys ; payload ESC \"）的解析结果。
 *
 * 仅保留核心子集键；未知键（U=/I=/P/Q/H/V 等）按协议静默忽略，不进本结构。
 */
struct KittyCommand {
    char action = 'T';          ///< a=：t/T/p/d/q；缺省 T（传输并显示）
    char medium = 'd';          ///< t=：传输介质；仅 d（直接）受支持
    int format = 32;            ///< f=：100=PNG，32=RGBA（缺省），24=RGB
    bool compressed = false;    ///< o=z：负载为 RFC 1950 zlib 流（先于 base64 压缩）
    quint32 imageId = 0;        ///< i=：图像 id；0 = 匿名图像（可显示，不占 id 命名空间）
    quint32 placementId = 0;    ///< p=：放置 id
    char deleteWhat = 0;        ///< d=：删除对象（a/A/i/I/c/C；其余变体忽略）
    int quiet = 0;              ///< q=：1 抑制成功应答，2 抑制失败应答
    int width = 0;              ///< s=：像素宽（f=24/32 必需）
    int height = 0;             ///< v=：像素高（f=24/32 必需）
    qint64 pngSize = 0;         ///< S=：解压后字节数（f=100 + o=z 必需）
    int srcX = 0;               ///< x=：源矩形左（像素）
    int srcY = 0;               ///< y=：源矩形上（像素）
    int srcW = 0;               ///< w=：源矩形宽（像素，0 = 整图宽）
    int srcH = 0;               ///< h=：源矩形高（像素，0 = 整图高）
    int cellXOff = 0;           ///< X=：单元格内像素水平偏移（须小于单元格像素宽）
    int cellYOff = 0;           ///< Y=：单元格内像素垂直偏移（须小于单元格像素高）
    int cols = 0;               ///< c=：显示区列数（单元格，0 = 按源矩形推算）
    int rows = 0;               ///< r=：显示区行数（单元格，0 = 按源矩形/宽高比推算）
    qint32 zIndex = 0;          ///< z=：z-index；<0 画在文本之下，>=0 画在文本之上
    bool cursorNoMove = false;  ///< C=1：放置后光标不移动
    QImage image;               ///< 解码后的 ARGB32 像素（仅传输类动作 a=t/T/q 有效）
};

/**
 * @brief kitty 图形协议纯逻辑解析器（无终端状态，可独立单测）。
 *
 * 逐条 APC 序列喂入 feed()；m=1 续块期间返回 NeedMore 并跨序列累积负载，
 * 末块（m=0 缺省）重组后解析控制键、base64 解码、（可选）zlib 解压、
 * 按 f= 解码像素。分块流被带新控制键的序列打断时丢弃半成品、按新命令处理。
 */
class KittyGraphicsParser
{
public:
    /** @brief 单张图宽/高硬上限（像素），超限报 EINVAL:image too large。 */
    static constexpr int MAX_DIMENSION = 10000;

    /** @brief feed() 的返回状态。 */
    enum class Status {
        NeedMore, ///< m=1 续块：已累积，等待后续 APC 序列
        Ready,    ///< 命令完整且（如需）像素解码成功，command 有效
        Error     ///< 解析/解码失败，errorCode/errorMessage/imageId 有效
    };

    /**
     * @brief feed() 的输出。
     */
    struct Result {
        KittyCommand command;      ///< Status==Ready 时有效
        QByteArray errorCode;      ///< Status==Error 时有效（EINVAL/ENOENT/ENOSPC 等）
        QByteArray errorMessage;   ///< 错误说明（与错误码一并回显给客户端）
        quint32 imageId = 0;       ///< 能定位 i= 时带回（供应答）；0 = 无法定位，调用方静默
        quint32 placementId = 0;   ///< p= 回显用
        int quiet = 0;             ///< q= 抑制级别（跨块取最后值）
    };

    KittyGraphicsParser() = default;

    /**
     * @brief 喂入一条 APC 序列内容（'G' 之后、ST 之前的字节）。
     * @param chunk 单条 APC 序列的键值段 + 负载段原文。
     * @param budgetRemaining 共享像素预算剩余字节数（用于解码前预检，避免大分配）。
     * @param out 输出结果（每次调用先清空再填）。
     * @return 见 Status；Error 后解析器自动复位，可直接喂下一条。
     */
    Status feed(const QByteArray &chunk, qint64 budgetRemaining, Result &out);

    /** @brief 丢弃半成品分块，复位到"等待首块"状态。 */
    void reset();

    /** @brief 是否处于分块累积中（已收 m=1，未收末块）。 */
    bool midChunk() const { return _accumulating; }

private:
    /** @brief 解析键值段（首块全量；续块仅 m/q，出现其他键视为新命令）。 */
    bool parseKeys(const QByteArray &keyPart, bool continuation, Result &out);
    /** @brief 末块收尾：base64 解码 → 可选 zlib 解压 → 按格式产出 ARGB32。 */
    bool decodePayload(qint64 budgetRemaining, Result &out);

    bool _accumulating = false;  ///< 分块累积中
    KittyCommand _pending;       ///< 首块解析出的控制键（跨块保留）
    QByteArray _payload;         ///< 跨块 base64 负载累积
};

#endif // KITTYGRAPHICSPARSER_H
```

- [ ] **步骤 1.3：实现 KittyGraphicsParser.cpp**

新建 `lib/src/emulation/KittyGraphicsParser.cpp`，完整内容：

```cpp
#include "KittyGraphicsParser.h"

#include <cstring>

namespace {

/**
 * @brief "4 字节大端长度前缀 + qUncompress"解压 RFC 1950 zlib 流。
 * @param zlibStream kitty o=z 负载解压前的原始 zlib 字节。
 * @param expected 预期解压长度（f=24/32 为 s*v*bpp；f=100 取 S 键）。
 * @return 解压结果；失败或长度不符时返回空 QByteArray。
 */
QByteArray inflateWithLengthPrefix(const QByteArray &zlibStream, qint64 expected)
{
    if (expected <= 0 || expected > qint64(std::numeric_limits<int>::max()))
        return {};
    QByteArray prefixed;
    prefixed.resize(4);
    prefixed[0] = char((expected >> 24) & 0xFF);
    prefixed[1] = char((expected >> 16) & 0xFF);
    prefixed[2] = char((expected >> 8) & 0xFF);
    prefixed[3] = char(expected & 0xFF);
    prefixed += zlibStream;
    return qUncompress(reinterpret_cast<const uchar *>(prefixed.constData()),
                       prefixed.size());
}

} // namespace

void KittyGraphicsParser::reset()
{
    _accumulating = false;
    _pending = KittyCommand{};
    _payload.clear();
}

bool KittyGraphicsParser::parseKeys(const QByteArray &keyPart, bool continuation, Result &out)
{
    const auto pairs = keyPart.split(',');
    for (const QByteArray &kv : pairs) {
        if (kv.isEmpty())
            continue;
        const int eq = kv.indexOf('=');
        const QByteArray key = eq < 0 ? kv : kv.left(eq);
        const QByteArray val = eq < 0 ? QByteArray() : kv.mid(eq + 1);
        if (continuation) {
            // 续块仅允许 m（及可选 q）；其余键由 feed() 的打断判定先行过滤
            if (key == "q")
                _pending.quiet = val.toInt();
            continue;
        }
        if (key.size() != 1)
            continue; // 未知键静默忽略（协议要求）
        switch (key.at(0)) {
        case 'a': _pending.action = val.isEmpty() ? 'T' : val.at(0); break;
        case 't': _pending.medium = val.isEmpty() ? 'd' : val.at(0); break;
        case 'f': _pending.format = val.toInt(); break;
        case 'o': _pending.compressed = (val == "z"); break;
        case 'i': _pending.imageId = val.toUInt(); break;
        case 'p': _pending.placementId = val.toUInt(); break;
        case 'd': _pending.deleteWhat = val.isEmpty() ? '\0' : val.at(0); break;
        case 'q': _pending.quiet = val.toInt(); break;
        case 's': _pending.width = val.toInt(); break;
        case 'v': _pending.height = val.toInt(); break;
        case 'S': _pending.pngSize = val.toLongLong(); break;
        case 'x': _pending.srcX = val.toInt(); break;
        case 'y': _pending.srcY = val.toInt(); break;
        case 'w': _pending.srcW = val.toInt(); break;
        case 'h': _pending.srcH = val.toInt(); break;
        case 'X': _pending.cellXOff = val.toInt(); break;
        case 'Y': _pending.cellYOff = val.toInt(); break;
        case 'c': _pending.cols = val.toInt(); break;
        case 'r': _pending.rows = val.toInt(); break;
        case 'z': _pending.zIndex = qint32(val.toInt()); break;
        case 'C': _pending.cursorNoMove = (val.toInt() == 1); break;
        case 'm': break; // 分块标志在 feed() 中处理
        default: break;  // U=/I=/P/Q/H/V 等未知键静默忽略
        }
    }
    Q_UNUSED(out);
    return true;
}

bool KittyGraphicsParser::decodePayload(qint64 budgetRemaining, Result &out)
{
    KittyCommand &cmd = out->command;
    auto fail = [&out](const char *code, const char *msg) {
        out->errorCode = QByteArray(code);
        out->errorMessage = QByteArray(msg);
        return false;
    };

    if (cmd.format != 100 && cmd.format != 32 && cmd.format != 24)
        return fail("EINVAL", "decode failed"); // 未知像素格式

    QByteArray raw = QByteArray::fromBase64(_payload); // Qt 默认忽略非法字符（宽容）

    if (cmd.compressed) {
        qint64 expected = 0;
        if (cmd.format == 100) {
            if (cmd.pngSize <= 0)
                return fail("EINVAL", "missing S"); // PNG 与压缩并用必须提供 S
            expected = cmd.pngSize;
        } else {
            if (cmd.width <= 0 || cmd.height <= 0)
                return fail("EINVAL", "missing size");
            expected = qint64(cmd.width) * cmd.height * (cmd.format == 24 ? 3 : 4);
        }
        raw = inflateWithLengthPrefix(raw, expected);
        if (raw.size() != expected)
            return fail("EINVAL", "decode failed");
    }

    if (cmd.format == 100) {
        QImage img = QImage::fromData(raw, "PNG");
        if (img.isNull())
            return fail("EINVAL", "decode failed");
        if (img.width() > MAX_DIMENSION || img.height() > MAX_DIMENSION)
            return fail("EINVAL", "image too large");
        if (qint64(img.width()) * img.height() * 4 > budgetRemaining)
            return fail("ENOSPC", "pixel budget exceeded"); // 解码前预检（按 ARGB32 计）
        cmd.image = img.convertToFormat(QImage::Format_ARGB32);
        return true;
    }

    if (cmd.width <= 0 || cmd.height <= 0)
        return fail("EINVAL", "missing size");
    if (cmd.width > MAX_DIMENSION || cmd.height > MAX_DIMENSION)
        return fail("EINVAL", "image too large");
    if (qint64(cmd.width) * cmd.height * 4 > budgetRemaining)
        return fail("ENOSPC", "pixel budget exceeded"); // 解码前预检

    const int bpp = cmd.format == 24 ? 3 : 4;
    const qint64 need = qint64(cmd.width) * cmd.height * bpp;
    if (raw.size() != need)
        return fail("EINVAL", "decode failed");

    QImage img(cmd.width, cmd.height, QImage::Format_ARGB32);
    const uchar *p = reinterpret_cast<const uchar *>(raw.constData());
    for (int y = 0; y < cmd.height; y++) {
        for (int x = 0; x < cmd.width; x++, p += bpp) {
            img.setPixel(x, y, bpp == 4 ? qRgba(p[0], p[1], p[2], p[3])
                                        : qRgb(p[0], p[1], p[2]));
        }
    }
    cmd.image = img;
    return true;
}

KittyGraphicsParser::Status KittyGraphicsParser::feed(const QByteArray &chunk,
                                                      qint64 budgetRemaining, Result &out)
{
    out = Result{};

    // 键值段与负载段以第一个 ';' 分隔；无 ';' 时整块为键值段
    const int sep = chunk.indexOf(';');
    const QByteArray keyPart = sep < 0 ? chunk : chunk.left(sep);
    const QByteArray payloadPart = sep < 0 ? QByteArray() : chunk.mid(sep + 1);

    if (_accumulating) {
        // 分块打断判定：续块出现 m/q 之外的键 → 丢弃半成品，本条按首块重新解析
        bool foreignKey = false;
        const auto pairs = keyPart.split(',');
        for (const QByteArray &kv : pairs) {
            const int eq = kv.indexOf('=');
            const QByteArray key = eq < 0 ? kv : kv.left(eq);
            if (!key.isEmpty() && key != "m" && key != "q")
                foreignKey = true;
        }
        if (foreignKey)
            reset();
    }

    bool more = false;
    if (!_accumulating) {
        _pending = KittyCommand{};
        _payload.clear();
        // 首块提取 m 后全量解析控制键
        const auto pairs = keyPart.split(',');
        for (const QByteArray &kv : pairs) {
            if (kv.startsWith("m="))
                more = (kv.mid(2).toInt() == 1);
        }
        parseKeys(keyPart, false, out);
    } else {
        const auto pairs = keyPart.split(',');
        for (const QByteArray &kv : pairs) {
            if (kv.startsWith("m="))
                more = (kv.mid(2).toInt() == 1);
        }
        parseKeys(keyPart, true, out); // 仅吸收 q=
    }
    _payload += payloadPart;

    if (more) {
        _accumulating = true;
        return Status::NeedMore;
    }

    // 末块收尾：仅传输类动作（含缺省 a=T 与查询 a=q）需要像素解码
    const bool needsPixels = (_pending.action == 't' || _pending.action == 'T'
                              || _pending.action == 'q');
    Status status = Status::Ready;
    if (needsPixels && !decodePayload(budgetRemaining, out))
        status = Status::Error;
    if (status == Status::Ready)
        out->command = _pending;
    // 应答所需的 id/q 无论成败都带出（能定位 i= 时回错误码，否则调用方静默）
    out->imageId = _pending.imageId;
    out->placementId = _pending.placementId;
    out->quiet = _pending.quiet;
    reset();
    return status;
}
```

在 `lib/CMakeLists.txt` 的 `QTERMWIDGET_SOURCES`（`SixelDecoder.cpp` 行之后）追加：

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/KittyGraphicsParser.cpp
```

`QTERMWIDGET_HEADERS`（`SixelDecoder.h` 行之后）追加：

```cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/KittyGraphicsParser.h
```

运行：

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_kittygraphics --output-on-failure
```

预期输出：`100% tests passed, 0 tests failed out of 1`（tst_kittygraphics 10 个用例全过）。

```bash
ctest --test-dir build --output-on-failure
```

预期：`100% tests passed, 0 tests failed out of 9`（8 旧 + 1 新）。

- [ ] **步骤 1.4：提交**

```bash
git add lib/src/emulation/KittyGraphicsParser.h lib/src/emulation/KittyGraphicsParser.cpp \
        lib/CMakeLists.txt tests/CMakeLists.txt tests/tst_kittygraphics.cpp
git commit -m "feat(emulation): 新增 Kitty 图形协议纯逻辑解析器 KittyGraphicsParser"
```

---

## 任务 2：Screen 统一图形锚定层泛化 + kitty 图像/放置表

**文件：**
- 修改 `lib/src/emulation/Screen.h`、`lib/src/emulation/Screen.cpp`
- 修改 `lib/src/display/TerminalDisplay.cpp`（仅改名跟进：`SixelImage`→`ScreenImage`、`sixelImage(`→`image(`）
- 修改 `tests/tst_emulation.cpp`（7 处改名跟进）
- 修改 `tests/tst_kittygraphics.cpp`（追加 Screen 级生命周期测试）

- [ ] **步骤 2.1：协议无关改名（行为不变重构）**

`lib/src/emulation/Screen.h`：
- `struct SixelImage`（71-74 行）改名 `struct ScreenImage`，注释改为"屏级存储的一张图像（sixel/kitty 共用）"。
- `sixelImage()`（392 行）改名 `image()`，注释相应调整。
- `_sixelImages`（820 行）改名 `_images`，`_nextImageId`（822 行）改名 `_nextImageHandle`；注释"Sixel 图像"段标题改为"图像锚定层（sixel/kitty 共用）"。
- `hasImages()`（415 行）实现改为 `return !_images.isEmpty();`，注释说明 kitty 已传输未放置的图像也计入（仅影响脏区短路灵敏度，不影响正确性）。

`lib/src/emulation/Screen.cpp`：跟进 1542、1570-1573、1585-1588、1601 行处改名。

`lib/src/display/TerminalDisplay.cpp`：1960 行 `const SixelImage *img = screen->sixelImage(p.imageId);` → `const ScreenImage *img = screen->image(p.imageId);`

`tests/tst_emulation.cpp`：578、615、618、693、737、745、766 行 `SixelImage`→`ScreenImage`、`sixelImage(`→`image(`。

运行：

```bash
cmake --build build --parallel && ctest --test-dir build --output-on-failure
```

预期：`100% tests passed, 0 tests failed out of 9`（纯改名，行为不变）。

```bash
git add -A && git commit -m "refactor(emulation): 图形存储泛化为协议无关命名（ScreenImage/_images）"
```

- [ ] **步骤 2.2：写失败测试（Screen 级 kitty 放置/删除/生命周期）**

在 `tests/tst_kittygraphics.cpp` 的 slots 区追加声明，文件尾部（`QTEST_MAIN` 之前）追加实现。辅助与测试代码完整如下：

```cpp
    // Screen 级生命周期（任务 2）
    void testScreenPlaceDefaultsAndRefs();
    void testScreenPlacementSurvivesScrollIntoHistory();
    void testScreenClearLineDestroysPlacement();
    void testScreenResizeCropsPlacements();
    void testScreenResetClearsAll();
    void testScreenDeleteVariants();
    void testScreenReplaceSameImageAndPlacement();
    void testScreenAnonymousImageFreedWithPlacement();
```

```cpp
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
    scr.setCursorYX(20, 0); // 放置在第 20 行
    QVERIFY(scr.kittyStoreImage(solidImage(8, 16, Qt::red), 3));
    quint32 ph = 0;
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(3), 3, KittyPlacementParams{}, &ph),
             KittyPlaceError::Ok);
    QCOMPARE(scr.kittyRefs(20).size(), 1);
    scr.resizeImage(10, 80); // 收缩：第 20 行被裁（先滚入历史；无历史则销毁）
    // 无历史时（HistoryScrollNone 容量为零）引用直接销毁
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
    scr.setCursorYX(5, 0);
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(10), 10, p), KittyPlaceError::Ok);
    scr.setCursorYX(10, 0);
    QCOMPARE(scr.kittyPlace(scr.kittyImageHandle(11), 11, p), KittyPlaceError::Ok);

    // d=i + p：只删指定 (i,p) 放置
    KittyPlacementParams withPid;
    withPid.placementId = 7;
    scr.setCursorYX(12, 0);
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
    scr.setCursorYX(10, 0);
    scr.kittyDeleteAtCursor(false);
    QCOMPARE(scr.kittyRefs(10).size(), 0);
    QVERIFY(scr.hasKittyImage(11)); // 小写不释放数据

    // d=A：删除全部可见放置并释放无引用图像
    scr.setCursorYX(15, 0);
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
    scr.setCursorYX(8, 4);
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
```

注意：`testScreenResizeCropsPlacements` 中 `setCursorYX(20,0)` 后 `resizeImage(10,80)` 会先触发 `addHistLine`+`scrollUp`（Screen.cpp:292-298 的保焦路径），`HistoryScrollNone` 容量为零时引用在 `addHistLine` 的防御分支（1390 行）销毁——断言放置死亡成立。

运行（预期编译失败：Screen 无 kitty 方法）：

```bash
cmake --build build --parallel 2>&1 | grep -m3 "error:"
```

预期：报 `kittyStoreImage` 等成员不存在。

- [ ] **步骤 2.3：Screen.h 声明 kitty 类型与接口**

在 `lib/src/emulation/Screen.h` 的 `ScreenImage` 定义之后追加：

```cpp
/**
 * @brief kitty 放置参数（执行层传入，不含锚定信息；锚定取当前光标位置）。
 */
struct KittyPlacementParams {
    quint32 placementId = 0; ///< p= 放置 id（0 = 匿名放置）
    int srcX = 0;            ///< x= 源矩形左（像素）
    int srcY = 0;            ///< y= 源矩形上（像素）
    int srcW = 0;            ///< w= 源矩形宽（0 = 整图宽）
    int srcH = 0;            ///< h= 源矩形高（0 = 整图高）
    int cellXOff = 0;        ///< X= 单元格内像素水平偏移（须小于单元格像素宽）
    int cellYOff = 0;        ///< Y= 单元格内像素垂直偏移（须小于单元格像素高）
    int cols = 0;            ///< c= 显示区列数（0 = 按源矩形与单元格尺寸向上取整推算）
    int rows = 0;            ///< r= 显示区行数（0 = 同上；只给一个时另一个按宽高比推算）
    qint32 zIndex = 0;       ///< z= z-index（<0 文本之下，>=0 文本之上）
};

/**
 * @brief kitty 图像的一次放置（锚定在单元格网格上，随内容滚动）。
 */
struct KittyPlacement {
    quint32 imageHandle;   ///< 内部图像句柄（Screen::_images 键）
    quint32 imageId;       ///< 客户端图像 id（i=，0 = 匿名图像）
    quint32 placementId;   ///< 客户端放置 id（p=，0 = 匿名放置）
    int anchorLine;        ///< 锚定绝对行（history+screen 统一编号；仅记录，绘制以行引用位置为准）
    int col;               ///< 锚定列
    int cols;              ///< 显示区列数（单元格）
    int rows;              ///< 显示区行数（单元格）
    int srcX;              ///< 源矩形左（像素，已与源图取交）
    int srcY;              ///< 源矩形上（像素）
    int srcW;              ///< 源矩形宽（像素）
    int srcH;              ///< 源矩形高（像素）
    int cellXOff;          ///< 单元格内像素水平偏移
    int cellYOff;          ///< 单元格内像素垂直偏移
    qint32 zIndex;         ///< z-index
    quint64 serial;        ///< 插入序（同 z 同 id 排序稳定化）
};

/**
 * @brief kitty 放置在单个网格行上的引用（与 sixel 的 ImagePlacement 同构）。
 */
struct KittyPlacementRef {
    quint32 placementHandle; ///< 放置句柄，经 Screen::kittyPlacement() 换取放置参数
    int rowOffset;           ///< 本行在放置内的行偏移（0 = 放置首行；绘制层只从 0 行画一次）
};

/**
 * @brief kittyPlace() 的失败原因。
 */
enum class KittyPlaceError {
    Ok,              ///< 成功
    NoSuchImage,     ///< 图像句柄不存在（ENOENT）
    InvalidArgument, ///< X/Y 越界、源矩形取交为空等（EINVAL）
    BudgetExceeded   ///< 像素预算超限（ENOSPC；预留，当前放置不产生像素不入预算）
};
```

`Screen` public 区（`hasImages()` 之后）追加：

```cpp
    /** @brief 共享像素预算剩余字节数（解析器解码前预检用）。 */
    qint64 imageBytesRemaining() const { return MAX_IMAGE_BYTES - _imageBytes; }

    /**
     * @brief 落库一张 kitty 图像（a=t/T 的存储步骤）。
     * @param image 解码后的 ARGB32 图像。
     * @param clientId 客户端图像 id（i=）；0 = 匿名图像（不占 id 命名空间，随最后放置死亡释放）。
     * @param handleOut 非空时返回内部图像句柄。
     * @return false = 预算超限（先淘汰无放置引用的 kitty 图像，仍不够才失败）。
     * @note 同 clientId 重传的"先删旧图"语义由调用方（执行层）先行处理，本函数不查重。
     */
    bool kittyStoreImage(const QImage &image, quint32 clientId, quint32 *handleOut = nullptr);

    /** @brief clientId → 内部图像句柄；不存在（含 clientId==0）返回 0。 */
    quint32 kittyImageHandle(quint32 clientId) const;
    /** @brief 是否已落库 clientId 对应的 kitty 图像。 */
    bool hasKittyImage(quint32 clientId) const { return kittyImageHandle(clientId) != 0; }

    /**
     * @brief 在当前光标位置放置一张已落库图像（a=T 的显示步骤 / a=p）。
     * @param imageHandle 内部图像句柄（kittyImageHandle() 或 kittyStoreImage() 返回值）。
     * @param clientId 客户端图像 id（匿名图像传 0；用于 (i,p) 替换语义与应答回显）。
     * @param params 放置参数；c/r 缺省按源矩形与单元格尺寸推算。
     * @param placementHandleOut 非空时返回放置句柄。
     * @param colsUsed/rowsUsed 非空时返回实际使用的显示区（供执行层移动光标）。
     * @note 同 (i≠0, p≠0) 重复放置视为替换（先删旧放置）；p=0 且 i≠0 多次放置并存；
     *       行引用挂在覆盖的每一行上，滚动/清行/resize/复位由共享挂钩管理；
     *       本函数不移动文本光标（kitty 光标语义由执行层按 C= 决定）。
     */
    KittyPlaceError kittyPlace(quint32 imageHandle, quint32 clientId,
                               const KittyPlacementParams &params,
                               quint32 *placementHandleOut = nullptr,
                               int *colsUsed = nullptr, int *rowsUsed = nullptr);

    /** @brief d=a/A：删除全部可见放置；freeData 时连同释放无引用图像数据。 */
    void kittyDeleteAll(bool freeData);
    /**
     * @brief d=i/I（+p=）：删除 clientId 图像的放置；placementId≠0 时只删该 (i,p) 放置。
     * @param freeData true（大写 I）时连同释放无其他引用（含回看历史）的图像数据。
     */
    void kittyDeleteByImage(quint32 clientId, quint32 placementId, bool freeData);
    /** @brief d=c/C：删除与当前光标单元格相交的放置；freeData 语义同上。 */
    void kittyDeleteAtCursor(bool freeData);

    /**
     * @brief 返回绝对行 @p absoluteLine（历史行 + 屏幕行统一编号）上的 kitty 放置引用表。
     * @return 引用表副本；该行无放置或行号越界时为空。
     */
    QVector<KittyPlacementRef> kittyRefs(int absoluteLine) const;

    /** @brief 返回放置句柄对应的放置参数；无效句柄（已回收）返回 nullptr。 */
    const KittyPlacement *kittyPlacement(quint32 placementHandle) const;
```

`Screen` private 区（`_graphicsDirty` 声明之后）追加：

```cpp
    /** @brief 未同步字体度量时的兜底单元格像素宽（常见等宽字体量级）。 */
    static constexpr int DEFAULT_CELL_PIXEL_WIDTH = 8;

    // kitty 放置 ----------------
    // 与 sixel 行引用同构的平行表：行级稀疏引用 + 放置句柄引用计数；
    // 行引用随行清除/滚出/丢弃而销毁，计数归零时回收放置；匿名图像随最后放置死亡释放
    typedef QVector<KittyPlacementRef> KittyRefLine;
    KittyRefLine *_kittyLines;                  // [lines + 1]，与 screenLines 平行
    std::deque<KittyRefLine> _historyKittyRefs; // 与 history 行一一对应
    QHash<quint32, KittyPlacement> _kittyPlacements;   // placementHandle → 放置参数
    QHash<quint32, int> _kittyPlacementRefs;           // placementHandle → 行引用计数
    QHash<quint64, quint32> _kittyPlacementKeys;       // (clientId<<32|placementId) → placementHandle
    QHash<quint32, quint32> _kittyImageHandles;        // clientId(i≠0) → imageHandle
    QSet<quint32> _kittyAnonymous;                     // 匿名图像（i=0）句柄集
    QList<quint32> _kittyEvictionOrder;                // kitty 图像落库顺序（预算淘汰用，旧→新）
    quint32 _nextKittyPlacementHandle = 1;
    quint64 _nextKittySerial = 1;

    void releaseKittyRefLine(KittyRefLine &row);
    void removeKittyPlacement(quint32 placementHandle);
    void removeKittyImage(quint32 imageHandle);
    bool kittyImageInUse(quint32 imageHandle) const;
    void evictUnreferencedKittyImages(qint64 bytesNeeded);
```

并在 `Screen.h` 顶部 include 区加 `#include <QList>`（若未含）。

- [ ] **步骤 2.4：Screen.cpp 实现 + 挂钩点接入**

构造函数初始化列表（Screen.cpp:63 `_imageLines(...)` 之后）追加 `_kittyLines(new KittyRefLine[lines + 1])`；析构（76 行）追加 `delete[] _kittyLines;`。

六个挂钩点逐一接入（均镜像既有 `_imageLines` 处理）：

1. `resizeImage`（324-330 行 sixel 块之后）：
```cpp
    // kitty 放置引用表数组随屏幕尺寸重建；收缩时被裁行的引用销毁（先 move 再 delete 旧数组）
    KittyRefLine *newKittyLines = new KittyRefLine[new_lines + 1];
    for (int i = 0; i < qMin(lines, new_lines + 1); i++)
        newKittyLines[i] = std::move(_kittyLines[i]);
    for (int i = qMin(lines, new_lines + 1); i < lines + 1; i++)
        releaseKittyRefLine(_kittyLines[i]); // 收缩时被裁行的引用销毁
    delete[] _kittyLines;
    _kittyLines = newKittyLines;
```

2. `clearImage`（966 行 `releaseImageLine(_imageLines[y]);` 之后）：`releaseKittyRefLine(_kittyLines[y]); // 清行连带销毁 kitty 放置引用`

3. `moveImage` 两个循环（999/1007 与 1012/1020 行之后）：分别加
```cpp
            releaseKittyRefLine(_kittyLines[(dest / columns) + i]); // 目标行被覆盖，旧引用销毁
```
和
```cpp
            _kittyLines[(dest / columns) + i] =
                    std::move(_kittyLines[(sourceBegin / columns) + i]); // 引用随行走
```
（注意保持与 sixel 相同的顺序：先 release 目标行，再 move。）

4. `addHistLine`（1382-1392 行 sixel 块之后）：
```cpp
        // kitty 放置引用随行进入 scrollback；历史满丢弃最旧行时同步销毁其引用
        if (newHistLines > oldHistLines) {
            _historyKittyRefs.push_back(std::move(_kittyLines[0]));
        } else if (oldHistLines > 0) {
            releaseKittyRefLine(_historyKittyRefs.front());
            _historyKittyRefs.pop_front();
            _historyKittyRefs.push_back(std::move(_kittyLines[0]));
        } else {
            releaseKittyRefLine(_kittyLines[0]); // 防御：历史容量为零时直接销毁
        }
        _kittyLines[0].clear();
```

5. `clearAllImages`（1595-1605 行）末尾追加 kitty 表清理：
```cpp
    for (int i = 0; i < lines + 1; i++)
        _kittyLines[i].clear();
    _historyKittyRefs.clear();
    _kittyPlacements.clear();
    _kittyPlacementRefs.clear();
    _kittyPlacementKeys.clear();
    _kittyImageHandles.clear();
    _kittyAnonymous.clear();
    _kittyEvictionOrder.clear();
```
（`_images`/`_imageBytes` 已在原函数清空，kitty 图像数据同表同预算，无需另算。）

6. `setScroll` 废弃历史分支（1623-1625 行之后）：
```cpp
        // 历史整体废弃（clearHistory）：同步销毁历史 kitty 放置引用
        for (KittyRefLine &row : _historyKittyRefs)
            releaseKittyRefLine(row);
        _historyKittyRefs.clear();
```

新增方法实现（追加在 `clearAllImages()` 之后）：

```cpp
quint32 Screen::kittyImageHandle(quint32 clientId) const
{
    if (clientId == 0)
        return 0; // 匿名图像不占 id 命名空间
    const auto it = _kittyImageHandles.constFind(clientId);
    return it == _kittyImageHandles.constEnd() ? 0 : it.value();
}

bool Screen::kittyImageInUse(quint32 imageHandle) const
{
    for (const KittyPlacement &pl : _kittyPlacements)
        if (pl.imageHandle == imageHandle)
            return true;
    return false;
}

void Screen::removeKittyImage(quint32 imageHandle)
{
    const auto it = _images.find(imageHandle);
    if (it == _images.end())
        return;
    _imageBytes -= qint64(it->image.width()) * it->image.height() * 4;
    _images.erase(it);
    _kittyAnonymous.remove(imageHandle);
    _kittyEvictionOrder.removeOne(imageHandle);
    // _kittyImageHandles 的反查清理由调用方负责（kittyDeleteByImage 已知 clientId）
    for (auto hit = _kittyImageHandles.begin(); hit != _kittyImageHandles.end(); ++hit) {
        if (hit.value() == imageHandle) {
            _kittyImageHandles.erase(hit);
            break;
        }
    }
    _graphicsDirty = true;
}

void Screen::evictUnreferencedKittyImages(qint64 bytesNeeded)
{
    // 预算紧张时优先淘汰无放置引用的 kitty 图像（上游建议行为），最旧的先淘汰
    for (int i = 0; i < _kittyEvictionOrder.size() && _imageBytes + bytesNeeded > MAX_IMAGE_BYTES;) {
        const quint32 handle = _kittyEvictionOrder.at(i);
        if (!kittyImageInUse(handle)) {
            removeKittyImage(handle); // 内部 removeOne 保持 i 指向下一元素
        } else {
            i++;
        }
    }
}

bool Screen::kittyStoreImage(const QImage &image, quint32 clientId, quint32 *handleOut)
{
    if (image.isNull())
        return false;
    const qint64 bytes = qint64(image.width()) * image.height() * 4;
    if (_imageBytes + bytes > MAX_IMAGE_BYTES)
        evictUnreferencedKittyImages(bytes);
    if (_imageBytes + bytes > MAX_IMAGE_BYTES)
        return false; // 淘汰后仍超限：失败（调用方回 ENOSPC）
    const quint32 handle = _nextImageHandle++;
    _images.insert(handle, ScreenImage {image, false});
    _imageBytes += bytes;
    if (clientId != 0)
        _kittyImageHandles.insert(clientId, handle);
    else
        _kittyAnonymous.insert(handle);
    _kittyEvictionOrder.append(handle);
    if (handleOut)
        *handleOut = handle;
    return true;
}

KittyPlaceError Screen::kittyPlace(quint32 imageHandle, quint32 clientId,
                                   const KittyPlacementParams &params,
                                   quint32 *placementHandleOut, int *colsUsed, int *rowsUsed)
{
    const auto imgIt = _images.constFind(imageHandle);
    if (imgIt == _images.constEnd())
        return KittyPlaceError::NoSuchImage;
    const QImage &img = imgIt->image;
    const int cellW = _cellPixelWidth > 0 ? _cellPixelWidth : DEFAULT_CELL_PIXEL_WIDTH;
    const int cellH = _cellPixelHeight > 0 ? _cellPixelHeight : DEFAULT_CELL_PIXEL_HEIGHT;
    // X/Y 必须小于单元格尺寸（协议约束）
    if (params.cellXOff < 0 || params.cellXOff >= cellW
            || params.cellYOff < 0 || params.cellYOff >= cellH)
        return KittyPlaceError::InvalidArgument;
    // 源矩形：缺省整图，与源图取交；取交为空则非法
    const QRect src = QRect(params.srcX, params.srcY,
                            params.srcW > 0 ? params.srcW : img.width(),
                            params.srcH > 0 ? params.srcH : img.height()) & img.rect();
    if (src.isEmpty())
        return KittyPlaceError::InvalidArgument;
    // 显示区：缺省按源矩形原始尺寸换算单元格数（向上取整）；只给一个按宽高比推算
    int cols = params.cols;
    int rows = params.rows;
    if (cols <= 0 && rows <= 0) {
        cols = qMax(1, (src.width() + cellW - 1) / cellW);
        rows = qMax(1, (src.height() + cellH - 1) / cellH);
    } else if (cols <= 0) {
        cols = qMax(1, int((qint64(src.width()) * rows * cellW + qint64(src.height()) * cellH - 1)
                           / (qint64(src.height()) * cellH)));
    } else if (rows <= 0) {
        rows = qMax(1, int((qint64(src.height()) * cols * cellH + qint64(src.width()) * cellW - 1)
                           / (qint64(src.width()) * cellW)));
    }

    // 同 (i≠0, p≠0) 重复放置 = 替换（可无闪烁移动/缩放）：先删旧放置
    if (clientId != 0 && params.placementId != 0) {
        const quint64 key = (quint64(clientId) << 32) | params.placementId;
        const auto it = _kittyPlacementKeys.constFind(key);
        if (it != _kittyPlacementKeys.constEnd())
            removeKittyPlacement(it.value());
    }

    KittyPlacement pl;
    pl.imageHandle = imageHandle;
    pl.imageId = clientId;
    pl.placementId = params.placementId;
    pl.anchorLine = history->getLines() + cuY;
    pl.col = cuX;
    pl.cols = cols;
    pl.rows = rows;
    pl.srcX = src.x();
    pl.srcY = src.y();
    pl.srcW = src.width();
    pl.srcH = src.height();
    pl.cellXOff = params.cellXOff;
    pl.cellYOff = params.cellYOff;
    pl.zIndex = params.zIndex;
    pl.serial = _nextKittySerial++;

    const quint32 handle = _nextKittyPlacementHandle++;
    _kittyPlacements.insert(handle, pl);
    _kittyPlacementRefs.insert(handle, 0);
    if (clientId != 0 && params.placementId != 0)
        _kittyPlacementKeys.insert((quint64(clientId) << 32) | params.placementId, handle);

    // 行级引用挂在放置覆盖的每一行（越下缘截断；滚动/清行/resize/复位由共享挂钩管理）
    for (int i = 0; i < rows && cuY + i < lines; i++) {
        _kittyLines[cuY + i].append({handle, i});
        _kittyPlacementRefs[handle]++;
    }
    _graphicsDirty = true;
    if (placementHandleOut)
        *placementHandleOut = handle;
    if (colsUsed)
        *colsUsed = cols;
    if (rowsUsed)
        *rowsUsed = rows;
    return KittyPlaceError::Ok;
}

void Screen::releaseKittyRefLine(KittyRefLine &row)
{
    if (row.isEmpty())
        return;
    _graphicsDirty = true; // 放置（部分）消失，字符层无变化，需显示层补刷
    for (const KittyPlacementRef &ref : row) {
        auto it = _kittyPlacementRefs.find(ref.placementHandle);
        if (it == _kittyPlacementRefs.end())
            continue;
        if (--it.value() > 0)
            continue;
        // 全部行引用销毁：回收放置；匿名图像随最后放置死亡释放
        _kittyPlacementRefs.erase(it);
        const auto plIt = _kittyPlacements.find(ref.placementHandle);
        if (plIt == _kittyPlacements.end())
            continue;
        const quint32 imageHandle = plIt->imageHandle;
        const quint64 key = (quint64(plIt->imageId) << 32) | plIt->placementId;
        _kittyPlacements.erase(plIt);
        _kittyPlacementKeys.remove(key);
        if (_kittyAnonymous.contains(imageHandle) && !kittyImageInUse(imageHandle))
            removeKittyImage(imageHandle);
    }
    row.clear();
}

void Screen::removeKittyPlacement(quint32 placementHandle)
{
    // 显式删除：从全部行（屏幕 + 回看历史）剥离该放置的引用。
    // 引用随滚动迁移（moveImage/addHistLine 挂钩），故只能全表扫描；删除为低频操作
    for (int i = 0; i < lines + 1; i++) {
        KittyRefLine &row = _kittyLines[i];
        for (int j = row.size() - 1; j >= 0; j--) {
            if (row[j].placementHandle == placementHandle) {
                KittyPlacementRef ref = row.takeAt(j);
                auto it = _kittyPlacementRefs.find(ref.placementHandle);
                if (it != _kittyPlacementRefs.end() && --it.value() <= 0)
                    _kittyPlacementRefs.erase(it);
            }
        }
    }
    for (KittyRefLine &row : _historyKittyRefs) {
        for (int j = row.size() - 1; j >= 0; j--) {
            if (row[j].placementHandle == placementHandle) {
                KittyPlacementRef ref = row.takeAt(j);
                auto it = _kittyPlacementRefs.find(ref.placementHandle);
                if (it != _kittyPlacementRefs.end() && --it.value() <= 0)
                    _kittyPlacementRefs.erase(it);
            }
        }
    }
    const auto plIt = _kittyPlacements.find(placementHandle);
    if (plIt == _kittyPlacements.end())
        return;
    const quint32 imageHandle = plIt->imageHandle;
    const quint64 key = (quint64(plIt->imageId) << 32) | plIt->placementId;
    _kittyPlacements.erase(plIt);
    _kittyPlacementKeys.remove(key);
    _graphicsDirty = true;
    // 匿名图像（i=0）无其他引用时释放；命名图像数据由 d 大写变体/重传/淘汰管理
    if (_kittyAnonymous.contains(imageHandle) && !kittyImageInUse(imageHandle))
        removeKittyImage(imageHandle);
}

void Screen::kittyDeleteAll(bool freeData)
{
    // 收集当前全部放置句柄后逐个删除（removeKittyPlacement 内部维护引用计数）
    const auto handles = _kittyPlacements.keys();
    for (const quint32 handle : handles)
        removeKittyPlacement(handle);
    if (freeData) {
        // 大写 A：连同释放无引用图像数据（此时 kitty 图像均已无放置）
        const auto clientIds = _kittyImageHandles.keys();
        for (const quint32 clientId : clientIds)
            removeKittyImage(_kittyImageHandles.value(clientId));
        const auto anonymous = _kittyAnonymous.values();
        for (const quint32 handle : anonymous)
            removeKittyImage(handle);
    }
}

void Screen::kittyDeleteByImage(quint32 clientId, quint32 placementId, bool freeData)
{
    const quint32 imageHandle = kittyImageHandle(clientId);
    if (imageHandle == 0)
        return;
    if (placementId != 0) {
        const quint64 key = (quint64(clientId) << 32) | placementId;
        const auto it = _kittyPlacementKeys.constFind(key);
        if (it != _kittyPlacementKeys.constEnd())
            removeKittyPlacement(it.value());
    } else {
        // 删除该图像全部放置（含匿名放置；先收集句柄避免迭代中改表）
        QList<quint32> handles;
        for (auto it = _kittyPlacements.constBegin(); it != _kittyPlacements.constEnd(); ++it)
            if (it->imageId == clientId)
                handles.append(it.key());
        for (const quint32 handle : handles)
            removeKittyPlacement(handle);
    }
    // 大写 I：连同释放无其他引用（含回看历史中的引用）的图像数据
    if (freeData && !kittyImageInUse(imageHandle))
        removeKittyImage(imageHandle);
}

void Screen::kittyDeleteAtCursor(bool freeData)
{
    const KittyRefLine row = _kittyLines[cuY]; // 副本：删除过程会改原行
    QList<quint32> imageHandles;
    for (const KittyPlacementRef &ref : row) {
        const KittyPlacement *pl = kittyPlacement(ref.placementHandle);
        if (!pl)
            continue;
        // 与光标单元格相交：行匹配（引用在本行即匹配），列落在放置覆盖区间内
        if (cuX >= pl->col && cuX < pl->col + pl->cols) {
            imageHandles.append(pl->imageHandle);
            removeKittyPlacement(ref.placementHandle);
        }
    }
    if (freeData)
        for (const quint32 imageHandle : imageHandles)
            if (!kittyImageInUse(imageHandle))
                removeKittyImage(imageHandle);
}

QVector<KittyPlacementRef> Screen::kittyRefs(int absoluteLine) const
{
    const int histLines = history->getLines();
    if (absoluteLine < 0 || absoluteLine >= histLines + lines)
        return {};
    if (absoluteLine < histLines) {
        if (absoluteLine < static_cast<int>(_historyKittyRefs.size()))
            return _historyKittyRefs[absoluteLine];
        return {};
    }
    return _kittyLines[absoluteLine - histLines];
}

const KittyPlacement *Screen::kittyPlacement(quint32 placementHandle) const
{
    const auto it = _kittyPlacements.constFind(placementHandle);
    return it == _kittyPlacements.constEnd() ? nullptr : &it.value();
}
```

`Screen.cpp` 顶部 include 区无需新增（QSet/QList 经 Screen.h 引入）。

运行：

```bash
cmake --build build --parallel && ctest --test-dir build --output-on-failure
```

预期：`100% tests passed, 0 tests failed out of 9`。

```bash
git add -A && git commit -m "feat(emulation): Screen 新增 kitty 图像表与放置表（统一图形锚定层）"
```

---

## 任务 3：APC 累积通道 + 命令执行（放置/删除/应答/光标/重传）

**文件：**
- 修改 `lib/src/emulation/Vt102Emulation.h`、`lib/src/emulation/Vt102Emulation.cpp`
- 修改 `tests/tst_kittygraphics.cpp`（追加字节流级测试）

- [ ] **步骤 3.1：写失败测试（字节流级：应答/放置/删除/重传/光标）**

`tests/tst_kittygraphics.cpp` slots 区追加声明，尾部追加实现：

```cpp
    // 字节流执行（任务 3）
    void testTransmitAndDisplayOkResponse();
    void testResponseEchoesPlacementId();
    void testQuietSuppressions();
    void testPlaceUnknownImageEnoent();
    void testUnsupportedMediumAndAction();
    void testQueryDoesNotStore();
    void testCursorMovesAfterPlacement();
    void testCursorStaysWithC1();
    void testRetransmitDeletesOldPlacements();
    void testDeleteViaByteStream();
    void testBudgetEnforcementAndEviction();
```

```cpp
/** @brief 初始化仿真器并挂接 sendData 抓取（镜像 tst_emulation 的应答测试写法）。 */
static void initKittyEmu(Vt102Emulation &emu, ScreenWindow *&win, QByteArray &sent)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    win = emu.createWindow();
    win->setWindowLines(24);
    QObject::connect(&emu, &Emulation::sendData,
                     [&sent](const char *d, int len) { sent.append(d, len); });
}

void TestKittyGraphics::testTransmitAndDisplayOkResponse()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    const QByteArray seq = kittySeq("a=T,f=32,s=1,v=1,i=42", rgbaPixel(255, 0, 0).toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=42;OK\033\\"));
    Screen *scr = win->screen();
    QVERIFY(scr->hasKittyImage(42));
    QCOMPARE(scr->kittyRefs(scr->getHistLines()).size(), 1); // 放置锚定在光标行
}

void TestKittyGraphics::testResponseEchoesPlacementId()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    const QByteArray seq = kittySeq("a=T,f=32,s=1,v=1,i=5,p=3", rgbaPixel(1, 2, 3).toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=5,p=3;OK\033\\"));
}

void TestKittyGraphics::testQuietSuppressions()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    // q=1：抑制成功应答
    QByteArray seq = kittySeq("a=T,f=32,s=1,v=1,i=1,q=1", rgbaPixel(0, 0, 0).toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray());
    QVERIFY(win->screen()->hasKittyImage(1)); // 命令本身仍执行
    // q=2：抑制失败应答
    seq = kittySeq("a=p,i=999,q=2");
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray());
    // q=2 不抑制成功应答
    seq = kittySeq("a=p,i=1,q=2");
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=1;OK\033\\"));
}

void TestKittyGraphics::testPlaceUnknownImageEnoent()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    const QByteArray seq = kittySeq("a=p,i=777");
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=777;ENOENT:no such image\033\\"));
}

void TestKittyGraphics::testUnsupportedMediumAndAction()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    // t=f（文件介质）不支持：EINVAL
    QByteArray seq = kittySeq("a=T,t=f,i=1");
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=1;EINVAL:unsupported medium\033\\"));
    sent.clear();
    // 动画动作 a=f：EINVAL
    seq = kittySeq("a=f,i=1");
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=1;EINVAL:unsupported action\033\\"));
}

void TestKittyGraphics::testQueryDoesNotStore()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    // a=q：试加载并应答，不存储不替换
    const QByteArray seq = kittySeq("a=q,f=32,s=1,v=1,i=88", rgbaPixel(7, 7, 7).toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=88;OK\033\\"));
    QVERIFY(!win->screen()->hasKittyImage(88));
    QVERIFY(win->screen()->kittyRefs(win->screen()->getHistLines()).isEmpty());
}

void TestKittyGraphics::testCursorMovesAfterPlacement()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    // 2x2 单元格图（16x32 像素 / 默认单元格 8x16）：光标右移 2 列、下移 2 行
    QImage img = solidImage(16, 32, Qt::red);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    const QByteArray seq = kittySeq("a=T,f=100,i=3", png.toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = win->screen();
    QCOMPARE(scr->getCursorX(), 2);
    QCOMPARE(scr->getCursorY(), 2);
}

void TestKittyGraphics::testCursorStaysWithC1()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    const QByteArray seq = kittySeq("a=T,f=32,s=16,v=32,i=4,C=1",
                                    QByteArray(16 * 32 * 4, 1).toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
    Screen *scr = win->screen();
    QCOMPARE(scr->getCursorX(), 0);
    QCOMPARE(scr->getCursorY(), 0);
    QCOMPARE(scr->kittyRefs(0).size(), 1);
}

void TestKittyGraphics::testRetransmitDeletesOldPlacements()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    // 首次 a=T i=9：存储并显示（C=1 固定光标便于断言）
    QByteArray seq = kittySeq("a=T,f=32,s=1,v=1,i=9,C=1", rgbaPixel(255, 0, 0).toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=9;OK\033\\"));
    sent.clear();
    Screen *scr = win->screen();
    QCOMPARE(scr->kittyRefs(0).size(), 1);
    // 同 id 重传：先删旧图及其全部放置，新数据落库但不自动显示
    seq = kittySeq("a=T,f=32,s=1,v=1,i=9,C=1", rgbaPixel(0, 255, 0).toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=9;OK\033\\"));
    QVERIFY(scr->hasKittyImage(9));              // 新数据已落库
    QCOMPARE(scr->kittyRefs(0).size(), 0);       // 旧放置已删，新图未自动显示
    // 重新放置后显示
    seq = kittySeq("a=p,i=9");
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(scr->kittyRefs(0).size(), 1);
}

void TestKittyGraphics::testDeleteViaByteStream()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    Screen *scr = win->screen();
    // 两张图各一个放置（C=1 固定光标）
    for (int id : {31, 32}) {
        const QByteArray keys = QByteArray("a=T,f=32,s=1,v=1,C=1,i=") + QByteArray::number(id);
        const QByteArray seq = kittySeq(keys, rgbaPixel(char(id), 0, 0).toBase64());
        emu.receiveData(seq.constData(), int(seq.size()));
    }
    QCOMPARE(scr->kittyRefs(0).size(), 2);
    sent.clear();
    // d=i + i=31：删除该图像全部放置（小写，数据保留）
    QByteArray seq = kittySeq("a=d,d=i,i=31");
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=31;OK\033\\"));
    QCOMPARE(scr->kittyRefs(0).size(), 1);
    QVERIFY(scr->hasKittyImage(31));
    sent.clear();
    // d=a：删除全部可见放置
    seq = kittySeq("a=d,d=a,i=32");
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(scr->kittyRefs(0).size(), 0);
    QVERIFY(scr->hasKittyImage(32));
    sent.clear();
    // d=A：释放无引用图像数据
    seq = kittySeq("a=d,d=A,i=32");
    emu.receiveData(seq.constData(), int(seq.size()));
    QVERIFY(!scr->hasKittyImage(31));
    QVERIFY(!scr->hasKittyImage(32));
    // 不支持的删除变体（d=n）：静默忽略，无应答无破坏
    sent.clear();
    seq = kittySeq("a=d,d=n,i=32");
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray());
}

void TestKittyGraphics::testBudgetEnforcementAndEviction()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    QByteArray sent;
    initKittyEmu(emu, win, sent);
    Screen *scr = win->screen();
    // 用 o=z 压缩全零像素保持线载极小：7000x5000 RGBA = 140MB（ARGB32 同量）
    const QByteArray zeros(7000 * 5000 * 4, 0);
    const QByteArray z = zlibStream(zeros).toBase64();
    const QByteArray head = QByteArray("a=T,f=32,s=7000,v=5000,o=z,C=1,i=");
    // 图 1：a=T 落库并放置（140MB，有放置引用，不可淘汰）
    QByteArray seq = kittySeq(head + "1", z);
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=1;OK\033\\"));
    sent.clear();
    // 图 2：a=t 仅落库（140MB，无放置引用，可淘汰）；140+140=280MB > 256MB，
    // 但图 2 入库时预算足够（此时只存了图 1）
    seq = kittySeq(head + "2", z);
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=2;OK\033\\"));
    sent.clear();
    QVERIFY(scr->hasKittyImage(1));
    QVERIFY(scr->hasKittyImage(2));
    // 图 3：再 140MB → 优先淘汰无放置引用的图 2 后成功
    seq = kittySeq(head + "3", z);
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=3;OK\033\\"));
    sent.clear();
    QVERIFY(scr->hasKittyImage(1));  // 有放置引用，不受影响
    QVERIFY(!scr->hasKittyImage(2)); // 无放置引用，被优先淘汰
    QVERIFY(scr->hasKittyImage(3));
    // 图 4：此时图 1、3 均有放置引用（140+140=280>256？——图 3 是 a=T 带放置，
    // 图 4 到来时无可淘汰项（图 1、3 都有引用）→ ENOSPC 拒绝，既有图像不受影响
    seq = kittySeq(head + "4", z);
    emu.receiveData(seq.constData(), int(seq.size()));
    QCOMPARE(sent, QByteArray("\033_Gi=4;ENOSPC:pixel budget exceeded\033\\"));
    QVERIFY(scr->hasKittyImage(1));
    QVERIFY(scr->hasKittyImage(3));
    QVERIFY(!scr->hasKittyImage(4));
}
```

说明：图 3 为 a=T 带放置，入库时预算占用 140（图1）+140（图2）=280MB……注意此处需核对：`kittyStoreImage` 在图 2 入库时 `_imageBytes` 为 140MB，+140=280>256 → 触发淘汰：图 1 有放置不可淘汰，无可淘汰项 → 图 2 应失败！修正测试：图 1 用 6000x4000（96MB），图 2 同 96MB，图 3 96MB（淘汰图 2 后 96+96=192≤256 成功），图 4 96MB（图 1、3 均有引用不可淘汰，192+96=288>256 → ENOSPC）。定稿用 **6000x4000**：

将上面测试中的 `7000 * 5000` 全部改为 `6000 * 4000`、`s=7000,v=5000` 改为 `s=6000,v=4000`（96MB/张）。预算演进：图1 96 → 图2 入库 96+96=192≤256 OK → 图3 入库需 288>256，淘汰无引用的图 2 → 192 OK → 图 4 需 288>256，图 1、3 均有放置引用不可淘汰 → ENOSPC。断言不变。

运行（预期编译失败或测试失败：APC 通道未实现，无应答字节）：

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_kittygraphics --output-on-failure
```

预期：新用例失败（`sent` 为空，与预期应答不等）。

- [ ] **步骤 3.2：Vt102Emulation.h 声明 APC 通道与执行器**

`lib/src/emulation/Vt102Emulation.h`：include 区加 `#include "KittyGraphicsParser.h"`；在 sixel 累积状态块（`///@}` 约 212 行）之后追加：

```cpp
  /**
   * @name Kitty 图形（APC "ESC _ G ... ESC \"）累积状态
   * @note 与 sixel DCS 通道同构：base64 图像负载远超 tokenBuffer（MAX_TOKEN_LENGTH），
   *       检测到 ESC _ G 后切换到独立字节流缓冲，ST 后交 KittyGraphicsParser；
   *       解析器成员跨 APC 序列存活（m=1 分块续传）。
   */
  ///@{
  /** @brief APC 累积上限（350MB，约对应 256MB 解码像素预算）；超限丢弃整条命令。 */
  static constexpr qint64 MAX_APC_DATA_LENGTH = 350LL * 1024 * 1024;
  bool _apcActive = false;     ///< 正在累积 APC 数据段
  bool _apcOverflow = false;   ///< 数据超上限：吞到 ST 后丢弃
  bool _apcEscPending = false; ///< 上一字节为 ESC（等待判定 ST 或中止）
  QByteArray _apcData;         ///< 'G' 之后、ST 之前的原始字节
  KittyGraphicsParser _kittyParser; ///< 分块重组/解析（跨 APC 序列存活）
  /** @brief ST 到达：喂解析器；NeedMore 等待续块，Ready/Error 交执行器。 */
  void finishApc();
  /** @brief CAN/SUB/ESC 中止：丢弃累积数据与半成品分块，复位状态。 */
  void abortApc();
  /** @brief 执行解析完成的 kitty 命令（放置/删除/应答/光标移动/重传语义）。 */
  void executeKittyCommand(const KittyGraphicsParser::Result &res,
                           const QByteArray &rawChunk);
  /**
   * @brief 经 sendString 回写 kitty 应答到 pty。
   * @param imageId 图像 id（i= 回显）。
   * @param placementId 放置 id（includePlacement 时回显 p=）。
   * @param includePlacement 是否在应答中包含 p=（成功应答且客户端给了 p= 时）。
   * @param ok true 回 OK；false 回 error（"CODE:message"）。
   */
  void sendKittyResponse(quint32 imageId, quint32 placementId, bool includePlacement,
                         bool ok, const QByteArray &error = {});
  ///@}
```

- [ ] **步骤 3.3：Vt102Emulation.cpp 接入 receiveChar + 通道函数**

`receiveChar` 中，sixel 早退块（266-295 行）之后插入镜像的 APC 早退块：

```cpp
    // Kitty APC 累积中：绕过 tokenizer，直至 ST（ESC \）结束或 CAN/SUB 中止
    if (_apcActive) {
        if (_apcEscPending) {
            _apcEscPending = false;
            if (cc == U'\\') { // ST：APC 序列结束，喂解析器
                finishApc();
                return;
            }
            // ESC 后非 '\'：中止本条；ESC 与当前字节属于后续序列，重投正常解析
            abortApc();
            receiveChar(ESC);
            receiveChar(cc);
            return;
        }
        if (cc == ESC) {
            _apcEscPending = true;
            return;
        }
        if (cc == CNTL('X') || cc == CNTL('Z')) { // CAN / SUB：中止本条
            abortApc();
            return;
        }
        if (cc >= 0x20 && cc < 0x7F) {
            if (_apcData.size() >= MAX_APC_DATA_LENGTH)
                _apcOverflow = true; // 超上限：继续吞到 ST，ST 后丢弃并复位通道
            else if (!_apcOverflow)
                _apcData.append(char(cc));
        }
        // 其余 C0 控制字符在 APC 数据段内忽略（与 DCS 通道一致）
        return;
    }
```

`sixel` 检测块（362-379 行）之后、通用 `if (Cse)` 吞吃块（380 行）之前插入检测：

```cpp
        // Kitty 图形：APC ESC _ G —— 'G' 为引导符（ESC _ 之后立即出现）。
        // 检测到后切换到独立累积通道（base64 负载可远超 MAX_TOKEN_LENGTH）；
        // 其他 APC（非 'G' 引导）维持原路径：tokenBuffer 累积、ST 后丢弃
        if (Cse && tokenBufferPos == 2 && tokenBuffer[1] == U'_' && cc == U'G') {
            _apcActive = true;
            _apcOverflow = false;
            _apcEscPending = false;
            _apcData.clear();
            resetTokenizer();
            return;
        }
```

`reset()` 中 `abortSixel();`（59 行）之后追加：

```cpp
    abortApc();            // 复位时丢弃未完成的 APC 累积
    _kittyParser.reset();  // 连半成品分块一并丢弃
```

`finishSixel()`/`abortSixel()`（662-689 行）之后追加通道函数：

```cpp
void Vt102Emulation::finishApc()
{
    const bool overflow = _apcOverflow;
    const QByteArray data = std::move(_apcData);
    _apcActive = false;
    _apcOverflow = false;
    _apcEscPending = false;
    _apcData.clear();
    resetTokenizer();
    if (overflow) {
        _kittyParser.reset(); // 超 350MB：丢弃整条命令并中止半成品分块
        return;
    }
    KittyGraphicsParser::Result res;
    const auto status = _kittyParser.feed(data, _currentScreen->imageBytesRemaining(), res);
    if (status == KittyGraphicsParser::Status::NeedMore)
        return; // m=1 续块：等待后续 APC 序列（显示位置以末块到达时的光标为准）
    // ENOSPC 且单块命令：先淘汰无放置引用图像后重试一次（多块的负载已随解析复位，无法重放）
    if (status == KittyGraphicsParser::Status::Error && res.errorCode == "ENOSPC"
            && !_kittyParser.midChunk()) {
        // 估计需求不可得（解析器已复位），直接按"淘汰一切可淘汰项"重试一次
        _currentScreen->evictAllUnreferencedKittyImages();
        status = _kittyParser.feed(data, _currentScreen->imageBytesRemaining(), res);
    }
    if (status == KittyGraphicsParser::Status::NeedMore)
        return;
    executeKittyCommand(res, data);
}

void Vt102Emulation::abortApc()
{
    _apcActive = false;
    _apcOverflow = false;
    _apcEscPending = false;
    _apcData.clear();
    _kittyParser.reset(); // 分块流被打断：丢弃半成品
    resetTokenizer();
}

void Vt102Emulation::sendKittyResponse(quint32 imageId, quint32 placementId,
                                       bool includePlacement, bool ok,
                                       const QByteArray &error)
{
    QByteArray resp = "\033_Gi=" + QByteArray::number(imageId);
    if (includePlacement)
        resp += ",p=" + QByteArray::number(placementId);
    resp += ';';
    resp += ok ? QByteArray("OK") : error;
    resp += "\033\\";
    sendString(resp.constData(), int(resp.size())); // 与 DECRQM 应答同路径：sendData → pty
}

void Vt102Emulation::executeKittyCommand(const KittyGraphicsParser::Result &res,
                                         const QByteArray &rawChunk)
{
    Q_UNUSED(rawChunk);
    Screen *scr = _currentScreen;

    // 解析/解码失败：能定位 i= 时回错误码（q=2 抑制），否则静默忽略
    if (!res.errorCode.isEmpty()) {
        if (res.imageId != 0 && res.quiet != 2)
            sendKittyResponse(res.imageId, 0, false, false,
                              res.errorCode + ':' + res.errorMessage);
        return;
    }

    const KittyCommand &cmd = res.command;
    const bool suppressOk = (cmd.quiet == 1);
    const bool suppressErr = (cmd.quiet == 2);
    const bool echoP = (cmd.placementId != 0);
    auto fail = [&](const char *code, const char *msg) {
        if (cmd.imageId != 0 && !suppressErr)
            sendKittyResponse(cmd.imageId, 0, false, false,
                              QByteArray(code) + ':' + msg);
    };
    auto ok = [&] {
        if (cmd.imageId != 0 && !suppressOk)
            sendKittyResponse(cmd.imageId, cmd.placementId, echoP, true);
    };

    // 不支持的传输介质（t=f/t/s）：回 EINVAL，不崩
    if (cmd.medium == 'f' || cmd.medium == 't' || cmd.medium == 's') {
        fail("EINVAL", "unsupported medium");
        return;
    }

    switch (cmd.action) {
    case 'q':
        // 查询：解析器已试加载（成败在此之前的 Error 路径），不存储不替换
        ok();
        return;
    case 't':
    case 'T': {
        // 重传语义：已有同 id 图像时先删旧图及其全部放置，新数据落库但不自动显示
        const bool retransmit = (cmd.imageId != 0) && scr->hasKittyImage(cmd.imageId);
        if (retransmit)
            scr->kittyDeleteByImage(cmd.imageId, 0, true);
        quint32 imageHandle = 0;
        if (!scr->kittyStoreImage(cmd.image, cmd.imageId, &imageHandle)) {
            fail("ENOSPC", "pixel budget exceeded");
            return;
        }
        if (cmd.action == 't' || retransmit) {
            ok();
            return;
        }
        // a=T 新图：落库并放置（匿名图像 i=0 也在此显示，不占 id 命名空间）
        KittyPlacementParams params;
        params.placementId = cmd.placementId;
        params.srcX = cmd.srcX; params.srcY = cmd.srcY;
        params.srcW = cmd.srcW; params.srcH = cmd.srcH;
        params.cellXOff = cmd.cellXOff; params.cellYOff = cmd.cellYOff;
        params.cols = cmd.cols; params.rows = cmd.rows;
        params.zIndex = cmd.zIndex;
        int colsUsed = 0, rowsUsed = 0;
        const auto err = scr->kittyPlace(imageHandle, cmd.imageId, params,
                                         nullptr, &colsUsed, &rowsUsed);
        if (err != KittyPlaceError::Ok) {
            fail("EINVAL", "bad placement");
            return;
        }
        // kitty 光标语义（与 sixel 的 xterm 语义不同）：右移放置列数、下移放置行数；
        // C=1 时光标不移动。越出屏幕/滚动区的落点由实现自定（取 Screen 现有钳位行为）
        if (!cmd.cursorNoMove) {
            scr->cursorRight(colsUsed);
            scr->cursorDown(rowsUsed);
        }
        ok();
        return;
    }
    case 'p': {
        const quint32 imageHandle = scr->kittyImageHandle(cmd.imageId);
        if (imageHandle == 0) {
            fail("ENOENT", "no such image");
            return;
        }
        KittyPlacementParams params;
        params.placementId = cmd.placementId;
        params.srcX = cmd.srcX; params.srcY = cmd.srcY;
        params.srcW = cmd.srcW; params.srcH = cmd.srcH;
        params.cellXOff = cmd.cellXOff; params.cellYOff = cmd.cellYOff;
        params.cols = cmd.cols; params.rows = cmd.rows;
        params.zIndex = cmd.zIndex;
        int colsUsed = 0, rowsUsed = 0;
        const auto err = scr->kittyPlace(imageHandle, cmd.imageId, params,
                                         nullptr, &colsUsed, &rowsUsed);
        if (err != KittyPlaceError::Ok) {
            fail("EINVAL", "bad placement");
            return;
        }
        if (!cmd.cursorNoMove) {
            scr->cursorRight(colsUsed);
            scr->cursorDown(rowsUsed);
        }
        ok();
        return;
    }
    case 'd': {
        // 删除命令到达时分块上传未完成的场景已由 abortApc/打断规则覆盖（新命令即打断）
        switch (cmd.deleteWhat) {
        case 'a': scr->kittyDeleteAll(false); ok(); return;
        case 'A': scr->kittyDeleteAll(true); ok(); return;
        case 'i': scr->kittyDeleteByImage(cmd.imageId, cmd.placementId, false); ok(); return;
        case 'I': scr->kittyDeleteByImage(cmd.imageId, cmd.placementId, true); ok(); return;
        case 'c': scr->kittyDeleteAtCursor(false); ok(); return;
        case 'C': scr->kittyDeleteAtCursor(true); ok(); return;
        default: return; // 其余删除变体（n/f/q/r/x/y/z）：忽略，无应答
        }
    }
    case 'f': // 动画帧管理：本轮不做
    case 'c': // 动画帧合成：本轮不做
        fail("EINVAL", "unsupported action");
        return;
    default:
        fail("EINVAL", "unsupported action");
        return;
    }
}
```

**配套小改动（Screen，归入本提交）：** `finishApc` 的 ENOSPC 重试需要"淘汰一切可淘汰项"入口。在 `Screen.h` public 区追加声明、在 `Screen.cpp` 实现：

```cpp
    /** @brief 淘汰全部无放置引用的 kitty 图像（ENOSPC 重试路径用）。 */
    void evictAllUnreferencedKittyImages();
```

```cpp
void Screen::evictAllUnreferencedKittyImages()
{
    for (int i = 0; i < _kittyEvictionOrder.size();) {
        const quint32 handle = _kittyEvictionOrder.at(i);
        if (!kittyImageInUse(handle))
            removeKittyImage(handle); // 内部 removeOne 保持 i 指向下一元素
        else
            i++;
    }
}
```

注意：`executeKittyCommand` 的 `rawChunk` 参数当前未用（保留给后续多命令复用），用 `Q_UNUSED` 标注；如评审要求可去掉该参数——选择保留以稳定接口。

运行：

```bash
cmake --build build --parallel && ctest --test-dir build --output-on-failure
```

预期：`100% tests passed, 0 tests failed out of 9`（tst_kittygraphics 全部用例含预算测试通过；预算测试峰值内存约 300MB、耗时数秒，属预期）。

```bash
git add -A && git commit -m "feat(emulation): Kitty 图形协议 APC 通道与命令执行（放置/删除/应答/重传）"
```

---

## 任务 4：TerminalDisplay z-index 双层叠加绘制 + 离屏像素测试

**文件：**
- 修改 `lib/src/display/TerminalDisplay.h`、`lib/src/display/TerminalDisplay.cpp`
- 修改 `tests/tst_kittygraphics.cpp`（追加离屏渲染测试）

- [ ] **步骤 4.1：写失败测试（z-index 分层离屏渲染）**

`tests/tst_kittygraphics.cpp` slots 区追加声明，尾部追加实现（渲染环境镜像 tst_sixel 的 `initSixelRenderEnv`）：

```cpp
    // 离屏渲染（任务 4）
    void testZNegativeDrawnUnderText();
    void testZPositiveDrawnOverText();
    void testSameZOrderedByImageId();
```

```cpp
/** @brief 构造 kitty 渲染测试环境（镜像 tst_sixel 的 initSixelRenderEnv）。 */
static void initKittyRenderEnv(Vt102Emulation &emu, ScreenWindow *&win, TerminalDisplay &display)
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

/** @brief 喂一张恰好 1 单元格的纯色 kitty 图（z 可指定，C=1 固定光标）。 */
static void placeKittyCellImage(Vt102Emulation &emu, quint32 id, qint32 z,
                                const QColor &color, int cellW, int cellH)
{
    const QImage img = solidImage(cellW, cellH, color);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    const QByteArray keys = QByteArray("a=T,f=100,C=1,i=") + QByteArray::number(id)
                            + ",z=" + QByteArray::number(z);
    const QByteArray seq = kittySeq(keys, png.toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
}

void TestKittyGraphics::testZNegativeDrawnUnderText()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initKittyRenderEnv(emu, win, display);
    const int cw = display.cellPixelWidth();
    const int ch = display.cellPixelHeight();

    // 第 0 行第 0 列放纯红 z=-1 图（文本下层），同格打印白色 'A'
    placeKittyCellImage(emu, 1, -1, QColor(255, 0, 0), cw, ch);
    const QByteArray text = "\033[37mA";
    emu.receiveData(text.constData(), int(text.size()));
    display.updateImage();

    QImage frame(display.size(), QImage::Format_ARGB32);
    frame.fill(Qt::black);
    display.render(&frame);

    // 定位单元格像素矩形：左边距 + 内容区原点（与 drawContents 同源换算）
    const QRect cell(display.contentsRect().left() + display.margin(),
                     display.contentsRect().top() + display.margin(), cw, ch);
    int redPx = 0, whitePx = 0;
    for (int y = cell.top(); y <= cell.bottom(); y++)
        for (int x = cell.left(); x <= cell.right(); x++) {
            const QColor c = frame.pixelColor(x, y);
            if (c.red() > 200 && c.green() < 80 && c.blue() < 80)
                redPx++;
            else if (c.red() > 180 && c.green() > 180 && c.blue() > 180)
                whitePx++;
        }
    QVERIFY(redPx > cw * ch / 2); // 图像在文本之下：字形覆盖不到的区域仍是红色
    QVERIFY(whitePx > 0);         // 字形笔画压在图像之上
}

void TestKittyGraphics::testZPositiveDrawnOverText()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initKittyRenderEnv(emu, win, display);
    const int cw = display.cellPixelWidth();
    const int ch = display.cellPixelHeight();

    // 先打印白色 'A'，再放纯蓝 z=0 图（文本上层，C=1 固定光标回第 0 列）
    const QByteArray text = "\033[37mA\033[D";
    emu.receiveData(text.constData(), int(text.size()));
    placeKittyCellImage(emu, 2, 0, QColor(0, 0, 255), cw, ch);
    display.updateImage();

    QImage frame(display.size(), QImage::Format_ARGB32);
    frame.fill(Qt::black);
    display.render(&frame);

    const QRect cell(display.contentsRect().left() + display.margin(),
                     display.contentsRect().top() + display.margin(), cw, ch);
    int whitePx = 0, bluePx = 0;
    for (int y = cell.top(); y <= cell.bottom(); y++)
        for (int x = cell.left(); x <= cell.right(); x++) {
            const QColor c = frame.pixelColor(x, y);
            if (c.red() > 180 && c.green() > 180 && c.blue() > 180)
                whitePx++;
            else if (c.blue() > 200 && c.red() < 80 && c.green() < 80)
                bluePx++;
        }
    QCOMPARE(whitePx, 0);        // z>=0 图像盖住文本：字形笔画不可见
    QVERIFY(bluePx > cw * ch / 2);
}

void TestKittyGraphics::testSameZOrderedByImageId()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initKittyRenderEnv(emu, win, display);
    const int cw = display.cellPixelWidth();
    const int ch = display.cellPixelHeight();

    // 同一单元格叠两张 z=0 不透明图：i=1 绿（先）、i=2 红（后）。
    // 同 z 重叠时 id 小者更低层 → 最终可见红色（i=2 在上）
    placeKittyCellImage(emu, 1, 0, QColor(0, 255, 0), cw, ch);
    placeKittyCellImage(emu, 2, 0, QColor(255, 0, 0), cw, ch);
    display.updateImage();

    QImage frame(display.size(), QImage::Format_ARGB32);
    frame.fill(Qt::black);
    display.render(&frame);

    const QRect cell(display.contentsRect().left() + display.margin(),
                     display.contentsRect().top() + display.margin(), cw, ch);
    const QColor center = frame.pixelColor(cell.center());
    QVERIFY(center.red() > 200 && center.green() < 80 && center.blue() < 80); // 红色（id 大者）在上
}
```

注意：绘制层（`drawImagesAboveText` 等）尚未实现，此时 `testZPositiveDrawnOverText` 与 `testSameZOrderedByImageId` 应失败（图像不画/次序不对），`testZNegativeDrawnUnderText` 在 z<0 尚未接入时红色缺失也失败。运行确认失败：

```bash
cmake --build build --parallel && ctest --test-dir build -R tst_kittygraphics --output-on-failure 2>&1 | tail -20
```

- [ ] **步骤 4.2：TerminalDisplay.h 声明**

`lib/src/display/TerminalDisplay.h`：将 `drawSixelImages` 声明（757 行附近）替换为：

```cpp
    /**
     * @brief 文本下层图形通道：sixel 图像切片 + kitty z<0 放置。
     * @note 在 paintEvent 中 drawBackground 之后、drawContents 之前调用；
     *       切片与放置经 Screen::imagePlacements()/kittyRefs() 按可见行查询。
     */
    void drawImagesBelowText(QPainter &paint, const QRect &rect);

    /**
     * @brief 文本上层图形通道：kitty z>=0 放置（半透明按 z 序 alpha 混合）。
     * @note 在 paintEvent 中 drawContents 之后调用；光标块由 redrawCursorOverImages() 补绘，
     *       保证"图像在文本之上、光标之下"。
     */
    void drawImagesAboveText(QPainter &paint, const QRect &rect);

    /**
     * @brief 光标复绘：z>=0 kitty 放置覆盖光标矩形时，在图像之上重绘光标块。
     * @note 光标块内嵌于 drawContents 的文本逐片段绘制（RE_CURSOR），
     *       上层图像会盖住它；本函数在 paintEvent 末尾按原样式补绘一次。
     */
    void redrawCursorOverImages(QPainter &paint);

private:
    /** @brief 按 aboveText 过滤绘制 kitty 放置（z 升序、同 z 按 imageId、再按插入序）。 */
    void drawKittyPlacements(QPainter &paint, const QRect &rect, bool aboveText);
```

（原 `drawContentsLegacy` 等声明保持不动；若 757 行前后在 private 区内，则无需再加 `private:`，按实际区段调整。）

- [ ] **步骤 4.3：TerminalDisplay.cpp 实现**

1. 1941 行 `drawSixelImages` 改名 `drawImagesBelowText`，函数体保留 sixel 切片循环，末尾追加 kitty z<0 通道调用：

```cpp
void TerminalDisplay::drawImagesBelowText(QPainter &paint, const QRect &rect)
{
    if (!_screenWindow)
        return;
    Screen *screen = _screenWindow->screen();
    if (!screen)
        return;

    const QPoint tL = contentsRect().topLeft();
    // 与 drawContents 相同的可见行范围换算
    const int luy = qMin(_usedLines - 1,
                         qMax(0, (rect.top() - tL.y() - _topMargin) / _fontHeight));
    const int rly = qMin(_usedLines - 1,
                         qMax(0, (rect.bottom() - tL.y() - _topMargin) / _fontHeight));
    const int topLine = _screenWindow->currentLine(); // 窗口第 0 行对应的绝对行

    // sixel 图像切片（文本下层，xterm/wezterm 语义）
    for (int y = luy; y <= rly; y++) {
        const auto placements = screen->imagePlacements(topLine + y);
        for (const ImagePlacement &p : placements) {
            const ScreenImage *img = screen->image(p.imageId);
            if (!img)
                continue;
            // 本行显示图像的第 rowOffset 个水平切片；末行切片可能不足一整行高，
            // 部分滚出的行经 QPainter 重绘区域自动裁剪
            const int srcY = p.rowOffset * _fontHeight;
            if (srcY >= img->image.height())
                continue;
            const int sliceH = qMin(_fontHeight, img->image.height() - srcY);
            const QRect target(_leftMargin + tL.x() + p.startCol * _fontWidth,
                               _topMargin + tL.y() + y * _fontHeight,
                               img->image.width(), sliceH);
            if (!target.intersects(rect))
                continue;
            const QRect source(0, srcY, img->image.width(), sliceH);
            paint.drawImage(target, img->image, source);
        }
    }

    drawKittyPlacements(paint, rect, false); // kitty z<0 放置同在文本下层
}

void TerminalDisplay::drawImagesAboveText(QPainter &paint, const QRect &rect)
{
    drawKittyPlacements(paint, rect, true); // kitty z>=0 放置：文本之上、光标之下
}

void TerminalDisplay::drawKittyPlacements(QPainter &paint, const QRect &rect, bool aboveText)
{
    if (!_screenWindow)
        return;
    Screen *screen = _screenWindow->screen();
    if (!screen)
        return;

    const QPoint tL = contentsRect().topLeft();
    const int luy = qMin(_usedLines - 1,
                         qMax(0, (rect.top() - tL.y() - _topMargin) / _fontHeight));
    const int rly = qMin(_usedLines - 1,
                         qMax(0, (rect.bottom() - tL.y() - _topMargin) / _fontHeight));
    const int topLine = _screenWindow->currentLine();

    // 收集可见放置：每个放置只从锚定行（rowOffset==0 的引用所在行）画一次
    struct Item {
        const KittyPlacement *pl;
        int viewRow; ///< rowOffset==0 引用所在的视图行（锚定行的当前位置，随滚动迁移）
    };
    QVector<Item> items;
    for (int y = luy; y <= rly; y++) {
        for (const KittyPlacementRef &ref : screen->kittyRefs(topLine + y)) {
            if (ref.rowOffset != 0)
                continue;
            const KittyPlacement *pl = screen->kittyPlacement(ref.placementHandle);
            if (!pl || (pl->zIndex >= 0) != aboveText)
                continue;
            items.append({pl, y});
        }
    }
    // z-index 排序：z 升序；同 z 重叠时 id 小者更低层；同 z 同 id 按插入序
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.pl->zIndex != b.pl->zIndex)
            return a.pl->zIndex < b.pl->zIndex;
        if (a.pl->imageId != b.pl->imageId)
            return a.pl->imageId < b.pl->imageId;
        return a.pl->serial < b.pl->serial;
    });

    const int rightEdge = _leftMargin + tL.x() + _usedColumns * _fontWidth;
    for (const Item &item : items) {
        const KittyPlacement *pl = item.pl;
        const ScreenImage *img = screen->image(pl->imageHandle);
        if (!img)
            continue;
        const QRect src = QRect(pl->srcX, pl->srcY, pl->srcW, pl->srcH) & img->image.rect();
        if (src.isEmpty())
            continue;
        // 目标矩形：(col, 锚定行) 单元格起 + X/Y 像素偏移，尺寸 c×r 单元格，超右缘截断
        const int tx = _leftMargin + tL.x() + pl->col * _fontWidth + pl->cellXOff;
        const int ty = _topMargin + tL.y() + item.viewRow * _fontHeight + pl->cellYOff;
        const int fullW = pl->cols * _fontWidth;
        const QRect target(tx, ty, qMin(fullW, rightEdge - tx), pl->rows * _fontHeight);
        if (target.width() <= 0 || !target.intersects(rect))
            continue;
        // 右侧截断时源矩形等比收缩，保持映射比例
        QRect s = src;
        if (fullW > target.width())
            s.setWidth(int(qint64(src.width()) * target.width() / fullW));
        paint.drawImage(target, img->image, s); // 半透明按 z 序 alpha 混合
    }
}

void TerminalDisplay::redrawCursorOverImages(QPainter &paint)
{
    if (!_screenWindow)
        return;
    Screen *screen = _screenWindow->screen();
    if (!screen)
        return;
    const QPoint cp = cursorPosition(); // 视图坐标
    if (cp.x() < 0 || cp.x() >= _usedColumns || cp.y() < 0 || cp.y() >= _usedLines)
        return;
    const Character &ch = _image[loc(cp.x(), cp.y())];
    if (!(ch.rendition & RE_CURSOR))
        return; // 光标隐藏（MODE_Cursor 关）或视图回看中
    const QPoint tL = contentsRect().topLeft();
    const QRect cursorRect(_leftMargin + tL.x() + cp.x() * _fontWidth,
                           _topMargin + tL.y() + cp.y() * _fontHeight,
                           _fontWidth, _fontHeight);
    // 仅当存在覆盖光标矩形的 z>=0 放置时才复绘（无图零开销短路）
    const int topLine = _screenWindow->currentLine();
    bool covered = false;
    for (int y = 0; y < _usedLines && !covered; y++) {
        for (const KittyPlacementRef &ref : screen->kittyRefs(topLine + y)) {
            if (ref.rowOffset != 0)
                continue;
            const KittyPlacement *pl = screen->kittyPlacement(ref.placementHandle);
            if (!pl || pl->zIndex < 0)
                continue;
            const QRect target(_leftMargin + tL.x() + pl->col * _fontWidth + pl->cellXOff,
                               _topMargin + tL.y() + y * _fontHeight + pl->cellYOff,
                               pl->cols * _fontWidth, pl->rows * _fontHeight);
            if (target.intersects(cursorRect)) {
                covered = true;
                break;
            }
        }
    }
    if (!covered)
        return;
    const QColor fg = ch.foregroundColor.color(_colorTable);
    const QColor bg = ch.backgroundColor.color(_colorTable);
    bool invert = false;
    drawCursor(paint, cursorRect, fg, bg, invert, true); // 与 preedit 路径同式的光标块复绘
}
```

2. paintEvent 的 rect 循环（1722-1727 行）改为四层，循环结束后补光标复绘：

```cpp
    const QRegion regToDraw = pe->region() & cr;
    for (auto rect = regToDraw.begin(); rect != regToDraw.end(); rect++) {
        drawBackground(paint, *rect, _colorTable[DEFAULT_BACK_COLOR].color,
                                     true /* use opacity setting */);
        drawImagesBelowText(paint, *rect); // sixel 图像 + kitty z<0：文本层之下
        drawContents(paint, *rect);
        drawImagesAboveText(paint, *rect); // kitty z>=0：文本层之上、光标之下
    }
    redrawCursorOverImages(paint); // 上层图像盖住光标块时补绘光标
```

3. updateImage 的两视图并集置脏（1420-1427 行）扩展 kitty 判定：

```cpp
        // 滚动前后两个视图中实际含图像放置的行强制整行标脏（sixel 切片与 kitty 放置同理）：
        // 新视图含图行补画，滚动前视图含图行抹除残留
        if (hasImages && !updateLine
                && (!scr->imagePlacements(viewTopLine + y).isEmpty()
                    || !scr->kittyRefs(viewTopLine + y).isEmpty()
                    || (prevViewTopLine != viewTopLine
                        && (!scr->imagePlacements(prevViewTopLine + y).isEmpty()
                            || !scr->kittyRefs(prevViewTopLine + y).isEmpty())))) {
            updateLine = true;
        }
```

4. 1367-1370 行注释中"sixel 图像切片"措辞扩展为"sixel/kitty 图像"（`hasImages()` 语义已在任务 2 覆盖 kitty 落库图像）。

`TerminalDisplay.cpp` 顶部确认 `#include <algorithm>`（std::sort；若已通过其他头间接引入也显式补上）。

运行：

```bash
cmake --build build --parallel && ctest --test-dir build --output-on-failure
```

预期：`100% tests passed, 0 tests failed out of 9`（含三个 z-index 离屏渲染用例）。

```bash
git add -A && git commit -m "feat(display): Kitty 图形 z-index 双层叠加绘制（文本下层泛化 + 文本上层通道）"
```

---

## 任务 5：文档收尾 + 全量验证

- [ ] **步骤 5.1：CHANGELOG / README 更新**

`CHANGELOG` 顶部（现有 "ZzQTermWidget Sixel 图形协议 / 2026-08-13" 条目之前）追加新条目：

```
ZzQTermWidget Kitty 图形协议（核心子集） / 2026-08-13
---------------------------------------------

 * 终端内嵌位图显示（Kitty 图形协议核心子集）：APC 通道（ESC _ G ... ESC \）分块传输，
   a=t/T/p/d/q 动作，f=100/32/24 像素格式，o=z zlib 压缩，i=/p= id 管理，源矩形裁剪 +
   单元格偏移 + c/r 显示区 + z-index 分层（z<0 文本之下、z>=0 文本之上、光标之下），
   d=a/i(+p)/c 大小写删除语义，OK/ENOENT/EINVAL/ENOSPC 应答（q=1/2 抑制），同 id 重传先删旧图。
 * 统一图形锚定层：Sixel 存储泛化为协议无关结构（ScreenImage/_images），sixel/kitty 共享
   行级引用、六个生命周期挂钩与 256MB 像素预算；预算紧张时优先淘汰无放置引用的图像。
 * 明确不做：动画、Unicode 占位符、文件/共享内存传输（t=f/t/s）、相对放置、图像编号 I=。
```

`README.md` 特性列表（30 行 Sixel 条目之后）追加：

```markdown
- 支持 Kitty 图形协议核心子集（APC 分块传输、PNG/RGBA/RGB、zlib 压缩、z-index 分层显示、id/放置管理与删除语义、命令应答）。
```

`AGENTS.md`：目录结构节无需变更（新文件落在既有 `lib/src/emulation/`、`tests/`）；"测试"节的套件数量描述如有点名数量则更新为 9 个（当前 AGENTS.md 未列数量，检查后可跳过）。

- [ ] **步骤 5.2：全量验证**

```bash
rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/zz/Qt/6.11.1/gcc_64
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

预期：`100% tests passed, 0 tests failed out of 9`。

人工检查清单：
- `git grep -n "SixelImage\|sixelImage\|_sixelImages\|drawSixelImages" -- '*.h' '*.cpp'` 无残留（除 SixelDecoder/sixel 通道本身）。
- 新代码注释全部中文 Doxygen；`lib/third_party/` 无改动（`git diff` 确认）。
- 命名总表逐项核对（KittyCommand 字段、Screen kitty 方法名、TerminalDisplay 三个新函数名）。

- [ ] **步骤 5.3：提交**

```bash
git add CHANGELOG README.md AGENTS.md
git commit -m "docs: Kitty 图形协议核心子集轮次收尾（CHANGELOG/README）"
```

---

## 自检清单（交付前逐项确认）

- [ ] 9 个测试套件全绿（8 旧 + tst_kittygraphics）。
- [ ] 命名与"命名总表"完全一致（跨任务无漂移）。
- [ ] sixel 路径行为不变：tst_sixel 未改动且全绿；sixel 光标仍移到图下（xterm 语义），kitty 光标右移 cols + 下移 rows、C=1 不动，两条路径并存。
- [ ] 应答字节与规格精确一致：成功 `\x1b_Gi=<id>[,p=<pid>];OK\x1b\\`；失败 `\x1b_Gi=<id>;<code>:<msg>\x1b\\`；q=1 抑成功、q=2 抑失败。
- [ ] 重传语义：同 id 重传先删旧图及全部放置，新数据落库不自动显示（testRetransmitDeletesOldPlacements 覆盖）。
- [ ] 匿名图像 i=0：可显示、不占 id 命名空间、随最后放置死亡释放（testScreenAnonymousImageFreedWithPlacement 覆盖）。
- [ ] 350MB APC 上限、10000×10000 尺寸上限、256MB 共享预算（含无引用淘汰）均有测试覆盖。
- [ ] 已知遗留（规格第 9 节）未被误实现：动画、Unicode 占位符、t=f/t/s、相对放置、I=、其余删除变体、z<INT32_MIN/2 细分档。
