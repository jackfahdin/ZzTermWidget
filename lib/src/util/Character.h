/*
 This file is part of Konsole, KDE's terminal.
 
 Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
 Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 02110-1301  USA.
*/
#ifndef CHARACTER_H
#define CHARACTER_H

#include <QHash>
#include <QSet>

#include "CharacterColor.h"

typedef unsigned char LineProperty;

static const int LINE_DEFAULT          = 0;
static const int LINE_WRAPPED          = (1 << 0);
static const int LINE_DOUBLEWIDTH      = (1 << 1);
static const int LINE_DOUBLEHEIGHT     = (1 << 2);

#define DEFAULT_RENDITION  0
#define RE_BOLD            (1 << 0)
#define RE_BLINK           (1 << 1)
#define RE_UNDERLINE       (1 << 2)
#define RE_REVERSE         (1 << 3) // Screen only
#define RE_INTENSIVE       (1 << 3) // Widget only
#define RE_ITALIC          (1 << 4)
#define RE_CURSOR          (1 << 5)
#define RE_EXTENDED_CHAR   (1 << 6)
#define RE_FAINT           (1 << 7)
#define RE_STRIKEOUT       (1 << 8)
#define RE_CONCEAL         (1 << 9)
#define RE_OVERLINE        (1 << 10)

/**
 * @brief 下划线样式掩码：rendition 位 11-13 存 UNDERLINE_* 取值，位 14-15 保留。
 * @note RE_UNDERLINE（位 2）保留为"有下划线"汇总位：任何非关样式都置位。
 */
#define RE_UNDERLINE_STYLE_MASK (7 << 11)

/** @brief 下划线样式取值（SGR 4:n 子参数映射 n-1，存于 rendition 位 11-13）。 */
static const int UNDERLINE_SINGLE = 0; ///< 单线（默认）
static const int UNDERLINE_DOUBLE = 1; ///< 双线
static const int UNDERLINE_CURLY  = 2; ///< 波浪线
static const int UNDERLINE_DOTTED = 3; ///< 点线
static const int UNDERLINE_DASHED = 4; ///< 虚线

class ScreenWindow;

/**
 * A single character in the terminal which consists of a unicode character
 * value, foreground and background colors and a set of rendition attributes
 * which specify how it should be drawn.
 */
class Character
{
public:
    /**
     * @brief 构造一个新的终端字符。
     *
     * @param _c 该字符的 Unicode 码点（UCS-4，支持 BMP 外字符）。
     * @param _f 绘制该字符使用的前景色。
     * @param _b 绘制该字符背景使用的颜色。
     * @param _r 一组渲染标志，指定该字符的绘制方式。
     */
    inline Character(char32_t _c = U' ',
            CharacterColor  _f = CharacterColor(COLOR_SPACE_DEFAULT,DEFAULT_FORE_COLOR),
            CharacterColor  _b = CharacterColor(COLOR_SPACE_DEFAULT,DEFAULT_BACK_COLOR),
            quint16  _r = DEFAULT_RENDITION)
        : character(_c)
        , rendition(_r)
        , foregroundColor(_f)
        , backgroundColor(_b) {
    }

    /**
     * @brief 该字符的 Unicode 码点。
     *
     * 带 RE_EXTENDED_CHAR 标志时，该字段为哈希码，
     * 可在创建该序列所用的 ExtendedCharTable 中查得原始 Unicode 字符序列。
     */
    char32_t character;

    /** @brief RENDITION 渲染标志的组合，指定绘制该字符的选项。 */
    quint16  rendition;

    /** The foreground color used to draw this character. */
    CharacterColor  foregroundColor;
    /** The color used to draw this character's background. */
    CharacterColor  backgroundColor;

    /**
     * @brief 独立下划线颜色（SGR 58）；COLOR_SPACE_DEFAULT 表示跟随前景色（SGR 59 复位态）。
     * @note 不参与 RE_REVERSE 前景/背景交换；绘制时 DEFAULT 回落片段实际文本色。
     */
    CharacterColor underlineColor = CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);

    /**
     * @brief 返回下划线样式（rendition 位 11-13）。
     * @return UNDERLINE_* 取值之一；仅当 RE_UNDERLINE 置位时有意义。
     */
    inline int underlineStyle() const {
        return (rendition >> 11) & 0x7;
    }

    /**
     * @brief 返回 true 表示设置了独立下划线色（SGR 58）；false = 跟随前景色。
     */
    inline bool hasCustomUnderlineColor() const {
        return underlineColor.isValid() && underlineColor._colorSpace != COLOR_SPACE_DEFAULT;
    }

    /**
     * Returns true if this character has a transparent background when
     * it is drawn with the specified @p palette.
     */
    bool   isTransparent(const ColorEntry* palette) const;
    /**
     * Returns true if this character should always be drawn in bold when
     * it is drawn with the specified @p palette, independent of whether
     * or not the character has the RE_BOLD rendition flag.
     */
    ColorEntry::FontWeight fontWeight(const ColorEntry* base) const;

    /**
     * returns true if the format (color, rendition flag) of the compared characters is equal
     */
    bool equalsFormat(const Character &other) const;

    /**
     * Compares two characters and returns true if they have the same unicode character value,
     * rendition and colors.
     */
    friend bool operator == (const Character& a, const Character& b);
    /**
     * Compares two characters and returns true if they have different unicode character values,
     * renditions or colors.
     */
    friend bool operator != (const Character& a, const Character& b);


    inline bool isLineChar() const {
        return (rendition & RE_EXTENDED_CHAR) ? false : ((character & 0xFF80) == 0x2500);
    }

    inline bool isSpace() const {
        return (rendition & RE_EXTENDED_CHAR) ? false : QChar::isSpace(character);
    }
};

inline bool operator == (const Character& a, const Character& b) {
    return a.character == b.character &&
           a.rendition == b.rendition &&
           a.foregroundColor == b.foregroundColor &&
           a.backgroundColor == b.backgroundColor &&
           a.underlineColor == b.underlineColor;
}

inline bool operator != (const Character& a, const Character& b) {
    return a.character != b.character ||
           a.rendition != b.rendition ||
           a.foregroundColor != b.foregroundColor ||
           a.backgroundColor != b.backgroundColor ||
           a.underlineColor != b.underlineColor;
}

inline bool Character::isTransparent(const ColorEntry* base) const {
    return ((backgroundColor._colorSpace == COLOR_SPACE_DEFAULT) &&
             base[backgroundColor._u+0+(backgroundColor._v?BASE_COLORS:0)].transparent)
        || ((backgroundColor._colorSpace == COLOR_SPACE_SYSTEM) &&
             base[backgroundColor._u+2+(backgroundColor._v?BASE_COLORS:0)].transparent);
}

inline bool Character::equalsFormat(const Character& other) const {
    return backgroundColor==other.backgroundColor &&
           foregroundColor==other.foregroundColor &&
           rendition==other.rendition &&
           underlineColor==other.underlineColor;
}

inline ColorEntry::FontWeight Character::fontWeight(const ColorEntry* base) const {
    if (backgroundColor._colorSpace == COLOR_SPACE_DEFAULT)
        return base[backgroundColor._u+0+(backgroundColor._v?BASE_COLORS:0)].fontWeight;
    else if (backgroundColor._colorSpace == COLOR_SPACE_SYSTEM)
        return base[backgroundColor._u+2+(backgroundColor._v?BASE_COLORS:0)].fontWeight;
    else
        return ColorEntry::UseCurrentFormat;
}

/**
 * A table which stores sequences of unicode characters, referenced
 * by hash keys.  The hash key itself is the same size as a unicode
 * character ( ushort ) so that it can occupy the same space in
 * a structure.
 */
class ExtendedCharTable
{
public:
    /** Constructs a new character table. */
    ExtendedCharTable();
    ~ExtendedCharTable();

    /**
     * Adds a sequences of unicode characters to the table and returns
     * a hash code which can be used later to look up the sequence
     * using lookupExtendedChar()
     *
     * If the same sequence already exists in the table, the hash
     * of the existing sequence will be returned.
     *
     * @param unicodePoints An array of unicode character points
     * @param length Length of @p unicodePoints
     */
    uint createExtendedChar(uint* unicodePoints , ushort length);
    /**
     * Looks up and returns a pointer to a sequence of unicode characters
     * which was added to the table using createExtendedChar().
     *
     * @param hash The hash key returned by createExtendedChar()
     * @param length This variable is set to the length of the
     * character sequence.
     *
     * @return A unicode character sequence of size @p length.
     */
    uint* lookupExtendedChar(uint hash , ushort& length) const;

    /**
     * Keeps track of all screens.
     * Used in createExtendedChar for checking all screens.
     */
    QSet<ScreenWindow*> windows;

    /** The global ExtendedCharTable instance. */
    static ExtendedCharTable instance;
private:
    // calculates the hash key of a sequence of unicode points of size 'length'
    uint extendedCharHash(uint* unicodePoints , ushort length) const;
    // tests whether the entry in the table specified by 'hash' matches the
    // character sequence 'unicodePoints' of size 'length'
    bool extendedCharMatch(uint hash , uint* unicodePoints , ushort length) const;
    // internal, maps hash keys to character sequence buffers.  The first ushort
    // in each value is the length of the buffer, followed by the ushorts in the buffer
    // themselves.
    QHash<uint,uint*> extendedCharTable;
};

Q_DECLARE_TYPEINFO(Character, Q_MOVABLE_TYPE);

#endif // CHARACTER_H

