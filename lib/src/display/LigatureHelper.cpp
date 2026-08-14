#include "LigatureHelper.h"

#include <QString>
#include <array>
#include <string_view>

namespace {

/**
 * @brief 构造可连字候选字符掩码（下标即码点，仅 ASCII 0..127 有效）。
 * @return 掩码表；集合：! # $ % & * + - . / : ; < = > ? @ \ ^ _ { | } ~
 */
constexpr std::array<bool, 128> buildCandidateMask()
{
    std::array<bool, 128> mask{};
    for (char c : std::string_view("!#$%&*+-./:;<=>?@\\^_{|}~"))
        mask[static_cast<unsigned char>(c)] = true;
    return mask;
}

/// 可连字候选字符掩码（编译期常量，O(1) 查表）
constexpr std::array<bool, 128> kCandidateMask = buildCandidateMask();

} // namespace

bool LigatureHelper::isCandidateChar(char32_t ch)
{
    return ch < 128 && kCandidateMask[ch];
}

QVector<LigatureHelper::Span> LigatureHelper::findCandidateSpans(const std::u32string &text)
{
    QVector<Span> spans;
    const int n = int(text.size());
    int runStart = -1; ///< 当前连续候选段起点；-1 = 不在段内
    // 边界哨兵：i == n 视为非候选，统一段收尾逻辑
    for (int i = 0; i <= n; i++) {
        const bool cand = i < n && isCandidateChar(text[i]);
        if (cand && runStart < 0) {
            runStart = i;
        } else if (!cand && runStart >= 0) {
            if (i - runStart >= 2) // 长度 ≥ 2 才是可连字序列
                spans.append({runStart, i - runStart});
            runStart = -1;
        }
    }
    return spans;
}

bool LigatureHelper::widthMatches(const QFontMetricsF &fm, const std::u32string &text,
                                  int cellWidth)
{
    if (text.empty())
        return false;
    const qreal shaped = fm.horizontalAdvance(QString::fromStdU32String(text));
    const qreal cells = qreal(text.size()) * cellWidth;
    return qAbs(shaped - cells) <= 0.5; // 整像素容差（规格 §3.2）
}
