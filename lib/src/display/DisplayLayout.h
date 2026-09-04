#pragma once

#include <QVector>

/**
 * @brief 显示行 →（缓冲区窗口相对行， 起始列偏移）的映射项。
 */
struct DisplayRow {
    int bufferLine;
    int columnOffset;
    /**
     * @brief 段内单元格数；0 表示占满整段 columns（等宽切分的默认值）。
     * @note 宽度感知折叠（buildFoldMapWideAware）恒为正值；段尾让位宽字符时
     *       小于 columns，合成/映射按此值取段内有效宽度。
     */
    int cellCount = 0;

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
 * @brief 宽度感知折叠映射：段尾落在宽字符首格时边界前移一格，宽字符不被拆半。
 * @param windowLineLengths 窗口各缓冲区行的有效长度。
 * @param wideHeads 每行宽字符首格标记：wideHeads[line][col] 为 true 表示 col
 *        是占两格字符的首格（其次格 character == 0）；行缺失或长度不足视为全 false。
 * @param maxRows 最多生成的显示行数。
 * @return 每项 cellCount 为该段实际单元格数；段尾让位宽字符时 < columns，
 *         合成时该段剩余列补空白。边界前移归纳保证段首永不为填充格。
 * @note 与 buildFoldMap 的等宽切分相比，边界处含宽字符的行段数可能多 1；
 *       columns <= 1 时让位条件（段内至少保留 1 格）自动失效，退化为逐格切分。
 */
inline QVector<DisplayRow> buildFoldMapWideAware(const QVector<int> &windowLineLengths,
                                                 const QVector<QVector<bool>> &wideHeads,
                                                 int columns, int maxRows) {
    QVector<DisplayRow> rows;
    if (columns <= 0)
        return rows;
    for (int line = 0; line < windowLineLengths.size() && rows.size() < maxRows; ++line) {
        const int len = windowLineLengths[line];
        const QVector<bool> heads =
                line < wideHeads.size() ? wideHeads[line] : QVector<bool>();
        int start = 0;
        // 空行（len == 0）计 1 段；每轮 end > start 恒成立，循环必终止
        while (start < qMax(len, 1) && rows.size() < maxRows) {
            int end = qMin(start + columns, qMax(len, start + 1));
            // 段尾是宽字符首格（其次格将被切到下一段）时边界前移一格，
            // 宽字符整体进入下一段；段内至少保留 1 格（columns == 1 时退化）
            if (end - 1 > start && end - 1 < heads.size() && heads[end - 1])
                --end;
            rows.append({line, start, end - start});
            start = end;
        }
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
