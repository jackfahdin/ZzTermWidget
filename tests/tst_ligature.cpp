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
 */
void TestLigature::testWidthMatches()
{
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QFontMetricsF fm(fixed);
    const QString arrow = QString::fromStdU32String(U"->");
    const qreal shaped = fm.horizontalAdvance(arrow);
    QVERIFY(shaped > 0);
    // qRound 已给出最近整数格宽；若此时仍超容差（如每格步进恰为 X.5px 的字体），
    // 则不存在满足容差的整数格宽，直接显式红（不做不可收敛的扫描）
    const int cellWidth = qRound(shaped / 2);
    QVERIFY(qAbs(shaped - 2.0 * cellWidth) <= 0.5);
    QVERIFY(LigatureHelper::widthMatches(fm, U"->", cellWidth));
    // 期望宽每格 +1px：2 格串差 2px，超出 ±0.5px 容差
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
