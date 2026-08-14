#ifndef LIGATUREHELPER_H
#define LIGATUREHELPER_H

#include <QFontMetricsF>
#include <QVector>
#include <string>

/**
 * @brief 编程连字判定设施（内部工具，不进公共头 qtermwidget.h）。
 *
 * 可连字字符集为 ASCII 运算符区字符（空格、字母数字、逗号、括号、引号不参与）；
 * 探测把样式均匀的片段拆出"可连字候选子区间"（连续候选字符、长度 ≥ 2）；
 * 整形宽度校验保证"整段绘制不破坏对齐"——等宽编程字体的连字字形恰好占 n 格宽，
 * 字体无连字字形时整形结果就是逐字宽度之和，校验恒成立（静默回退语义）。
 * 连字是否真的发生由字体决定，本设施不感知也不缓存（字体切换后自动失效）。
 */
class LigatureHelper
{
public:
    /**
     * @brief 可连字候选子区间（片段内一段连续候选字符）。
     */
    struct Span {
        int start;  ///< 起始索引（字符下标，从 0 起）
        int length; ///< 长度（字符数，≥ 2）
    };

    /**
     * @brief 判定字符是否属于可连字 ASCII 运算符集合。
     * @param ch 字符码点。
     * @return true = 候选字符（! # $ % & * + - . / : ; < = > ? @ \ ^ _ { | } ~）。
     */
    static bool isCandidateChar(char32_t ch);

    /**
     * @brief 探测片段内的全部可连字候选子区间。
     * @param text 样式均匀的片段文本。
     * @return 候选子区间列表，按起始索引升序、互不重叠；无候选时为空。
     */
    static QVector<Span> findCandidateSpans(const std::u32string &text);

    /**
     * @brief 整形宽度校验：子串整形后总宽与"格数 × 单元格宽"相等才允许整段绘制。
     * @param fm 当前绘制字体度量（须为含粗斜体调整后的 painter 字体）。
     * @param text 候选子串。
     * @param cellWidth 单元格像素宽（_fontWidth）。
     * @return true = 整形总宽与格宽之和在 ±0.5px 容差内；空串恒 false。
     * @note Qt 整形默认启用 liga/calt；校验只保证对齐不破，连字发生与否由字体决定。
     */
    static bool widthMatches(const QFontMetricsF &fm, const std::u32string &text,
                             int cellWidth);
};

#endif // LIGATUREHELPER_H
