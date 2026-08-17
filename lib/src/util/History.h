/*
 This file is part of Konsole, an X terminal.
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
#ifndef HISTORY_H
#define HISTORY_H

#include <deque>

#include <QBitRef>
#include <QHash>
#include <QVector>
#include <QTemporaryFile>

#include "Character.h"

/*
   An extendable tmpfile(1) based buffer.
*/

/**
 * @brief 基于临时文件的可扩展行存储（Row(X)），仅支持尾部追加与随机读取。
 * @note 移植自上游 Konsole；读写操作通过 QTemporaryFile 完成，跨平台。
 */
class HistoryFile
{
public:
    HistoryFile();
    virtual ~HistoryFile();

    virtual void add(const unsigned char* bytes, int len);
    virtual void get(unsigned char* bytes, int len, int loc);
    virtual int  len() const;

    //mmaps the file in read-only mode
    void map();
    //un-mmaps the file
    void unmap();
    //returns true if the file is mmap'ed
    bool isMapped() const;

private:
    int  length;
    QTemporaryFile tmpFile;

    //pointer to start of mmap'ed file data, or 0 if the file is not mmap'ed
    char* fileMap;

    //incremented whenever 'add' is called and decremented whenever
    //'get' is called.
    //this is used to detect when a large number of lines are being read and processed from the history
    //and automatically mmap the file for better performance (saves the overhead of many seek-read calls).
    int readWriteBalance = 0;

    //when readWriteBalance goes below this threshold, the file will be mmap'ed automatically
    static const int MAP_THRESHOLD = -1000;
};

//////////////////////////////////////////////////////////////////////
// Abstract base class for file and buffer versions
//////////////////////////////////////////////////////////////////////
class HistoryType;
class HistoryScroll
{
public:
    HistoryScroll(HistoryType*);
    virtual ~HistoryScroll();

    virtual bool hasScroll();

    // access to history
    virtual int  getLines() = 0;
    virtual int  getLineLen(int lineno) = 0;
    virtual void getCells(int lineno, int colno, int count, Character res[]) = 0;
    virtual bool isWrappedLine(int lineno) = 0;

    // backward compatibility (obsolete)
    Character   getCell(int lineno, int colno) { Character res; getCells(lineno,colno,1,&res); return res; }

    // adding lines.
    virtual void addCells(const Character a[], int count) = 0;
    // convenience method - this is virtual so that subclasses can take advantage
    // of QVector's implicit copying
    virtual void addCellsVector(const QVector<Character>& cells) {
        addCells(cells.data(),cells.size());
    }

    virtual void addLine(bool previousWrapped=false) = 0;

    /**
     * @brief 在滚动缓冲头部前插更老的历史行（外部历史读回注入通道，旧→新顺序）。
     * @param lines 行数组，每行为一个 Character 序列（char32_t 管线）。
     * @param wrappedFlags 与 lines 等长的折行标志（LINE_WRAPPED 语义同 addLine）。
     * @return 实际前插的行数；不支持的滚动类型返回 0。
     * @note 基类默认不支持前插；HistoryScrollBuffer 以独立前插区实现。
     *       HistoryScrollFile 为无限历史（行不会离开内存），无读回场景，不支持前插。
     */
    virtual int prependLines(const QVector<QVector<Character>> &lines,
                             const QVector<bool> &wrappedFlags);

    /**
     * @brief 前插区（读回注入）当前行数。
     * @return 前插区行数；不支持前插的滚动类型恒为 0。
     * @note 供上层（Screen 平行表/基线记账）在缩容等路径判别前端丢弃量与
     *       前插区是否清空（如 setMaxNbLines 缩容弹出前插区最老行）。
     */
    virtual int prependedLineCount() const { return 0; }

    /**
     * @brief 最近一次 addCellsVector/addCells 因满员覆盖所丢弃行的整体行索引（含前插区偏移）。
     * @return 被丢弃行的整体索引；本次未发生丢弃或不支持时返回 -1。
     * @note 供上层（Screen 平行表）精确定位被丢弃行：HistoryScrollBuffer 满员覆盖时
     *       丢弃的是环形区最老行（整体索引 = 前插区行数，前插区非空时位于中部而非表头）；
     *       文件型历史不丢行，恒为 -1。
     */
    virtual int lastDroppedLineIndex() const { return -1; }

    //
    // FIXME:  Passing around constant references to HistoryType instances
    // is very unsafe, because those references will no longer
    // be valid if the history scroll is deleted.
    //
    const HistoryType& getType() const { return *m_histType; }

protected:
    HistoryType* m_histType;
};

//////////////////////////////////////////////////////////////////////
// File-based history (e.g. file log, no limitation in length)
//////////////////////////////////////////////////////////////////////

/**
 * @brief 基于临时文件的历史滚动缓冲，行数无限制（无限历史）。
 */
class HistoryScrollFile : public HistoryScroll
{
public:
    HistoryScrollFile(const QString &logFileName);
    ~HistoryScrollFile() override;

    int  getLines() override;
    int  getLineLen(int lineno) override;
    void getCells(int lineno, int colno, int count, Character res[]) override;
    bool isWrappedLine(int lineno) override;

    void addCells(const Character a[], int count) override;
    void addLine(bool previousWrapped=false) override;

private:
    int startOfLine(int lineno);

    QString m_logFileName;
    HistoryFile index;     // lines Row(int)
    HistoryFile cells;     // text  Row(Character)
    HistoryFile lineflags; // flags Row(unsigned char)
};

class HistoryScrollBuffer : public HistoryScroll
{
public:
    typedef QVector<Character> HistoryLine;

    HistoryScrollBuffer(unsigned int maxNbLines = 1000);
    ~HistoryScrollBuffer() override;

    int  getLines() override;
    int  getLineLen(int lineno) override;
    void getCells(int lineno, int colno, int count, Character res[]) override;
    bool isWrappedLine(int lineno) override;

    void addCells(const Character a[], int count) override;
    void addCellsVector(const QVector<Character>& cells) override;
    void addLine(bool previousWrapped=false) override;

    int prependLines(const QVector<QVector<Character>> &lines,
                     const QVector<bool> &wrappedFlags) override;

    int lastDroppedLineIndex() const override { return _lastDroppedLineIndex; }
    int prependedLineCount() const override { return static_cast<int>(_prepended.size()); }

    void setMaxNbLines(unsigned int nbLines);
    unsigned int maxNbLines() const { return _maxLineCount; }

private:
    int bufferIndex(int lineNumber) const;

    HistoryLine* _historyBuffer;
    QBitArray _wrappedLine;
    int _maxLineCount;
    int _usedLines;
    int _head;
    /** @brief 最近一次满员覆盖丢弃行的整体索引（= 前插区行数）；未丢行为 -1。 */
    int _lastDroppedLineIndex = -1;

    // 历史读回前插区：逻辑上位于环形区之前，front 为最老行；
    // 容量独立于环形区、上限同为 _maxLineCount（总内存占用 ≤2× 上限，规格 §5.1 内存有界）
    std::deque<HistoryLine> _prepended;   ///< 前插区行数据（旧→新）
    std::deque<bool> _prependedWrapped;   ///< 前插区折行标志（与 _prepended 一一对应）
};

class HistoryScrollNone : public HistoryScroll
{
public:
    HistoryScrollNone();
    ~HistoryScrollNone() override;

    bool hasScroll() override;

    int  getLines() override;
    int  getLineLen(int lineno) override;
    void getCells(int lineno, int colno, int count, Character res[]) override;
    bool isWrappedLine(int lineno) override;

    void addCells(const Character a[], int count) override;
    void addLine(bool previousWrapped=false) override;
};


typedef QVector<Character> TextLine;

class CharacterFormat
{
public:
    bool equalsFormat(const CharacterFormat &other) const {
        return other.rendition==rendition && other.fgColor==fgColor && other.bgColor==bgColor;
    }

    bool equalsFormat(const Character &c) const {
        return c.rendition==rendition && c.foregroundColor==fgColor && c.backgroundColor==bgColor;
    }

    void setFormat(const Character& c) {
        rendition=c.rendition;
        fgColor=c.foregroundColor;
        bgColor=c.backgroundColor;
    }

    CharacterColor fgColor, bgColor;
    quint16 startPos;
    quint16 rendition;
};

class HistoryType
{
public:
    HistoryType();
    virtual ~HistoryType();

    /**
     * Returns true if the history is enabled ( can store lines of output )
     * or false otherwise.
     */
    virtual bool isEnabled()           const = 0;
    /**
     * Returns true if the history size is unlimited.
     */
    bool isUnlimited() const { return maximumLineCount() == 0; }
    /**
     * Returns the maximum number of lines which this history type
     * can store or 0 if the history can store an unlimited number of lines.
     */
    virtual int maximumLineCount()    const = 0;

    virtual HistoryScroll* scroll(HistoryScroll *) const = 0;
};

class HistoryTypeNone : public HistoryType
{
public:
    HistoryTypeNone();

    bool isEnabled() const override;
    int maximumLineCount() const override;

    HistoryScroll* scroll(HistoryScroll *) const override;
};

/**
 * @brief 基于临时文件的历史类型，行数无限制（用于无限历史，对齐上游）。
 */
class HistoryTypeFile : public HistoryType
{
public:
    HistoryTypeFile(const QString& fileName = QString());

    bool isEnabled() const override;
    virtual const QString& getFileName() const;
    int maximumLineCount() const override;

    HistoryScroll* scroll(HistoryScroll *) const override;

protected:
    QString m_fileName;
};

class HistoryTypeBuffer : public HistoryType
{
    friend class HistoryScrollBuffer;

public:
    HistoryTypeBuffer(unsigned int nbLines);

    bool isEnabled() const override;
    int maximumLineCount() const override;

    HistoryScroll* scroll(HistoryScroll *) const override;

protected:
  unsigned int m_nbLines;
};

#endif // HISTORY_H
