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
