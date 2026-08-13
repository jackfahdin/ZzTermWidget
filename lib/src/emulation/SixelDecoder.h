#ifndef SIXELDECODER_H
#define SIXELDECODER_H

#include <optional>

#include <QByteArray>
#include <QImage>

/**
 * @brief Sixel 图形（DCS P1;P2;P3 q ... ST 的 data 段）解码结果。
 */
struct SixelDecodeResult {
    QImage image;               ///< ARGB32 图像；未被数据覆盖的像素全透明
    bool transparentBackground; ///< true = 透明底（未着色区域透出文本背景）
};

/**
 * @brief Sixel 数据段纯逻辑解码器（无终端状态，可独立单测）。
 *
 * 支持的语法子集（覆盖 lsix/img2sixel/tmux 生态）：# 调色板定义/选择
 * （RGB 与 HLS）、! 重复、" 光栅属性、$ 回车、- 换行、?–~ 6-bit 位带。
 * P1（宽高比）与 P3 按设计忽略：1 sixel 像素映射 1 设备像素，不重采样。
 */
class SixelDecoder
{
public:
    /** @brief 单张图宽/高上限（像素），超限解码失败。 */
    static constexpr int MAX_DIMENSION = 4096;

    /**
     * @brief 解码 sixel 数据段。
     * @param data DCS 头 'q' 之后、ST 之前的原始字节。
     * @param p2 DCS 第二参数：1 = 透明底；2 = 以 0 号色填底（0 号寄存器须被数据流定义，
     *        否则按透明底处理）；其他值按透明底处理。
     * @return 解码结果；空图、语法无法产出尺寸或资源超限时返回 std::nullopt
     *         （调用方静默丢弃，不影响后续字节流）。
     */
    static std::optional<SixelDecodeResult> decode(const QByteArray &data, int p2);
};

#endif // SIXELDECODER_H
