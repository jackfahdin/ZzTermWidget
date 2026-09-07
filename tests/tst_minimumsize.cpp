#include <QtTest>
#include <QFontDatabase>

#include "qtermwidget.h"

/**
 * @brief 选一个真实存在的等宽字体（跨平台测试环境用）。
 * @return 按平台习惯优先 DejaVu Sans Mono/Menlo/Consolas/Courier New，
 *         再退任意 fixedPitch 族，最后回退系统 FixedFont。
 */
static QFont monospaceFont()
{
    static const QStringList preferred = {
        QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Menlo"),
        QStringLiteral("Consolas"),         QStringLiteral("Courier New"),
    };
    QFontDatabase db;
    const QStringList available = db.families();
    for (const QString &name : preferred)
        if (available.contains(name))
            return QFont(name);
    for (const QString &name : available)
        if (db.isFixedPitch(name))
            return QFont(name);
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

/**
 * @brief setMinimumTerminalSize 的回归测试：极端小尺寸下终端网格不得
 *        低于设定的最小行列数（防止 shell 重绘模型退化产生显示残迹）。
 */
class TestMinimumSize : public QObject
{
    Q_OBJECT
private slots:
    /** @brief 默认不限制：最小尺寸为零。 */
    void defaultNoLimit();
    /** @brief 设置后控件像素最小尺寸与网格联动，resize 不得突破。 */
    void minimumGridEnforced();
    /** @brief 取消限制后恢复为零。 */
    void clearLimit();
    /** @brief 字体放大后像素最小尺寸随字体度量联动增大。 */
    void fontChangeRefreshesMinimum();
};

void TestMinimumSize::defaultNoLimit()
{
    QTermWidget w;
    QCOMPARE(w.minimumSize(), QSize(0, 0));
}

void TestMinimumSize::minimumGridEnforced()
{
    QTermWidget w;
    w.setTerminalFont(monospaceFont());
    w.setMinimumTerminalSize(24, 6);

    const QSize minSize = w.minimumSize();
    QVERIFY(minSize.width() > 0 && minSize.height() > 0);

    // 试图把控件压到 1x1：Qt 会用像素最小尺寸兜底（无需 show，resize 即受约束）
    w.resize(1, 1);
    QCoreApplication::processEvents();
    QVERIFY(w.width() >= minSize.width());
    QVERIFY(w.height() >= minSize.height());

    // 先放大再压小，终端实际网格不得低于设定的最小行列数
    w.resize(2000, 1000);
    QCoreApplication::processEvents();
    w.resize(1, 1);
    QCoreApplication::processEvents();
    QTRY_VERIFY(w.screenColumnsCount() >= 24);
    QTRY_VERIFY(w.screenLinesCount() >= 6);
}

void TestMinimumSize::clearLimit()
{
    QTermWidget w;
    w.setTerminalFont(monospaceFont());
    w.setMinimumTerminalSize(24, 6);
    QVERIFY(w.minimumSize().width() > 0);

    w.setMinimumTerminalSize(0, 0);
    QCOMPARE(w.minimumSize(), QSize(0, 0));
}

void TestMinimumSize::fontChangeRefreshesMinimum()
{
    QTermWidget w;
    QFont small = monospaceFont();
    small.setPointSize(8);
    w.setTerminalFont(small);
    w.setMinimumTerminalSize(24, 6);
    const QSize before = w.minimumSize();

    QFont big = monospaceFont();
    big.setPointSize(20);
    w.setTerminalFont(big);
    const QSize after = w.minimumSize();

    QVERIFY(after.width() > before.width());
    QVERIFY(after.height() > before.height());
}

QTEST_MAIN(TestMinimumSize)
#include "tst_minimumsize.moc"
