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

#include "History.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <cerrno>

#include <QtDebug>

// Reasonable line size
#define LINE_SIZE 1024

/*
 An arbitrary long scroll.

 One can modify the scroll only by adding either cells
 or newlines, but access it randomly.

 The model is that of an arbitrary wide typewriter scroll
 in that the scroll is a serie of lines and each line is
 a serie of cells with no overwriting permitted.

 The implementation provides arbitrary length and numbers
 of cells and line/column indexed read access to the scroll
 at constant costs.

 KDE4: Can we use QTemporaryFile here, instead of KTempFile?

 FIXME: some complain about the history buffer consuming the
        memory of their machines. This problem is critical
        since the history does not behave gracefully in cases
        where the memory is used up completely.

        I put in a workaround that should handle it problem
        now gracefully. I'm not satisfied with the solution.

 FIXME: Terminating the history is not properly indicated
        in the menu. We should throw a signal.

 FIXME: There is noticeable decrease in speed, also. Perhaps,
        there whole feature needs to be revisited therefore.
        Disadvantage of a more elaborated, say block-oriented
        scheme with wrap around would be it's complexity.
*/

/*
  A Row(X) data type which allows adding elements to the end.
*/

// History File ///////////////////////////////////////////

/**
 * @brief 构造基于 QTemporaryFile 的行存储。
 * @note 与上游的 POSIX mmap/lseek 实现不同，此处统一走 QFile 接口以保证跨平台。
 */
HistoryFile::HistoryFile()
  : length(0),
    fileMap(nullptr)
{
  if (tmpFile.open())
  {
    tmpFile.setAutoRemove(true);
  }
}

HistoryFile::~HistoryFile()
{
    if (fileMap)
        unmap();
}

//TODO:  Mapping the entire file in will cause problems if the history file becomes exceedingly large,
//(ie. larger than available memory).  HistoryFile::map() should only map in sections of the file at a time,
//to avoid this.
void HistoryFile::map()
{
    Q_ASSERT( fileMap == nullptr );

    fileMap = reinterpret_cast<char*>(tmpFile.map(0, length));

    //if mmap'ing fails, fall back to the seek-read combination
    if ( fileMap == nullptr )
    {
            readWriteBalance = 0;
    }
}

void HistoryFile::unmap()
{
    bool result = tmpFile.unmap( reinterpret_cast<uchar*>(fileMap) );
    Q_ASSERT( result ); Q_UNUSED( result )

    fileMap = nullptr;
}

bool HistoryFile::isMapped() const
{
    return (fileMap != nullptr);
}

void HistoryFile::add(const unsigned char* bytes, int len)
{
  if ( fileMap )
          unmap();

  readWriteBalance++;

  if (!tmpFile.seek(length)) { qWarning("HistoryFile::add.seek failed"); return; }
  qint64 rc = tmpFile.write(reinterpret_cast<const char*>(bytes), len);
  if (rc < 0) { qWarning("HistoryFile::add.write failed"); return; }
  length += static_cast<int>(rc);
}

void HistoryFile::get(unsigned char* bytes, int len, int loc)
{
  //count number of get() calls vs. number of add() calls.
  //If there are many more get() calls compared with add()
  //calls (decided by using MAP_THRESHOLD) then mmap the log
  //file to improve performance.
  readWriteBalance--;
  if ( !fileMap && readWriteBalance < MAP_THRESHOLD )
          map();

  if ( fileMap )
  {
    for (int i=0;i<len;i++)
            bytes[i]=fileMap[loc+i];
  }
  else
  {
      if (loc < 0 || len < 0 || loc + len > length)
        fprintf(stderr,"getHist(...,%d,%d): invalid args.\n",len,loc);
      if (!tmpFile.seek(loc)) { qWarning("HistoryFile::get.seek failed"); return; }
      qint64 rc = tmpFile.read(reinterpret_cast<char*>(bytes), len);
      // 短读（或读失败）时将未覆盖的尾部清零并告警，避免上层使用未初始化数据
      if (rc < len)
      {
        if (rc < 0) rc = 0;
        std::fill_n(bytes + rc, len - rc, static_cast<unsigned char>(0));
        qWarning("HistoryFile::get.read short: expected %d bytes, got %lld",
                 len, static_cast<long long>(rc));
      }
  }
}

int HistoryFile::len() const
{
  return length;
}


// History Scroll abstract base class //////////////////////////////////////


HistoryScroll::HistoryScroll(HistoryType *t) : m_histType(t) {
}

HistoryScroll::~HistoryScroll() { 
    delete m_histType; 
}

bool HistoryScroll::hasScroll() { 
    return true; 
}

int HistoryScroll::prependLines(const QVector<QVector<Character>> &,
                                const QVector<bool> &) {
    return 0; // 默认不支持前插
}

// File-based history (e.g. file log, no limitation in length) ///////////////////

/*
  The history can be seen as an array of lines. Each line
  is an array of cells. The index addresses the start of
  the lines in the cells buffer.

  Note that index[0] addresses the second line
  (line #1), while the first line (line #0) starts
  at 0 in cells.
*/

HistoryScrollFile::HistoryScrollFile(const QString &logFileName)
  : HistoryScroll(new HistoryTypeFile(logFileName)),
  m_logFileName(logFileName)
{
}

HistoryScrollFile::~HistoryScrollFile()
{
}

int HistoryScrollFile::getLines()
{
  return index.len() / sizeof(int);
}

int HistoryScrollFile::getLineLen(int lineno)
{
  return (startOfLine(lineno+1) - startOfLine(lineno)) / sizeof(Character);
}

bool HistoryScrollFile::isWrappedLine(int lineno)
{
  // 有效行号范围为 [0, getLines())，上游遗留的 <= 会越界读取 lineflags
  if (lineno>=0 && lineno < getLines()) {
    unsigned char flag;
    lineflags.get((unsigned char*)&flag,sizeof(unsigned char),(lineno)*sizeof(unsigned char));
    return flag;
  }
  return false;
}

int HistoryScrollFile::startOfLine(int lineno)
{
  if (lineno <= 0) return 0;
  if (lineno <= getLines())
    {

    if (!index.isMapped())
            index.map();

    int res = 0;
    index.get((unsigned char*)&res,sizeof(int),(lineno-1)*sizeof(int));
    return res;
    }
  return cells.len();
}

void HistoryScrollFile::getCells(int lineno, int colno, int count, Character res[])
{
  cells.get((unsigned char*)res,count*sizeof(Character),startOfLine(lineno)+colno*sizeof(Character));
}

void HistoryScrollFile::addCells(const Character text[], int count)
{
  cells.add((unsigned char*)text,count*sizeof(Character));
}

void HistoryScrollFile::addLine(bool previousWrapped)
{
  if (index.isMapped())
          index.unmap();

  int locn = cells.len();
  index.add((unsigned char*)&locn,sizeof(int));
  unsigned char flags = previousWrapped ? 0x01 : 0x00;
  lineflags.add((unsigned char*)&flags,sizeof(unsigned char));
}


// Buffer-based history (limited to a fixed nb of lines) ////////////////////////

HistoryScrollBuffer::HistoryScrollBuffer(unsigned int maxLineCount)
    : HistoryScroll(new HistoryTypeBuffer(maxLineCount)), _historyBuffer(),
      _maxLineCount(0), _usedLines(0), _head(0) {
    setMaxNbLines(maxLineCount);
}

HistoryScrollBuffer::~HistoryScrollBuffer() { 
    delete[] _historyBuffer; 
}

void HistoryScrollBuffer::addCellsVector(const QVector<Character> &cells) {
    _head++;
    if (_usedLines < _maxLineCount) {
        _usedLines++;
        _lastDroppedLineIndex = -1; // 未满员：本次无丢弃
    } else {
        // 满员覆盖：被丢弃的是环形区最老行，整体索引 = 前插区行数
        //（前插区非空时位于中部而非表头，上层平行表须按此索引弹出）
        _lastDroppedLineIndex = static_cast<int>(_prepended.size());
    }

    if (_head >= _maxLineCount) {
        _head = 0;
    }

    _historyBuffer[bufferIndex(_usedLines - 1)] = cells;
    _wrappedLine[bufferIndex(_usedLines - 1)] = false;
}

void HistoryScrollBuffer::addCells(const Character a[], int count) {
    HistoryLine newLine(count);
    std::copy(a, a + count, newLine.begin());

    addCellsVector(newLine);
}

void HistoryScrollBuffer::addLine(bool previousWrapped) {
    _wrappedLine[bufferIndex(_usedLines - 1)] = previousWrapped;
}

int HistoryScrollBuffer::prependLines(const QVector<QVector<Character>> &lines,
                                      const QVector<bool> &wrappedFlags) {
    // 防御：lines 与 wrappedFlags 不等长时按较短者截断（上层 Screen 已截断，此处兜底）
    const int count = qMin(lines.size(), wrappedFlags.size());

    // 剩余容量不足时保留输入中较新的行（紧邻既有历史，不产生索引空洞）；
    // 被丢弃的最老行由上层日志引擎兜底持有，用户再次越顶时可重新读回
    const int room = qMax(0, _maxLineCount - static_cast<int>(_prepended.size()));
    const int n = qMin(count, room);
    const int skip = count - n; // 输入中最老的 skip 行不入缓冲
    for (int i = count - 1; i >= skip; i--) {
        _prepended.push_front(lines[i]);
        _prependedWrapped.push_front(wrappedFlags[i]);
    }
    return n;
}

int HistoryScrollBuffer::getLines() {
    return static_cast<int>(_prepended.size()) + _usedLines;
}

int HistoryScrollBuffer::getLineLen(int lineNumber) {
    Q_ASSERT(lineNumber >= 0 && lineNumber < getLines());

    if (lineNumber < static_cast<int>(_prepended.size()))
        return _prepended[lineNumber].size();

    lineNumber -= static_cast<int>(_prepended.size());
    if (lineNumber < _usedLines) {
        return _historyBuffer[bufferIndex(lineNumber)].size();
    } else {
        return 0;
    }
}

bool HistoryScrollBuffer::isWrappedLine(int lineNumber) {
    Q_ASSERT(lineNumber >= 0 && lineNumber < getLines());

    if (lineNumber < static_cast<int>(_prepended.size()))
        return _prependedWrapped[lineNumber];

    lineNumber -= static_cast<int>(_prepended.size());
    if (lineNumber < _usedLines) {
        return _wrappedLine[bufferIndex(lineNumber)];
    } else
        return false;
}

void HistoryScrollBuffer::getCells(int lineNumber, int startColumn, int count,
                                   Character buffer[]) {
    if (count == 0)
        return;

    Q_ASSERT(lineNumber < getLines());

    if (lineNumber < static_cast<int>(_prepended.size())) {
        const HistoryLine &line = _prepended[lineNumber];
        Q_ASSERT(startColumn <= line.size() - count);
        memcpy(buffer, line.constData() + startColumn, count * sizeof(Character));
        return;
    }

    lineNumber -= static_cast<int>(_prepended.size());

    if (lineNumber >= _usedLines) {
        memset(static_cast<void *>(buffer), 0, count * sizeof(Character));
        return;
    }

    const HistoryLine &line = _historyBuffer[bufferIndex(lineNumber)];

    Q_ASSERT(startColumn <= line.size() - count);

    memcpy(buffer, line.constData() + startColumn, count * sizeof(Character));
}

void HistoryScrollBuffer::setMaxNbLines(unsigned int lineCount) {
    HistoryLine *oldBuffer = _historyBuffer;
    HistoryLine *newBuffer = new HistoryLine[lineCount];

    for (int i = 0; i < qMin(_usedLines, (int)lineCount); i++) {
        newBuffer[i] = oldBuffer[bufferIndex(i)];
    }

    _usedLines = qMin(_usedLines, (int)lineCount);
    _maxLineCount = lineCount;
    _head = (_usedLines == _maxLineCount) ? 0 : _usedLines - 1;

    _historyBuffer = newBuffer;
    delete[] oldBuffer;

    _wrappedLine.resize(lineCount);
    dynamic_cast<HistoryTypeBuffer *>(m_histType)->m_nbLines = lineCount;

    // 前插区容量同步收敛到新上限（丢弃最老的前插行，上层日志引擎兜底可再读回）
    while (static_cast<int>(_prepended.size()) > static_cast<int>(lineCount)) {
        _prepended.pop_front();
        _prependedWrapped.pop_front();
    }
    _lastDroppedLineIndex = -1; // 容量调整路径的丢弃不经 addCellsVector 记账，复位防误读
}

int HistoryScrollBuffer::bufferIndex(int lineNumber) const {
    Q_ASSERT(lineNumber >= 0);
    Q_ASSERT(lineNumber < _maxLineCount);
    Q_ASSERT((_usedLines == _maxLineCount) || lineNumber <= _head);

    if (_usedLines == _maxLineCount) {
        return (_head + lineNumber + 1) % _maxLineCount;
    } else {
        return lineNumber;
    }
}

HistoryScrollNone::HistoryScrollNone() : HistoryScroll(new HistoryTypeNone()) {}
HistoryScrollNone::~HistoryScrollNone() {}
bool HistoryScrollNone::hasScroll() { return false; }
int HistoryScrollNone::getLines() { return 0; }
int HistoryScrollNone::getLineLen(int) { return 0; }
bool HistoryScrollNone::isWrappedLine(int /*lineno*/) { return false; }
void HistoryScrollNone::getCells(int, int, int, Character[]) {}
void HistoryScrollNone::addCells(const Character[], int) {}
void HistoryScrollNone::addLine(bool) {}

HistoryType::HistoryType() {}
HistoryType::~HistoryType() {}
HistoryTypeNone::HistoryTypeNone() {}
bool HistoryTypeNone::isEnabled() const { return false; }
HistoryScroll *HistoryTypeNone::scroll(HistoryScroll *old) const {
    delete old;
    return new HistoryScrollNone();
}
int HistoryTypeNone::maximumLineCount() const { return 0; }

HistoryTypeFile::HistoryTypeFile(const QString& fileName)
  : m_fileName(fileName)
{
}

bool HistoryTypeFile::isEnabled() const
{
  return true;
}

const QString& HistoryTypeFile::getFileName() const
{
  return m_fileName;
}

HistoryScroll* HistoryTypeFile::scroll(HistoryScroll *old) const
{
  if (dynamic_cast<HistoryScrollFile *>(old))
     return old; // Unchanged.

  HistoryScroll *newScroll = new HistoryScrollFile(m_fileName);

  Character line[LINE_SIZE];
  int lines = (old != nullptr) ? old->getLines() : 0;
  for(int i = 0; i < lines; i++)
  {
     int size = old->getLineLen(i);
     if (size > LINE_SIZE)
     {
        Character *tmp_line = new Character[size];
        old->getCells(i, 0, size, tmp_line);
        newScroll->addCells(tmp_line, size);
        newScroll->addLine(old->isWrappedLine(i));
        delete [] tmp_line;
     }
     else
     {
        old->getCells(i, 0, size, line);
        newScroll->addCells(line, size);
        newScroll->addLine(old->isWrappedLine(i));
     }
  }

  delete old;
  return newScroll;
}

int HistoryTypeFile::maximumLineCount() const
{
  return 0;
}

HistoryTypeBuffer::HistoryTypeBuffer(unsigned int nbLines)
    : m_nbLines(nbLines) {}
bool HistoryTypeBuffer::isEnabled() const { return true; }
int HistoryTypeBuffer::maximumLineCount() const { return m_nbLines; }
HistoryScroll *HistoryTypeBuffer::scroll(HistoryScroll *old) const {
    if (old) {
        HistoryScrollBuffer *oldBuffer = dynamic_cast<HistoryScrollBuffer *>(old);
        if (oldBuffer) {
            oldBuffer->setMaxNbLines(m_nbLines);
            return oldBuffer;
        }

        HistoryScroll *newScroll = new HistoryScrollBuffer(m_nbLines);
        int lines = old->getLines();
        int startLine = 0;
        if (lines > (int)m_nbLines)
            startLine = lines - m_nbLines;

        Character line[LINE_SIZE];
        for (int i = startLine; i < lines; i++) {
            int size = old->getLineLen(i);
            if (size > LINE_SIZE) {
                Character *tmp_line = new Character[size];
                old->getCells(i, 0, size, tmp_line);
                newScroll->addCells(tmp_line, size);
                newScroll->addLine(old->isWrappedLine(i));
                delete[] tmp_line;
            } else {
                old->getCells(i, 0, size, line);
                newScroll->addCells(line, size);
                newScroll->addLine(old->isWrappedLine(i));
            }
        }
        delete old;
        return newScroll;
    }
    return new HistoryScrollBuffer(m_nbLines);
}
