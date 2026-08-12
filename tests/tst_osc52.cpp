#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include "Vt102Emulation.h"

/**
 * @brief OSC 52 剪贴板访问开关的行为测试。
 */
class TestOsc52 : public QObject
{
    Q_OBJECT
private slots:
    void testDisabledBySwitch();
    void testEnabledByDefault();
};

void TestOsc52::testDisabledBySwitch()
{
    Vt102Emulation emu;
    // 显式 setCodec 与真实应用路径一致（同 tst_emulation 的约定）
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    emu.setOsc52Enabled(false);
    QApplication::clipboard()->clear();
    // OSC 52 ; c ; aGVsbG8= (base64 "hello") BEL
    emu.receiveData("\033]52;c;aGVsbG8=\007", 16);
    QVERIFY(QApplication::clipboard()->text().isEmpty());
}

void TestOsc52::testEnabledByDefault()
{
    Vt102Emulation emu;
    // 显式 setCodec 与真实应用路径一致（同 tst_emulation 的约定）
    emu.setCodec(QStringEncoder(QStringConverter::Utf8));
    emu.setImageSize(24, 80);
    QApplication::clipboard()->clear();
    emu.receiveData("\033]52;c;aGVsbG8=\007", 16);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello"));
}

QTEST_MAIN(TestOsc52)
#include "tst_osc52.moc"
