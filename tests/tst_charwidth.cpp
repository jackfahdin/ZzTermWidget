#include <QtTest>
#include "CharWidth.h"

/**
 * @brief CharWidth 字符宽度判定的基线回归测试。
 *
 * 覆盖 utf8proc 宽度判定与 fork 的特殊规则（PUA=1、易经符号=2）。
 */
class TestCharWidth : public QObject
{
    Q_OBJECT
private slots:
    void testAscii();
    void testCjk();
    void testPrivateUse();
    void testYiJing();
    void testCombining();
    void testEmoji();
};

void TestCharWidth::testAscii()
{
    QCOMPARE(CharWidth::unicode_width(U'a'), 1);
}

void TestCharWidth::testCjk()
{
    QCOMPARE(CharWidth::unicode_width(U'中'), 2);
}

void TestCharWidth::testPrivateUse()
{
    // PUA（私用区）强制宽度 1（对齐 tmux/glibc 行为）
    QCOMPARE(CharWidth::unicode_width(char32_t(0xE000)), 1);
}

void TestCharWidth::testYiJing()
{
    // 易经六十四卦符号强制宽度 2
    QCOMPARE(CharWidth::unicode_width(char32_t(0x4DC0)), 2);
    QCOMPARE(CharWidth::unicode_width(char32_t(0x4DFF)), 2);
}

void TestCharWidth::testCombining()
{
    // 组合用变音符（U+0301）宽度 0
    QCOMPARE(CharWidth::unicode_width(char32_t(0x0301)), 0);
}

void TestCharWidth::testEmoji()
{
    // BMP 外字符：当前 wchar_t 接口在 Windows 会截断；char32_t 改造后必须稳定为 2
    QCOMPARE(CharWidth::unicode_width(char32_t(0x1F600)), 2);
}

QTEST_APPLESS_MAIN(TestCharWidth)
#include "tst_charwidth.moc"
