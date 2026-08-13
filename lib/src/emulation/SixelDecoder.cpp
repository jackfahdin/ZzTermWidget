#include "SixelDecoder.h"

#include <array>

#include <QColor>

namespace {

/** @brief sixel 位带数据字符区间：'?'(0x3F)–'~'(0x7E)，值 = 字节 - 0x3F（6 bit）。 */
bool isSixelChar(char c) { return c >= '?' && c <= '~'; }

/**
 * @brief 解析一个十进制参数。
 * @param data 完整数据段。
 * @param i 当前下标；返回时前进到数字串之后。
 * @param hasValue 输出：是否确实读到数字（区分"缺省"与"0"）。
 * @return 参数值（缺省为 0），钳制到 [0, 100000] 防溢出。
 */
int parseParam(const QByteArray &data, int &i, bool &hasValue)
{
    int value = 0;
    hasValue = false;
    while (i < data.size() && data[i] >= '0' && data[i] <= '9') {
        value = qMin(value * 10 + (data[i] - '0'), 100000);
        hasValue = true;
        i++;
    }
    return value;
}

/** @brief 跳过形如 "n;n;n" 的参数列（当前下标在引导符之后）。 */
void skipParams(const QByteArray &data, int &i)
{
    bool dummy;
    parseParam(data, i, dummy);
    while (i < data.size() && data[i] == ';') {
        i++;
        parseParam(data, i, dummy);
    }
}

/** @brief 返回 6-bit 位带值 @p bits 的最高置位位号 +1（即该字符贡献的像素行数）。 */
int bitHeight(int bits)
{
    int h = 0;
    while (bits) {
        h++;
        bits >>= 1;
    }
    return h;
}

} // namespace

std::optional<SixelDecodeResult> SixelDecoder::decode(const QByteArray &data, int p2)
{
    // 第一遍：仅推演写入光标轨迹，确定图像像素尺寸
    //（光栅属性声明的 Ph/Pv 与实际写入范围取并集）
    int x = 0, y = 0;
    int extentX = 0, extentY = 0;
    int rasterW = 0, rasterH = 0;
    for (int i = 0; i < data.size();) {
        const char c = data[i];
        if (c == '"') { // 光栅属性 "Pan;Pad;Ph;Pv：仅采纳 Ph/Pv
            i++;
            bool has = false;
            parseParam(data, i, has); // Pan（宽高比分子，忽略）
            if (i < data.size() && data[i] == ';') {
                i++;
                parseParam(data, i, has); // Pad（宽高比分母，忽略）
            }
            if (i < data.size() && data[i] == ';') {
                i++;
                const int v = parseParam(data, i, has);
                if (has)
                    rasterW = v;
            }
            if (i < data.size() && data[i] == ';') {
                i++;
                const int v = parseParam(data, i, has);
                if (has)
                    rasterH = v;
            }
        } else if (c == '#') { // 调色板：第一遍不建表，跳过参数
            i++;
            skipParams(data, i);
        } else if (c == '!') { // 重复：!Pn<sixel>
            i++;
            bool has = false;
            const int n = parseParam(data, i, has);
            if (i < data.size() && isSixelChar(data[i])) {
                const int count = has ? qMax(n, 1) : 1;
                const int bits = data[i] - 0x3F;
                i++;
                x += count;
                extentX = qMax(extentX, x);
                if (bits != 0) // 有置位位才贡献高度
                    extentY = qMax(extentY, y + bitHeight(bits));
            }
        } else if (c == '$') { // 回车：x 归 0
            x = 0;
            i++;
        } else if (c == '-') { // 换带：x 归 0，y 下移 6 像素
            x = 0;
            y += 6;
            i++;
        } else if (isSixelChar(c)) {
            const int bits = c - 0x3F;
            x++;
            extentX = qMax(extentX, x);
            if (bits != 0)
                extentY = qMax(extentY, y + bitHeight(bits));
            i++;
        } else {
            i++; // 未知/控制字节：忽略
        }
    }

    const int width = qMax(extentX, rasterW);
    const int height = qMax(extentY, rasterH);
    if (width <= 0 || height <= 0)
        return std::nullopt; // 空图：丢弃
    if (width > MAX_DIMENSION || height > MAX_DIMENSION)
        return std::nullopt; // 超单图资源上限：丢弃（调用方已吞到 ST）

    // 第二遍：建调色板并逐位写像素
    std::array<QRgb, 256> palette {};
    std::array<bool, 256> paletteDefined {};

    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    int cx = 0, cy = 0;
    int currentColor = 0;
    const auto putSixel = [&](char c) {
        const int bits = c - 0x3F;
        if (bits == 0 || cx >= width)
            return;
        // 未定义寄存器的引用定死为不透明黑（寄存器编号在选择时已钳制到 [0,255]）
        const QRgb color = paletteDefined[currentColor] ? palette[currentColor]
                                                        : qRgb(0, 0, 0);
        for (int b = 0; b < 6; b++) {
            if ((bits & (1 << b)) == 0)
                continue;
            const int py = cy + b;
            if (py >= height)
                break;
            reinterpret_cast<QRgb *>(image.scanLine(py))[cx] = color;
        }
    };

    for (int i = 0; i < data.size();) {
        const char c = data[i];
        if (c == '#') { // #Pc 选择颜色；#Pc;Pu;Px;Py;Pz 定义并选择颜色
            i++;
            bool has = false;
            int pc = parseParam(data, i, has);
            pc = qBound(0, pc, 255); // 寄存器编号越界：钳制到有效范围
            int params[4] = {0, 0, 0, 0};
            bool paramHas[4] = {false, false, false, false};
            int nParams = 0;
            while (nParams < 4 && i < data.size() && data[i] == ';') {
                i++;
                params[nParams] = parseParam(data, i, paramHas[nParams]);
                nParams++;
            }
            currentColor = pc;
            if (nParams == 4 && paramHas[0]) {
                if (params[0] == 1) { // HLS：Px=色相(0-360) Py=亮度% Pz=饱和度%
                    // 用浮点 fromHslF：整数版 fromHsl 的 0-255 亮度无法精确表示
                    // 50%（127/255 ≈ 0.498），会得到 254 而非规格要求的 255
                    const QColor qc = QColor::fromHslF(qBound(0, params[1], 360) / 360.0,
                                                       qBound(0, params[3], 100) / 100.0,
                                                       qBound(0, params[2], 100) / 100.0);
                    palette[pc] = qc.rgb();
                } else { // Pu=2（未知值按 RGB 处理）：RGB 百分比 0-100
                    palette[pc] = qRgb(qBound(0, params[1], 100) * 255 / 100,
                                       qBound(0, params[2], 100) * 255 / 100,
                                       qBound(0, params[3], 100) * 255 / 100);
                }
                paletteDefined[pc] = true;
            }
        } else if (c == '!') {
            i++;
            bool has = false;
            const int n = parseParam(data, i, has);
            if (i < data.size() && isSixelChar(data[i])) {
                const int count = has ? qMax(n, 1) : 1;
                const char v = data[i];
                i++;
                for (int k = 0; k < count; k++) {
                    putSixel(v);
                    cx++;
                }
            }
        } else if (c == '"') { // 光栅属性第二遍跳过（尺寸已采纳）
            i++;
            skipParams(data, i);
        } else if (c == '$') {
            cx = 0;
            i++;
        } else if (c == '-') {
            cx = 0;
            cy += 6;
            i++;
        } else if (isSixelChar(c)) {
            putSixel(c);
            cx++;
            i++;
        } else {
            i++; // 未知/控制字节：忽略
        }
    }

    // P2=2：以 0 号色填底（0 号寄存器须被数据流定义，否则按透明底处理）
    bool transparent = true;
    if (p2 == 2 && paletteDefined[0]) {
        for (int yy = 0; yy < height; yy++) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(yy));
            for (int xx = 0; xx < width; xx++)
                if (qAlpha(line[xx]) == 0)
                    line[xx] = palette[0];
        }
        transparent = false;
    }

    return SixelDecodeResult {image, transparent};
}
