#include <QtTest>
#include <QFontDatabase>
#include <string_view>
#include "LigatureHelper.h"
#include "TerminalDisplay.h"

/**
 * @brief LigatureHelper 判定设施与连字开关 API 单测。
 * @note 候选字符集 / 子区间探测为纯逻辑；宽度校验用系统等宽字体
 *       （offscreen 平台由 tests/CMakeLists.txt 统一注入）。
 */
class TestLigature : public QObject
{
    Q_OBJECT
private slots:
    void testCandidateMask();
    void testFindCandidateSpans();
    void testWidthMatches();
    void testSwitchApi();
};

/**
 * @brief 可连字字符集：恰好为 ASCII 运算符区
 *        "! # $ % & * + - . / : ; < = > ? @ \ ^ _ { | } ~"；
 *        空格、字母数字、逗号、括号、引号与非 ASCII 一律排除。
 */
void TestLigature::testCandidateMask()
{
    for (char c : std::string_view("!#$%&*+-./:;<=>?@\\^_{|}~"))
        QVERIFY2(LigatureHelper::isCandidateChar(char32_t(c)),
                 qPrintable(QStringLiteral("候选字符 '%1' 未命中").arg(c)));
    for (char c : std::string_view(" abcXYZ019,()[]\"'`"))
        QVERIFY2(!LigatureHelper::isCandidateChar(char32_t(c)),
                 qPrintable(QStringLiteral("非候选字符 '%1' 误命中").arg(c)));
    QVERIFY(!LigatureHelper::isCandidateChar(0x4E2D));  // 中
    QVERIFY(!LigatureHelper::isCandidateChar(0x2500));  // ─（制表符）
    QVERIFY(!LigatureHelper::isCandidateChar(0));       // 宽字符尾格占位
}

/**
 * @brief 子区间探测：连续候选字符段长度 ≥ 2 才输出，段间/串尾收尾正确，
 *        输出按起始索引升序且互不重叠。
 */
void TestLigature::testFindCandidateSpans()
{
    using Span = LigatureHelper::Span;
    const auto spans = [](const char32_t *s) {
        return LigatureHelper::findCandidateSpans(std::u32string(s));
    };
    { // 单段居中
        const QVector<Span> r = spans(U"a->b");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].start, 1);
        QCOMPARE(r[0].length, 2);
    }
    { // 全长段、长度 3
        const QVector<Span> r = spans(U"==>");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].start, 0);
        QCOMPARE(r[0].length, 3);
    }
    // 孤立候选字符（长度 1）不构成连字序列
    QVERIFY(spans(U"-a-").isEmpty());
    { // 多段
        const QVector<Span> r = spans(U"a--b--c");
        QCOMPARE(r.size(), 2);
        QCOMPARE(r[0].start, 1);
        QCOMPARE(r[0].length, 2);
        QCOMPARE(r[1].start, 4);
        QCOMPARE(r[1].length, 2);
    }
    // 空串、无候选内容（字母/空格/括号/引号/逗号/CJK）
    QVERIFY(spans(U"").isEmpty());
    QVERIFY(spans(U"if (a, b) \"x\"").isEmpty());
    QVERIFY(spans(U"\x4E2D\x6587").isEmpty());
    { // 段尾在串尾收尾
        const QVector<Span> r = spans(U"x!=");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].start, 1);
        QCOMPARE(r[0].length, 2);
    }
}

/**
 * @brief 整形宽度校验：以实测整形宽度反推格宽（字体无关），
 *        期望宽人为偏移即超出 0.5px 容差判定失败；空串不可连字。
 * @note 早期版本假设系统等宽字体步进均匀，macOS CI 的 "Monospace" 族缺失
 *       回退到比例字体时不成立（CI 故障根因 B）；现只断言容差逻辑本身。
 *       Windows Courier New 等字体的单格 advance 恰为 X.5px：qRound(shaped/2)
 *       与真实半宽差 0.25px，2 格累计误差 0.5px 压线/越线造成假失败（MinGW CI
 *       实测），故对 pixelSize 8..40 扫描系统 FixedFont，取首个满足整数格宽
 *       容差的字号做断言；全部字号都不满足则该环境字体度量无法构造整数格宽，
 *       QSKIP。
 */
void TestLigature::testWidthMatches()
{
    const QString arrow = QString::fromStdU32String(U"->");
    const QFont base = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QFontMetricsF fm(base);
    qreal shaped = 0;
    int cellWidth = 0;
    int pixelSize = -1;
    for (int ps = 8; ps <= 40; ps++) {
        QFont f = base;
        f.setPixelSize(ps);
        const QFontMetricsF m(f);
        const qreal w = m.horizontalAdvance(arrow);
        const int cw = qRound(w / 2);
        if (w > 0 && qAbs(w - 2.0 * cw) <= 0.5) {
            fm = m;
            shaped = w;
            cellWidth = cw;
            pixelSize = ps;
            break;
        }
    }
    if (pixelSize < 0)
        QSKIP("系统 FixedFont 在 pixelSize 8..40 内无满足整数格宽容差（±0.5px）的"
              "字号（如单格 advance 恒为 X.5px 的字体），无法构造宽度校验断言");
    QVERIFY2(LigatureHelper::widthMatches(fm, U"->", cellWidth),
             qPrintable(QStringLiteral("pixelSize %1：shaped=%2 与 2×格宽 %3 应在容差内")
                        .arg(pixelSize).arg(shaped).arg(cellWidth)));
    // 期望宽每格 +1px：2 格串差 ≥1.5px，超出 ±0.5px 容差
    QVERIFY(!LigatureHelper::widthMatches(fm, U"->", cellWidth + 1));
    QVERIFY(!LigatureHelper::widthMatches(fm, U"", cellWidth));
}

/**
 * @brief 开关 API：默认关（opt-in），set/get 往返正确。
 */
void TestLigature::testSwitchApi()
{
    TerminalDisplay display;
    QVERIFY(!display.ligaturesEnabled());
    display.setLigaturesEnabled(true);
    QVERIFY(display.ligaturesEnabled());
    display.setLigaturesEnabled(false);
    QVERIFY(!display.ligaturesEnabled());
}

QTEST_MAIN(TestLigature)
#include "tst_ligature.moc"
