/*
    This file is part of Konsole, an X terminal.

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

#ifndef VT102EMULATION_H
#define VT102EMULATION_H

#include <cstdio>

#include <QByteArray>
#include <QKeyEvent>
#include <QHash>
#include <QTimer>
#include <QVector>

#include "Emulation.h"
#include "KittyGraphicsParser.h"
#include "Screen.h"

#define MODE_AppScreen       (MODES_SCREEN+0)   // Mode #1
#define MODE_AppCuKeys       (MODES_SCREEN+1)   // Application cursor keys (DECCKM)
#define MODE_AppKeyPad       (MODES_SCREEN+2)   //
#define MODE_Mouse1000       (MODES_SCREEN+3)   // Send mouse X,Y position on press and release
#define MODE_Mouse1001       (MODES_SCREEN+4)   // Use Highlight mouse tracking
#define MODE_Mouse1002       (MODES_SCREEN+5)   // Use cell motion mouse tracking
#define MODE_Mouse1003       (MODES_SCREEN+6)   // Use all motion mouse tracking
#define MODE_Mouse1005       (MODES_SCREEN+7)   // Xterm-style extended coordinates
#define MODE_Mouse1006       (MODES_SCREEN+8)   // 2nd Xterm-style extended coordinates
#define MODE_Mouse1015       (MODES_SCREEN+9)   // Urxvt-style extended coordinates
#define MODE_Ansi            (MODES_SCREEN+10)   // Use US Ascii for character sets G0-G3 (DECANM)
#define MODE_132Columns      (MODES_SCREEN+11)  // 80 <-> 132 column mode switch (DECCOLM)
#define MODE_Allow132Columns (MODES_SCREEN+12)  // Allow DECCOLM mode
#define MODE_BracketedPaste  (MODES_SCREEN+13)  // Xterm-style bracketed paste mode
#define MODE_SynchronizedOutput (MODES_SCREEN+14) // 同步输出（BSU/ESU，CSI ? 2026）
#define MODE_total           (MODES_SCREEN+15)

struct CharCodes
{
  // coding info
  char charset[4]; //
  int  cu_cs;      // actual charset.
  bool graphic;    // Some VT100 tricks
  bool pound  ;    // Some VT100 tricks
  bool sa_graphic; // saved graphic
  bool sa_pound;   // saved pound
};

/**
 * Provides an xterm compatible terminal emulation based on the DEC VT102 terminal.
 * A full description of this terminal can be found at http://vt100.net/docs/vt102-ug/
 *
 * In addition, various additional xterm escape sequences are supported to provide
 * features such as mouse input handling.
 * See http://rtfm.etla.org/xterm/ctlseq.html for a description of xterm's escape
 * sequences.
 *
 */
class Vt102Emulation : public Emulation
{
Q_OBJECT

public:
  /** Constructs a new emulation */
  Vt102Emulation();
  ~Vt102Emulation() override;

  // reimplemented from Emulation
  void clearEntireScreen() override;
  void reset() override;
  char eraseChar() const override;

signals:
  /**
    * Requests that the background color of views on this session
    * should be changed.
    */
  void changeBackgroundColorRequest(const QColor &);
  /** TODO: Document me. */
  void openUrlRequest(const QString & url);

public slots:
  // reimplemented from Emulation
  void sendString(const char*,int length = -1) override;
  void sendText(const QString& text) override;
  void sendKeyEvent(QKeyEvent*, bool fromPaste) override;
  void sendMouseEvent(int buttons, int column, int line, int eventType) override;
  virtual void focusLost();
  virtual void focusGained();

protected:
  // reimplemented from Emulation
  void setMode(int mode) override;
  void resetMode(int mode) override;
  void receiveChar(char32_t cc) override;

private slots:
  //causes changeTitle() to be emitted for each (int,QString) pair in pendingTitleUpdates
  //used to buffer multiple title updates
  void updateTitle();

private:
  void doTitleChanged( int what, const QString & caption );
  char32_t applyCharset(char32_t c);
  void setCharset(int n, int cs);
  void useCharset(int n);
  void setAndUseCharset(int n, int cs);
  void saveCursor();
  void restoreCursor();
  void resetCharset(int scrno);

  void setMargins(int top, int bottom);
  //set margins for all screens back to their defaults
  void setDefaultMargins();

  // returns true if 'mode' is set or false otherwise
  bool getMode    (int mode);
  // saves the current boolean value of 'mode'
  void saveMode   (int mode);
  // restores the boolean value of 'mode'
  void restoreMode(int mode);
  // resets all modes
  // (except MODE_Allow132Columns)
  void resetModes();

  void resetTokenizer();
  #define MAX_TOKEN_LENGTH 100000 // Max length of tokens (e.g. window title)
  void addToCurrentToken(char32_t cc);
  char32_t tokenBuffer[MAX_TOKEN_LENGTH];
  int tokenBufferPos;
#define MAXARGS 15
  void addDigit(int dig);
  void addArgument();
  int argv[MAXARGS];
  int argc;
  void initTokenizer();
  int prevCC;
  /**
   * @brief 超长 token 丢弃标志：true 时吞吃后续字节直至序列终止符（BEL 或 ST）。
   * @note 由 addToCurrentToken() 溢出时置位，防止残余字节被当作新序列解析。
   */
  bool tokenDiscard = false;

  // Set of flags for each of the ASCII characters which indicates
  // what category they fall into (printable character, control, digit etc.)
  // for the purposes of decoding terminal output
  int charClass[256];

  void reportDecodingError();

  void processToken(int code, char32_t p, int q);
  void processOSC();

  /**
   * @name kitty 键盘协议（级别 1+2）协商状态
   * @note kitty 规范要求主/备屏独立 flags 栈；本轮按规格实现单栈，
   *       实际应用（neovim 等）push/pop 配对使用，行为一致。
   */
  ///@{
  /** @brief 已实现的 flags 掩码：仅消歧义（1）与事件类型（2）。 */
  static constexpr quint32 KITTY_FLAGS_SUPPORTED = 0b11;
  /** @brief flags 栈深度上限，防恶意输入撑爆内存。 */
  static constexpr int KITTY_FLAGS_STACK_MAX = 64;
  /** @brief 当前生效 flags（默认全关，纯应用协商）。 */
  quint32 _kittyFlags = 0;
  /** @brief CSI > u 压入的历史 flags。 */
  QVector<quint32> _kittyFlagsStack;
  ///@}

  /** @brief 压入新的 kitty flags（CSI > flags u）。 */
  void kittyFlagsPush(quint32 flags);
  /** @brief 弹出 count 层历史 flags（CSI < [count] u）。 */
  void kittyFlagsPop(int count);
  /** @brief 设置 kitty flags（CSI = flags ; mode u）。 */
  void kittyFlagsSet(quint32 flags, int mode);
  /** @brief 应答 CSI ? flags u 查询。 */
  void reportKittyKeyboardFlags();

  /**
   * @name Sixel 图形（DCS P1;P2;P3 q ... ST）累积状态
   * @note sixel 数据量可达 MB 级，远超 tokenBuffer（MAX_TOKEN_LENGTH=100000），
   *       检测到 sixel 头后切换到独立字节流缓冲，ST 后整体交 SixelDecoder。
   */
  ///@{
  /** @brief sixel 数据缓冲上限（32MB）；超限置 _sixelOverflow 吞到 ST 后丢弃。 */
  static constexpr int MAX_SIXEL_DATA_LENGTH = 32 * 1024 * 1024;
  bool _sixelActive = false;     ///< 正在累积 sixel 数据段
  bool _sixelOverflow = false;   ///< 数据超上限：吞到 ST 并丢弃
  bool _sixelEscPending = false; ///< 上一字节为 ESC（等待判定 ST 或中止）
  int _sixelP2 = 0;              ///< DCS 第二参数（透明底/填底语义）
  QByteArray _sixelData;         ///< 'q' 之后、ST 之前的原始数据段
  /** @brief ST 到达：解码并锚定（失败/超限静默丢弃），复位累积状态。 */
  void finishSixel();
  /** @brief CAN/SUB/ESC 中止：丢弃累积数据，复位累积状态。 */
  void abortSixel();
  ///@}

  /**
   * @name Kitty 图形（APC "ESC _ G ... ESC \"）累积状态
   * @note 与 sixel DCS 通道同构：base64 图像负载远超 tokenBuffer（MAX_TOKEN_LENGTH），
   *       检测到 ESC _ G 后切换到独立字节流缓冲，ST 后交 KittyGraphicsParser；
   *       解析器成员跨 APC 序列存活（m=1 分块续传）。
   */
  ///@{
  /** @brief APC 累积上限（350MB，约对应 256MB 解码像素预算）；超限丢弃整条命令。 */
  static constexpr qint64 MAX_APC_DATA_LENGTH = 350LL * 1024 * 1024;
  bool _apcActive = false;     ///< 正在累积 APC 数据段
  bool _apcOverflow = false;   ///< 数据超上限：吞到 ST 后丢弃
  bool _apcEscPending = false; ///< 上一字节为 ESC（等待判定 ST 或中止）
  QByteArray _apcData;         ///< 'G' 之后、ST 之前的原始字节
  KittyGraphicsParser _kittyParser; ///< 分块重组/解析（跨 APC 序列存活）
  /** @brief ST 到达：喂解析器；NeedMore 等待续块，Ready/Error 交执行器。 */
  void finishApc();
  /** @brief CAN/SUB/ESC 中止：丢弃累积数据与半成品分块，复位状态。 */
  void abortApc();
  /** @brief 执行解析完成的 kitty 命令（放置/删除/应答/光标移动/重传语义）。 */
  void executeKittyCommand(const KittyGraphicsParser::Result &res,
                           const QByteArray &rawChunk);
  /**
   * @brief 经 sendString 回写 kitty 应答到 pty。
   * @param imageId 图像 id（i= 回显）。
   * @param placementId 放置 id（includePlacement 时回显 p=）。
   * @param includePlacement 是否在应答中包含 p=（成功应答且客户端给了 p= 时）。
   * @param ok true 回 OK；false 回 error（"CODE:message"）。
   */
  void sendKittyResponse(quint32 imageId, quint32 placementId, bool includePlacement,
                         bool ok, const QByteArray &error = {});
  ///@}
  /**
   * @brief 按 kitty 键盘协议编码按键事件。
   * @param event 按键事件。
   * @param out 编码输出（未发送任何字节时为空）。
   * @return true 表示事件已被协议处理（发送或吞掉）；false 表示回落传统编码。
   */
  bool encodeKittyKeyEvent(QKeyEvent *event, QByteArray &out);
  void processWindowAttributeChange(int attributeToChange, QString newValue);
  void requestWindowAttribute(int);

  void reportTerminalType();
  void reportSecondaryAttributes();
  void reportStatus();
  void reportAnswerBack();
  void reportCursorPosition();
  void reportTerminalParms(int p);

  // DECRPM responses to DECRQM queries
  void reportAnsiMode(int mode, int status);
  void reportDecMode(int mode, int status);

  void onScrollLock();
  void scrollLock(const bool lock);

  // clears the screen and resizes it to the specified
  // number of columns
  void clearScreenAndSetColumns(int columnCount);

  CharCodes _charset[2];

  class TerminalState
  {
  public:
    // Initializes all modes to false
    TerminalState()
    { memset(&mode,false,MODE_total * sizeof(bool)); }

    bool mode[MODE_total];
  };

  TerminalState _currentModes;
  TerminalState _savedModes;

  //hash table and timer for buffering calls to the session instance
  //to update the name of the session
  //or window title.
  //these calls occur when certain escape sequences are seen in the
  //output from the terminal
  QHash<int,QString> _pendingTitleUpdates;
  QTimer* _titleUpdateTimer;

  bool _reportFocusEvents;
  QStringEncoder _toUtf8;

  bool _isTitleChanged; ///< flag if the title/icon was changed by user
  QString _userTitle;
  QString _iconText; // as set by: echo -en '\033]1;IconText\007
  QString _nameTitle;
  QString _iconName;
  QColor _modifiedBackground; // as set by: echo -en '\033]11;Color\007
};

#endif // VT102EMULATION_H
