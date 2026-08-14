#include <QtTest>
#include <QFontDatabase>
#include <QImage>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "Screen.h"
#include "TerminalDisplay.h"

/**
 * @brief 渲染性能 benchmark 基线：解析吞吐 / 全量重绘 / 局部刷新 / 整屏滚动四用例。
 * @note 不设硬性性能断言（机器差异大），仅保证可编译可运行；优化前后各跑一遍，
 *       数字人工对比并记入 CHANGELOG。数值仅在 Release 构建下有参考意义。
 * @note 局部刷新与整屏滚动为真实增量口径：receiveData 产帧 → notifyOutputChanged()
 *       驱动 updateImage()（脏区比对段）→ 按 lastDirtyRegion() 离屏渲染（渲染段）；
 *       帧负载交替变化保证每次迭代都有真实脏区。
 *       帧驱动必须走 notifyOutputChanged()（生产环境 outputChanged→updateImage 通路）：
 *       循环内无事件循环，bufferedUpdate 定时器不触发，直接调 updateImage() 只会拿
 *       陈旧 _windowBuffer 比对出空脏区（空 region 的 render 退化为整幅渲染）。
 *       同理 showBulk() 不触发，Screen::scrolledLines 跨帧累积，滚动用例须在
 *       notifyOutputChanged() 后复位滚动/丢行计数，否则 scrollCount 失真。
 */
class TestBenchmark : public QObject
{
    Q_OBJECT
private slots:
    void testParseThroughput();
    void testDrawFullRepaint();
    void testLocalRefresh();
    void testFullScreenScroll_data();
    void testFullScreenScroll();
    void testLigatureRefresh_data();
    void testLigatureRefresh();
private:
    int m_frame = 0; ///< 帧奇偶计数：交替负载保证每次迭代都产生真实脏区
};

/**
 * @brief 构造混合内容负载：普通文本 / SGR 颜色转义 / CJK 宽字符 / 样式与制表符混排。
 * @param lineCount 生成行数。
 * @return UTF-8 编码的字节流。
 */
static QByteArray buildMixedPayload(int lineCount)
{
    QByteArray payload;
    for (int i = 0; i < lineCount; i++) {
        switch (i % 4) {
        case 0:
            payload += "build/output line " + QByteArray::number(i)
                     + ": plain ascii text lorem ipsum dolor sit\r\n";
            break;
        case 1:
            payload += "\033[38;5;" + QByteArray::number(16 + (i % 216))
                     + "m256色前景 \033[48;2;30;120;200mRGB底色\033[0m 混合样式输出\r\n";
            break;
        case 2:
            payload += "CJK 宽字符混排：中文测试文本 abc 123 コンソール 端末エミュレータ\r\n";
            break;
        default:
            payload += "\033[1;3;4m粗斜下划线\033[0m 制表符 ─│┌┐└┘ 与 emoji 😀 混排\r\n";
            break;
        }
    }
    return payload;
}

/**
 * @brief 构造整屏滚动负载：持续行输出（行号递增）使视图匀速上滚。
 * @param from 起始行号。 @param count 行数。
 * @return UTF-8 编码的字节流。
 */
static QByteArray buildScrollPayload(int from, int count)
{
    QByteArray payload;
    for (int i = from; i < from + count; i++)
        payload += "scroll payload line " + QByteArray::number(i)
                 + " mixed 文本 abc 123 lorem ipsum\r\n";
    return payload;
}

/**
 * @brief 构造显示测试环境：仿真 + 窗口 + 离屏显示组件（等宽字体，关闪烁）。
 * @param lines/columns 终端行列数（默认 24x80；滚动场景另测 40x160 大屏变体）。
 */
static void initDisplayEnv(Vt102Emulation &emu, ScreenWindow *&win, TerminalDisplay &display,
                           int lines = 24, int columns = 80)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(lines, columns);
    win = emu.createWindow();
    win->setWindowLines(lines);
    display.setVTFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    display.setBlinkingCursor(false);
    display.setBlinkingTextEnabled(false);
    display.setScreenWindow(win);
    display.resize(columns * 10, lines * 25);
}

/**
 * @brief 解析吞吐：向 Emulation+Screen 喂大块混合输出，计量 receiveData 吞吐。
 */
void TestBenchmark::testParseThroughput()
{
    Vt102Emulation emu;
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    ScreenWindow *win = emu.createWindow();
    win->setWindowLines(24);
    const QByteArray payload = buildMixedPayload(2000); // 约 200KB 混合内容
    QBENCHMARK {
        emu.receiveData(payload.constData(), int(payload.size()));
    }
}

/**
 * @brief 绘制吞吐：预填满屏混合内容后，循环全量重绘（render 到 QImage）。
 */
void TestBenchmark::testDrawFullRepaint()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initDisplayEnv(emu, win, display);
    const QByteArray content = buildMixedPayload(24);
    emu.receiveData(content.constData(), int(content.size()));
    display.updateImage();
    QImage image(display.size(), QImage::Format_ARGB32);
    display.render(&image); // warmup：吃掉 _drawTextTestFlag 一次性度量
    QBENCHMARK {
        display.render(&image);
    }
}

/**
 * @brief 局部刷新：模拟 nvim 帧负载——光标行内容 + 状态行时钟交替小区域更新。
 * @note 真实增量口径：updateImage() 比对段 + 按 lastDirtyRegion() 离屏渲染段；
 *       帧 A/B 仅时钟字符不同，每次迭代都产生单行少格真实脏区。
 */
void TestBenchmark::testLocalRefresh()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initDisplayEnv(emu, win, display);
    const QByteArray content = buildMixedPayload(24);
    emu.receiveData(content.constData(), int(content.size()));
    display.updateImage();
    QImage image(display.size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    display.render(&image); // warmup
    const QByteArray frameA =
            "\033[2;5H\033[38;5;45mmain.cpp\033[0m"            // 光标行内容更新
            "\033[24;1H\033[7m NORMAL  main.cpp  12:5  utf-8 \033[0m" // 状态行重写
            "\033[2;10H";                                       // 光标归位
    const QByteArray frameB =
            "\033[2;5H\033[38;5;45mmain.cpp\033[0m"
            "\033[24;1H\033[7m NORMAL  main.cpp  12:6  utf-8 \033[0m"
            "\033[2;10H";
    QBENCHMARK {
        const QByteArray &frame = ((m_frame++ & 1) == 0) ? frameA : frameB;
        emu.receiveData(frame.constData(), int(frame.size()));
        win->notifyOutputChanged(); // 驱动本帧 updateImage()（脏区比对段），见文件头说明
        display.render(&image, QPoint(), display.lastDirtyRegion()); // 脏区渲染段
    }
}

void TestBenchmark::testFullScreenScroll_data()
{
    QTest::addColumn<int>("lines");
    QTest::addColumn<int>("columns");
    QTest::addColumn<bool>("fastPath");
    // 行列号为名义像素预设（columns*10 x lines*25 像素），实际格数以显示组件按字体
    // 换算为准（测试内反向对齐 Screen）；fastPath 列为滚动快路径开关的 A/B 对照
    QTest::newRow("24x80 快路径") << 24 << 80 << true;
    QTest::newRow("24x80 全量比对") << 24 << 80 << false;
    QTest::newRow("40x160 快路径") << 40 << 160 << true; // 大屏变体：比对段开销随格数放大
    QTest::newRow("40x160 全量比对") << 40 << 160 << false;
}

/**
 * @brief 整屏滚动：持续行输出使视图匀速上滚，每次迭代滚 4 行。
 * @note 滚动帧的脏区只有新进 4 行 + 1 行边界陈旧行（scrollImage 像素搬迁为既有行为，
 *       区域下沿钳到 _lines-2 使末行上方留一行未搬迁）；本用例的对比价值在
 *       updateImage() 比对段耗时（任务 3 快路径的裁决证据），fastPath 列做开关 A/B。
 * @note 首帧后对齐 Screen/窗口/显示三者行列：生产环境经 imageSizeChanged 保持一致；
 *       不一致时窗口比屏幕高，新进内容落在底部空白尾区上方而非窗口底沿，
 *       不存在纯整屏滚动形态（快路径无从命中，计量的也不是目标场景）。
 */
void TestBenchmark::testFullScreenScroll()
{
    QFETCH(int, lines);
    QFETCH(int, columns);
    QFETCH(bool, fastPath);
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initDisplayEnv(emu, win, display, lines, columns);
    display.setScrollOptimizationEnabled(fastPath);
    display.updateImage(); // 首帧：几何就位（updateImageSize 重算并同步 windowLines）
    emu.setImageSize(display.lines(), display.columns()); // 对齐三者行列
    const QByteArray content = buildScrollPayload(0, display.lines() + 10); // 填满并进入滚动态
    emu.receiveData(content.constData(), int(content.size()));
    win->notifyOutputChanged();
    win->screen()->resetScrolledLines();
    win->screen()->resetDroppedLines();
    QImage image(display.size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    display.render(&image); // warmup
    int lineNo = display.lines() + 10;
    QBENCHMARK {
        const QByteArray out = buildScrollPayload(lineNo, 4);
        lineNo += 4;
        emu.receiveData(out.constData(), int(out.size()));
        win->notifyOutputChanged(); // 驱动本帧 updateImage()，见文件头说明
        // 镜像 Emulation::showBulk() 的消费后复位，保证 scrollCount 为本帧滚动量
        win->screen()->resetScrolledLines();
        win->screen()->resetDroppedLines();
        display.render(&image, QPoint(), display.lastDirtyRegion());
    }
}

void TestBenchmark::testLigatureRefresh_data()
{
    QTest::addColumn<bool>("ligatures");
    // 开关 A/B 对照：连字场景重绘耗时增量（规格 §3.6 门禁：开启增量 <10%）
    QTest::newRow("连字重负载 关") << false;
    QTest::newRow("连字重负载 开") << true;
}

/**
 * @brief 连字场景：运算符密集的代码行负载全量重绘，开关 A/B 对照。
 * @note 沿用本套件惯例不设硬性断言（机器差异大）：两行数字人工对比并记入
 *       CHANGELOG，判定口径为规格 §3.6——开启后重绘耗时增量 <10% 为通过；
 *       关闭行同时是"开关关闭零开销"的回归参照（应与既有全量重绘基线无统计学差异）。
 * @note 拆分绘制采用"条带裁剪重绘同一布局"，整形开销 O(片段长 × 条带数)，
 *       "开"行量化的正是这个开销。
 */
void TestBenchmark::testLigatureRefresh()
{
    QFETCH(bool, ligatures);
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initDisplayEnv(emu, win, display);
    // 本 fork bidi 默认开（TerminalDisplay.cpp），连字拆分路径位于 drawCharacters 的
    // 非 bidi 分支，不关 bidi 会结构性屏蔽被测路径（拆分计数恒 0，测的是空路径）。
    // 镜像 tst_rendering.cpp testLigatureSplitPathTaken 的先例。
    display.setBidiEnabled(false);
    display.setLigaturesEnabled(ligatures);
    QByteArray content;
    for (int i = 0; i < 24; i++)
        content += "if (a[i] >= b && c != d) { x += y->z; } // => ok == done\r\n";
    emu.receiveData(content.constData(), int(content.size()));
    display.updateImage();
    QImage image(display.size(), QImage::Format_ARGB32);
    image.fill(Qt::black);
    display.render(&image); // warmup：吃掉 _drawTextTestFlag 一次性度量
    // 路径命中自检（QBENCHMARK 宏内不可放 QVERIFY，须在宏前断言）：开启连字后拆分
    // 计数必须为正值，否则本轮测量未走被测路径，数字无效；防被测路径被上游默认
    // 开关变化再次静默屏蔽后无报警流出。
    if (ligatures)
        QVERIFY(display.ligatureSplitFragmentCount() > 0);
    QBENCHMARK {
        display.render(&image);
    }
}

QTEST_MAIN(TestBenchmark)
#include "tst_benchmark.moc"
