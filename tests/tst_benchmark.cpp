#include <QtTest>
#include <QFontDatabase>
#include <QImage>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "Screen.h"
#include "TerminalDisplay.h"

/**
 * @brief 渲染性能 benchmark 基线：解析吞吐 / 全量重绘 / TUI 局部重绘三用例。
 * @note 不设硬性性能断言（机器差异大），仅保证可编译可运行；优化前后各跑一遍，
 *       数字人工对比并记入 CHANGELOG。数值仅在 Release 构建下有参考意义。
 */
class TestBenchmark : public QObject
{
    Q_OBJECT
private slots:
    void testParseThroughput();
    void testDrawFullRepaint();
    void testTuiPartialRepaint();
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
 * @brief 构造显示测试环境：仿真 + 窗口 + 离屏显示组件（24x80，等宽字体，关闪烁）。
 */
static void initDisplayEnv(Vt102Emulation &emu, ScreenWindow *&win, TerminalDisplay &display)
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
 * @brief TUI 局部重绘：模拟 nvim 帧负载——光标行内容 + 状态行交替小区域更新。
 */
void TestBenchmark::testTuiPartialRepaint()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initDisplayEnv(emu, win, display);
    const QByteArray content = buildMixedPayload(24);
    emu.receiveData(content.constData(), int(content.size()));
    display.updateImage();
    const QByteArray frame =
            "\033[2;5H\033[38;5;45mmain.cpp\033[0m"            // 光标行内容更新
            "\033[24;1H\033[7m NORMAL  main.cpp  12:5  utf-8 \033[0m" // 状态行重写
            "\033[2;10H";                                       // 光标归位
    QImage image(display.size(), QImage::Format_ARGB32);
    display.render(&image); // warmup
    QBENCHMARK {
        emu.receiveData(frame.constData(), int(frame.size()));
        display.updateImage();   // public slot，绕过 bufferedUpdate 定时器直接驱帧
        display.render(&image);
    }
}

QTEST_MAIN(TestBenchmark)
#include "tst_benchmark.moc"
