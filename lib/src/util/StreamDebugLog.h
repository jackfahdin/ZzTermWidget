#pragma once

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>

/**
 * @brief 诊断用的原始字节流日志（仅调试用）。
 *
 * 设置环境变量 ZZQTW_STREAM_LOG=<文件路径> 后启用：
 * 记录远端下行字节流（RECV）、终端尺寸变更（SIZE）、
 * 光标定位越界钳制（CUP-CLAMP）等事件，用于排查
 * resize 与整屏重绘交互导致的显示错乱。
 * 未设置环境变量时开销为零。
 */
namespace StreamDebugLog {

/**
 * @brief 返回日志文件指针；未启用时返回 nullptr。
 * @return 已追加模式打开的 QFile，进程生命周期内单例。
 */
inline QFile *logFile()
{
    static QFile *file = []() -> QFile * {
        const QByteArray path = qgetenv("ZZQTW_STREAM_LOG");
        if (path.isEmpty()) {
            return nullptr;
        }
        auto *f = new QFile(QString::fromLocal8Bit(path));
        if (!f->open(QIODevice::Append | QIODevice::WriteOnly)) {
            delete f;
            return nullptr;
        }
        return f;
    }();
    return file;
}

/**
 * @brief 返回进程启动以来的毫秒时间戳基准。
 * @return 首次调用时刻的 Unix 毫秒时间。
 */
inline qint64 baseTime()
{
    static const qint64 base = QDateTime::currentMSecsSinceEpoch();
    return base;
}

/**
 * @brief 写一条带相对时间戳的文本事件。
 * @param text 单行文本内容（不含换行）。
 */
inline void writeLine(const QString &text)
{
    QFile *f = logFile();
    if (!f) {
        return;
    }
    static QMutex mutex;
    const QMutexLocker locker(&mutex);
    const qint64 rel = QDateTime::currentMSecsSinceEpoch() - baseTime();
    f->write(QStringLiteral("T+%1 %2\n").arg(rel).arg(text).toUtf8());
    f->flush();
}

/**
 * @brief 把原始字节流转义为可读的 ASCII 形式。
 * @param data 字节流指针。
 * @param len 字节数。
 * @return 转义后的字符串：ESC→\e，CR→\r，LF→\n，其余不可打印字节→\xNN。
 */
inline QString escape(const char *data, int len)
{
    QString out;
    out.reserve(len * 2);
    for (int i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == 0x1b) {
            out += QStringLiteral("\\e");
        } else if (c == '\r') {
            out += QStringLiteral("\\r");
        } else if (c == '\n') {
            out += QStringLiteral("\\n");
        } else if (c >= 0x20 && c < 0x7f) {
            out += QLatin1Char(static_cast<char>(c));
        } else {
            out += QStringLiteral("\\x%1").arg(c, 2, 16, QLatin1Char('0'));
        }
    }
    return out;
}

} // namespace StreamDebugLog
