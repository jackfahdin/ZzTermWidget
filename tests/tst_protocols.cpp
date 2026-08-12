#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <cstring>
#include "Vt102Emulation.h"
#include "ScreenWindow.h"
#include "Screen.h"
#include "Filter.h"
#include "TerminalDisplay.h"

/**
 * @brief 协议三件套的显示层/交互层测试（OSC 8 热点、同步输出攒帧、kitty 释放事件路由）。
 * @note 需要 QApplication（剪贴板与 QWidget），故独立于 tst_emulation.cpp。
 */
class TestProtocols : public QObject
{
    Q_OBJECT
private slots:
    void testOsc8HotSpotHit();
    void testOsc8CopyLinkAction();
    void testFilterChainOrderPriority();
};

/**
 * @brief 测试用过滤器：开放受保护的 addHotSpot 以便手工构造热点。
 */
class TestSpotFilter : public Filter
{
public:
    using Filter::addHotSpot;
    void process() override {}
};

/**
 * @brief 构造带 OSC 8 链接 "ab" 的仿真环境（链接位于第 0 行 0-1 列）。
 */
static void initOsc8Emu(Vt102Emulation &emu, ScreenWindow *&win)
{
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    win = emu.createWindow();
    win->setWindowLines(24);
    const char *seq = "\033]8;;https://example.com\007ab\033]8;;\007";
    emu.receiveData(seq, int(strlen(seq)));
}

void TestProtocols::testOsc8HotSpotHit()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    initOsc8Emu(emu, win);

    Osc8Filter filter;
    filter.setScreenWindow(win);
    filter.process();

    Filter::HotSpot *spot = filter.hotSpotAt(0, 1);
    QVERIFY(spot != nullptr);
    QCOMPARE(spot->type(), Filter::HotSpot::Link);
    auto *osc8Spot = dynamic_cast<Osc8Filter::HotSpot *>(spot);
    QVERIFY(osc8Spot != nullptr);
    QCOMPARE(osc8Spot->uri(), QStringLiteral("https://example.com"));
    QVERIFY(filter.hotSpotAt(0, 10) == nullptr); // 链接区域外无热点
}

void TestProtocols::testOsc8CopyLinkAction()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    initOsc8Emu(emu, win);

    Osc8Filter filter;
    filter.setScreenWindow(win);
    filter.process();

    Filter::HotSpot *spot = filter.hotSpotAt(0, 0);
    QVERIFY(spot != nullptr);
    const QList<QAction *> acts = spot->actions();
    QVERIFY(!acts.isEmpty());
    QApplication::clipboard()->clear();
    acts.first()->trigger(); // "复制链接地址"
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("https://example.com"));
}

void TestProtocols::testFilterChainOrderPriority()
{
    // OSC 8 优先机制：FilterChain::hotSpotAt 按注册顺序返回首个命中，
    // Osc8Filter 在 TerminalDisplay 构造体内先于 UrlFilter 注册
    FilterChain chain;
    TestSpotFilter first, second;
    first.addHotSpot(new Osc8Filter::HotSpot(0, 0, 0, 5, QStringLiteral("https://a/")));
    second.addHotSpot(new Osc8Filter::HotSpot(0, 0, 0, 5, QStringLiteral("https://b/")));
    chain.addFilter(&first);
    chain.addFilter(&second);
    auto *spot = dynamic_cast<Osc8Filter::HotSpot *>(chain.hotSpotAt(0, 2));
    QVERIFY(spot != nullptr);
    QCOMPARE(spot->uri(), QStringLiteral("https://a/")); // 先注册者优先
}

QTEST_MAIN(TestProtocols)
#include "tst_protocols.moc"
