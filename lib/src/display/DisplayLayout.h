#pragma once

#include <QVector>

/**
 * @brief 显示行 →（缓冲区窗口相对行， 起始列偏移）的映射项。
 */
struct DisplayRow {
    int bufferLine;
    int columnOffset;

    bool operator==(const DisplayRow &) const = default;
};

/**
 * @brief 计算一条有效长度为 effectiveLength 的行折叠后的显示段数。
 * @note 空行计 1 段；columns <= 0 时防御性返回 1。
 */
inline int foldCountForLine(int effectiveLength, int columns) {
    if (columns <= 0)
        return 1;
    return qMax(1, (effectiveLength + columns - 1) / columns);
}

/**
 * @brief 从窗口顶行起生成显示行映射，最多 maxRows 个显示行。
 * @param windowLineLengths 窗口各缓冲区行的有效长度。
 */
inline QVector<DisplayRow> buildFoldMap(const QVector<int> &windowLineLengths,
                                        int columns, int maxRows) {
    QVector<DisplayRow> rows;
    for (int line = 0; line < windowLineLengths.size() && rows.size() < maxRows; ++line) {
        const int segments = foldCountForLine(windowLineLengths[line], columns);
        for (int seg = 0; seg < segments && rows.size() < maxRows; ++seg)
            rows.append({line, seg * columns});
    }
    return rows;
}

/**
 * @brief 全缓冲行 line 的首个折叠段在全缓冲显示行序列中的偏移（前缀和）。
 * @note 用于软折叠模式下垂直滚动条 range/value 的显示行换算。
 */
inline int displayRowOffsetOfLine(const QVector<int> &lineLengths, int columns, int line) {
    int offset = 0;
    for (int i = 0; i < line && i < lineLengths.size(); ++i)
        offset += foldCountForLine(lineLengths[i], columns);
    return offset;
}
