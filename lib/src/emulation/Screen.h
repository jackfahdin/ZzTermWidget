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
#ifndef SCREEN_H
#define SCREEN_H

#include <deque>

#include <QHash>
#include <QImage>
#include <QRect>
#include <QSet>
#include <QTextStream>
#include <QVarLengthArray>

#include "Character.h"
#include "History.h"

#define MODE_Origin    0
#define MODE_Wrap      1
#define MODE_Insert    2
#define MODE_Screen    3
#define MODE_Cursor    4
#define MODE_NewLine   5
#define MODES_SCREEN   6

class TerminalCharacterDecoder;

/**
 * @brief OSC 8 超链接在一行内覆盖的列区间段。
 */
struct HyperlinkSegment {
    int startCol;   ///< 起始列（含）
    int endCol;     ///< 结束列（含）
    quint32 linkId; ///< 链接标识，经 Screen::hyperlinkUri() 换取 URI
};

/**
 * @brief Sixel 图像在单个网格行上的放置引用。
 *
 * 一张图像纵向跨 ceil(像素高/单元格像素高) 个网格行，每行一条引用；
 * 同一 imageId 的多行引用共享屏级 SixelImage（引用计数管理生命周期）。
 */
struct ImagePlacement {
    int startCol;    ///< 起始列（锚定列）
    quint32 imageId; ///< 图像标识，经 Screen::sixelImage() 换取像素数据
    int rowOffset;   ///< 本行在图像内的行偏移（0 = 图像首行），绘制时换算源裁剪
};

/**
 * @brief 屏级存储的一张 sixel 图像。
 */
struct SixelImage {
    QImage image;               ///< ARGB32 像素数据（隐式共享，拷贝廉价）
    bool transparentBackground; ///< true = 透明底（未着色区域透出文本背景）
};

/**
    \brief An image of characters with associated attributes.

    The terminal emulation ( Emulation ) receives a serial stream of
    characters from the program currently running in the terminal.
    From this stream it creates an image of characters which is ultimately
    rendered by the display widget ( TerminalDisplay ).  Some types of emulation
    may have more than one screen image.

    getImage() is used to retrieve the currently visible image
    which is then used by the display widget to draw the output from the
    terminal.

    The number of lines of output history which are kept in addition to the current
    screen image depends on the history scroll being used to store the output.
    The scroll is specified using setScroll()
    The output history can be retrieved using writeToStream()

    The screen image has a selection associated with it, specified using
    setSelectionStart() and setSelectionEnd().  The selected text can be retrieved
    using selectedText().  When getImage() is used to retrieve the visible image,
    characters which are part of the selection have their colours inverted.
*/
class Screen
{
public:
    /** Construct a new screen image of size @p lines by @p columns. */
    Screen(int lines, int columns);
    ~Screen();

    // VT100/2 Operations
    // Cursor Movement

    /**
     * Move the cursor up by @p n lines.  The cursor will stop at the
     * top margin.
     */
    void cursorUp(int n);
    /**
     * Move the cursor down by @p n lines.  The cursor will stop at the
     * bottom margin.
     */
    void cursorDown(int n);
    /**
     * Move the cursor to the left by @p n columns.
     * The cursor will stop at the first column.
     */
    void cursorLeft(int n);
    /**
     * Moves cursor to beginning of the line by @p n lines down.
     * The cursor will stop at the beginning of the line.
     */
    void cursorNextLine(int n);
    /**
     * Moves cursor to beginning of the line by @p n lines up.
     * The cursor will stop at the beginning of the line.
     */
    void cursorPreviousLine(int n);
    /**
     * Move the cursor to the right by @p n columns.
     * The cursor will stop at the right-most column.
     */
    void cursorRight(int n);
    /** Position the cursor on line @p y. */
    void setCursorY(int y);
    /** Position the cursor at column @p x. */
    void setCursorX(int x);
    /** Position the cursor at line @p y, column @p x. */
    void setCursorYX(int y, int x);
    /**
     * Sets the margins for scrolling the screen.
     *
     * @param topLine The top line of the new scrolling margin.
     * @param bottomLine The bottom line of the new scrolling margin.
     */
    void setMargins(int topLine , int bottomLine);
    /** Returns the top line of the scrolling region. */
    int topMargin() const;
    /** Returns the bottom line of the scrolling region. */
    int bottomMargin() const;

    /**
     * Resets the scrolling margins back to the top and bottom lines
     * of the screen.
     */
    void setDefaultMargins();

    /**
     * Moves the cursor down one line, if the MODE_NewLine mode
     * flag is enabled then the cursor is returned to the leftmost
     * column first.
     *
     * Equivalent to NextLine() if the MODE_NewLine flag is set
     * or index() otherwise.
     */
    void newLine();
    /**
     * Moves the cursor down one line and positions it at the beginning
     * of the line.  Equivalent to calling Return() followed by index()
     */
    void nextLine();

    /**
     * Move the cursor down one line.  If the cursor is on the bottom
     * line of the scrolling region (as returned by bottomMargin()) the
     * scrolling region is scrolled up by one line instead.
     */
    void index();
    /**
     * Move the cursor up one line.  If the cursor is on the top line
     * of the scrolling region (as returned by topMargin()) the scrolling
     * region is scrolled down by one line instead.
     */
    void reverseIndex();

    /**
     * Scroll the scrolling region of the screen up by @p n lines.
     * The scrolling region is initially the whole screen, but can be changed
     * using setMargins()
     */
    void scrollUp(int n);
    /**
     * Scroll the scrolling region of the screen down by @p n lines.
     * The scrolling region is initially the whole screen, but can be changed
     * using setMargins()
     */
    void scrollDown(int n);
    /**
     * Moves the cursor to the beginning of the current line.
     * Equivalent to setCursorX(0)
     */
    void toStartOfLine();
    /**
     * Moves the cursor one column to the left and erases the character
     * at the new cursor position.
     */
    void backspace();
    /** Moves the cursor @p n tab-stops to the right. */
    void tab(int n = 1);
    /** Moves the cursor @p n tab-stops to the left. */
    void backtab(int n);

    // Editing

    /**
     * Erase @p n characters beginning from the current cursor position.
     * This is equivalent to over-writing @p n characters starting with the current
     * cursor position with spaces.
     * If @p n is 0 then one character is erased.
     */
    void eraseChars(int n);
    /**
     * Delete @p n characters beginning from the current cursor position.
     * If @p n is 0 then one character is deleted.
     */
    void deleteChars(int n);
    /**
     * Insert @p n blank characters beginning from the current cursor position.
     * The position of the cursor is not altered.
     * If @p n is 0 then one character is inserted.
     */
    void insertChars(int n);
    /**
     * Repeat the preceding graphic character @count times, including SPACE.
     * If @count is 0 then the character is repeated once.
     */
    void repeatChars(int count);
    /**
     * Removes @p n lines beginning from the current cursor position.
     * The position of the cursor is not altered.
     * If @p n is 0 then one line is removed.
     */
    void deleteLines(int n);
    /**
     * Inserts @p lines beginning from the current cursor position.
     * The position of the cursor is not altered.
     * If @p n is 0 then one line is inserted.
     */
    void insertLines(int n);
    /** Clears all the tab stops. */
    void clearTabStops();
    /**  Sets or removes a tab stop at the cursor's current column. */
    void changeTabStop(bool set);

    /** Resets (clears) the specified screen @p mode. */
    void resetMode(int mode);
    /** Sets (enables) the specified screen @p mode. */
    void setMode(int mode);
    /**
     * Saves the state of the specified screen @p mode.  It can be restored
     * using restoreMode()
     */
    void saveMode(int mode);
    /** Restores the state of a screen @p mode saved by calling saveMode() */
    void restoreMode(int mode);
    /** Returns whether the specified screen @p mode is enabled or not .*/
    bool getMode(int mode) const;

    /**
     * Saves the current position and appearance (text color and style) of the cursor.
     * It can be restored by calling restoreCursor()
     */
    void saveCursor();
    /** Restores the position and appearance of the cursor.  See saveCursor() */
    void restoreCursor();

    /** Clear the whole screen, moving the current screen contents into the history first. */
    void clearEntireScreen();
    /**
     * Clear the area of the screen from the current cursor position to the end of
     * the screen.
     */
    void clearToEndOfScreen();
    /**
     * Clear the area of the screen from the current cursor position to the start
     * of the screen.
     */
    void clearToBeginOfScreen();
    /** Clears the whole of the line on which the cursor is currently positioned. */
    void clearEntireLine();
    /** Clears from the current cursor position to the end of the line. */
    void clearToEndOfLine();
    /** Clears from the current cursor position to the beginning of the line. */
    void clearToBeginOfLine();

    /** Fills the entire screen with the letter 'E' */
    void helpAlign();

    /**
     * Enables the given @p rendition flag.  Rendition flags control the appearance
     * of characters on the screen.
     *
     * @see Character::rendition
     */
    void setRendition(int rendition);
    /**
     * Disables the given @p rendition flag.  Rendition flags control the appearance
     * of characters on the screen.
     *
     * @see Character::rendition
     */
    void resetRendition(int rendition);

    /**
     * Sets the cursor's foreground color.
     * @param space The color space used by the @p color argument
     * @param color The new foreground color.  The meaning of this depends on
     * the color @p space used.
     *
     * @see CharacterColor
     */
    void setForeColor(int space, int color);
    /**
     * Sets the cursor's background color.
     * @param space The color space used by the @p color argument.
     * @param color The new background color.  The meaning of this depends on
     * the color @p space used.
     *
     * @see CharacterColor
     */
    void setBackColor(int space, int color);
    /**
     * Resets the cursor's color back to the default and sets the
     * character's rendition flags back to the default settings.
     */
    void setDefaultRendition();

    /** Returns the column which the cursor is positioned at. */
    int  getCursorX() const;
    /** Returns the line which the cursor is positioned on. */
    int  getCursorY() const;
    
    QString getScreenText(int row1, int col1, int row2, int col2, int mode);

    /**
     * @brief 设置/结束当前 OSC 8 超链接上下文。
     * @param uri 链接目标；空串表示结束当前链接（后续文本不再属于链接）。
     * @param osc8Id OSC 8 params 中的 id 参数（可为空）；相同 id 且 URI 未变的分段复用同一 linkId。
     */
    void setCurrentHyperlink(const QString &uri, const QString &osc8Id);

    /**
     * @brief 返回绝对行 @p absoluteLine（历史行 + 屏幕行统一编号）上的超链接段表。
     * @return 段表副本；该行无链接或行号越界时为空。
     */
    QVector<HyperlinkSegment> linkSegments(int absoluteLine) const;

    /**
     * @brief 返回 @p linkId 对应的 URI；无效 id 返回空串。
     */
    QString hyperlinkUri(quint32 linkId) const;

    /**
     * @brief 返回绝对行 @p absoluteLine、列 @p column 处的超链接 URI；无链接返回空串。
     */
    QString hyperlinkAt(int absoluteLine, int column) const;

    /**
     * @brief 在当前光标位置锚定一张 sixel 图像，并将文本光标移到图像最后一行之下。
     * @param image 解码后的 ARGB32 图像。
     * @param transparentBackground 透明底标志（仅记录，供绘制层参考）。
     * @note 图像占 ceil(像素高/单元格像素高) 个网格行；逐行放置引用后 index() 下移
     *       光标，触底时触发滚动（xterm 语义），已放置引用随行走。
     *       超出累计像素预算（MAX_IMAGE_BYTES）时整张静默丢弃，光标不动。
     */
    void anchorImage(const QImage &image, bool transparentBackground);

    /**
     * @brief 返回绝对行 @p absoluteLine（历史行 + 屏幕行统一编号）上的图像放置引用表。
     * @return 引用表副本；该行无图像或行号越界时为空。
     */
    QVector<ImagePlacement> imagePlacements(int absoluteLine) const;

    /**
     * @brief 返回 @p imageId 对应的图像；无效 id 返回 nullptr。
     */
    const SixelImage *sixelImage(quint32 imageId) const;

    /**
     * @brief 设置网格单元格的像素尺寸（锚定时换算图像占用行数）。
     * @note 由显示层经 Emulation::setCellPixelSize 同步真实字体度量；
     *       未设置时按 DEFAULT_CELL_PIXEL_HEIGHT 兜底（无显示组件的测试环境）。
     */
    void setCellPixelSize(int width, int height);

    /**
     * @brief 查询图形脏标志：图像锚定/清空不改动字符单元格，字符级脏区比对
     *        感知不到，显示层 updateImage() 取到此标志时整屏标脏补刷一次。
     */
    bool graphicsDirty() const { return _graphicsDirty; }
    /** @brief 清除图形脏标志（显示层取走后调用）。 */
    void clearGraphicsDirty() { _graphicsDirty = false; }

    /** Clear the entire screen and move the cursor to the home position.
     * Equivalent to calling clearEntireScreen() followed by home().
     */
    void clear();
    /**
     * Sets the position of the cursor to the 'home' position at the top-left
     * corner of the screen (0,0)
     */
    void home();
    /**
     * Resets the state of the screen.  This resets the various screen modes
     * back to their default states.  The cursor style and colors are reset
     * (as if setDefaultRendition() had been called)
     *
     * <ul>
     * <li>Line wrapping is enabled.</li>
     * <li>Origin mode is disabled.</li>
     * <li>Insert mode is disabled.</li>
     * <li>Cursor mode is enabled.  TODO Document me</li>
     * <li>Screen mode is disabled. TODO Document me</li>
     * <li>New line mode is disabled.  TODO Document me</li>
     * </ul>
     *
     * If @p clearScreen is true then the screen contents are erased entirely,
     * otherwise they are unaltered.
     */
    void reset(bool clearScreen = true);

    /**
     * Displays a new character at the current cursor position.
     *
     * If the cursor is currently positioned at the right-edge of the screen and
     * line wrapping is enabled then the character is added at the start of a new
     * line below the current one.
     *
     * If the MODE_Insert screen mode is currently enabled then the character
     * is inserted at the current cursor position, otherwise it will replace the
     * character already at the current cursor position.
     */
    void displayCharacter(char32_t c);

    // Do composition with last shown character FIXME: Not implemented yet for KDE 4
    void compose(const QString& compose);

    /**
     * Resizes the image to a new fixed size of @p new_lines by @p new_columns.
     * In the case that @p new_columns is smaller than the current number of columns,
     * existing lines are not truncated.  This prevents characters from being lost
     * if the terminal display is resized smaller and then larger again.
     *
     * The top and bottom margins are reset to the top and bottom of the new
     * screen size.  Tab stops are also reset and the current selection is
     * cleared.
     */
    void resizeImage(int new_lines, int new_columns);

    /**
     * Returns the current screen image.
     * The result is an array of Characters of size [getLines()][getColumns()] which
     * must be freed by the caller after use.
     *
     * @param dest Buffer to copy the characters into
     * @param size Size of @p dest in Characters
     * @param startLine Index of first line to copy
     * @param endLine Index of last line to copy
     */
    void getImage( Character* dest , int size , int startLine , int endLine ) const;

    /**
     * Returns the additional attributes associated with lines in the image.
     * The most important attribute is LINE_WRAPPED which specifies that the
     * line is wrapped,
     * other attributes control the size of characters in the line.
     */
    QVector<LineProperty> getLineProperties( int startLine , int endLine ) const;


    /** Return the number of lines. */
    int getLines() const { return lines; }
    /** Return the number of columns. */
    int getColumns() const { return columns; }
    /** Return the number of lines in the history buffer. */
    int getHistLines() const;
    /**
     * Sets the type of storage used to keep lines in the history.
     * If @p copyPreviousScroll is true then the contents of the previous
     * history buffer are copied into the new scroll.
     */
    void setScroll(const HistoryType& , bool copyPreviousScroll = true);
    /** Returns the type of storage used to keep lines in the history. */
    const HistoryType& getScroll() const;
    /**
     * Returns true if this screen keeps lines that are scrolled off the screen
     * in a history buffer.
     */
    bool hasScroll() const;

    /**
     * Sets the start of the selection.
     *
     * @param column The column index of the first character in the selection.
     * @param line The line index of the first character in the selection.
     * @param blockSelectionMode True if the selection is in column mode.
     */
    void setSelectionStart(const int column, const int line, const bool blockSelectionMode);

    /**
     * Sets the end of the current selection.
     *
     * @param column The column index of the last character in the selection.
     * @param line The line index of the last character in the selection.
     */
    void setSelectionEnd(const int column, const int line);

    /**
     * Retrieves the start of the selection or the cursor position if there
     * is no selection.
     */
    void getSelectionStart(int& column , int& line) const;

    /**
     * Retrieves the end of the selection or the cursor position if there
     * is no selection.
     */
    void getSelectionEnd(int& column , int& line) const;

    /** Clears the current selection */
    void clearSelection();
    bool isClearSelection();

    /**
      *  Returns true if the character at (@p column, @p line) is part of the
      *  current selection.
      */
    bool isSelected(const int column,const int line) const;

    /**
     * Convenience method.  Returns the currently selected text.
     * @param preserveLineBreaks Specifies whether new line characters should
     * be inserted into the returned text at the end of each terminal line.
     */
    QString selectedText(bool preserveLineBreaks) const;

    /**
     * Copies part of the output to a stream.
     *
     * @param decoder A decoder which converts terminal characters into text
     * @param fromLine The first line in the history to retrieve
     * @param toLine The last line in the history to retrieve
     */
    void writeLinesToStream(TerminalCharacterDecoder* decoder, int fromLine, int toLine) const;

    /**
     * Copies the selected characters, set using @see setSelBeginXY and @see setSelExtentXY
     * into a stream.
     *
     * @param decoder A decoder which converts terminal characters into text.
     * PlainTextDecoder is the most commonly used decoder which converts characters
     * into plain text with no formatting.
     * @param preserveLineBreaks Specifies whether new line characters should
     * be inserted into the returned text at the end of each terminal line.
     */
    void writeSelectionToStream(TerminalCharacterDecoder* decoder , bool
                                preserveLineBreaks = true) const;

    /**
     * Checks if the text between from and to is inside the current
     * selection. If this is the case, the selection is cleared. The
     * from and to are coordinates in the current viewable window.
     * The loc(x,y) macro can be used to generate these values from a
     * column,line pair.
     *
     * @param from The start of the area to check.
     * @param to The end of the area to check
     */
    void checkSelection(int from, int to);

    /**
     * Sets or clears an attribute of the current line.
     *
     * @param property The attribute to set or clear
     * Possible properties are:
     * LINE_WRAPPED:     Specifies that the line is wrapped.
     * LINE_DOUBLEWIDTH: Specifies that the characters in the current line
     *                   should be double the normal width.
     * LINE_DOUBLEHEIGHT:Specifies that the characters in the current line
     *                   should be double the normal height.
     *                   Double-height lines are formed of two lines containing the same characters,
     *                   with both having the LINE_DOUBLEHEIGHT attribute.
     *                   This allows other parts of the code to work on the
     *                   assumption that all lines are the same height.
     *
     * @param enable true to apply the attribute to the current line or false to remove it
     */
    void setLineProperty(LineProperty property , bool enable);

    /**
     * Returns the number of lines that the image has been scrolled up or down by,
     * since the last call to resetScrolledLines().
     *
     * a positive return value indicates that the image has been scrolled up,
     * a negative return value indicates that the image has been scrolled down.
     */
    int scrolledLines() const;

    /**
     * Returns the region of the image which was last scrolled.
     *
     * This is the area of the image from the top margin to the
     * bottom margin when the last scroll occurred.
     */
    QRect lastScrolledRegion() const;

    /**
     * Resets the count of the number of lines that the image has been scrolled up or down by,
     * see scrolledLines()
     */
    void resetScrolledLines();

    /**
     * Returns the number of lines of output which have been
     * dropped from the history since the last call
     * to resetDroppedLines()
     *
     * If the history is not unlimited then it will drop
     * the oldest lines of output if new lines are added when
     * it is full.
     */
    int droppedLines() const;

    /**
     * Resets the count of the number of lines dropped from
     * the history.
     */
    void resetDroppedLines();

    /**
      * Fills the buffer @p dest with @p count instances of the default (ie. blank)
      * Character style.
      */
    static void fillWithDefaultChar(Character* dest, int count);
    
    QSet<uint> usedExtendedChars() const {
        QSet<uint> result;
        for (int i = 0; i < lines; ++i) {
            const ImageLine &il = screenLines[i];
            for (int j = 0; j < columns; ++j) {
                if (il[j].rendition & RE_EXTENDED_CHAR) {
                    result << il[j].character;
                }
            }
        }
        return result;
    }

private:
    Screen(const Screen &) = delete;
    Screen &operator=(const Screen &) = delete;

    //copies a line of text from the screen or history into a stream using a
    //specified character decoder.  Returns the number of lines actually copied,
    //which may be less than 'count' if (start+count) is more than the number of characters on
    //the line
    //
    //line - the line number to copy, from 0 (the earliest line in the history) up to
    //         history->getLines() + lines - 1
    //start - the first column on the line to copy
    //count - the number of characters on the line to copy
    //decoder - a decoder which converts terminal characters (an Character array) into text
    //appendNewLine - if true a new line character (\n) is appended to the end of the line
    int  copyLineToStream(int line,
                          int start,
                          int count,
                          TerminalCharacterDecoder* decoder,
                          bool appendNewLine,
                          bool preserveLineBreaks) const;

    //fills a section of the screen image with the character 'c'
    //the parameters are specified as offsets from the start of the screen image.
    //the loc(x,y) macro can be used to generate these values from a column,line pair.
    void clearImage(int loca, int loce, char c);

    //move screen image between 'sourceBegin' and 'sourceEnd' to 'dest'.
    //the parameters are specified as offsets from the start of the screen image.
    //the loc(x,y) macro can be used to generate these values from a column,line pair.
    //
    //NOTE: moveImage() can only move whole lines
    void moveImage(int dest, int sourceBegin, int sourceEnd);
    // scroll up 'i' lines in current region, clearing the bottom 'i' lines
    void scrollUp(int from, int i);
    // scroll down 'i' lines in current region, clearing the top 'i' lines
    void scrollDown(int from, int i);

    void addHistLine();

    void initTabStops();

    void updateEffectiveRendition();
    void reverseRendition(Character& p) const;

    bool isSelectionValid() const;
    // copies text from 'startIndex' to 'endIndex' to a stream
    // startIndex and endIndex are positions generated using the loc(x,y) macro
    void writeToStream(TerminalCharacterDecoder* decoder, int startIndex,
                       int endIndex, bool preserveLineBreaks = true) const;
    // copies 'count' lines from the screen buffer into 'dest',
    // starting from 'startLine', where 0 is the first line in the screen buffer
    void copyFromScreen(Character* dest, int startLine, int count) const;
    // copies 'count' lines from the history buffer into 'dest',
    // starting from 'startLine', where 0 is the first line in the history
    void copyFromHistory(Character* dest, int startLine, int count) const;


    // screen image ----------------
    int lines;
    int columns;

    typedef QVector<Character> ImageLine;      // [0..columns]
    ImageLine*          screenLines;    // [lines]

    int _scrolledLines;
    QRect _lastScrolledRegion;

    int _droppedLines;

    QVarLengthArray<LineProperty,64> lineProperties;

    // history buffer ---------------
    HistoryScroll* history;

    // cursor location
    int cuX;
    int cuY;

    // cursor color and rendition info
    CharacterColor currentForeground;
    CharacterColor currentBackground;
    quint16 currentRendition;

    // margins ----------------
    int _topMargin;
    int _bottomMargin;

    // states ----------------
    bool currentModes[MODES_SCREEN];
    bool savedModes[MODES_SCREEN];

    // ----------------------------

    QBitArray tabStops;

    // selection -------------------
    int selBegin; // The first location selected.
    int selTopLeft;    // TopLeft Location.
    int selBottomRight;    // Bottom Right Location.
    bool blockSelectionMode;  // Column selection mode

    // effective colors and rendition ------------
    CharacterColor effectiveForeground; // These are derived from
    CharacterColor effectiveBackground; // the cu_* variables above
    quint16 effectiveRendition;         // to speed up operation

    class SavedState {
    public:
        SavedState()
        : cursorColumn(0),cursorLine(0),rendition(0) {}

        int cursorColumn;
        int cursorLine;
        quint16 rendition;
        CharacterColor foreground;
        CharacterColor background;
    };
    SavedState savedState;

    // last position where we added a character
    int lastPos;

    // used in REP (repeating char)
    char32_t lastDrawnChar;

    // OSC 8 超链接 ----------------
    // 行级稀疏段表：无链接的行为空 QVector（零额外堆分配）；链接 URI 用段引用计数管理，
    // 段随行清除/滚出/丢弃而销毁，计数归零时回收 URI 与 id 映射
    typedef QVector<HyperlinkSegment> HyperlinkLine;
    HyperlinkLine *_linkLines;               // [lines + 1]，与 screenLines 平行
    std::deque<HyperlinkLine> _historyLinks; // 与 history 行一一对应
    QHash<quint32, QString> _hyperlinkUris;  // linkId → URI
    QHash<quint32, int> _hyperlinkRefs;      // linkId → 段引用计数
    QHash<QString, quint32> _hyperlinkIds;   // OSC 8 id 参数 → linkId
    quint32 _currentHyperlinkId = 0;         // 0 = 无活动链接
    quint32 _nextHyperlinkId = 1;

    void addHyperlinkSegment(int y, int startCol, int endCol);
    void releaseHyperlinkLine(HyperlinkLine &row);
    void clearAllHyperlinks();

    // Sixel 图像 ----------------
    // 结构与 OSC 8 链接段表同构：行级稀疏引用表（无图像的行为空 QVector，零额外堆分配）
    // + 屏级像素表 + 引用计数；引用随行清除/滚出/丢弃而销毁，计数归零时回收像素数据
    typedef QVector<ImagePlacement> ImageRefLine;
    ImageRefLine *_imageLines;               // [lines + 1]，与 screenLines 平行
    std::deque<ImageRefLine> _historyImages; // 与 history 行一一对应
    QHash<quint32, SixelImage> _sixelImages; // imageId → 像素数据
    QHash<quint32, int> _imageRefs;          // imageId → 行引用计数
    quint32 _nextImageId = 1;
    qint64 _imageBytes = 0;                  // 现存图像累计字节数（像素预算记账）
    int _cellPixelWidth = 0;                 // 单元格像素宽（0 = 未同步）
    int _cellPixelHeight = 0;                // 单元格像素高（0 = 未同步，用兜底值）
    bool _graphicsDirty = false;             // 图像锚定/清空后需显示层整屏补刷一次

    /** @brief 累计像素预算：现存 sixel 图像总字节数上限（256MB，ARGB32 4 字节/像素）。 */
    static constexpr qint64 MAX_IMAGE_BYTES = 256LL * 1024 * 1024;
    /** @brief 未同步字体度量时的兜底单元格像素高（常见等宽字体行高量级）。 */
    static constexpr int DEFAULT_CELL_PIXEL_HEIGHT = 16;

    void releaseImageLine(ImageRefLine &row);
    void clearAllImages();

    static Character defaultChar;
};

#endif // SCREEN_H
