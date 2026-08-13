#include "KittyGraphicsParser.h"

#include <cstring>
#include <limits>

namespace {

/**
 * @brief "4 字节大端长度前缀 + qUncompress"解压 RFC 1950 zlib 流。
 * @param zlibStream kitty o=z 负载解压前的原始 zlib 字节。
 * @param expected 预期解压长度（f=24/32 为 s*v*bpp；f=100 取 S 键）。
 * @return 解压结果；失败或长度不符时返回空 QByteArray。
 */
QByteArray inflateWithLengthPrefix(const QByteArray &zlibStream, qint64 expected)
{
    if (expected <= 0 || expected > qint64(std::numeric_limits<int>::max()))
        return {};
    QByteArray prefixed;
    prefixed.resize(4);
    prefixed[0] = char((expected >> 24) & 0xFF);
    prefixed[1] = char((expected >> 16) & 0xFF);
    prefixed[2] = char((expected >> 8) & 0xFF);
    prefixed[3] = char(expected & 0xFF);
    prefixed += zlibStream;
    return qUncompress(reinterpret_cast<const uchar *>(prefixed.constData()),
                       prefixed.size());
}

} // namespace

void KittyGraphicsParser::reset()
{
    _accumulating = false;
    _pending = KittyCommand{};
    _payload.clear();
}

bool KittyGraphicsParser::parseKeys(const QByteArray &keyPart, bool continuation, Result &out)
{
    const auto pairs = keyPart.split(',');
    for (const QByteArray &kv : pairs) {
        if (kv.isEmpty())
            continue;
        const int eq = kv.indexOf('=');
        const QByteArray key = eq < 0 ? kv : kv.left(eq);
        const QByteArray val = eq < 0 ? QByteArray() : kv.mid(eq + 1);
        if (continuation) {
            // 续块仅允许 m（及可选 q）；其余键由 feed() 的打断判定先行过滤
            if (key == "q")
                _pending.quiet = val.toInt();
            continue;
        }
        if (key.size() != 1)
            continue; // 未知键静默忽略（协议要求）
        switch (key.at(0)) {
        case 'a': _pending.action = val.isEmpty() ? 'T' : val.at(0); break;
        case 't': _pending.medium = val.isEmpty() ? 'd' : val.at(0); break;
        case 'f': _pending.format = val.toInt(); break;
        case 'o': _pending.compressed = (val == "z"); break;
        case 'i': _pending.imageId = val.toUInt(); break;
        case 'p': _pending.placementId = val.toUInt(); break;
        case 'd': _pending.deleteWhat = val.isEmpty() ? '\0' : val.at(0); break;
        case 'q': _pending.quiet = val.toInt(); break;
        case 's': _pending.width = val.toInt(); break;
        case 'v': _pending.height = val.toInt(); break;
        case 'S': _pending.pngSize = val.toLongLong(); break;
        case 'x': _pending.srcX = val.toInt(); break;
        case 'y': _pending.srcY = val.toInt(); break;
        case 'w': _pending.srcW = val.toInt(); break;
        case 'h': _pending.srcH = val.toInt(); break;
        case 'X': _pending.cellXOff = val.toInt(); break;
        case 'Y': _pending.cellYOff = val.toInt(); break;
        case 'c': _pending.cols = val.toInt(); break;
        case 'r': _pending.rows = val.toInt(); break;
        case 'z': _pending.zIndex = qint32(val.toInt()); break;
        case 'C': _pending.cursorNoMove = (val.toInt() == 1); break;
        case 'm': break; // 分块标志在 feed() 中处理
        default: break;  // U=/I=/P/Q/H/V 等未知键静默忽略
        }
    }
    Q_UNUSED(out);
    return true;
}

bool KittyGraphicsParser::decodePayload(qint64 budgetRemaining, Result &out)
{
    KittyCommand &cmd = _pending; // 控制键在 parseKeys 中已写入 _pending
    auto fail = [&out](const char *code, const char *msg) {
        out.errorCode = QByteArray(code);
        out.errorMessage = QByteArray(msg);
        return false;
    };

    if (cmd.medium != 'd')
        return fail("EINVAL", "unsupported medium"); // 仅支持直接传输；先于尺寸/解码检查

    if (cmd.format != 100 && cmd.format != 32 && cmd.format != 24)
        return fail("EINVAL", "decode failed"); // 未知像素格式

    QByteArray raw = QByteArray::fromBase64(_payload); // Qt 默认忽略非法字符（宽容）

    if (cmd.compressed) {
        qint64 expected = 0;
        if (cmd.format == 100) {
            if (cmd.pngSize <= 0)
                return fail("EINVAL", "missing S"); // PNG 与压缩并用必须提供 S
            // S 完全客户端可控：钳制到像素预算量级，防止单键迫使 qUncompress 大分配
            if (cmd.pngSize > MAX_PNG_STREAM_BYTES)
                return fail("EINVAL", "image too large");
            expected = cmd.pngSize;
        } else {
            if (cmd.width <= 0 || cmd.height <= 0)
                return fail("EINVAL", "missing size");
            // 解压前预检：尺寸/预算比较无需解压即可做，消除最大 400MB 瞬时分配
            if (cmd.width > MAX_DIMENSION || cmd.height > MAX_DIMENSION)
                return fail("EINVAL", "image too large");
            if (qint64(cmd.width) * cmd.height * 4 > budgetRemaining)
                return fail("ENOSPC", "pixel budget exceeded");
            expected = qint64(cmd.width) * cmd.height * (cmd.format == 24 ? 3 : 4);
        }
        raw = inflateWithLengthPrefix(raw, expected);
        if (raw.size() != expected)
            return fail("EINVAL", "decode failed");
    }

    if (cmd.format == 100) {
        QImage img = QImage::fromData(raw, "PNG");
        if (img.isNull())
            return fail("EINVAL", "decode failed");
        if (img.width() > MAX_DIMENSION || img.height() > MAX_DIMENSION)
            return fail("EINVAL", "image too large");
        if (qint64(img.width()) * img.height() * 4 > budgetRemaining)
            return fail("ENOSPC", "pixel budget exceeded"); // 解码前预检（按 ARGB32 计）
        cmd.image = img.convertToFormat(QImage::Format_ARGB32);
        return true;
    }

    if (cmd.width <= 0 || cmd.height <= 0)
        return fail("EINVAL", "missing size");
    if (cmd.width > MAX_DIMENSION || cmd.height > MAX_DIMENSION)
        return fail("EINVAL", "image too large");
    if (qint64(cmd.width) * cmd.height * 4 > budgetRemaining)
        return fail("ENOSPC", "pixel budget exceeded"); // 解码前预检

    const int bpp = cmd.format == 24 ? 3 : 4;
    const qint64 need = qint64(cmd.width) * cmd.height * bpp;
    if (raw.size() != need)
        return fail("EINVAL", "decode failed");

    QImage img(cmd.width, cmd.height, QImage::Format_ARGB32);
    const uchar *p = reinterpret_cast<const uchar *>(raw.constData());
    for (int y = 0; y < cmd.height; y++) {
        for (int x = 0; x < cmd.width; x++, p += bpp) {
            img.setPixel(x, y, bpp == 4 ? qRgba(p[0], p[1], p[2], p[3])
                                        : qRgb(p[0], p[1], p[2]));
        }
    }
    cmd.image = img;
    return true;
}

KittyGraphicsParser::Status KittyGraphicsParser::feed(const QByteArray &chunk,
                                                      qint64 budgetRemaining, Result &out)
{
    out = Result{};

    // 键值段与负载段以第一个 ';' 分隔；无 ';' 时整块为键值段
    const int sep = chunk.indexOf(';');
    const QByteArray keyPart = sep < 0 ? chunk : chunk.left(sep);
    const QByteArray payloadPart = sep < 0 ? QByteArray() : chunk.mid(sep + 1);

    if (_accumulating) {
        // 分块打断判定：续块出现 m/q 之外的键 → 丢弃半成品，本条按首块重新解析
        bool foreignKey = false;
        const auto pairs = keyPart.split(',');
        for (const QByteArray &kv : pairs) {
            const int eq = kv.indexOf('=');
            const QByteArray key = eq < 0 ? kv : kv.left(eq);
            if (!key.isEmpty() && key != "m" && key != "q")
                foreignKey = true;
        }
        if (foreignKey)
            reset();
    }

    bool more = false;
    if (!_accumulating) {
        _pending = KittyCommand{};
        _payload.clear();
        // 首块提取 m 后全量解析控制键
        const auto pairs = keyPart.split(',');
        for (const QByteArray &kv : pairs) {
            if (kv.startsWith("m="))
                more = (kv.mid(2).toInt() == 1);
        }
        parseKeys(keyPart, false, out);
    } else {
        const auto pairs = keyPart.split(',');
        for (const QByteArray &kv : pairs) {
            if (kv.startsWith("m="))
                more = (kv.mid(2).toInt() == 1);
        }
        parseKeys(keyPart, true, out); // 仅吸收 q=
    }
    _payload += payloadPart;

    if (more) {
        _accumulating = true;
        return Status::NeedMore;
    }

    // 末块收尾：仅传输类动作（含缺省 a=T 与查询 a=q）需要像素解码
    const bool needsPixels = (_pending.action == 't' || _pending.action == 'T'
                              || _pending.action == 'q');
    Status status = Status::Ready;
    if (needsPixels && !decodePayload(budgetRemaining, out))
        status = Status::Error;
    if (status == Status::Ready)
        out.command = _pending;
    // 应答所需的 id/q 无论成败都带出（能定位 i= 时回错误码，否则调用方静默）
    out.imageId = _pending.imageId;
    out.placementId = _pending.placementId;
    out.quiet = _pending.quiet;
    reset();
    return status;
}
