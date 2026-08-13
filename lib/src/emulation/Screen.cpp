/*
 This file is part of Konsole, an X terminal.

 Copyright 2007-2008 by Robert Knight <robert.knight@gmail.com>
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
#include "Screen.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QDate>
#include <QTextStream>
#include <QVarLengthArray>

#include "CharWidth.h"
#include "TerminalCharacterDecoder.h"

// Macro to convert x,y position on screen to position within an image.
//
// Originally the image was stored as one large contiguous block of
// memory, so a position within the image could be represented as an
// offset from the beginning of the block.  For efficiency reasons this
// is no longer the case.
// Many internal parts of this class still use this representation for
// parameters and so on, notably moveImage() and clearImage(). This macro
// converts from an X,Y position into an image offset.
#ifndef loc
#define loc(X, Y) ((Y) * columns + (X))
#endif

Character Screen::defaultChar = Character(
        ' ', CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR),
        CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR), DEFAULT_RENDITION);

// #define REVERSE_WRAPPED_LINES  // for wrapped line debug

Screen::Screen(int l, int c)
        : lines(l), columns(c), screenLines(new ImageLine[lines + 1]),
            _scrolledLines(0), _droppedLines(0), history(new HistoryScrollNone()),
            cuX(0), cuY(0), currentRendition(0), _topMargin(0), _bottomMargin(0),
            selBegin(0), selTopLeft(0), selBottomRight(0), blockSelectionMode(false),
            effectiveForeground(CharacterColor()),
            effectiveBackground(CharacterColor()), effectiveRendition(0),
            lastPos(-1), _linkLines(new HyperlinkLine[lines + 1]),
            _imageLines(new ImageRefLine[lines + 1]),
            _kittyLines(new KittyRefLine[lines + 1]) {
    lineProperties.resize(lines + 1);
    for (int i = 0; i < lines + 1; i++)
            lineProperties[i] = LINE_DEFAULT;

    initTabStops();
    clearSelection();
    reset();
}

Screen::~Screen() {
    delete[] screenLines;
    delete[] _linkLines;
    delete[] _imageLines;
    delete[] _kittyLines;
    delete history;
}

void Screen::cursorUp(int n) {
    if (n == 0)
        n = 1; // Default
    int stop = cuY < _topMargin ? 0 : _topMargin;
    cuX = qMin(columns - 1, cuX); // nowrap!
    cuY = qMax(stop, cuY - n);
}

void Screen::cursorDown(int n) {
    if (n == 0)
        n = 1; // Default
    int stop = cuY > _bottomMargin ? lines - 1 : _bottomMargin;
    cuX = qMin(columns - 1, cuX); // nowrap!
    cuY = qMin(stop, cuY + n);
}

void Screen::cursorLeft(int n) {
    if (n == 0)
        n = 1;                      // Default
    cuX = qMin(columns - 1, cuX); // nowrap!
    cuX = qMax(0, cuX - n);
}

void Screen::cursorNextLine(int n) {
    if (n == 0) {
        n = 1; // Default
    }
    cuX = 0;
    while (n > 0) {
        if (cuY < lines - 1) {
            cuY += 1;
        }
        n--;
    }
}

void Screen::cursorPreviousLine(int n) {
    if (n == 0) {
        n = 1; // Default
    }
    cuX = 0;
    while (n > 0) {
        if (cuY > 0) {
            cuY -= 1;
        }
        n--;
    }
}

void Screen::cursorRight(int n) {
    if (n == 0)
        n = 1; // Default
    cuX = qMin(columns - 1, cuX + n);
}

void Screen::setMargins(int top, int bot) {
    if (top == 0)
        top = 1; // Default
    if (bot == 0)
        bot = lines; // Default
    top = top - 1; // Adjust to internal lineno
    bot = bot - 1; // Adjust to internal lineno
    if (!(0 <= top && top < bot &&
                bot < lines)) { // Debug()<<" setRegion("<<top<<","<<bot<<") : bad
                                                // range.";
        return; // Default error action: ignore
    }
    _topMargin = top;
    _bottomMargin = bot;
    cuX = 0;
    cuY = getMode(MODE_Origin) ? top : 0;
}

int Screen::topMargin() const { return _topMargin; }
int Screen::bottomMargin() const { return _bottomMargin; }

void Screen::index() {
    if (cuY == _bottomMargin)
        scrollUp(1);
    else if (cuY < lines - 1)
        cuY += 1;
}

void Screen::reverseIndex() {
    if (cuY == _topMargin)
        scrollDown(_topMargin, 1);
    else if (cuY > 0)
        cuY -= 1;
}

void Screen::nextLine() {
    toStartOfLine();
    index();
}

void Screen::eraseChars(int n) {
    if (n == 0)
        n = 1; // Default
    int p = qMax(0, qMin(cuX + n - 1, columns - 1));
    clearImage(loc(cuX, cuY), loc(p, cuY), ' ');
}

void Screen::deleteChars(int n) {
    Q_ASSERT(n >= 0);

    // always delete at least one char
    if (n == 0)
        n = 1;

    // if cursor is beyond the end of the line there is nothing to do
    if (cuX >= screenLines[cuY].count())
        return;

    if (cuX + n > screenLines[cuY].count())
        n = screenLines[cuY].count() - cuX;

    Q_ASSERT(n >= 0);
    Q_ASSERT(cuX + n <= screenLines[cuY].count());

    screenLines[cuY].remove(cuX, n);
}

void Screen::insertChars(int n) {
    if (n == 0)
        n = 1; // Default

    if (screenLines[cuY].size() < cuX)
        screenLines[cuY].resize(cuX);

    screenLines[cuY].insert(cuX, n, ' ');

    if (screenLines[cuY].count() > columns)
        screenLines[cuY].resize(columns);
}

void Screen::repeatChars(int count) {
    if (count == 0) {
        count = 1;
    }
    /**
     * From ECMA-48 version 5, section 8.3.103
     * If the character preceding REP is a control function or part of a
     * control function, the effect of REP is not defined by this Standard.
     *
     * So, a "normal" program should always use REP immediately after a visible
     * character (those other than escape sequences). So, lastDrawnChar can be
     * safely used.
     */
    for (int i = 0; i < count; i++) {
        displayCharacter(lastDrawnChar);
    }
}

void Screen::deleteLines(int n) {
    if (n == 0)
        n = 1; // Default
    scrollUp(cuY, n);
}

void Screen::insertLines(int n) {
    if (n == 0)
        n = 1; // Default
    scrollDown(cuY, n);
}

void Screen::setMode(int m) {
    currentModes[m] = true;
    switch (m) {
    case MODE_Origin:
        cuX = 0;
        cuY = _topMargin;
        break; // FIXME: home
    }
}

void Screen::resetMode(int m) {
    currentModes[m] = false;
    switch (m) {
    case MODE_Origin:
        cuX = 0;
        cuY = 0;
        break; // FIXME: home
    }
}

void Screen::saveMode(int m) { savedModes[m] = currentModes[m]; }

void Screen::restoreMode(int m) { currentModes[m] = savedModes[m]; }

bool Screen::getMode(int m) const { return currentModes[m]; }

void Screen::saveCursor() {
    savedState.cursorColumn = cuX;
    savedState.cursorLine = cuY;
    savedState.rendition = currentRendition;
    savedState.foreground = currentForeground;
    savedState.background = currentBackground;
}

void Screen::restoreCursor() {
    cuX = qMin(savedState.cursorColumn, columns - 1);
    cuY = qMin(savedState.cursorLine, lines - 1);
    currentRendition = savedState.rendition;
    currentForeground = savedState.foreground;
    currentBackground = savedState.background;
    updateEffectiveRendition();
}

void Screen::resizeImage(int new_lines, int new_columns) {
    if ((new_lines == lines) && (new_columns == columns))
        return;

    if (cuY > new_lines - 1) {   // attempt to preserve focus and lines
        _bottomMargin = lines - 1; // FIXME: margin lost
        for (int i = 0; i < cuY - (new_lines - 1); i++) {
            addHistLine();
            scrollUp(0, 1);
        }
    }

    // create new screen lines and copy from old to new

    ImageLine *newScreenLines = new ImageLine[new_lines + 1];
    for (int i = 0; i < qMin(lines, new_lines + 1); i++)
        newScreenLines[i] = screenLines[i];
    for (int i = lines; (i > 0) && (i < new_lines + 1); i++)
        newScreenLines[i].resize(new_columns);

    lineProperties.resize(new_lines + 1);
    for (int i = lines; (i > 0) && (i < new_lines + 1); i++)
        lineProperties[i] = LINE_DEFAULT;

    clearSelection();

    // OSC 8：段表数组随屏幕尺寸重建；收缩时被裁行的段表回收（先 move 再 delete 旧数组）
    HyperlinkLine *newLinkLines = new HyperlinkLine[new_lines + 1];
    for (int i = 0; i < qMin(lines, new_lines + 1); i++)
        newLinkLines[i] = std::move(_linkLines[i]);
    for (int i = qMin(lines, new_lines + 1); i < lines + 1; i++)
        releaseHyperlinkLine(_linkLines[i]);
    delete[] _linkLines;
    _linkLines = newLinkLines;

    // Sixel：图像引用表数组随屏幕尺寸重建；收缩时被裁行的引用销毁（先 move 再 delete 旧数组）
    ImageRefLine *newImageLines = new ImageRefLine[new_lines + 1];
    for (int i = 0; i < qMin(lines, new_lines + 1); i++)
        newImageLines[i] = std::move(_imageLines[i]);
    for (int i = qMin(lines, new_lines + 1); i < lines + 1; i++)
        releaseImageLine(_imageLines[i]); // 收缩时被裁行的引用销毁
    delete[] _imageLines;
    _imageLines = newImageLines;

    // kitty 放置引用表数组随屏幕尺寸重建；收缩时被裁行的引用销毁（先 move 再 delete 旧数组）
    KittyRefLine *newKittyLines = new KittyRefLine[new_lines + 1];
    for (int i = 0; i < qMin(lines, new_lines + 1); i++)
        newKittyLines[i] = std::move(_kittyLines[i]);
    for (int i = qMin(lines, new_lines + 1); i < lines + 1; i++)
        releaseKittyRefLine(_kittyLines[i]); // 收缩时被裁行的引用销毁
    delete[] _kittyLines;
    _kittyLines = newKittyLines;

    delete[] screenLines;
    screenLines = newScreenLines;

    lines = new_lines;
    columns = new_columns;
    cuX = qMin(cuX, columns - 1);
    cuY = qMin(cuY, lines - 1);

    // FIXME: try to keep values, evtl.
    _topMargin = 0;
    _bottomMargin = lines - 1;
    initTabStops();
    clearSelection();
}

void Screen::setDefaultMargins() {
    _topMargin = 0;
    _bottomMargin = lines - 1;
}

/*
 Clarifying rendition here and in the display.
 
 currently, the display's color table is
 0       1       2 .. 9    10 .. 17
 dft_fg, dft_bg, dim 0..7, intensive 0..7
 
 currentForeground, currentBackground contain values 0..8;
 - 0    = default color
 - 1..8 = ansi specified color
 
 re_fg, re_bg contain values 0..17
 due to the TerminalDisplay's color table
 
 rendition attributes are
 
 attr           widget screen
 -------------- ------ ------
 RE_UNDERLINE     XX     XX    affects foreground only
 RE_BLINK         XX     XX    affects foreground only
 RE_BOLD          XX     XX    affects foreground only
 RE_REVERSE       --     XX
 RE_TRANSPARENT   XX     --    affects background only
 RE_INTENSIVE     XX     --    affects foreground only
 
 Note that RE_BOLD is used in both widget
 and screen rendition. Since xterm/vt102
 is to poor to distinguish between bold
 (which is a font attribute) and intensive
 (which is a color attribute), we translate
 this and RE_BOLD in falls eventually apart
 into RE_BOLD and RE_INTENSIVE.
*/
void Screen::reverseRendition(Character &p) const {
    CharacterColor f = p.foregroundColor;
    CharacterColor b = p.backgroundColor;

    p.foregroundColor = b;
    p.backgroundColor = f; // p->r &= ~RE_TRANSPARENT;
}

void Screen::updateEffectiveRendition() {
    effectiveRendition = currentRendition;
    if (currentRendition & RE_REVERSE) {
        effectiveForeground = currentBackground;
        effectiveBackground = currentForeground;
    } else {
        effectiveForeground = currentForeground;
        effectiveBackground = currentBackground;
    }

    if (currentRendition & RE_BOLD)
        effectiveForeground.setIntensive();
}

void Screen::copyFromHistory(Character *dest, int startLine, int count) const {
    Q_ASSERT(startLine >= 0 && count > 0 &&
                     startLine + count <= history->getLines());

    for (int line = startLine; line < startLine + count; line++) {
        const int length = qMin(columns, history->getLineLen(line));
        const int destLineOffset = (line - startLine) * columns;

        history->getCells(line, 0, length, dest + destLineOffset);

        for (int column = length; column < columns; column++)
            dest[destLineOffset + column] = defaultChar;

        // invert selected text
        if (selBegin != -1) {
            for (int column = 0; column < columns; column++) {
                if (isSelected(column, line)) {
                    reverseRendition(dest[destLineOffset + column]);
                }
            }
        }
    }
}

void Screen::copyFromScreen(Character *dest, int startLine, int count) const {
    Q_ASSERT(startLine >= 0 && count > 0 && startLine + count <= lines);

    for (int line = startLine; line < (startLine + count); line++) {
        int srcLineStartIndex = line * columns;
        int destLineStartIndex = (line - startLine) * columns;

        for (int column = 0; column < columns; column++) {
            int srcIndex = srcLineStartIndex + column;
            int destIndex = destLineStartIndex + column;

            dest[destIndex] = screenLines[srcIndex / columns].value(
                    srcIndex % columns, defaultChar);

            // invert selected text
            if (selBegin != -1 && isSelected(column, line + history->getLines()))
                reverseRendition(dest[destIndex]);
        }
    }
}

void Screen::getImage(Character *dest, int size, int startLine,
                                            int endLine) const {
    Q_ASSERT(startLine >= 0);
    Q_ASSERT(endLine >= startLine && endLine < history->getLines() + lines);

    const int mergedLines = endLine - startLine + 1;

    Q_ASSERT(size >= mergedLines * columns);
    Q_UNUSED(size);

    const int linesInHistoryBuffer =
            qBound(0, history->getLines() - startLine, mergedLines);
    const int linesInScreenBuffer = mergedLines - linesInHistoryBuffer;

    // copy lines from history buffer
    if (linesInHistoryBuffer > 0)
        copyFromHistory(dest, startLine, linesInHistoryBuffer);

    // copy lines from screen buffer
    if (linesInScreenBuffer > 0)
        copyFromScreen(dest + linesInHistoryBuffer * columns,
                                     startLine + linesInHistoryBuffer - history->getLines(),
                                     linesInScreenBuffer);

    // invert display when in screen mode
    if (getMode(MODE_Screen)) {
        for (int i = 0; i < mergedLines * columns; i++)
            reverseRendition(dest[i]); // for reverse display
    }

    // mark the character at the current cursor position
    int cursorIndex = loc(cuX, cuY + linesInHistoryBuffer);
    if (getMode(MODE_Cursor) && cursorIndex < columns * mergedLines)
        dest[cursorIndex].rendition |= RE_CURSOR;
}

QVector<LineProperty> Screen::getLineProperties(int startLine,
                                                                                                int endLine) const {
    Q_ASSERT(startLine >= 0);
    Q_ASSERT(endLine >= startLine && endLine < history->getLines() + lines);

    const int mergedLines = endLine - startLine + 1;
    const int linesInHistory =
            qBound(0, history->getLines() - startLine, mergedLines);
    const int linesInScreen = mergedLines - linesInHistory;

    QVector<LineProperty> result(mergedLines);
    int index = 0;

    // copy properties for lines in history
    for (int line = startLine; line < startLine + linesInHistory; line++) {
        // TODO Support for line properties other than wrapped lines
        if (history->isWrappedLine(line)) {
            result[index] = static_cast<LineProperty>(result[index] | LINE_WRAPPED);
        }
        index++;
    }

    // copy properties for lines in screen buffer
    const int firstScreenLine = startLine + linesInHistory - history->getLines();
    for (int line = firstScreenLine; line < firstScreenLine + linesInScreen;
             line++) {
        result[index] = lineProperties[line];
        index++;
    }

    return result;
}

void Screen::reset(bool clearScreen) {
    setMode(MODE_Wrap);
    saveMode(MODE_Wrap); // wrap at end of margin
    resetMode(MODE_Origin);
    saveMode(MODE_Origin); // position refers to [1,1]
    resetMode(MODE_Insert);
    saveMode(MODE_Insert);  // overstroke
    setMode(MODE_Cursor);   // cursor visible
    resetMode(MODE_Screen); // screen not inverse
    resetMode(MODE_NewLine);

    _topMargin = 0;
    _bottomMargin = lines - 1;

    setDefaultRendition();
    saveCursor();

    clearAllHyperlinks(); // OSC 8：复位时丢弃全部链接段表与 URI 映射
    clearAllImages(); // 复位时丢弃全部图像与引用

    if (clearScreen)
        clear();
}

void Screen::clear() {
    clearEntireScreen();
    home();
}

void Screen::backspace() {
    cuX = qMin(columns - 1, cuX); // nowrap!
    cuX = qMax(0, cuX - 1);

    if (screenLines[cuY].size() < cuX + 1)
        screenLines[cuY].resize(cuX + 1);
}

void Screen::tab(int n) {
    // note that TAB is a format effector (does not write ' ');
    if (n == 0)
        n = 1;
    while ((n > 0) && (cuX < columns - 1)) {
        cursorRight(1);
        while ((cuX < columns - 1) && !tabStops[cuX])
            cursorRight(1);
        n--;
    }
}

void Screen::backtab(int n) {
    // note that TAB is a format effector (does not write ' ');
    if (n == 0)
        n = 1;
    while ((n > 0) && (cuX > 0)) {
        cursorLeft(1);
        while ((cuX > 0) && !tabStops[cuX])
            cursorLeft(1);
        n--;
    }
}

void Screen::clearTabStops() {
    for (int i = 0; i < columns; i++)
        tabStops[i] = false;
}

void Screen::changeTabStop(bool set) {
    if (cuX >= columns)
        return;
    tabStops[cuX] = set;
}

void Screen::initTabStops() {
    tabStops.resize(columns);

    // Arrg! The 1st tabstop has to be one longer than the other.
    // i.e. the kids start counting from 0 instead of 1.
    // Other programs might behave correctly. Be aware.
    for (int i = 0; i < columns; i++)
        tabStops[i] = (i % 8 == 0 && i != 0);
}

void Screen::newLine() {
    if (getMode(MODE_NewLine))
        toStartOfLine();
    index();
}

void Screen::checkSelection(int from, int to) {
    if (selBegin == -1)
        return;
    int scr_TL = loc(0, history->getLines());
    // Clear entire selection if it overlaps region [from, to]
    if ((selBottomRight >= (from + scr_TL)) && (selTopLeft <= (to + scr_TL)))
        clearSelection();
}

static inline bool isRegionalIndicator(char32_t c) {
    return (c >= 0x1F1E6 && c <= 0x1F1FF); // for creating flag codes
}

void Screen::displayCharacter(char32_t c) {
    // Note that VT100 does wrapping BEFORE putting the character.
    // This has impact on the assumption of valid cursor positions.
    // We indicate the fact that a newline has to be triggered by
    // putting the cursor one right to the last column of the screen.

    int w = CharWidth::unicode_width(c);
    if (w < 0)
        return; // Non-printable character
    if (w == 0
        // Also, make an extended character with a pair of flag codes
        || (w == 1 && isRegionalIndicator(c)))
    {
        if (w == 0 && QChar(c).category() != QChar::Mark_NonSpacing)
            return;
        // Find previous "real character" to try to combine with
        int charToCombineWithX = qMin(cuX, screenLines[cuY].length());
        int charToCombineWithY = cuY;
        bool previousChar = true;
        do {
            if (charToCombineWithX > 0) {
                --charToCombineWithX;
            } else if (charToCombineWithY > 0 && lineProperties.at(charToCombineWithY - 1) & LINE_WRAPPED) { 
                // Try previous line
                --charToCombineWithY;
                charToCombineWithX = screenLines[charToCombineWithY].length() - 1;
            } else {
                // Give up
                previousChar = false;
                break;
            }

            // Failsafe
            if (charToCombineWithX < 0) {
                previousChar = false;
                break;
            }
        } while (w == 0 && screenLines[charToCombineWithY][charToCombineWithX] == 0);

        if (!previousChar) {
            if (w == 0) {
                w = 2;
            }
            goto notcombine;
        }

        Character& currentChar = screenLines[charToCombineWithY][charToCombineWithX];

        if (w > 0 && !isRegionalIndicator(currentChar.character)) {
            goto notcombine; // a single regional indicator (useless)
        }

        if ((currentChar.rendition & RE_EXTENDED_CHAR) == 0) {
            uint chars[2] = { static_cast<uint>(currentChar.character), static_cast<uint>(c) };
            currentChar.rendition |= RE_EXTENDED_CHAR;
            currentChar.character = ExtendedCharTable::instance.createExtendedChar(chars, 2);

            // when there is a pair of flag codes
            if (w > 0) {
                if (cuX + 1 > columns) {
                    if (getMode(MODE_Wrap)) {
                        lineProperties[cuY] = static_cast<LineProperty>(lineProperties[cuY] | LINE_WRAPPED);
                        nextLine();
                    } else {
                        cuX = columns - 1;
                    }
                }

                if (screenLines[cuY].size() < cuX + 1) {
                    screenLines[cuY].resize(cuX + 1);
                }

                // NOTE: This is needed for correct selection.
                Character& ch = screenLines[cuY][cuX];
                ch.character = 0;
                ch.foregroundColor = effectiveForeground;
                ch.backgroundColor = effectiveBackground;
                ch.rendition = effectiveRendition;

                if (getMode(MODE_Insert)) {
                    insertChars(1);
                }

                lastPos = loc(cuX,cuY);
                checkSelection(lastPos, lastPos);
                lastDrawnChar = currentChar.character;

                cuX++;
            }
        } else {
            ushort extendedCharLength;
            const uint* oldChars = ExtendedCharTable::instance.lookupExtendedChar(currentChar.character, extendedCharLength);
            Q_ASSERT(oldChars);
            if (oldChars && extendedCharLength < 8) {
                Q_ASSERT(extendedCharLength > 1);
                Q_ASSERT(extendedCharLength < 65535); // redundant due to above check
                auto chars = std::make_unique<uint[]>(extendedCharLength + 1);
                std::copy_n(oldChars, extendedCharLength, chars.get());
                chars[extendedCharLength] = c;
                currentChar.character = ExtendedCharTable::instance.createExtendedChar(chars.get(), extendedCharLength + 1);
            }
        }
        return;
    }

notcombine:
    if (cuX + w > columns) {
        if (getMode(MODE_Wrap)) {
            lineProperties[cuY] = static_cast<LineProperty>(lineProperties[cuY] | LINE_WRAPPED);
            nextLine();
        } else {
            cuX = columns - w;
        }
    }

    // ensure current line vector has enough elements
    int size = screenLines[cuY].size();
    if (size < cuX + w) {
        screenLines[cuY].resize(cuX + w);
    }

    if (getMode(MODE_Insert))
        insertChars(w);

    const int writeStartX = cuX; // 折行后的最终写入起始列（OSC 8 段表用）
    lastPos = loc(cuX, cuY);

    // check if selection is still valid.
    checkSelection(lastPos, lastPos);

    Character &currentChar = screenLines[cuY][cuX];

    currentChar.character = c;
    currentChar.foregroundColor = effectiveForeground;
    currentChar.backgroundColor = effectiveBackground;
    currentChar.rendition = effectiveRendition;

    lastDrawnChar = c;

    int i = 0;
    int newCursorX = cuX + w--;
    while (w) {
        i++;

        if (screenLines[cuY].size() < cuX + i + 1)
            screenLines[cuY].resize(cuX + i + 1);

        Character &ch = screenLines[cuY][cuX + i];
        ch.character = 0;
        ch.foregroundColor = effectiveForeground;
        ch.backgroundColor = effectiveBackground;
        ch.rendition = effectiveRendition;

        w--;
    }
    cuX = newCursorX;

    // OSC 8：活动链接期间写入的字符计入当前行的链接段表
    // 已知简化：MODE_Insert 插入模式下既有段表不随字符右移，段与单元格可能错位（组合场景罕见）
    if (_currentHyperlinkId != 0)
        addHyperlinkSegment(cuY, writeStartX, newCursorX - 1);
}

void Screen::compose(const QString & /*compose*/) {
    Q_ASSERT(0 /*Not implemented yet*/);

    /*  if (lastPos == -1)
            return;

            QChar c(image[lastPos].character);
            compose.prepend(c);
    //compose.compose(); ### FIXME!
    image[lastPos].character = compose[0].unicode();*/
}

int Screen::scrolledLines() const { return _scrolledLines; }
int Screen::droppedLines() const { return _droppedLines; }
void Screen::resetDroppedLines() { _droppedLines = 0; }
void Screen::resetScrolledLines() { _scrolledLines = 0; }

void Screen::scrollUp(int n) {
    if (n == 0)
        n = 1; // Default
    if (_topMargin == 0)
        addHistLine(); // history.history
    scrollUp(_topMargin, n);
}

QRect Screen::lastScrolledRegion() const { return _lastScrolledRegion; }

void Screen::scrollUp(int from, int n) {
    if (n <= 0)
        return;
    if (from > _bottomMargin)
        return;
    if (from + n > _bottomMargin)
        n = _bottomMargin + 1 - from;

    _scrolledLines -= n;
    _lastScrolledRegion =
            QRect(0, _topMargin, columns - 1, (_bottomMargin - _topMargin));

    // FIXME: make sure `topMargin', `bottomMargin', `from', `n' is in bounds.
    moveImage(loc(0, from), loc(0, from + n), loc(columns, _bottomMargin));
    clearImage(loc(0, _bottomMargin - n + 1), loc(columns - 1, _bottomMargin),
                         ' ');
}

void Screen::scrollDown(int n) {
    if (n == 0)
        n = 1; // Default
    scrollDown(_topMargin, n);
}

void Screen::scrollDown(int from, int n) {
    _scrolledLines += n;

    // FIXME: make sure `topMargin', `bottomMargin', `from', `n' is in bounds.
    if (n <= 0)
        return;
    if (from > _bottomMargin)
        return;
    if (from + n > _bottomMargin)
        n = _bottomMargin - from;
    moveImage(loc(0, from + n), loc(0, from),
                        loc(columns - 1, _bottomMargin - n));
    clearImage(loc(0, from), loc(columns - 1, from + n - 1), ' ');
}

void Screen::setCursorYX(int y, int x) {
    setCursorY(y);
    setCursorX(x);
}

void Screen::setCursorX(int x) {
    if (x == 0)
        x = 1; // Default
    x -= 1;  // Adjust
    cuX = qMax(0, qMin(columns - 1, x));
}

void Screen::setCursorY(int y) {
    if (y == 0)
        y = 1; // Default
    y -= 1;  // Adjust
    cuY = qMax(0, qMin(lines - 1, y + (getMode(MODE_Origin) ? _topMargin : 0)));
}

void Screen::home() {
    cuX = 0;
    cuY = 0;
}

void Screen::toStartOfLine() { cuX = 0; }

int Screen::getCursorX() const { return cuX; }

int Screen::getCursorY() const { return cuY; }

QString Screen::getScreenText(int row1, int col1, int row2, int col2, int mode) {
    Q_ASSERT(row1 >= 0 && row1 < lines);
    Q_ASSERT(row2 >= 0 && row2 < lines);
    Q_ASSERT(col1 >= 0 && col1 < columns);
    Q_ASSERT(col2 >= 0 && col2 < columns);

    QString text;
    int startLine = qMin(row1, row2);
    int endLine = qMax(row1, row2);
    int startCol = qMin(col1, col2);
    int endCol = qMax(col1, col2);

    if (mode == 1) {
        for (int i = startLine; i <= endLine; i++) {
            if (lines <= i) // 行号越界防御（原用 screenLines->size()，误取第 0 行字符数作行数）
                break;
            char32_t prevChar = 0;
            for (int j = startCol; j <= endCol; j++) {
                if (screenLines[i].count() <= j)
                    break;
                // 以 UCS-4 码点追加：BMP 外字符（如 emoji）在此还原为代理对
                char32_t c = screenLines[i][j].character;
                if (c == 0) {
                    // 宽字符的占位单元格：前导字符为 BMP（仅占 1 个 UTF-16 单元）时
                    // 补一个 NUL 保持文本索引与列对齐；前导字符为 BMP 外字符时
                    // 其代理对已占 2 个 UTF-16 单元，占位跳过
                    if (prevChar <= 0xFFFF)
                        text += QChar(0);
                    continue;
                }
                text += QString::fromUcs4(&c, 1);
                prevChar = c;
            }
        }
    } else if (mode == 2) {
        for (int i = startLine; i <= endLine; i++) {
            if (lines <= i) // 行号越界防御（原用 screenLines->size()，误取第 0 行字符数作行数）
                break;
            int size = 0;
            char32_t prevChar = 0;
            for (int j = startCol; j <= endCol; j++) {
                if (screenLines[i].count() <= j)
                    break;
                // 以 UCS-4 码点追加：BMP 外字符（如 emoji）在此还原为代理对；
                // 宽字符占位单元格的处理同 mode 1
                char32_t c = screenLines[i][j].character;
                if (c == 0) {
                    if (prevChar <= 0xFFFF)
                        text += QChar(0);
                    continue;
                }
                text += QString::fromUcs4(&c, 1);
                prevChar = c;
                size++;
            }
            if (size != 0) {
                text += '\n';
            }
        }
    }

    return text;
}

void Screen::clearImage(int loca, int loce, char c) {
    int scr_TL = loc(0, history->getLines());
    // FIXME: check positions

    // Clear entire selection if it overlaps region to be moved...
    if ((selBottomRight > (loca + scr_TL)) && (selTopLeft < (loce + scr_TL))) {
        clearSelection();
    }

    int topLine = loca / columns;
    int bottomLine = loce / columns;

    Character clearCh(c, currentForeground, currentBackground, DEFAULT_RENDITION);

    // if the character being used to clear the area is the same as the
    // default character, the affected lines can simply be shrunk.
    bool isDefaultCh = (clearCh == Character());

    for (int y = topLine; y <= bottomLine; y++) {
        lineProperties[y] = 0;
        releaseHyperlinkLine(_linkLines[y]); // 清行连带清除链接段表
        releaseImageLine(_imageLines[y]); // 清行连带销毁图像引用
        releaseKittyRefLine(_kittyLines[y]); // 清行连带销毁 kitty 放置引用

        int endCol = (y == bottomLine) ? loce % columns : columns - 1;
        int startCol = (y == topLine) ? loca % columns : 0;

        QVector<Character> &line = screenLines[y];

        if (isDefaultCh && endCol == columns - 1) {
            line.resize(startCol);
        } else {
            if (line.size() < endCol + 1)
                line.resize(endCol + 1);

            Character *data = line.data();
            for (int i = startCol; i <= endCol; i++)
                data[i] = clearCh;
        }
    }
}

void Screen::moveImage(int dest, int sourceBegin, int sourceEnd) {
    Q_ASSERT(sourceBegin <= sourceEnd);

    int lines = (sourceEnd - sourceBegin) / columns;

    // move screen image and line properties:
    // the source and destination areas of the image may overlap,
    // so it matters that we do the copy in the right order -
    // forwards if dest < sourceBegin or backwards otherwise.
    //(search the web for 'memmove implementation' for details)
    if (dest < sourceBegin) {
        for (int i = 0; i <= lines; i++) {
            releaseHyperlinkLine(_linkLines[(dest / columns) + i]); // 目标行被覆盖，旧段表回收
            releaseImageLine(_imageLines[(dest / columns) + i]); // 目标行被覆盖，旧引用销毁
            releaseKittyRefLine(_kittyLines[(dest / columns) + i]); // 目标行被覆盖，旧引用销毁
            screenLines[(dest / columns) + i] =
                    screenLines[(sourceBegin / columns) + i];
            lineProperties[(dest / columns) + i] =
                    lineProperties[(sourceBegin / columns) + i];
            _linkLines[(dest / columns) + i] =
                    std::move(_linkLines[(sourceBegin / columns) + i]); // 段表随行走（move 防止引用计数双降）
            _imageLines[(dest / columns) + i] =
                    std::move(_imageLines[(sourceBegin / columns) + i]); // 引用随行走（move 防止引用计数双降）
            _kittyLines[(dest / columns) + i] =
                    std::move(_kittyLines[(sourceBegin / columns) + i]); // 引用随行走
        }
    } else {
        for (int i = lines; i >= 0; i--) {
            releaseHyperlinkLine(_linkLines[(dest / columns) + i]); // 目标行被覆盖，旧段表回收
            releaseImageLine(_imageLines[(dest / columns) + i]); // 目标行被覆盖，旧引用销毁
            releaseKittyRefLine(_kittyLines[(dest / columns) + i]); // 目标行被覆盖，旧引用销毁
            screenLines[(dest / columns) + i] =
                    screenLines[(sourceBegin / columns) + i];
            lineProperties[(dest / columns) + i] =
                    lineProperties[(sourceBegin / columns) + i];
            _linkLines[(dest / columns) + i] =
                    std::move(_linkLines[(sourceBegin / columns) + i]); // 段表随行走（move 防止引用计数双降）
            _imageLines[(dest / columns) + i] =
                    std::move(_imageLines[(sourceBegin / columns) + i]); // 引用随行走（move 防止引用计数双降）
            _kittyLines[(dest / columns) + i] =
                    std::move(_kittyLines[(sourceBegin / columns) + i]); // 引用随行走
        }
    }

    if (lastPos != -1) {
        int diff = dest - sourceBegin; // Scroll by this amount
        lastPos += diff;
        if ((lastPos < 0) || (lastPos >= (lines * columns)))
            lastPos = -1;
    }

    // Adjust selection to follow scroll.
    if (selBegin != -1) {
        bool beginIsTL = (selBegin == selTopLeft);
        int diff = dest - sourceBegin; // Scroll by this amount
        int scr_TL = loc(0, history->getLines());
        int srca = sourceBegin + scr_TL; // Translate index from screen to global
        int srce = sourceEnd + scr_TL;   // Translate index from screen to global
        int desta = srca + diff;
        int deste = srce + diff;

        if ((selTopLeft >= srca) && (selTopLeft <= srce))
            selTopLeft += diff;
        else if ((selTopLeft >= desta) && (selTopLeft <= deste))
            selBottomRight = -1; // Clear selection (see below)

        if ((selBottomRight >= srca) && (selBottomRight <= srce))
            selBottomRight += diff;
        else if ((selBottomRight >= desta) && (selBottomRight <= deste))
            selBottomRight = -1; // Clear selection (see below)

        if (selBottomRight < 0) {
            clearSelection();
        } else {
            if (selTopLeft < 0)
                selTopLeft = 0;
        }

        if (beginIsTL)
            selBegin = selTopLeft;
        else
            selBegin = selBottomRight;
    }
}

void Screen::clearToEndOfScreen() {
    clearImage(loc(cuX, cuY), loc(columns - 1, lines - 1), ' ');
}

void Screen::clearToBeginOfScreen() {
    clearImage(loc(0, 0), loc(cuX, cuY), ' ');
}

void Screen::clearEntireScreen() {
    // Add entire screen to history
    for (int i = 0; i < (lines - 1); i++) {
        addHistLine();
        scrollUp(0, 1);
    }

    clearImage(loc(0, 0), loc(columns - 1, lines - 1), ' ');
}

/*! fill screen with 'E'
    This is to aid screen alignment
    */

void Screen::helpAlign() {
    clearImage(loc(0, 0), loc(columns - 1, lines - 1), 'E');
}

void Screen::clearToEndOfLine() {
    clearImage(loc(cuX, cuY), loc(columns - 1, cuY), ' ');
}

void Screen::clearToBeginOfLine() {
    clearImage(loc(0, cuY), loc(cuX, cuY), ' ');
}

void Screen::clearEntireLine() {
    clearImage(loc(0, cuY), loc(columns - 1, cuY), ' ');
}

void Screen::setRendition(int re) {
    currentRendition |= re;
    updateEffectiveRendition();
}

void Screen::resetRendition(int re) {
    currentRendition &= ~re;
    updateEffectiveRendition();
}

void Screen::setDefaultRendition() {
    setForeColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
    setBackColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR);
    currentRendition = DEFAULT_RENDITION;
    updateEffectiveRendition();
}

void Screen::setForeColor(int space, int color) {
    currentForeground = CharacterColor(space, color);

    if (currentForeground.isValid())
        updateEffectiveRendition();
    else
        setForeColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
}

void Screen::setBackColor(int space, int color) {
    currentBackground = CharacterColor(space, color);

    if (currentBackground.isValid())
        updateEffectiveRendition();
    else
        setBackColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR);
}

void Screen::clearSelection() {
    selBottomRight = -1;
    selTopLeft = -1;
    selBegin = -1;
}

bool Screen::isClearSelection() {
    return selBottomRight == -1 && selTopLeft == -1 && selBegin == -1;
}

void Screen::getSelectionStart(int &column, int &line) const {
    if (selTopLeft != -1) {
        column = selTopLeft % columns;
        line = selTopLeft / columns;
    } else {
        column = cuX + getHistLines();
        line = cuY + getHistLines();
    }
}
void Screen::getSelectionEnd(int &column, int &line) const {
    if (selBottomRight != -1) {
        column = selBottomRight % columns;
        line = selBottomRight / columns;
    } else {
        column = cuX + getHistLines();
        line = cuY + getHistLines();
    }
}
void Screen::setSelectionStart(const int x, const int y, const bool mode) {
    selBegin = loc(x, y);
    /* FIXME, HACK to correct for x too far to the right... */
    if (x == columns)
        selBegin--;

    selBottomRight = selBegin;
    selTopLeft = selBegin;
    blockSelectionMode = mode;
}

void Screen::setSelectionEnd(const int x, const int y) {
    if (selBegin == -1)
        return;

    int endPos = loc(x, y);

    if (endPos < selBegin) {
        selTopLeft = endPos;
        selBottomRight = selBegin;
    } else {
        /* FIXME, HACK to correct for x too far to the right... */
        if (x == columns)
            endPos--;

        selTopLeft = selBegin;
        selBottomRight = endPos;
    }

    // Normalize the selection in column mode
    if (blockSelectionMode) {
        int topRow = selTopLeft / columns;
        int topColumn = selTopLeft % columns;
        int bottomRow = selBottomRight / columns;
        int bottomColumn = selBottomRight % columns;

        selTopLeft = loc(qMin(topColumn, bottomColumn), topRow);
        selBottomRight = loc(qMax(topColumn, bottomColumn), bottomRow);
    }
}

bool Screen::isSelected(const int x, const int y) const {
    bool columnInSelection = true;
    if (blockSelectionMode) {
        columnInSelection =
                x >= (selTopLeft % columns) && x <= (selBottomRight % columns);
    }

    int pos = loc(x, y);
    return pos >= selTopLeft && pos <= selBottomRight && columnInSelection;
}

QString Screen::selectedText(bool preserveLineBreaks) const {
    QString result;
    QTextStream stream(&result, QIODevice::ReadWrite);

    PlainTextDecoder decoder;
    decoder.begin(&stream);
    writeSelectionToStream(&decoder, preserveLineBreaks);
    decoder.end();

    return result;
}

bool Screen::isSelectionValid() const {
    return selTopLeft >= 0 && selBottomRight >= 0;
}

void Screen::writeSelectionToStream(TerminalCharacterDecoder *decoder,
                                                                        bool preserveLineBreaks) const {
    if (!isSelectionValid())
        return;
    writeToStream(decoder, selTopLeft, selBottomRight, preserveLineBreaks);
}

void Screen::writeToStream(TerminalCharacterDecoder *decoder, int startIndex,
                            int endIndex, bool preserveLineBreaks) const {
    int top = startIndex / columns;
    int left = startIndex % columns;

    int bottom = endIndex / columns;
    int right = endIndex % columns;

    Q_ASSERT(top >= 0 && left >= 0 && bottom >= 0 && right >= 0);

    for (int y = top; y <= bottom; y++) {
        int start = 0;
        if (y == top || blockSelectionMode)
            start = left;

        int count = -1;
        if (y == bottom || blockSelectionMode)
            count = right - start + 1;

        const bool appendNewLine = (y != bottom);
        int copied = copyLineToStream(y, start, count, decoder, appendNewLine,
                                                                    preserveLineBreaks);

        // if the selection goes beyond the end of the last line then
        // append a new line character.
        //
        // this makes it possible to 'select' a trailing new line character after
        // the text on a line.
        if (y == bottom && copied < count) {
            Character newLineChar('\n');
            decoder->decodeLine(&newLineChar, 1, 0);
        }
    }
}

int Screen::copyLineToStream(int line, int start, int count,
                            TerminalCharacterDecoder *decoder,
                            bool appendNewLine,
                            bool preserveLineBreaks) const {
    // 行字符缓冲：栈上小容量、超出自动堆分配（替代原静态 1024 上限，消除线程安全与越界隐患）
    const int worstCase = qMax(columns, line < history->getLines() ? history->getLineLen(line) : 0) + 1;
    QVarLengthArray<Character> characterBuffer(worstCase);

    LineProperty currentLineProperties = 0;

    // determine if the line is in the history buffer or the screen image
    if (line < history->getLines()) {
        const int lineLength = history->getLineLen(line);

        // ensure that start position is before end of line
        start = qMin(start, qMax(0, lineLength - 1));

        // retrieve line from history buffer.  It is assumed
        // that the history buffer does not store trailing white space
        // at the end of the line, so it does not need to be trimmed here
        if (count == -1) {
            count = lineLength - start;
        } else {
            count = qMin(start + count, lineLength) - start;
        }

        // safety checks
        Q_ASSERT(start >= 0);
        Q_ASSERT(count >= 0);
        Q_ASSERT((start + count) <= history->getLineLen(line));

        history->getCells(line, start, count, characterBuffer.data());

        if (history->isWrappedLine(line))
            currentLineProperties |= LINE_WRAPPED;
    } else {
        if (count == -1)
            count = columns - start;

        Q_ASSERT(count >= 0);

        const int screenLine = line - history->getLines();

        Character *data = screenLines[screenLine].data();
        int length = screenLines[screenLine].count();

        // retrieve line from screen image
        for (int i = start; i < qMin(start + count, length); i++) {
            characterBuffer[i - start] = data[i];
        }

        // count cannot be any greater than length
        count = qBound(0, count, length >= start ? length - start : 0);

        Q_ASSERT(screenLine < lineProperties.count());
        currentLineProperties |= lineProperties[screenLine];
    }

    // add new line character at end
    const bool omitLineBreak =
            (currentLineProperties & LINE_WRAPPED) || !preserveLineBreaks;

    // 边界说明：换行符写入索引 count，其最大取值为 worstCase - 1（行满时 count == columns 或
    // == 历史行长），故条件为 count < worstCase 即可保证不越界；若收紧为 count + 1 < worstCase
    // 会在行恰好写满时静默丢弃 '\n'，导致复制/导出时满行与下一行粘连
    if (!omitLineBreak && appendNewLine && (count < worstCase)) {
        characterBuffer[count] = '\n';
        count++;
    }

    // decode line and write to text stream
    decoder->decodeLine(characterBuffer.data(), count,
                                            currentLineProperties);

    return count;
}

void Screen::writeLinesToStream(TerminalCharacterDecoder *decoder, int fromLine,
                                int toLine) const {
    writeToStream(decoder, loc(0, fromLine), loc(columns - 1, toLine));
}

void Screen::addHistLine() {
    // add line to history buffer
    // we have to take care about scrolling, too...

    if (hasScroll()) {
        int oldHistLines = history->getLines();

        history->addCellsVector(screenLines[0]);
        history->addLine(lineProperties[0] & LINE_WRAPPED);

        int newHistLines = history->getLines();

        // OSC 8：链接段随行进入 scrollback；历史满丢弃最旧行时同步丢弃其段表
        if (newHistLines > oldHistLines) {
            _historyLinks.push_back(std::move(_linkLines[0]));
        } else if (oldHistLines > 0) {
            releaseHyperlinkLine(_historyLinks.front());
            _historyLinks.pop_front();
            _historyLinks.push_back(std::move(_linkLines[0]));
        } else {
            releaseHyperlinkLine(_linkLines[0]); // 防御：历史容量为零（行无法入库）时直接丢弃
        }
        _linkLines[0].clear();

        // Sixel：图像引用随行进入 scrollback；历史满丢弃最旧行时同步销毁其引用
        if (newHistLines > oldHistLines) {
            _historyImages.push_back(std::move(_imageLines[0]));
        } else if (oldHistLines > 0) {
            releaseImageLine(_historyImages.front());
            _historyImages.pop_front();
            _historyImages.push_back(std::move(_imageLines[0]));
        } else {
            releaseImageLine(_imageLines[0]); // 防御：历史容量为零（行无法入库）时直接销毁
        }
        _imageLines[0].clear();

        // kitty 放置引用随行进入 scrollback；历史满丢弃最旧行时同步销毁其引用
        if (newHistLines > oldHistLines) {
            _historyKittyRefs.push_back(std::move(_kittyLines[0]));
        } else if (oldHistLines > 0) {
            releaseKittyRefLine(_historyKittyRefs.front());
            _historyKittyRefs.pop_front();
            _historyKittyRefs.push_back(std::move(_kittyLines[0]));
        } else {
            releaseKittyRefLine(_kittyLines[0]); // 防御：历史容量为零时直接销毁
        }
        _kittyLines[0].clear();

        bool beginIsTL = (selBegin == selTopLeft);

        // If the history is full, increment the count
        // of dropped lines
        if (newHistLines == oldHistLines)
            _droppedLines++;

        // Adjust selection for the new point of reference
        if (newHistLines > oldHistLines) {
            if (selBegin != -1) {
                selTopLeft += columns;
                selBottomRight += columns;
            }
        }

        if (selBegin != -1) {
            // Scroll selection in history up
            int top_BR = loc(0, 1 + newHistLines);

            if (selTopLeft < top_BR)
                selTopLeft -= columns;

            if (selBottomRight < top_BR)
                selBottomRight -= columns;

            if (selBottomRight < 0)
                clearSelection();
            else {
                if (selTopLeft < 0)
                    selTopLeft = 0;
            }

            if (beginIsTL)
                selBegin = selTopLeft;
            else
                selBegin = selBottomRight;
        }
    }
}

void Screen::setCurrentHyperlink(const QString &uri, const QString &osc8Id) {
    if (uri.isEmpty()) {
        _currentHyperlinkId = 0; // 空 URI：结束当前链接
        return;
    }
    if (!osc8Id.isEmpty()) {
        // 相同 id 且 URI 未变：复用 linkId（分段属于同一链接）
        auto it = _hyperlinkIds.find(osc8Id);
        if (it != _hyperlinkIds.end() && _hyperlinkUris.value(it.value()) == uri) {
            _currentHyperlinkId = it.value();
            return;
        }
    }
    const quint32 id = _nextHyperlinkId++;
    _hyperlinkUris.insert(id, uri);
    _hyperlinkRefs.insert(id, 0);
    if (!osc8Id.isEmpty())
        _hyperlinkIds.insert(osc8Id, id);
    _currentHyperlinkId = id;
}

QVector<HyperlinkSegment> Screen::linkSegments(int absoluteLine) const {
    const int histLines = history->getLines();
    if (absoluteLine < 0 || absoluteLine >= histLines + lines)
        return {};
    if (absoluteLine < histLines) {
        if (absoluteLine < static_cast<int>(_historyLinks.size()))
            return _historyLinks[absoluteLine];
        return {};
    }
    return _linkLines[absoluteLine - histLines];
}

QString Screen::hyperlinkUri(quint32 linkId) const {
    return _hyperlinkUris.value(linkId);
}

QString Screen::hyperlinkAt(int absoluteLine, int column) const {
    const auto segments = linkSegments(absoluteLine);
    for (const HyperlinkSegment &seg : segments) {
        if (column >= seg.startCol && column <= seg.endCol)
            return _hyperlinkUris.value(seg.linkId);
    }
    return {};
}

void Screen::addHyperlinkSegment(int y, int startCol, int endCol) {
    HyperlinkLine &row = _linkLines[y];
    // 与行尾相邻的同 id 段合并，避免逐字符产生碎段
    if (!row.isEmpty() && row.last().linkId == _currentHyperlinkId
            && row.last().endCol >= startCol - 1) {
        row.last().endCol = qMax(row.last().endCol, endCol);
        return;
    }
    row.append({startCol, endCol, _currentHyperlinkId});
    _hyperlinkRefs[_currentHyperlinkId]++;
}

void Screen::releaseHyperlinkLine(HyperlinkLine &row) {
    for (const HyperlinkSegment &seg : row) {
        auto it = _hyperlinkRefs.find(seg.linkId);
        if (it != _hyperlinkRefs.end() && --it.value() == 0) {
            _hyperlinkRefs.erase(it);
            _hyperlinkUris.remove(seg.linkId);
            // 被回收的若是当前活动链接（应用未发空 URI 关闭），同步复位，
            // 否则后续写入的字符会携带已失效的 linkId，产生无法解析的段
            if (_currentHyperlinkId == seg.linkId)
                _currentHyperlinkId = 0;
            // id 参数映射若仍指向被回收的 linkId，一并移除（映射表很小，线性扫可接受）
            for (auto keyIt = _hyperlinkIds.begin(); keyIt != _hyperlinkIds.end();) {
                if (keyIt.value() == seg.linkId)
                    keyIt = _hyperlinkIds.erase(keyIt);
                else
                    ++keyIt;
            }
        }
    }
    row.clear();
}

void Screen::clearAllHyperlinks() {
    // 整体丢弃（reset 路径），无需逐个维护引用计数
    for (int i = 0; i < lines + 1; i++)
        _linkLines[i].clear();
    _historyLinks.clear();
    _hyperlinkUris.clear();
    _hyperlinkRefs.clear();
    _hyperlinkIds.clear();
    _currentHyperlinkId = 0;
}

void Screen::setCellPixelSize(int width, int height)
{
    _cellPixelWidth = width;
    _cellPixelHeight = height;
}

void Screen::anchorImage(const QImage &image, bool transparentBackground)
{
    if (image.isNull())
        return;
    const qint64 bytes = qint64(image.width()) * image.height() * 4;
    if (_imageBytes + bytes > MAX_IMAGE_BYTES)
        return; // 超累计像素预算：整张静默丢弃（调用方已吞到 ST），光标不动
    const int cellH = _cellPixelHeight > 0 ? _cellPixelHeight : DEFAULT_CELL_PIXEL_HEIGHT;
    const int gridRows = qMax(1, (image.height() + cellH - 1) / cellH);

    const quint32 id = _nextImageHandle++;
    _images.insert(id, ScreenImage {image, transparentBackground});
    _imageRefs.insert(id, 0);
    _imageBytes += bytes;
    _graphicsDirty = true;

    // 逐行放置引用并 index() 下移光标：触底时 index() 触发滚动，已放置引用经
    // moveImage/addHistLine 挂钩随行走——图像随行滚动、滚入历史保留
    for (int i = 0; i < gridRows; i++) {
        ImageRefLine &row = _imageLines[cuY];
        row.append({cuX, id, i});
        _imageRefs[id]++;
        index(); // 不改 cuX：文本光标仅垂直移到图像最后一行之下
    }
}

QVector<ImagePlacement> Screen::imagePlacements(int absoluteLine) const
{
    const int histLines = history->getLines();
    if (absoluteLine < 0 || absoluteLine >= histLines + lines)
        return {};
    if (absoluteLine < histLines) {
        if (absoluteLine < static_cast<int>(_historyImages.size()))
            return _historyImages[absoluteLine];
        return {};
    }
    return _imageLines[absoluteLine - histLines];
}

const ScreenImage *Screen::image(quint32 imageId) const
{
    const auto it = _images.constFind(imageId);
    return it == _images.constEnd() ? nullptr : &it.value();
}

void Screen::releaseImageLine(ImageRefLine &row)
{
    if (row.isEmpty())
        return;
    _graphicsDirty = true; // 引用销毁让图像（部分）消失，字符层无变化，需显示层补刷
    for (const ImagePlacement &p : row) {
        auto it = _imageRefs.find(p.imageId);
        if (it != _imageRefs.end() && --it.value() == 0) {
            _imageRefs.erase(it);
            const auto imgIt = _images.find(p.imageId);
            if (imgIt != _images.end()) {
                _imageBytes -= qint64(imgIt->image.width()) * imgIt->image.height() * 4;
                _images.erase(imgIt);
            }
        }
    }
    row.clear();
}

void Screen::clearAllImages()
{
    // 整体丢弃（reset 路径），无需逐个维护引用计数
    for (int i = 0; i < lines + 1; i++)
        _imageLines[i].clear();
    _historyImages.clear();
    _images.clear();
    _imageRefs.clear();
    _imageBytes = 0;
    // kitty 放置表同步清空（图像数据同表同预算，已由上方清理）
    for (int i = 0; i < lines + 1; i++)
        _kittyLines[i].clear();
    _historyKittyRefs.clear();
    _kittyPlacements.clear();
    _kittyPlacementRefs.clear();
    _kittyPlacementKeys.clear();
    _kittyImageHandles.clear();
    _kittyAnonymous.clear();
    _kittyEvictionOrder.clear();
    _graphicsDirty = true; // 图像消失同样需要补刷
}

quint32 Screen::kittyImageHandle(quint32 clientId) const
{
    if (clientId == 0)
        return 0; // 匿名图像不占 id 命名空间
    const auto it = _kittyImageHandles.constFind(clientId);
    return it == _kittyImageHandles.constEnd() ? 0 : it.value();
}

bool Screen::kittyImageInUse(quint32 imageHandle) const
{
    for (const KittyPlacement &pl : _kittyPlacements)
        if (pl.imageHandle == imageHandle)
            return true;
    return false;
}

void Screen::removeKittyImage(quint32 imageHandle)
{
    const auto it = _images.find(imageHandle);
    if (it == _images.end())
        return;
    _imageBytes -= qint64(it->image.width()) * it->image.height() * 4;
    _images.erase(it);
    _kittyAnonymous.remove(imageHandle);
    _kittyEvictionOrder.removeOne(imageHandle);
    // _kittyImageHandles 的反查清理由调用方负责（kittyDeleteByImage 已知 clientId）
    for (auto hit = _kittyImageHandles.begin(); hit != _kittyImageHandles.end(); ++hit) {
        if (hit.value() == imageHandle) {
            _kittyImageHandles.erase(hit);
            break;
        }
    }
    _graphicsDirty = true;
}

void Screen::evictUnreferencedKittyImages(qint64 bytesNeeded)
{
    // 预算紧张时优先淘汰无放置引用的 kitty 图像（上游建议行为），最旧的先淘汰
    for (int i = 0; i < _kittyEvictionOrder.size() && _imageBytes + bytesNeeded > MAX_IMAGE_BYTES;) {
        const quint32 handle = _kittyEvictionOrder.at(i);
        if (!kittyImageInUse(handle)) {
            removeKittyImage(handle); // 内部 removeOne 保持 i 指向下一元素
        } else {
            i++;
        }
    }
}

void Screen::evictAllUnreferencedKittyImages()
{
    for (int i = 0; i < _kittyEvictionOrder.size();) {
        const quint32 handle = _kittyEvictionOrder.at(i);
        if (!kittyImageInUse(handle))
            removeKittyImage(handle); // 内部 removeOne 保持 i 指向下一元素
        else
            i++;
    }
}

bool Screen::kittyStoreImage(const QImage &image, quint32 clientId, quint32 *handleOut)
{
    if (image.isNull())
        return false;
    const qint64 bytes = qint64(image.width()) * image.height() * 4;
    if (_imageBytes + bytes > MAX_IMAGE_BYTES)
        evictUnreferencedKittyImages(bytes);
    if (_imageBytes + bytes > MAX_IMAGE_BYTES)
        return false; // 淘汰后仍超限：失败（调用方回 ENOSPC）
    const quint32 handle = _nextImageHandle++;
    _images.insert(handle, ScreenImage {image, false});
    _imageBytes += bytes;
    if (clientId != 0)
        _kittyImageHandles.insert(clientId, handle);
    else
        _kittyAnonymous.insert(handle);
    _kittyEvictionOrder.append(handle);
    if (handleOut)
        *handleOut = handle;
    return true;
}

KittyPlaceError Screen::kittyPlace(quint32 imageHandle, quint32 clientId,
                                   const KittyPlacementParams &params,
                                   quint32 *placementHandleOut, int *colsUsed, int *rowsUsed)
{
    const auto imgIt = _images.constFind(imageHandle);
    if (imgIt == _images.constEnd())
        return KittyPlaceError::NoSuchImage;
    const QImage &img = imgIt->image;
    const int cellW = _cellPixelWidth > 0 ? _cellPixelWidth : DEFAULT_CELL_PIXEL_WIDTH;
    const int cellH = _cellPixelHeight > 0 ? _cellPixelHeight : DEFAULT_CELL_PIXEL_HEIGHT;
    // X/Y 必须小于单元格尺寸（协议约束）
    if (params.cellXOff < 0 || params.cellXOff >= cellW
            || params.cellYOff < 0 || params.cellYOff >= cellH)
        return KittyPlaceError::InvalidArgument;
    // 源矩形：缺省整图，与源图取交；取交为空则非法
    const QRect src = QRect(params.srcX, params.srcY,
                            params.srcW > 0 ? params.srcW : img.width(),
                            params.srcH > 0 ? params.srcH : img.height()) & img.rect();
    if (src.isEmpty())
        return KittyPlaceError::InvalidArgument;
    // 显示区：缺省按源矩形原始尺寸换算单元格数（向上取整）；只给一个按宽高比推算
    int cols = params.cols;
    int rows = params.rows;
    if (cols <= 0 && rows <= 0) {
        cols = qMax(1, (src.width() + cellW - 1) / cellW);
        rows = qMax(1, (src.height() + cellH - 1) / cellH);
    } else if (cols <= 0) {
        cols = qMax(1, int((qint64(src.width()) * rows * cellW + qint64(src.height()) * cellH - 1)
                           / (qint64(src.height()) * cellH)));
    } else if (rows <= 0) {
        rows = qMax(1, int((qint64(src.height()) * cols * cellH + qint64(src.width()) * cellW - 1)
                           / (qint64(src.width()) * cellW)));
    }

    // 同 (i≠0, p≠0) 重复放置 = 替换（可无闪烁移动/缩放）：先删旧放置
    if (clientId != 0 && params.placementId != 0) {
        const quint64 key = (quint64(clientId) << 32) | params.placementId;
        const auto it = _kittyPlacementKeys.constFind(key);
        if (it != _kittyPlacementKeys.constEnd())
            removeKittyPlacement(it.value());
    }

    KittyPlacement pl;
    pl.imageHandle = imageHandle;
    pl.imageId = clientId;
    pl.placementId = params.placementId;
    pl.anchorLine = history->getLines() + cuY;
    pl.col = cuX;
    pl.cols = cols;
    pl.rows = rows;
    pl.srcX = src.x();
    pl.srcY = src.y();
    pl.srcW = src.width();
    pl.srcH = src.height();
    pl.cellXOff = params.cellXOff;
    pl.cellYOff = params.cellYOff;
    pl.zIndex = params.zIndex;
    pl.serial = _nextKittySerial++;

    const quint32 handle = _nextKittyPlacementHandle++;
    _kittyPlacements.insert(handle, pl);
    _kittyPlacementRefs.insert(handle, 0);
    if (clientId != 0 && params.placementId != 0)
        _kittyPlacementKeys.insert((quint64(clientId) << 32) | params.placementId, handle);

    // 行级引用挂在放置覆盖的每一行（越下缘截断；滚动/清行/resize/复位由共享挂钩管理）
    for (int i = 0; i < rows && cuY + i < lines; i++) {
        _kittyLines[cuY + i].append({handle, i});
        _kittyPlacementRefs[handle]++;
    }
    _graphicsDirty = true;
    if (placementHandleOut)
        *placementHandleOut = handle;
    if (colsUsed)
        *colsUsed = cols;
    if (rowsUsed)
        *rowsUsed = rows;
    return KittyPlaceError::Ok;
}

void Screen::releaseKittyRefLine(KittyRefLine &row)
{
    if (row.isEmpty())
        return;
    _graphicsDirty = true; // 放置（部分）消失，字符层无变化，需显示层补刷
    for (const KittyPlacementRef &ref : row) {
        auto it = _kittyPlacementRefs.find(ref.placementHandle);
        if (it == _kittyPlacementRefs.end())
            continue;
        if (--it.value() > 0)
            continue;
        // 全部行引用销毁：回收放置；匿名图像随最后放置死亡释放
        _kittyPlacementRefs.erase(it);
        const auto plIt = _kittyPlacements.find(ref.placementHandle);
        if (plIt == _kittyPlacements.end())
            continue;
        const quint32 imageHandle = plIt->imageHandle;
        const quint64 key = (quint64(plIt->imageId) << 32) | plIt->placementId;
        _kittyPlacements.erase(plIt);
        _kittyPlacementKeys.remove(key);
        if (_kittyAnonymous.contains(imageHandle) && !kittyImageInUse(imageHandle))
            removeKittyImage(imageHandle);
    }
    row.clear();
}

void Screen::removeKittyPlacement(quint32 placementHandle)
{
    // 显式删除：从全部行（屏幕 + 回看历史）剥离该放置的引用。
    // 引用随滚动迁移（moveImage/addHistLine 挂钩），故只能全表扫描；删除为低频操作
    for (int i = 0; i < lines + 1; i++) {
        KittyRefLine &row = _kittyLines[i];
        for (int j = row.size() - 1; j >= 0; j--) {
            if (row[j].placementHandle == placementHandle) {
                KittyPlacementRef ref = row.takeAt(j);
                auto it = _kittyPlacementRefs.find(ref.placementHandle);
                if (it != _kittyPlacementRefs.end() && --it.value() <= 0)
                    _kittyPlacementRefs.erase(it);
            }
        }
    }
    for (KittyRefLine &row : _historyKittyRefs) {
        for (int j = row.size() - 1; j >= 0; j--) {
            if (row[j].placementHandle == placementHandle) {
                KittyPlacementRef ref = row.takeAt(j);
                auto it = _kittyPlacementRefs.find(ref.placementHandle);
                if (it != _kittyPlacementRefs.end() && --it.value() <= 0)
                    _kittyPlacementRefs.erase(it);
            }
        }
    }
    const auto plIt = _kittyPlacements.find(placementHandle);
    if (plIt == _kittyPlacements.end())
        return;
    const quint32 imageHandle = plIt->imageHandle;
    const quint64 key = (quint64(plIt->imageId) << 32) | plIt->placementId;
    _kittyPlacements.erase(plIt);
    _kittyPlacementKeys.remove(key);
    _graphicsDirty = true;
    // 匿名图像（i=0）无其他引用时释放；命名图像数据由 d 大写变体/重传/淘汰管理
    if (_kittyAnonymous.contains(imageHandle) && !kittyImageInUse(imageHandle))
        removeKittyImage(imageHandle);
}

void Screen::kittyDeleteAll(bool freeData)
{
    // d=a/A 只删除"屏幕上可见"的放置（上游协议原文：Delete all placements visible
    // on screen）：收集屏幕行范围（_kittyLines[0..lines-1]）内被引用的放置句柄；
    // 锚定在回看历史行的放置保留（removeKittyPlacement 内部维护引用计数）
    QList<quint32> handles;
    for (int i = 0; i < lines; i++)
        for (const KittyPlacementRef &ref : _kittyLines[i])
            if (!handles.contains(ref.placementHandle))
                handles.append(ref.placementHandle);
    for (const quint32 handle : handles)
        removeKittyPlacement(handle);
    if (freeData) {
        // 大写 A：连同释放无引用图像数据；仍被他处放置（如回看历史）引用的图像保留
        // （kittyImageInUse 扫全放置表，历史放置存活即视为使用中）
        const auto clientIds = _kittyImageHandles.keys();
        for (const quint32 clientId : clientIds) {
            const quint32 handle = _kittyImageHandles.value(clientId);
            if (!kittyImageInUse(handle))
                removeKittyImage(handle);
        }
        const auto anonymous = _kittyAnonymous.values();
        for (const quint32 handle : anonymous)
            if (!kittyImageInUse(handle))
                removeKittyImage(handle);
    }
}

void Screen::kittyDeleteByImage(quint32 clientId, quint32 placementId, bool freeData)
{
    const quint32 imageHandle = kittyImageHandle(clientId);
    if (imageHandle == 0)
        return;
    if (placementId != 0) {
        const quint64 key = (quint64(clientId) << 32) | placementId;
        const auto it = _kittyPlacementKeys.constFind(key);
        if (it != _kittyPlacementKeys.constEnd())
            removeKittyPlacement(it.value());
    } else {
        // 删除该图像全部放置（含匿名放置；先收集句柄避免迭代中改表）
        QList<quint32> handles;
        for (auto it = _kittyPlacements.constBegin(); it != _kittyPlacements.constEnd(); ++it)
            if (it->imageId == clientId)
                handles.append(it.key());
        for (const quint32 handle : handles)
            removeKittyPlacement(handle);
    }
    // 大写 I：连同释放无其他引用（含回看历史中的引用）的图像数据
    if (freeData && !kittyImageInUse(imageHandle))
        removeKittyImage(imageHandle);
}

void Screen::kittyDeleteAtCursor(bool freeData)
{
    const KittyRefLine row = _kittyLines[cuY]; // 副本：删除过程会改原行
    QList<quint32> imageHandles;
    for (const KittyPlacementRef &ref : row) {
        const KittyPlacement *pl = kittyPlacement(ref.placementHandle);
        if (!pl)
            continue;
        // 与光标单元格相交：行匹配（引用在本行即匹配），列落在放置覆盖区间内
        if (cuX >= pl->col && cuX < pl->col + pl->cols) {
            imageHandles.append(pl->imageHandle);
            removeKittyPlacement(ref.placementHandle);
        }
    }
    if (freeData)
        for (const quint32 imageHandle : imageHandles)
            if (!kittyImageInUse(imageHandle))
                removeKittyImage(imageHandle);
}

QVector<KittyPlacementRef> Screen::kittyRefs(int absoluteLine) const
{
    const int histLines = history->getLines();
    if (absoluteLine < 0 || absoluteLine >= histLines + lines)
        return {};
    if (absoluteLine < histLines) {
        if (absoluteLine < static_cast<int>(_historyKittyRefs.size()))
            return _historyKittyRefs[absoluteLine];
        return {};
    }
    return _kittyLines[absoluteLine - histLines];
}

const KittyPlacement *Screen::kittyPlacement(quint32 placementHandle) const
{
    const auto it = _kittyPlacements.constFind(placementHandle);
    return it == _kittyPlacements.constEnd() ? nullptr : &it.value();
}

int Screen::getHistLines() const { return history->getLines(); }

void Screen::setScroll(const HistoryType &t, bool copyPreviousScroll) {
    clearSelection();

    if (copyPreviousScroll)
        history = t.scroll(history);
    else {
        HistoryScroll *oldScroll = history;
        history = t.scroll(nullptr);
        delete oldScroll;
        // 历史整体废弃（clearHistory）：同步丢弃历史链接段表
        for (HyperlinkLine &row : _historyLinks)
            releaseHyperlinkLine(row);
        _historyLinks.clear();
        // 历史整体废弃（clearHistory）：同步销毁历史图像引用
        for (ImageRefLine &row : _historyImages)
            releaseImageLine(row);
        _historyImages.clear();
        // 历史整体废弃（clearHistory）：同步销毁历史 kitty 放置引用
        for (KittyRefLine &row : _historyKittyRefs)
            releaseKittyRefLine(row);
        _historyKittyRefs.clear();
    }
}

bool Screen::hasScroll() const { return history->hasScroll(); }

const HistoryType &Screen::getScroll() const { return history->getType(); }

void Screen::setLineProperty(LineProperty property, bool enable) {
    if (enable)
        lineProperties[cuY] = static_cast<LineProperty>(lineProperties[cuY] | property);
    else
        lineProperties[cuY] = static_cast<LineProperty>(lineProperties[cuY] & ~property);
}

void Screen::fillWithDefaultChar(Character *dest, int count) {
    for (int i = 0; i < count; i++)
        dest[i] = defaultChar;
}
