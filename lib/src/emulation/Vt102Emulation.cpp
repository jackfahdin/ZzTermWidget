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
#include "Vt102Emulation.h"
#include <cstdio>
#include <string>

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QKeyEvent>
#include <QRegularExpression>

#include "KeyboardTranslator.h"
#include "Screen.h"
#include "SixelDecoder.h"

Vt102Emulation::Vt102Emulation()
        : Emulation(), prevCC(0), _titleUpdateTimer(new QTimer(this)),
            _reportFocusEvents(false), _toUtf8(QStringEncoder::Utf8),
            _isTitleChanged(false) {
    _titleUpdateTimer->setSingleShot(true);
    QObject::connect(_titleUpdateTimer, &QTimer::timeout, this, &Vt102Emulation::updateTitle);
    initTokenizer();
    reset();
}

Vt102Emulation::~Vt102Emulation() {}

void Vt102Emulation::clearEntireScreen() {
    _currentScreen->clearEntireScreen();
    bufferedUpdate();
}

void Vt102Emulation::reset() {
    resetTokenizer();
    // 退出 token 丢弃模式：否则 reset 后仍会持续吞吃后续输入
    tokenDiscard = false;
    abortSixel(); // 复位时丢弃未完成的 sixel 累积
    abortApc();            // 复位时丢弃未完成的 APC 累积
    _kittyParser.reset();  // 连半成品分块一并丢弃
    _kittyFlags = 0;
    _kittyFlagsStack.clear();
    resetModes();
    resetCharset(0);
    _screen[0]->reset();
    resetCharset(1);
    _screen[1]->reset();

    bufferedUpdate();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                     Processing the incoming byte stream                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/* Incoming Bytes Event pipeline

     This section deals with decoding the incoming character stream.
     Decoding means here, that the stream is first separated into `tokens'
     which are then mapped to a `meaning' provided as operations by the
     `Screen' class or by the emulation class itself.

     The pipeline proceeds as follows:

     - Tokenizing the ESC codes (onReceiveChar)
     - VT100 code page translation of plain characters (applyCharset)
     - Interpretation of ESC codes (processToken)

     The escape codes and their meaning are described in the
     technical reference of this program.
*/

// Tokens ------------------------------------------------------------------ --

/*
     Since the tokens are the central notion if this section, we've put them
     in front. They provide the syntactical elements used to represent the
     terminals operations as byte sequences.

     They are encodes here into a single machine word, so that we can later
     switch over them easily. Depending on the token itself, additional
     argument variables are filled with parameter values.

     The tokens are defined below:

     - CHR        - Printable characters     (32..255 but DEL (=127))
     - CTL        - Control characters       (0..31 but ESC (= 27), DEL)
     - ESC        - Escape codes of the form <ESC><CHR but `[]()+*#'>
     - ESC_DE     - Escape codes of the form <ESC><any of `()+*#%'> C
     - CSI_PN     - Escape codes of the form <ESC>'['     {Pn} ';' {Pn} C
     - CSI_PS     - Escape codes of the form <ESC>'['     {Pn} ';' ...  C
     - CSI_PS_SP  - Escape codes of the form <ESC>'['     {Pn} ';' ... {Space} C
     - CSI_PR     - Escape codes of the form <ESC>'[' '?' {Pn} ';' ...  C
     - CSI_PE     - Escape codes of the form <ESC>'[' '!' {Pn} ';' ...  C
     - VT52       - VT52 escape codes
                                    - <ESC><Chr>
                                    - <ESC>'Y'{Pc}{Pc}
     - XTE_HA     - Xterm window/terminal attribute commands
                                    of the form <ESC>`]' {Pn} `;' {Text} <BEL>
                                    (Note that these are handled differently to the other formats)

     The last two forms allow list of arguments. Since the elements of
     the lists are treated individually the same way, they are passed
     as individual tokens to the interpretation. Further, because the
     meaning of the parameters are names (although represented as numbers),
     they are includes within the token ('N').

*/

#define TY_CONSTRUCT(T, A, N)                                                  \
    (((((int)N) & 0xffff) << 16) | ((((int)A) & 0xff) << 8) | (((int)T) & 0xff))

#define TY_CHR() TY_CONSTRUCT(0, 0, 0)
#define TY_CTL(A) TY_CONSTRUCT(1, A, 0)
#define TY_ESC(A) TY_CONSTRUCT(2, A, 0)
#define TY_ESC_CS(A, B) TY_CONSTRUCT(3, A, B)
#define TY_ESC_DE(A) TY_CONSTRUCT(4, A, 0)
#define TY_CSI_PS(A, N) TY_CONSTRUCT(5, A, N)
#define TY_CSI_PN(A) TY_CONSTRUCT(6, A, 0)
#define TY_CSI_PR(A, N) TY_CONSTRUCT(7, A, N)
#define TY_CSI_PS_SP(A, N) TY_CONSTRUCT(11, A, N)

#define TY_VT52(A) TY_CONSTRUCT(8, A, 0)
#define TY_CSI_PG(A) TY_CONSTRUCT(9, A, 0)
#define TY_CSI_PE(A) TY_CONSTRUCT(10, A, 0)
#define TY_CSI_PQ(A) TY_CONSTRUCT(12,A,0)
#define TY_CSI_PL(A) TY_CONSTRUCT(13,A,0)

#define MAX_ARGUMENT 4096

// Tokenizer --------------------------------------------------------------- --

/* The tokenizer's state

     The state is represented by the buffer (tokenBuffer, tokenBufferPos),
     and accompanied by decoded arguments kept in (argv,argc).
     Note that they are kept internal in the tokenizer.
*/

void Vt102Emulation::resetTokenizer() {
    tokenBufferPos = 0;
    argc = 0;
    argv[0] = 0;
    argv[1] = 0;
    argSeparators[0] = 0;
    argSeparators[1] = 0;
    prevCC = 0;
}

void Vt102Emulation::addDigit(int digit) {
    if (argv[argc] < MAX_ARGUMENT)
        argv[argc] = 10 * argv[argc] + digit;
}

/**
 * @brief 结束当前参数并开始下一个参数。
 * @param sep 引入新参数的前导分隔符（';' 或 ':'），记录到 argSeparators 供 SGR 消费。
 */
void Vt102Emulation::addArgument(char sep) {
    argc = qMin(argc + 1, MAXARGS - 1);
    argv[argc] = 0;
    argSeparators[argc] = sep;
}

void Vt102Emulation::addToCurrentToken(char32_t cc) {
    if (tokenBufferPos >= MAX_TOKEN_LENGTH - 1) {
        // token 超长（如超长窗口标题）：丢弃整个序列并复位解析器，避免静默截断产生错误语义
        qWarning("Vt102Emulation: token exceeds MAX_TOKEN_LENGTH, sequence discarded");
        resetTokenizer();
        tokenDiscard = true; // 吞吃本序列的剩余字节，直至终止符
        return;
    }
    tokenBuffer[tokenBufferPos++] = cc;
}

// Character Class flags used while decoding
#define CTL 1  // Control character
#define CHR 2  // Printable character
#define CPN 4  // TODO: Document me
#define DIG 8  // Digit
#define SCS 16 // TODO: Document me
#define GRP 32 // TODO: Document me
#define CPS 64 // Character which indicates end of window resize
               // escape sequence '\e[8;<row>;<col>t'

void Vt102Emulation::initTokenizer() {
    int i;
    quint8 *s;
    for (i = 0; i < 256; ++i)
        charClass[i] = 0;
    for (i = 0; i < 32; ++i)
        charClass[i] |= CTL;
    for (i = 32; i < 256; ++i)
        charClass[i] |= CHR;
    for (s = (quint8 *)"@ABCDEFGHILMPSTXZbcdfry"; *s; ++s)
        charClass[*s] |= CPN;
    // resize = \e[8;<row>;<col>t
    for (s = (quint8 *)"t"; *s; ++s)
        charClass[*s] |= CPS;
    for (s = (quint8 *)"0123456789"; *s; ++s)
        charClass[*s] |= DIG;
    for (s = (quint8 *)"()+*%"; *s; ++s)
        charClass[*s] |= SCS;
    for (s = (quint8 *)"()+*#[]%_^PX"; *s; ++s)
        charClass[*s] |= GRP;

    resetTokenizer();
}

/* Ok, here comes the nasty part of the decoder.

     Instead of keeping an explicit state, we deduce it from the
     token scanned so far. It is then immediately combined with
     the current character to form a scanning decision.

     This is done by the following defines.

     - P is the length of the token scanned so far.
     - L (often P-1) is the position on which contents we base a decision.
     - C is a character or a group of characters (taken from 'charClass').

     - 'cc' is the current character
     - 's' is a pointer to the start of the token buffer
     - 'p' is the current position within the token buffer

     Note that they need to applied in proper order.
*/

#define lec(P, L, C) (p == (P) && s[(L)] == (C))
#define lun()        (p == 1 && cc >= 32)
#define les(P, L, C) (p == (P) && s[L] < 256 && (charClass[s[(L)]] & (C)) == (C))
#define eec(C)       (p >= 3 && cc == (C))
#define ees(C)       (p >= 3 && cc < 256 && (charClass[cc] & (C)) == (C))
#define eps(C)       (p >= 3 && s[2] != '?' && s[2] != '!' && s[2] != '<' && s[2] != '=' && s[2] != '>' && cc < 256 && (charClass[cc] & (C)) == (C))
#define epp()        (p >= 3 && s[2] == '?')
#define epe()        (p >= 3 && s[2] == '!')
#define elt( )       (p >= 3 && s[2] == '<')
#define eeq( )       (p >= 3 && s[2] == '=')
#define egt()        (p >= 3 && s[2] == '>')
#define esp()        (p == 4 && s[3] == ' ')
#define Cse        (tokenBufferPos >= 2 && (tokenBuffer[1] == ']' || tokenBuffer[1] == 'P' || tokenBuffer[1] == '_' || tokenBuffer[1] == '^' || tokenBuffer[1] == 'X'))
#define Cte        (Cse      && ((tokenBuffer[1] == ']' && cc == 7) || (prevCC == 27 && cc == 92) )) // 27, 92 => "\e\\" (ST); BEL only for OSC
#define ces(C)     (cc < 256 && (charClass[cc] & (C)) == (C) && !Cte)

#define CNTL(c)      ((c) - '@')
#define ESC 27
#define DEL 127

// process an incoming unicode character
void Vt102Emulation::receiveChar(char32_t cc) {
    // Sixel 数据累积中：绕过 tokenizer 状态推导，直至 ST（ESC \）结束或 CAN/SUB/ESC 中止
    if (_sixelActive) {
        if (_sixelEscPending) {
            _sixelEscPending = false;
            if (cc == U'\\') { // ST：数据段结束，交解码器
                finishSixel();
                return;
            }
            // ESC 后非 '\'：按规格中止该图；ESC 与当前字节属于后续序列，重投正常解析
            abortSixel();
            receiveChar(ESC);
            receiveChar(cc);
            return;
        }
        if (cc == ESC) {
            _sixelEscPending = true;
            return;
        }
        if (cc == CNTL('X') || cc == CNTL('Z')) { // CAN / SUB：中止该图
            abortSixel();
            return;
        }
        if (cc >= 0x20 && cc < 0x7F) {
            if (_sixelData.size() >= MAX_SIXEL_DATA_LENGTH)
                _sixelOverflow = true; // 超上限：继续吞到 ST，ST 后丢弃
            else if (!_sixelOverflow)
                _sixelData.append(char(cc));
        }
        // 其余 C0 控制字符按 DEC 惯例在 DCS 数据段内忽略
        return;
    }

    // Kitty APC 累积中：绕过 tokenizer，直至 ST（ESC \）结束或 CAN/SUB 中止
    if (_apcActive) {
        if (_apcEscPending) {
            _apcEscPending = false;
            if (cc == U'\\') { // ST：APC 序列结束，喂解析器
                finishApc();
                return;
            }
            // ESC 后非 '\'：中止本条；ESC 与当前字节属于后续序列，重投正常解析
            abortApc();
            receiveChar(ESC);
            receiveChar(cc);
            return;
        }
        if (cc == ESC) {
            _apcEscPending = true;
            return;
        }
        if (cc == CNTL('X') || cc == CNTL('Z')) { // CAN / SUB：中止本条
            abortApc();
            return;
        }
        if (cc >= 0x20 && cc < 0x7F) {
            if (_apcData.size() >= MAX_APC_DATA_LENGTH)
                _apcOverflow = true; // 超上限：继续吞到 ST，ST 后丢弃并复位通道
            else if (!_apcOverflow)
                _apcData.append(char(cc));
        }
        // 其余 C0 控制字符在 APC 数据段内忽略（与 DCS 通道一致）
        return;
    }

    if ((cc == U'\r') || (cc == U'\n'))
        dupDisplayCharacter(cc);
    if (cc == DEL)
        return; // VT100: ignore.

    // 丢弃模式：吞吃超长序列的剩余字节，直至终止符（OSC 的 BEL 或通用的 ST）
    if (tokenDiscard) {
        const bool terminated = (cc == 7) || (prevCC == ESC && cc == '\\');
        prevCC = cc;
        if (terminated) {
            tokenDiscard = false;
            resetTokenizer();
        }
        return;
    }

    if (ces(CTL)) {
        // ignore control characters in the text part of Cse escape sequences, aka: OSC "ESC]", DCS
        // "ESCP", APC "ESC_", SOS "ESCX", and PM  "ESC^".
        if (Cse) {
            // Store in prevCC so Cte can detect the ST terminator (prevCC == 27 && cc == 92 => ESC \).
            prevCC = cc;
            return;
        }

        // DEC HACK ALERT! Control Characters are allowed *within* esc sequences in
        // VT100 This means, they do neither a resetTokenizer() nor a pushToToken().
        // Some of them, do of course. Guess this originates from a weakly layered
        // handling of the X-on X-off protocol, which comes really below this level.
        if (cc == CNTL('X') || cc == CNTL('Z') || cc == ESC)
            resetTokenizer(); // VT100: CAN or SUB
        if (cc != ESC) {
            processToken(TY_CTL(cc + '@'), 0, 0);
            return;
        }
    }
    // advance the state
    addToCurrentToken(cc);
    if (tokenDiscard)
        return; // 刚触发超长丢弃，当前字节已被吞吃，不再参与状态推导

    char32_t *s = tokenBuffer;
    int p = tokenBufferPos;

    if (getMode(MODE_Ansi)) {
        if (lec(1, 0, ESC)) {
            return;
        }
        if (lec(1, 0, ESC + 128)) {
            s[0] = ESC;
            receiveChar('[');
            return;
        }
        if (les(2, 1, GRP)) {
            return;
        }
        if (Cte) {
            if (tokenBufferPos >= 2 && tokenBuffer[1] == ']')
                processOSC();
            resetTokenizer();
            return;
        }
        // Sixel 图形：DCS P1;P2;P3 q —— 'q' 为引导符，此前仅允许数字与 ';'
        //（排除 DECRQSS 的 DCS $ q 等带中间字节的变体）。检测到后切换到独立
        // 累积通道，数据不再进 tokenBuffer（sixel 流可远超 MAX_TOKEN_LENGTH）
        if (Cse && tokenBuffer[1] == U'P' && cc == U'q') {
            bool headerOk = true;
            for (int i = 2; i < tokenBufferPos - 1; i++)
                if ((tokenBuffer[i] < U'0' || tokenBuffer[i] > U'9') && tokenBuffer[i] != U';')
                    headerOk = false;
            if (headerOk) {
                _sixelActive = true;
                _sixelOverflow = false;
                _sixelEscPending = false;
                _sixelData.clear();
                // 解析 P1;P2;P3：P1（宽高比）与 P3 按设计忽略，仅取 P2（透明底/填底语义）
                const QString header = QString::fromUcs4(tokenBuffer + 2, tokenBufferPos - 3);
                const auto parts = header.split(QLatin1Char(';'), Qt::KeepEmptyParts);
                _sixelP2 = parts.size() >= 2 ? parts[1].toInt() : 0;
                resetTokenizer();
                return;
            }
        }
        // Kitty 图形：APC ESC _ G —— 'G' 为引导符（ESC _ 之后立即出现）。
        // 检测到后切换到独立累积通道（base64 负载可远超 MAX_TOKEN_LENGTH）；
        // 其他 APC（非 'G' 引导）维持原路径：tokenBuffer 累积、ST 后丢弃。
        // 注：cc 已经 addToCurrentToken 入缓冲，故 ESC _ G 三字节齐备时 pos==3
        if (Cse && tokenBufferPos == 3 && tokenBuffer[1] == U'_' && cc == U'G') {
            _apcActive = true;
            _apcOverflow = false;
            _apcEscPending = false;
            _apcData.clear();
            resetTokenizer();
            return;
        }
        if (Cse) {
            prevCC = cc;
            return;
        }
        if (lec(3, 2, '?')) {
            return;
        }
        if (lec(3, 2, '>')) {
            return;
        }
        if (lec(3, 2, '!')) {
            return;
        }
        if (lec(3, 2, '<')) { 
            return; 
        }
        if (lec(3, 2, '=')) { 
            return; 
        }
        if (lun()) {
            processToken(TY_CHR(), applyCharset(cc), 0);
            resetTokenizer();
            return;
        }
        if (lec(2, 0, ESC)) {
            processToken(TY_ESC(s[1]), 0, 0);
            resetTokenizer();
            return;
        }
        if (les(3, 1, SCS)) {
            processToken(TY_ESC_CS(s[1], s[2]), 0, 0);
            resetTokenizer();
            return;
        }
        if (lec(3, 1, '#')) {
            processToken(TY_ESC_DE(s[2]), 0, 0);
            resetTokenizer();
            return;
        }
        if (eps(CPN)) {
            processToken(TY_CSI_PN(cc), argv[0], argv[1]);
            resetTokenizer();
            return;
        }
        if (esp()) {
            return;
        }

        // DECRQM：吞吃 CSI Pd $ p 序列中的 '$' 中间字节，等待最终字节 'p' 再分发
        if (eec('$')) { return; } // 吞吃 '$'，等待最终字节

        // CSI with '<' private marker (e.g. SGR mouse reporting: CSI < ... M/m).
        // Once ESC[< is seen, consume bytes until a CSI final byte (0x40-0x7E).
        if (elt()) {
            if (cc >= 0x40 && cc <= 0x7E) {
                processToken(TY_CSI_PL(cc), 0, 0);
                resetTokenizer();
                return;
            }
            if (ees(DIG)) { addDigit(cc-'0'); return; }
            if (eec(';') || eec(':')) { addArgument(static_cast<char>(cc)); return; }
            return;
        }

        // CSI with '=' private marker (e.g. Kitty keyboard protocol: CSI = ... u).
        // Once ESC[= is seen, consume bytes until a CSI final byte (0x40-0x7E).
        if (eeq()) {
            if (cc >= 0x40 && cc <= 0x7E) {
                processToken(TY_CSI_PQ(cc), 0, 0);
                resetTokenizer();
                return;
            }
            if (ees(DIG)) { addDigit(cc-'0'); return; }
            if (eec(';') || eec(':')) { addArgument(static_cast<char>(cc)); return; }
            return;
        }

        if (lec(5, 4, 'q') && s[3] == ' ') {
            processToken(TY_CSI_PS_SP(cc, argv[0]), argv[0], 0);
            resetTokenizer();
            return;
        }

        // resize = \e[8;<row>;<col>t
        if (eps(CPS)) {
            processToken(TY_CSI_PS(cc, argv[0]), argv[1], argv[2]);
            resetTokenizer();
            return;
        }

        if (epe()) {
            processToken(TY_CSI_PE(cc), 0, 0);
            resetTokenizer();
            return;
        }
        if (ees(DIG)) {
            addDigit(cc - '0');
            return;
        }
        if (eec(';') || eec(':')) {
            addArgument(static_cast<char>(cc));
            return;
        }

        // Per ECMA-48, bytes 0x3C-0x3F (< = > ?) are valid CSI parameter bytes.
        // When they appear after s[2] (the private-marker position), consume them
        // so they don't fall through to the dispatch loop or get printed.
        // This handles sequences like ESC[2:=z where '=' appears mid-parameter.
        if (p >= 4 && cc >= 0x3C && cc <= 0x3F) {
            return;
        }

        /**
         * @brief kitty 键盘协议：CSI ? u（查询）与 CSI > flags u（压栈）在通用参数分发前整体拦截。
         * @note 避免下方 for 循环按参数逐个触发导致重复应答/重复压栈。
         */
        if (epp() && cc == U'u') {
            reportKittyKeyboardFlags();
            resetTokenizer();
            return;
        }
        if (egt() && cc == U'u') {
            kittyFlagsPush(argv[0]);
            resetTokenizer();
            return;
        }

        for (int i = 0; i <= argc; i++) {
            if (epp())
                processToken(TY_CSI_PR(cc, argv[i]), 0, 0);
            else if (egt())
                processToken(TY_CSI_PG(cc), 0, 0); // spec. case for ESC]>0c or ESC]>c
            else if (cc == 'm' && argv[i] == 4 && i + 1 <= argc && argSeparators[i + 1] == ':') {
                // SGR 4:n 冒口子参数：下划线样式（ECMA-48 子参数；与 4;n 分号语义严格区分）
                const int sub = argv[++i]; // 消费子参数
                if (sub == 0)
                    processToken(TY_CSI_PS(cc, 24), 0, 0);        // 4:0 = 关下划线
                else if (sub >= 1 && sub <= 5)
                    processToken(TY_CSI_PS(cc, 4), sub - 1, 0);   // p 携带样式（0=单线…4=虚线）
                // 非法样式（>=6）：连同子参数整体忽略，不影响其余 SGR 参数
            } else if (cc == 'm' && argv[i] == 58 && i + 1 <= argc && argSeparators[i + 1] == ':') {
                // SGR 58 冒号形式：58:5:n / 58:2[:色彩空间空位]:r:g:b（必须先于 58 分号分支：
                // 58:2::r:g:b 拍平后同样满足 argv[i]==58 && argv[i+1]==2）
                if (argv[i + 1] == 5 && i + 2 <= argc) {
                    processToken(TY_CSI_PS(cc, 58), COLOR_SPACE_256, argv[i + 2]);
                    i += 2;
                } else if (argv[i + 1] == 2) {
                    int j = i + 2;
                    // 容忍色彩空间空位：58:2::r:g:b 拍平后该槽为 0
                    // （要求其后仍够 r/g/b 三槽才跳过；已知歧义：r=0 的真彩写法被当空位，整体忽略）
                    if (j <= argc && argv[j] == 0 && argc - j >= 3)
                        j++;
                    if (argc - j >= 2) {
                        processToken(TY_CSI_PS(cc, 58), COLOR_SPACE_RGB,
                                             (argv[j] << 16) | (argv[j + 1] << 8) | argv[j + 2]);
                        i = j + 2;
                    } else {
                        i += 1; // 参数不足：忽略 58 与模式槽，其余参数按独立 SGR 解释
                    }
                } else {
                    i += 1; // 未知 58 模式：忽略 58 与模式槽
                }
            } else if (cc == 'm' && argc - i >= 4 && argv[i] == 58 && argv[i + 1] == 2) {
                // SGR 58 分号真彩：58;2;r;g;b（镜像 38/48 写法，定长消费 5 槽，尾巴不吞）
                i += 2;
                processToken(TY_CSI_PS(cc, 58), COLOR_SPACE_RGB,
                                         (argv[i] << 16) | (argv[i + 1] << 8) | argv[i + 2]);
                i += 2;
            } else if (cc == 'm' && argc - i >= 2 && argv[i] == 58 && argv[i + 1] == 5) {
                // SGR 58 分号 256 色：58;5;n
                i += 2;
                processToken(TY_CSI_PS(cc, 58), COLOR_SPACE_256, argv[i]);
            } else if (cc == 'm' && argv[i] == 58) {
                // 参数不足的 58：忽略该参数，不吞后续独立 SGR
            } else if (cc == 'm' && (argv[i] == 38 || argv[i] == 48) && i + 1 <= argc
                       && argSeparators[i + 1] == ':') {
                // SGR 38/48 冒口子参数：38:5:n / 38:2[:色彩空间空位]:r:g:b（必须先于
                // 38/48 分号定长分支：38:2::r:g:b 拍平后同样满足 argv[i+1]==2；
                // 顺序红线同 58）
                const int colorCmd = argv[i];
                if (argv[i + 1] == 5 && i + 2 <= argc) {
                    processToken(TY_CSI_PS(cc, colorCmd), COLOR_SPACE_256, argv[i + 2]);
                    i += 2;
                } else if (argv[i + 1] == 2) {
                    int j = i + 2;
                    // 容忍色彩空间空位：38:2::r:g:b 拍平后该槽为 0（同 58 口径；
                    // 已知歧义：r=0 且其后仍够 ≥3 槽的真彩写法被当空位整体跳过，
                    // 与 58 一致，注释声明为遗留）
                    if (j <= argc && argv[j] == 0 && argc - j >= 3)
                        j++;
                    if (argc - j >= 2) {
                        processToken(TY_CSI_PS(cc, colorCmd), COLOR_SPACE_RGB,
                                             (argv[j] << 16) | (argv[j + 1] << 8) | argv[j + 2]);
                        i = j + 2;
                    } else {
                        i += 1; // 参数不足：忽略 38/48 与模式槽，其余参数按独立 SGR 解释
                    }
                } else {
                    i += 1; // 未知 38/48 模式：忽略该参数与模式槽
                }
            } else if (cc == 'm' && argc - i >= 4 && (argv[i] == 38 || argv[i] == 48) &&
                             argv[i + 1] == 2) {
                // ESC[ ... 48;2;<red>;<green>;<blue> ... m -or- ESC[ ...
                // 38;2;<red>;<green>;<blue> ... m
                i += 2;
                processToken(TY_CSI_PS(cc, argv[i - 2]), COLOR_SPACE_RGB,
                                         (argv[i] << 16) | (argv[i + 1] << 8) | argv[i + 2]);
                i += 2;
            } else if (cc == 'm' && argc - i >= 2 &&
                                 (argv[i] == 38 || argv[i] == 48) && argv[i + 1] == 5) {
                // ESC[ ... 48;5;<index> ... m -or- ESC[ ... 38;5;<index> ... m
                i += 2;
                processToken(TY_CSI_PS(cc, argv[i - 2]), COLOR_SPACE_256, argv[i]);
            } else
                processToken(TY_CSI_PS(cc, argv[i]), 0, 0);
        }
        resetTokenizer();
    } else {
        // VT52 Mode
        if (lec(1, 0, ESC))
            return;
        if (les(1, 0, CHR)) {
            processToken(TY_CHR(), s[0], 0);
            resetTokenizer();
            return;
        }
        if (lec(2, 1, 'Y'))
            return;
        if (lec(3, 1, 'Y'))
            return;
        if (p < 4) {
            processToken(TY_VT52(s[1]), 0, 0);
            resetTokenizer();
            return;
        }
        processToken(TY_VT52(s[1]), s[2], s[3]);
        resetTokenizer();
        return;
    }
}

void Vt102Emulation::processOSC() {
    int i = 2;
    while (i < tokenBufferPos && tokenBuffer[i] != ';')
        i++;
    if (i == tokenBufferPos) {
        reportDecodingError();
        return;
    }

    int command = -1;
    switch (i - 1) {
    case 2:
        command = tokenBuffer[2] - U'0';
        break;
    case 3:
        command = 10 * (tokenBuffer[2] - U'0') + (tokenBuffer[3] - U'0');
        break;
    default:
        reportDecodingError();
        return;
    }

    switch (command) {
    /*
     * Operating System Controls https://www.xfree86.org/current/ctlseqs.html
     *
     * Ps = 0 → Change Icon Name and Window Title to Pt
     * Ps = 1 → Change Icon Name to Pt
     * Ps = 2 → Change Window Title to Pt
     */
    case 0:
    case 1:
    case 2:
    case 7: {
        QString newValue =
                QString::fromUcs4(tokenBuffer + 3 + 1, tokenBufferPos - 3 - 2);
        processWindowAttributeChange(command, newValue);
        break;
    }
    //  Ps = 52 → Manipulate Selection Data. These controls may be disabled using
    //  the allowWindowOps resource.
    case 52: {
        // 开关关闭时吞掉整个 OSC 52（见 Emulation::setOsc52Enabled 的安全说明）
        if (!_osc52Enabled)
            break;
        /* The first, Pc , may contain any character from the set c p s 0 1 2 3 4 5
         * 6 7 . It is used to construct a list of selection parameters for
         * clipboard, primary, select, or cut buffers 0 through 8 respectively, in
         * the order given. If the parameter is empty, xterm uses s 0 , to specify
         * the configurable primary/clipboard selection and cut buffer 0. The second
         * parameter, Pd , gives the selection data. Normally this is a string
         * encoded in base64. The data becomes the new selection, which is then
         * available for pasting by other applications. If the second parameter is a
         * ? , xterm replies to the host with the selection data encoded using the
         * same protocol.
         */
        QString arg =
                QString::fromUcs4(tokenBuffer + 4 + 1, tokenBufferPos - 4 - 2);
        QStringList args = arg.split(";", Qt::SkipEmptyParts);
        auto processOSC52Text = [&](QString base64, QClipboard::Mode mode) {
            QClipboard *clipboard = QApplication::clipboard();
            if (base64 == "!") {
                clipboard->clear(mode);
            } else {
                QByteArray data = QByteArray::fromBase64(base64.toUtf8());
                clipboard->setText(QString::fromUtf8(data), mode);
            }
        };
        if (args.size() == 1 && args.at(0) != "?") {
            processOSC52Text(args.at(0), QClipboard::Clipboard);
        } else if (args.size() == 2) {
            if (args.at(0) == "c" && args.at(1) != "?") {
                processOSC52Text(args.at(1), QClipboard::Clipboard);
            }
            if (QApplication::clipboard()->supportsSelection()) {
                if (args.at(0) == "p" && args.at(1) != "?") {
                    processOSC52Text(args.at(1), QClipboard::Selection);
                }
            }
        }
        break;
    }
    //  Ps = 8 → Hyperlink（OSC 8）：ESC ] 8 ; params ; URI ST。params 为 ':' 分隔的
    //  键值对，仅识别 id=<value>（相同 id 的分段视为同一链接），未知键忽略；
    //  空 URI 表示当前链接结束。非法格式安全忽略，不产生热点。
    case 8: {
        const QString content = QString::fromUcs4(tokenBuffer + 4, tokenBufferPos - 5);
        const int sep = content.indexOf(QLatin1Char(';'));
        if (sep < 0)
            break; // 缺 URI 段：非法序列，忽略
        const QString params = content.left(sep);
        const QString uri = content.mid(sep + 1);
        QString id;
        if (!params.isEmpty()) {
            const auto pairs = params.split(QLatin1Char(':'), Qt::SkipEmptyParts);
            for (const QString &kv : pairs) {
                if (kv.startsWith(QLatin1String("id=")))
                    id = kv.mid(3);
            }
        }
        _currentScreen->setCurrentHyperlink(uri, id);
        break;
    }
    default:
        reportDecodingError();
        break;
    }
}

void Vt102Emulation::finishSixel()
{
    const bool overflow = _sixelOverflow;
    const QByteArray data = std::move(_sixelData);
    const int p2 = _sixelP2;
    _sixelActive = false;
    _sixelOverflow = false;
    _sixelEscPending = false;
    _sixelData.clear();
    resetTokenizer();
    if (overflow)
        return; // 数据超上限：静默丢弃
    const auto result = SixelDecoder::decode(data, p2);
    if (!result)
        return; // 解码失败/宽高超限：静默丢弃，不影响后续字节流
    _currentScreen->anchorImage(result->image, result->transparentBackground);
    // 无需显式刷新：receiveData 末尾的 bufferedUpdate 会经 outputChanged 通知显示层，
    // Screen::_graphicsDirty 标志保证 updateImage 整屏标脏一次
}

void Vt102Emulation::abortSixel()
{
    _sixelActive = false;
    _sixelOverflow = false;
    _sixelEscPending = false;
    _sixelData.clear();
    resetTokenizer();
}

void Vt102Emulation::finishApc()
{
    const bool overflow = _apcOverflow;
    const QByteArray data = std::move(_apcData);
    _apcActive = false;
    _apcOverflow = false;
    _apcEscPending = false;
    _apcData.clear();
    resetTokenizer();
    if (overflow) {
        _kittyParser.reset(); // 超 350MB：丢弃整条命令并中止半成品分块
        return;
    }
    KittyGraphicsParser::Result res;
    // 多块命令在末块 feed 前处于分块累积中；ENOSPC 后解析器已复位、负载不可重放，
    // 故仅单块命令（首块即末块）可走"淘汰后重试"
    const bool wasMidChunk = _kittyParser.midChunk();
    auto status = _kittyParser.feed(data, _currentScreen->imageBytesRemaining(), res);
    if (status == KittyGraphicsParser::Status::NeedMore)
        return; // m=1 续块：等待后续 APC 序列（显示位置以末块到达时的光标为准）
    // ENOSPC 且单块命令：先淘汰无放置引用图像后重试一次
    if (status == KittyGraphicsParser::Status::Error && res.errorCode == "ENOSPC"
            && !wasMidChunk) {
        _currentScreen->evictAllUnreferencedKittyImages();
        res = KittyGraphicsParser::Result{};
        status = _kittyParser.feed(data, _currentScreen->imageBytesRemaining(), res);
    }
    if (status == KittyGraphicsParser::Status::NeedMore)
        return;
    executeKittyCommand(res, data);
}

void Vt102Emulation::abortApc()
{
    _apcActive = false;
    _apcOverflow = false;
    _apcEscPending = false;
    _apcData.clear();
    _kittyParser.reset(); // 分块流被打断：丢弃半成品
    resetTokenizer();
}

void Vt102Emulation::sendKittyResponse(quint32 imageId, quint32 placementId,
                                       bool includePlacement, bool ok,
                                       const QByteArray &error)
{
    QByteArray resp = "\033_Gi=" + QByteArray::number(imageId);
    if (includePlacement)
        resp += ",p=" + QByteArray::number(placementId);
    resp += ';';
    resp += ok ? QByteArray("OK") : error;
    resp += "\033\\";
    sendString(resp.constData(), int(resp.size())); // 与 DECRQM 应答同路径：sendData → pty
}

void Vt102Emulation::executeKittyCommand(const KittyGraphicsParser::Result &res,
                                         const QByteArray &rawChunk)
{
    Q_UNUSED(rawChunk); // 保留给后续多命令复用，稳定接口
    Screen *scr = _currentScreen;

    // 解析/解码失败：能定位 i= 时回错误码（q=2 抑制），否则静默忽略
    if (!res.errorCode.isEmpty()) {
        if (res.imageId != 0 && res.quiet != 2)
            sendKittyResponse(res.imageId, 0, false, false,
                              res.errorCode + ':' + res.errorMessage);
        return;
    }

    const KittyCommand &cmd = res.command;
    const bool suppressOk = (cmd.quiet == 1);
    const bool suppressErr = (cmd.quiet == 2);
    const bool echoP = (cmd.placementId != 0);
    auto fail = [&](const char *code, const char *msg) {
        if (cmd.imageId != 0 && !suppressErr)
            sendKittyResponse(cmd.imageId, 0, false, false,
                              QByteArray(code) + ':' + msg);
    };
    auto ok = [&] {
        if (cmd.imageId != 0 && !suppressOk)
            sendKittyResponse(cmd.imageId, cmd.placementId, echoP, true);
    };

    // 不支持的传输介质（t=f/t/s）：回 EINVAL，不崩（解析器对需像素的动作已先行同码拒绝，此处兜底）
    if (cmd.medium == 'f' || cmd.medium == 't' || cmd.medium == 's') {
        fail("EINVAL", "unsupported medium");
        return;
    }

    switch (cmd.action) {
    case 'q':
        // 查询：解析器已试加载（成败在此之前的 Error 路径），不存储不替换
        ok();
        return;
    case 't':
    case 'T': {
        // 重传语义：已有同 id 图像时先删旧图及其全部放置，新数据落库但不自动显示
        const bool retransmit = (cmd.imageId != 0) && scr->hasKittyImage(cmd.imageId);
        if (retransmit)
            scr->kittyDeleteByImage(cmd.imageId, 0, true);
        quint32 imageHandle = 0;
        if (!scr->kittyStoreImage(cmd.image, cmd.imageId, &imageHandle)) {
            fail("ENOSPC", "pixel budget exceeded");
            return;
        }
        if (cmd.action == 't' || retransmit) {
            ok();
            return;
        }
        // a=T 新图：落库并放置（匿名图像 i=0 也在此显示，不占 id 命名空间）
        KittyPlacementParams params;
        params.placementId = cmd.placementId;
        params.srcX = cmd.srcX; params.srcY = cmd.srcY;
        params.srcW = cmd.srcW; params.srcH = cmd.srcH;
        params.cellXOff = cmd.cellXOff; params.cellYOff = cmd.cellYOff;
        params.cols = cmd.cols; params.rows = cmd.rows;
        params.zIndex = cmd.zIndex;
        int colsUsed = 0, rowsUsed = 0;
        const auto err = scr->kittyPlace(imageHandle, cmd.imageId, params,
                                         nullptr, &colsUsed, &rowsUsed);
        if (err != KittyPlaceError::Ok) {
            fail("EINVAL", "bad placement");
            return;
        }
        // kitty 光标语义（与 sixel 的 xterm 语义不同）：右移放置列数、下移放置行数；
        // C=1 时光标不移动。越出屏幕/滚动区的落点由实现自定（取 Screen 现有钳位行为）
        if (!cmd.cursorNoMove) {
            scr->cursorRight(colsUsed);
            scr->cursorDown(rowsUsed);
        }
        ok();
        return;
    }
    case 'p': {
        const quint32 imageHandle = scr->kittyImageHandle(cmd.imageId);
        if (imageHandle == 0) {
            fail("ENOENT", "no such image");
            return;
        }
        KittyPlacementParams params;
        params.placementId = cmd.placementId;
        params.srcX = cmd.srcX; params.srcY = cmd.srcY;
        params.srcW = cmd.srcW; params.srcH = cmd.srcH;
        params.cellXOff = cmd.cellXOff; params.cellYOff = cmd.cellYOff;
        params.cols = cmd.cols; params.rows = cmd.rows;
        params.zIndex = cmd.zIndex;
        int colsUsed = 0, rowsUsed = 0;
        const auto err = scr->kittyPlace(imageHandle, cmd.imageId, params,
                                         nullptr, &colsUsed, &rowsUsed);
        if (err != KittyPlaceError::Ok) {
            fail("EINVAL", "bad placement");
            return;
        }
        if (!cmd.cursorNoMove) {
            scr->cursorRight(colsUsed);
            scr->cursorDown(rowsUsed);
        }
        ok();
        return;
    }
    case 'd': {
        // 删除命令到达时分块上传未完成的场景已由 abortApc/打断规则覆盖（新命令即打断）
        switch (cmd.deleteWhat) {
        case 'a': scr->kittyDeleteAll(false); ok(); return;
        case 'A': scr->kittyDeleteAll(true); ok(); return;
        case 'i': scr->kittyDeleteByImage(cmd.imageId, cmd.placementId, false); ok(); return;
        case 'I': scr->kittyDeleteByImage(cmd.imageId, cmd.placementId, true); ok(); return;
        case 'c': scr->kittyDeleteAtCursor(false); ok(); return;
        case 'C': scr->kittyDeleteAtCursor(true); ok(); return;
        default: return; // 其余删除变体（n/f/q/r/x/y/z）：忽略，无应答
        }
    }
    case 'f': // 动画帧管理：本轮不做
    case 'c': // 动画帧合成：本轮不做
        fail("EINVAL", "unsupported action");
        return;
    default:
        fail("EINVAL", "unsupported action");
        return;
    }
}

void Vt102Emulation::processWindowAttributeChange(int attributeToChange, QString newValue) {
    _pendingTitleUpdates[attributeToChange] = newValue;
    _titleUpdateTimer->start(20);
}

void Vt102Emulation::updateTitle() {
    QListIterator<int> iter(_pendingTitleUpdates.keys());
    while (iter.hasNext()) {
        int arg = iter.next();
        doTitleChanged(arg, _pendingTitleUpdates[arg]);
    }
    _pendingTitleUpdates.clear();
}

void Vt102Emulation::doTitleChanged(int what, const QString &caption) {
    // set to true if anything is actually changed (eg. old _nameTitle != new
    // _nameTitle )
    bool modified = false;

    // (btw: what=0 changes _userTitle and icon, what=1 only icon, what=2 only
    // _nameTitle
    if ((what == 0) || (what == 2)) {
        _isTitleChanged = true;
        if (_userTitle != caption) {
            _userTitle = caption;
            modified = true;
        }
    }

    if ((what == 0) || (what == 1)) {
        _isTitleChanged = true;
        if (_iconText != caption) {
            _iconText = caption;
            modified = true;
        }
    }

    if (what == 11) {
        QString colorString = caption.section(QLatin1Char(';'), 0, 0);
        QColor backColor = QColor(colorString);
        if (backColor.isValid()) { // change color via \033]11;Color\007
            if (backColor != _modifiedBackground) {
                _modifiedBackground = backColor;
                emit changeBackgroundColorRequest(backColor);
            }
        }
    }

    if (what == 30) {
        _isTitleChanged = true;
        if (_nameTitle != caption) {
            _nameTitle = caption;
            return;
        }
    }

    if (what == 31) {
        QString cwd = caption;
        cwd =
                cwd.replace(QRegularExpression(QLatin1String("^~")), QDir::homePath());
        emit openUrlRequest(cwd);
    }

    // change icon via \033]32;Icon\007
    if (what == 32) {
        _isTitleChanged = true;
        if (_iconName != caption) {
            _iconName = caption;

            modified = true;
        }
    }

    if (what == 50) {
        emit profileChangeCommandReceived(caption);
        return;
    }

    if (modified) {
        emit titleChanged(what, caption);
    }
}

// Interpreting Codes ---------------------------------------------------------

/*
     Now that the incoming character stream is properly tokenized,
     meaning is assigned to them. These are either operations of
     the current _screen, or of the emulation class itself.

     The token to be interpreteted comes in as a machine word
     possibly accompanied by two parameters.

     Likewise, the operations assigned to, come with up to two
     arguments. One could consider to make up a proper table
     from the function below.

     The technical reference manual provides more information
     about this mapping.
*/

void Vt102Emulation::processToken(int token, char32_t p, int q) {
    switch (token) {
    case TY_CHR():
        _currentScreen->displayCharacter(p);
        dupDisplayCharacter(p);
        break; // UTF16

        //             127 DEL    : ignored on input

    case TY_CTL('@'): /* NUL: ignored                      */
        break;
    case TY_CTL('A'): /* SOH: ignored                      */
        break;
    case TY_CTL('B'): /* STX: ignored                      */
        break;
    case TY_CTL('C'): /* ETX: ignored                      */
        break;
    case TY_CTL('D'): /* EOT: ignored                      */
        break;
    case TY_CTL('E'):
        reportAnswerBack();
        break;          // VT100
    case TY_CTL('F'): /* ACK: ignored                      */
        break;
    case TY_CTL('G'):
        emit stateSet(NOTIFYBELL);
        break; // VT100
    case TY_CTL('H'):
        _currentScreen->backspace();
        break; // VT100
    case TY_CTL('I'):
        _currentScreen->tab();
        break; // VT100
    case TY_CTL('J'):
        _currentScreen->newLine();
        break; // VT100
    case TY_CTL('K'):
        _currentScreen->newLine();
        break; // VT100
    case TY_CTL('L'):
        _currentScreen->newLine();
        break; // VT100
    case TY_CTL('M'):
        _currentScreen->toStartOfLine();
        break; // VT100

    case TY_CTL('N'):
        useCharset(1);
        break; // VT100
    case TY_CTL('O'):
        useCharset(0);
        break; // VT100

    case TY_CTL('P'): /* DLE: ignored                      */
        break;
    case TY_CTL('Q'): /* DC1: XON continue                 */
        break;          // VT100
    case TY_CTL('R'): /* DC2: ignored                      */
        break;
    case TY_CTL('S'): /* DC3: XOFF halt                    */
        break;          // VT100
    case TY_CTL('T'): /* DC4: ignored                      */
        break;
    case TY_CTL('U'): /* NAK: ignored                      */
        break;
    case TY_CTL('V'): /* SYN: ignored                      */
        break;
    case TY_CTL('W'): /* ETB: ignored                      */
        break;
    case TY_CTL('X'):
        _currentScreen->displayCharacter(0x2592);
        dupDisplayCharacter(0x2592);
        break;          // VT100
    case TY_CTL('Y'): /* EM : ignored                      */
        break;
    case TY_CTL('Z'):
        _currentScreen->displayCharacter(0x2592);
        dupDisplayCharacter(0x2592);
        break;          // VT100
    case TY_CTL('['): /* ESC: cannot be seen here.         */
        break;
    case TY_CTL('\\'): /* FS : ignored                      */
        break;
    case TY_CTL(']'): /* GS : ignored                      */
        break;
    case TY_CTL('^'): /* RS : ignored                      */
        break;
    case TY_CTL('_'): /* US : ignored                      */
        break;

    case TY_ESC('D'):
        _currentScreen->index();
        break; // VT100
    case TY_ESC('E'):
        _currentScreen->nextLine();
        break; // VT100
    case TY_ESC('H'):
        _currentScreen->changeTabStop(true);
        break; // VT100
    case TY_ESC('M'):
        _currentScreen->reverseIndex();
        break; // VT100
    case TY_ESC('Z'):
        reportTerminalType();
        break;
    case TY_ESC('c'):
        reset();
        break;

    case TY_ESC('n'):
        useCharset(2);
        break;
    case TY_ESC('o'):
        useCharset(3);
        break;
    case TY_ESC('7'):
        saveCursor();
        break;
    case TY_ESC('8'):
        restoreCursor();
        break;

    case TY_ESC('='):
        setMode(MODE_AppKeyPad);
        break;
    case TY_ESC('>'):
        resetMode(MODE_AppKeyPad);
        break;
    case TY_ESC('<'):
        setMode(MODE_Ansi);
        break; // VT100

    case TY_ESC_CS('(', '0'):
        setCharset(0, '0');
        break; // VT100
    case TY_ESC_CS('(', 'A'):
        setCharset(0, 'A');
        break; // VT100
    case TY_ESC_CS('(', 'B'):
        setCharset(0, 'B');
        break; // VT100

    case TY_ESC_CS(')', '0'):
        setCharset(1, '0');
        break; // VT100
    case TY_ESC_CS(')', 'A'):
        setCharset(1, 'A');
        break; // VT100
    case TY_ESC_CS(')', 'B'):
        setCharset(1, 'B');
        break; // VT100

    case TY_ESC_CS('*', '0'):
        setCharset(2, '0');
        break; // VT100
    case TY_ESC_CS('*', 'A'):
        setCharset(2, 'A');
        break; // VT100
    case TY_ESC_CS('*', 'B'):
        setCharset(2, 'B');
        break; // VT100

    case TY_ESC_CS('+', '0'):
        setCharset(3, '0');
        break; // VT100
    case TY_ESC_CS('+', 'A'):
        setCharset(3, 'A');
        break; // VT100
    case TY_ESC_CS('+', 'B'):
        setCharset(3, 'B');
        break; // VT100

    case TY_ESC_CS('%', 'G'): /*No longer updating codec*/
        break;                  // LINUX
    case TY_ESC_CS('%', '@'): /*No longer updating codec*/
        break;                  // LINUX

    case TY_ESC_DE('3'): /* Double height line, top half    */
        _currentScreen->setLineProperty(LINE_DOUBLEWIDTH, true);
        _currentScreen->setLineProperty(LINE_DOUBLEHEIGHT, true);
        break;
    case TY_ESC_DE('4'): /* Double height line, bottom half */
        _currentScreen->setLineProperty(LINE_DOUBLEWIDTH, true);
        _currentScreen->setLineProperty(LINE_DOUBLEHEIGHT, true);
        break;
    case TY_ESC_DE('5'): /* Single width, single height line*/
        _currentScreen->setLineProperty(LINE_DOUBLEWIDTH, false);
        _currentScreen->setLineProperty(LINE_DOUBLEHEIGHT, false);
        break;
    case TY_ESC_DE('6'): /* Double width, single height line*/
        _currentScreen->setLineProperty(LINE_DOUBLEWIDTH, true);
        _currentScreen->setLineProperty(LINE_DOUBLEHEIGHT, false);
        break;
    case TY_ESC_DE('8'):
        _currentScreen->helpAlign();
        break;

        // resize = \e[8;<row>;<col>t
    case TY_CSI_PS('t', 8):
        setImageSize(p /*lines */, q /* columns */);
        emit imageResizeRequest(QSize(q, p));
        break;

        // change tab text color : \e[28;<color>t  color: 0-16,777,215
    case TY_CSI_PS('t', 28):
        emit changeTabTextColorRequest(p);
        break;

    case TY_CSI_PS('K', 0):
        _currentScreen->clearToEndOfLine();
        break;
    case TY_CSI_PS('K', 1):
        _currentScreen->clearToBeginOfLine();
        break;
    case TY_CSI_PS('K', 2):
        _currentScreen->clearEntireLine();
        break;
    case TY_CSI_PS('J', 0):
        _currentScreen->clearToEndOfScreen();
        break;
    case TY_CSI_PS('J', 1):
        _currentScreen->clearToBeginOfScreen();
        break;
    case TY_CSI_PS('J', 2):
        _currentScreen->clearEntireScreen();
        break;
    case TY_CSI_PS('J', 3):
        clearHistory();
        break;
    case TY_CSI_PS('g', 0):
        _currentScreen->changeTabStop(false);
        break; // VT100
    case TY_CSI_PS('g', 3):
        _currentScreen->clearTabStops();
        break; // VT100
    case TY_CSI_PS('h', 4):
        _currentScreen->setMode(MODE_Insert);
        break;
    case TY_CSI_PS('h', 20):
        setMode(MODE_NewLine);
        break;
    case TY_CSI_PS('i', 0): /* IGNORE: attached printer          */
        break;                // VT100
    case TY_CSI_PS('l', 4):
        _currentScreen->resetMode(MODE_Insert);
        break;
    case TY_CSI_PS('l', 20):
        resetMode(MODE_NewLine);
        break;
    case TY_CSI_PS('s', 0):
        saveCursor();
        break;
    case TY_CSI_PS('u', 0):
        restoreCursor();
        break;

    case TY_CSI_PS('m', 0):
        _currentScreen->setDefaultRendition();
        break;
    case TY_CSI_PS('m', 1):
        _currentScreen->setRendition(RE_BOLD);
        break; // VT100
    case TY_CSI_PS('m', 2):
        _currentScreen->setRendition(RE_FAINT);
        break;
    case TY_CSI_PS('m', 3):
        _currentScreen->setRendition(RE_ITALIC);
        break; // VT100
    case TY_CSI_PS('m', 4):
        _currentScreen->setRendition(RE_UNDERLINE);
        _currentScreen->setUnderlineStyle(int(p)); // p：冒口子参数样式（0=单线…4=虚线）；分号形式恒 0
        break; // VT100
    case TY_CSI_PS('m', 5):
        _currentScreen->setRendition(RE_BLINK);
        break; // VT100
    case TY_CSI_PS('m', 7):
        _currentScreen->setRendition(RE_REVERSE);
        break;
    case TY_CSI_PS('m', 8):
        _currentScreen->setRendition(RE_CONCEAL);
        break;
    case TY_CSI_PS('m', 9):
        _currentScreen->setRendition(RE_STRIKEOUT);
        break;
    case TY_CSI_PS('m', 53):
        _currentScreen->setRendition(RE_OVERLINE);
        break;
    case TY_CSI_PS('m', 10): /* IGNORED: mapping related          */
        break;                 // LINUX
    case TY_CSI_PS('m', 11): /* IGNORED: mapping related          */
        break;                 // LINUX
    case TY_CSI_PS('m', 12): /* IGNORED: mapping related          */
        break;                 // LINUX
    case TY_CSI_PS('m', 21):
        _currentScreen->resetRendition(RE_BOLD);
        break;
    case TY_CSI_PS('m', 22):
        _currentScreen->resetRendition(RE_BOLD);
        _currentScreen->resetRendition(RE_FAINT);
        break;
    case TY_CSI_PS('m', 23):
        _currentScreen->resetRendition(RE_ITALIC);
        break; // VT100
    case TY_CSI_PS('m', 24):
        _currentScreen->resetRendition(RE_UNDERLINE | RE_UNDERLINE_STYLE_MASK); // 关下划线并清样式位
        break;
    case TY_CSI_PS('m', 25):
        _currentScreen->resetRendition(RE_BLINK);
        break;
    case TY_CSI_PS('m', 27):
        _currentScreen->resetRendition(RE_REVERSE);
        break;
    case TY_CSI_PS('m', 28):
        _currentScreen->resetRendition(RE_CONCEAL);
        break;
    case TY_CSI_PS('m', 29):
        _currentScreen->resetRendition(RE_STRIKEOUT);
        break;
    case TY_CSI_PS('m', 55):
        _currentScreen->resetRendition(RE_OVERLINE);
        break;

    case TY_CSI_PS('m', 30):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 0);
        break;
    case TY_CSI_PS('m', 31):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 1);
        break;
    case TY_CSI_PS('m', 32):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 2);
        break;
    case TY_CSI_PS('m', 33):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 3);
        break;
    case TY_CSI_PS('m', 34):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 4);
        break;
    case TY_CSI_PS('m', 35):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 5);
        break;
    case TY_CSI_PS('m', 36):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 6);
        break;
    case TY_CSI_PS('m', 37):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 7);
        break;

    case TY_CSI_PS('m', 38):
        _currentScreen->setForeColor(p, q);
        break;

    case TY_CSI_PS('m', 39):
        _currentScreen->setForeColor(COLOR_SPACE_DEFAULT, 0);
        break;

    case TY_CSI_PS('m', 40):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 0);
        break;
    case TY_CSI_PS('m', 41):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 1);
        break;
    case TY_CSI_PS('m', 42):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 2);
        break;
    case TY_CSI_PS('m', 43):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 3);
        break;
    case TY_CSI_PS('m', 44):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 4);
        break;
    case TY_CSI_PS('m', 45):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 5);
        break;
    case TY_CSI_PS('m', 46):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 6);
        break;
    case TY_CSI_PS('m', 47):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 7);
        break;

    case TY_CSI_PS('m', 48):
        _currentScreen->setBackColor(p, q);
        break;

    case TY_CSI_PS('m', 49):
        _currentScreen->setBackColor(COLOR_SPACE_DEFAULT, 1);
        break;

    case TY_CSI_PS('m', 58):
        _currentScreen->setUnderlineColor(p, q);
        break;

    case TY_CSI_PS('m', 59):
        _currentScreen->setUnderlineColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
        break;

    case TY_CSI_PS('m', 90):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 8);
        break;
    case TY_CSI_PS('m', 91):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 9);
        break;
    case TY_CSI_PS('m', 92):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 10);
        break;
    case TY_CSI_PS('m', 93):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 11);
        break;
    case TY_CSI_PS('m', 94):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 12);
        break;
    case TY_CSI_PS('m', 95):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 13);
        break;
    case TY_CSI_PS('m', 96):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 14);
        break;
    case TY_CSI_PS('m', 97):
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, 15);
        break;

    case TY_CSI_PS('m', 100):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 8);
        break;
    case TY_CSI_PS('m', 101):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 9);
        break;
    case TY_CSI_PS('m', 102):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 10);
        break;
    case TY_CSI_PS('m', 103):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 11);
        break;
    case TY_CSI_PS('m', 104):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 12);
        break;
    case TY_CSI_PS('m', 105):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 13);
        break;
    case TY_CSI_PS('m', 106):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 14);
        break;
    case TY_CSI_PS('m', 107):
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, 15);
        break;

    case TY_CSI_PS('n', 5):
        reportStatus();
        break;
    case TY_CSI_PS('n', 6):
        reportCursorPosition();
        break;
    case TY_CSI_PS('q', 0): /* IGNORED: LEDs off                 */
        break;                // VT100
    case TY_CSI_PS('q', 1): /* IGNORED: LED1 on                  */
        break;                // VT100
    case TY_CSI_PS('q', 2): /* IGNORED: LED2 on                  */
        break;                // VT100
    case TY_CSI_PS('q', 3): /* IGNORED: LED3 on                  */
        break;                // VT100
    case TY_CSI_PS('q', 4): /* IGNORED: LED4 on                  */
        break;                // VT100
    case TY_CSI_PS('x', 0):
        reportTerminalParms(2);
        break; // VT100
    case TY_CSI_PS('x', 1):
        reportTerminalParms(3);
        break; // VT100

    case TY_CSI_PS_SP('q', 0): /* fall through */
    case TY_CSI_PS_SP('q', 1):
        emit cursorChanged(KeyboardCursorShape::BlockCursor, true);
        break;
    case TY_CSI_PS_SP('q', 2):
        emit cursorChanged(KeyboardCursorShape::BlockCursor, false);
        break;
    case TY_CSI_PS_SP('q', 3):
        emit cursorChanged(KeyboardCursorShape::UnderlineCursor, true);
        break;
    case TY_CSI_PS_SP('q', 4):
        emit cursorChanged(KeyboardCursorShape::UnderlineCursor, false);
        break;
    case TY_CSI_PS_SP('q', 5):
        emit cursorChanged(KeyboardCursorShape::IBeamCursor, true);
        break;
    case TY_CSI_PS_SP('q', 6):
        emit cursorChanged(KeyboardCursorShape::IBeamCursor, false);
        break;

    case TY_CSI_PN('@'):
        _currentScreen->insertChars(p);
        break;
    case TY_CSI_PN('A'):
        _currentScreen->cursorUp(p);
        break; // VT100
    case TY_CSI_PN('B'):
        _currentScreen->cursorDown(p);
        break; // VT100
    case TY_CSI_PN('C'):
        _currentScreen->cursorRight(p);
        break; // VT100
    case TY_CSI_PN('D'):
        _currentScreen->cursorLeft(p);
        break; // VT100
    case TY_CSI_PN('E'):
        _currentScreen->cursorNextLine(p);
        break; // VT100
    case TY_CSI_PN('F'):
        _currentScreen->cursorPreviousLine(p);
        break; // VT100
    case TY_CSI_PN('G'):
        _currentScreen->setCursorX(p);
        break; // LINUX
    case TY_CSI_PN('H'):
        _currentScreen->setCursorYX(p, q);
        break; // VT100
    case TY_CSI_PN('I'):
        _currentScreen->tab(p);
        break;
    case TY_CSI_PN('L'):
        _currentScreen->insertLines(p);
        break;
    case TY_CSI_PN('M'):
        _currentScreen->deleteLines(p);
        break;
    case TY_CSI_PN('P'):
        _currentScreen->deleteChars(p);
        break;
    case TY_CSI_PN('S'):
        _currentScreen->scrollUp(p);
        break;
    case TY_CSI_PN('T'):
        _currentScreen->scrollDown(p);
        break;
    case TY_CSI_PN('X'):
        _currentScreen->eraseChars(p);
        break;
    case TY_CSI_PN('Z'):
        _currentScreen->backtab(p);
        break;
    case TY_CSI_PN('b'):
        _currentScreen->repeatChars(p);
        break;
    case TY_CSI_PN('c'):
        reportTerminalType();
        break; // VT100
    case TY_CSI_PN('d'):
        _currentScreen->setCursorY(p);
        break; // LINUX
    case TY_CSI_PN('f'):
        _currentScreen->setCursorYX(p, q);
        break; // VT100
    case TY_CSI_PN('r'):
        setMargins(p, q);
        break;             // VT100
    case TY_CSI_PN('y'): /* IGNORED: Confidence test          */
        break;             // VT100

    case TY_CSI_PR('h', 1):
        setMode(MODE_AppCuKeys);
        break; // VT100
    case TY_CSI_PR('l', 1):
        resetMode(MODE_AppCuKeys);
        break; // VT100
    case TY_CSI_PR('s', 1):
        saveMode(MODE_AppCuKeys);
        break; // FIXME
    case TY_CSI_PR('r', 1):
        restoreMode(MODE_AppCuKeys);
        break; // FIXME

    case TY_CSI_PR('l', 2):
        resetMode(MODE_Ansi);
        break; // VT100

    case TY_CSI_PR('h', 3):
        setMode(MODE_132Columns);
        break; // VT100
    case TY_CSI_PR('l', 3):
        resetMode(MODE_132Columns);
        break; // VT100

    case TY_CSI_PR('h', 4): /* IGNORED: soft scrolling           */
        break;                // VT100
    case TY_CSI_PR('l', 4): /* IGNORED: soft scrolling           */
        break;                // VT100

    case TY_CSI_PR('h', 5):
        _currentScreen->setMode(MODE_Screen);
        break; // VT100
    case TY_CSI_PR('l', 5):
        _currentScreen->resetMode(MODE_Screen);
        break; // VT100

    case TY_CSI_PR('h', 6):
        _currentScreen->setMode(MODE_Origin);
        break; // VT100
    case TY_CSI_PR('l', 6):
        _currentScreen->resetMode(MODE_Origin);
        break; // VT100
    case TY_CSI_PR('s', 6):
        _currentScreen->saveMode(MODE_Origin);
        break; // FIXME
    case TY_CSI_PR('r', 6):
        _currentScreen->restoreMode(MODE_Origin);
        break; // FIXME

    case TY_CSI_PR('h', 7):
        _currentScreen->setMode(MODE_Wrap);
        break; // VT100
    case TY_CSI_PR('l', 7):
        _currentScreen->resetMode(MODE_Wrap);
        break; // VT100
    case TY_CSI_PR('s', 7):
        _currentScreen->saveMode(MODE_Wrap);
        break; // FIXME
    case TY_CSI_PR('r', 7):
        _currentScreen->restoreMode(MODE_Wrap);
        break; // FIXME

    case TY_CSI_PR('h', 8): /* IGNORED: autorepeat on            */
        break;                // VT100
    case TY_CSI_PR('l', 8): /* IGNORED: autorepeat off           */
        break;                // VT100
    case TY_CSI_PR('s', 8): /* IGNORED: autorepeat on            */
        break;                // VT100
    case TY_CSI_PR('r', 8): /* IGNORED: autorepeat off           */
        break;                // VT100

    case TY_CSI_PR('h', 9): /* IGNORED: interlace                */
        break;                // VT100
    case TY_CSI_PR('l', 9): /* IGNORED: interlace                */
        break;                // VT100
    case TY_CSI_PR('s', 9): /* IGNORED: interlace                */
        break;                // VT100
    case TY_CSI_PR('r', 9): /* IGNORED: interlace                */
        break;                // VT100

    case TY_CSI_PR('h', 12): /* IGNORED: Cursor blink             */
        break;                 // att610
    case TY_CSI_PR('l', 12): /* IGNORED: Cursor blink             */
        break;                 // att610
    case TY_CSI_PR('s', 12): /* IGNORED: Cursor blink             */
        break;                 // att610
    case TY_CSI_PR('r', 12): /* IGNORED: Cursor blink             */
        break;                 // att610

    case TY_CSI_PR('h', 25):
        setMode(MODE_Cursor);
        break; // VT100
    case TY_CSI_PR('l', 25):
        resetMode(MODE_Cursor);
        break; // VT100
    case TY_CSI_PR('s', 25):
        saveMode(MODE_Cursor);
        break; // VT100
    case TY_CSI_PR('r', 25):
        restoreMode(MODE_Cursor);
        break; // VT100

    case TY_CSI_PR('h', 40):
        setMode(MODE_Allow132Columns);
        break; // XTERM
    case TY_CSI_PR('l', 40):
        resetMode(MODE_Allow132Columns);
        break; // XTERM

    case TY_CSI_PR('h', 41): /* IGNORED: obsolete more(1) fix     */
        break;                 // XTERM
    case TY_CSI_PR('l', 41): /* IGNORED: obsolete more(1) fix     */
        break;                 // XTERM
    case TY_CSI_PR('s', 41): /* IGNORED: obsolete more(1) fix     */
        break;                 // XTERM
    case TY_CSI_PR('r', 41): /* IGNORED: obsolete more(1) fix     */
        break;                 // XTERM

    case TY_CSI_PR('h', 47):
        setMode(MODE_AppScreen);
        break; // VT100
    case TY_CSI_PR('l', 47):
        resetMode(MODE_AppScreen);
        break; // VT100
    case TY_CSI_PR('s', 47):
        saveMode(MODE_AppScreen);
        break; // XTERM
    case TY_CSI_PR('r', 47):
        restoreMode(MODE_AppScreen);
        break; // XTERM

    case TY_CSI_PR('h', 67): /* IGNORED: DECBKM                   */
        break;                 // XTERM
    case TY_CSI_PR('l', 67): /* IGNORED: DECBKM                   */
        break;                 // XTERM
    case TY_CSI_PR('s', 67): /* IGNORED: DECBKM                   */
        break;                 // XTERM
    case TY_CSI_PR('r', 67): /* IGNORED: DECBKM                   */
        break;                 // XTERM

        // XTerm defines the following modes:
        // SET_VT200_MOUSE             1000
        // SET_VT200_HIGHLIGHT_MOUSE   1001
        // SET_BTN_EVENT_MOUSE         1002
        // SET_ANY_EVENT_MOUSE         1003
        //

        // Note about mouse modes:
        // There are four mouse modes which xterm-compatible terminals can support -
        // 1000,1001,1002,1003 Konsole currently supports mode 1000 (basic mouse
        // press and release) and mode 1002 (dragging the mouse).
        // TODO:  Implementation of mouse modes 1001 (something called highlight
        // tracking) and 1003 (a slight variation on dragging the mouse)
        //

    case TY_CSI_PR('h', 1000):
        setMode(MODE_Mouse1000);
        break; // XTERM
    case TY_CSI_PR('l', 1000):
        resetMode(MODE_Mouse1000);
        break; // XTERM
    case TY_CSI_PR('s', 1000):
        saveMode(MODE_Mouse1000);
        break; // XTERM
    case TY_CSI_PR('r', 1000):
        restoreMode(MODE_Mouse1000);
        break; // XTERM

    case TY_CSI_PR('h', 1001): /* IGNORED: hilite mouse tracking    */
        break;                   // XTERM
    case TY_CSI_PR('l', 1001):
        resetMode(MODE_Mouse1001);
        break;                   // XTERM
    case TY_CSI_PR('s', 1001): /* IGNORED: hilite mouse tracking    */
        break;                   // XTERM
    case TY_CSI_PR('r', 1001): /* IGNORED: hilite mouse tracking    */
        break;                   // XTERM

    case TY_CSI_PR('h', 1002):
        setMode(MODE_Mouse1002);
        break; // XTERM
    case TY_CSI_PR('l', 1002):
        resetMode(MODE_Mouse1002);
        break; // XTERM
    case TY_CSI_PR('s', 1002):
        saveMode(MODE_Mouse1002);
        break; // XTERM
    case TY_CSI_PR('r', 1002):
        restoreMode(MODE_Mouse1002);
        break; // XTERM

    case TY_CSI_PR('h', 1003):
        setMode(MODE_Mouse1003);
        break; // XTERM
    case TY_CSI_PR('l', 1003):
        resetMode(MODE_Mouse1003);
        break; // XTERM
    case TY_CSI_PR('s', 1003):
        saveMode(MODE_Mouse1003);
        break; // XTERM
    case TY_CSI_PR('r', 1003):
        restoreMode(MODE_Mouse1003);
        break; // XTERM

    case TY_CSI_PR('h', 1004):
        _reportFocusEvents = true;
        break;
    case TY_CSI_PR('l', 1004):
        _reportFocusEvents = false;
        break;

    case TY_CSI_PR('h', 1005):
        setMode(MODE_Mouse1005);
        break; // XTERM
    case TY_CSI_PR('l', 1005):
        resetMode(MODE_Mouse1005);
        break; // XTERM
    case TY_CSI_PR('s', 1005):
        saveMode(MODE_Mouse1005);
        break; // XTERM
    case TY_CSI_PR('r', 1005):
        restoreMode(MODE_Mouse1005);
        break; // XTERM

    case TY_CSI_PR('h', 1006):
        setMode(MODE_Mouse1006);
        break; // XTERM
    case TY_CSI_PR('l', 1006):
        resetMode(MODE_Mouse1006);
        break; // XTERM
    case TY_CSI_PR('s', 1006):
        saveMode(MODE_Mouse1006);
        break; // XTERM
    case TY_CSI_PR('r', 1006):
        restoreMode(MODE_Mouse1006);
        break; // XTERM

    case TY_CSI_PR('h', 1015):
        setMode(MODE_Mouse1015);
        break; // URXVT
    case TY_CSI_PR('l', 1015):
        resetMode(MODE_Mouse1015);
        break; // URXVT
    case TY_CSI_PR('s', 1015):
        saveMode(MODE_Mouse1015);
        break; // URXVT
    case TY_CSI_PR('r', 1015):
        restoreMode(MODE_Mouse1015);
        break; // URXVT

    case TY_CSI_PR('h', 1034): /* IGNORED: 8bitinput activation     */
        break;                   // XTERM

    case TY_CSI_PR('h', 1047):
        setMode(MODE_AppScreen);
        break; // XTERM
    case TY_CSI_PR('l', 1047):
        _screen[1]->clearEntireScreen();
        resetMode(MODE_AppScreen);
        break; // XTERM
    case TY_CSI_PR('s', 1047):
        saveMode(MODE_AppScreen);
        break; // XTERM
    case TY_CSI_PR('r', 1047):
        restoreMode(MODE_AppScreen);
        break; // XTERM

    // FIXME: Unitoken: save translations
    case TY_CSI_PR('h', 1048):
        saveCursor();
        break; // XTERM
    case TY_CSI_PR('l', 1048):
        restoreCursor();
        break; // XTERM
    case TY_CSI_PR('s', 1048):
        saveCursor();
        break; // XTERM
    case TY_CSI_PR('r', 1048):
        restoreCursor();
        break; // XTERM

    // FIXME: every once new sequences like this pop up in xterm.
    //        Here's a guess of what they could mean.
    case TY_CSI_PR('h', 1049):
        saveCursor();
        _screen[1]->clearEntireScreen();
        setMode(MODE_AppScreen);
        break; // XTERM
    case TY_CSI_PR('l', 1049):
        resetMode(MODE_AppScreen);
        restoreCursor();
        break; // XTERM

    case TY_CSI_PR('h', 2004):
        setMode(MODE_BracketedPaste);
        break; // XTERM
    case TY_CSI_PR('l', 2004):
        resetMode(MODE_BracketedPaste);
        break; // XTERM
    case TY_CSI_PR('s', 2004):
        saveMode(MODE_BracketedPaste);
        break; // XTERM
    case TY_CSI_PR('r', 2004):
        restoreMode(MODE_BracketedPaste);
        break; // XTERM

    case TY_CSI_PR('h', 2026):
        if (!getMode(MODE_SynchronizedOutput))
            setMode(MODE_SynchronizedOutput); // 幂等：嵌套 set 不重复发信号
        break; // BSU：开始批量更新
    case TY_CSI_PR('l', 2026):
        if (getMode(MODE_SynchronizedOutput))
            resetMode(MODE_SynchronizedOutput);
        break; // ESU：结束批量更新

    // FIXME: weird DEC reset sequence
    case TY_CSI_PE('p'): /* IGNORED: reset         (        ) */
        break;

    // DECRQM — Request Mode (Host To Terminal)
    // ANSI mode queries: CSI Pd $ p  →  TY_CSI_PS('p', Pd)
    // NOTE: Screen-owned modes must be queried via _currentScreen->getMode()
    case TY_CSI_PS('p',   2) : reportAnsiMode( 2, 2); break; // KAM - Not supported
    case TY_CSI_PS('p',   4) : reportAnsiMode( 4, _currentScreen->getMode(MODE_Insert) ? 1 : 2); break; // IRM
    case TY_CSI_PS('p',  10) : reportAnsiMode(10, 4); break; // HEM - Permanently reset
    case TY_CSI_PS('p',  20) : reportAnsiMode(20, getMode(MODE_NewLine) ? 1 : 2); break; // LNM

    // DEC private mode queries: CSI ? Pd $ p  →  TY_CSI_PR('p', Pd)
    case TY_CSI_PR('p',   1) : reportDecMode(  1, getMode(MODE_AppCuKeys) ? 1 : 2); break; // DECCKM
    case TY_CSI_PR('p',   2) : reportDecMode(  2, getMode(MODE_Ansi) ? 1 : 2);      break; // DECANM
    case TY_CSI_PR('p',   3) : reportDecMode(  3, getMode(MODE_132Columns) ? 1 : 2); break; // DECCOLM
    case TY_CSI_PR('p',   4) : reportDecMode(  4, 4); break; // DECSCLM - Permanently reset
    case TY_CSI_PR('p',   5) : reportDecMode(  5, _currentScreen->getMode(MODE_Screen) ? 1 : 2); break; // DECSCNM
    case TY_CSI_PR('p',   6) : reportDecMode(  6, _currentScreen->getMode(MODE_Origin) ? 1 : 2); break; // DECOM
    case TY_CSI_PR('p',   7) : reportDecMode(  7, _currentScreen->getMode(MODE_Wrap) ? 1 : 2);   break; // DECAWM
    case TY_CSI_PR('p',   8) : reportDecMode(  8, 4); break; // DECARM - Permanently reset
    case TY_CSI_PR('p',   9) : reportDecMode(  9, 4); break; // DECINLM - Permanently reset
    case TY_CSI_PR('p',  10) : reportDecMode( 10, 4); break; // DECEDM - Permanently reset
    case TY_CSI_PR('p',  25) : reportDecMode( 25, _currentScreen->getMode(MODE_Cursor) ? 1 : 2); break; // DECTCEM
    case TY_CSI_PR('p',  47) : reportDecMode( 47, getMode(MODE_AppScreen) ? 1 : 2);            break; // Alt screen
    case TY_CSI_PR('p', 1000) : reportDecMode(1000, getMode(MODE_Mouse1000) ? 1 : 2);          break; // VT200 mouse
    case TY_CSI_PR('p', 1002) : reportDecMode(1002, getMode(MODE_Mouse1002) ? 1 : 2);          break; // Cell motion mouse
    case TY_CSI_PR('p', 1003) : reportDecMode(1003, getMode(MODE_Mouse1003) ? 1 : 2);          break; // All motion mouse
    case TY_CSI_PR('p', 1004) : reportDecMode(1004, _reportFocusEvents ? 1 : 2);               break; // Focus events
    case TY_CSI_PR('p', 1005) : reportDecMode(1005, getMode(MODE_Mouse1005) ? 1 : 2);          break; // UTF-8 mouse
    case TY_CSI_PR('p', 1006) : reportDecMode(1006, getMode(MODE_Mouse1006) ? 1 : 2);          break; // SGR mouse
    case TY_CSI_PR('p', 1015) : reportDecMode(1015, getMode(MODE_Mouse1015) ? 1 : 2);          break; // URXVT mouse
    case TY_CSI_PR('p', 1047) : reportDecMode(1047, getMode(MODE_AppScreen) ? 1 : 2);          break; // Alt screen (xterm)
    case TY_CSI_PR('p', 1049) : reportDecMode(1049, getMode(MODE_AppScreen) ? 1 : 2);          break; // Alt screen + cursor
    case TY_CSI_PR('p', 2004) : reportDecMode(2004, getMode(MODE_BracketedPaste) ? 1 : 2);     break; // Bracketed paste
    case TY_CSI_PR('p', 2026) : reportDecMode(2026, getMode(MODE_SynchronizedOutput) ? 1 : 2); break; // Synchronized output

    // FIXME: when changing between vt52 and ansi mode evtl do some resetting.
    case TY_VT52('A'):
        _currentScreen->cursorUp(1);
        break; // VT52
    case TY_VT52('B'):
        _currentScreen->cursorDown(1);
        break; // VT52
    case TY_VT52('C'):
        _currentScreen->cursorRight(1);
        break; // VT52
    case TY_VT52('D'):
        _currentScreen->cursorLeft(1);
        break; // VT52

    case TY_VT52('F'):
        setAndUseCharset(0, '0');
        break; // VT52
    case TY_VT52('G'):
        setAndUseCharset(0, 'B');
        break; // VT52

    case TY_VT52('H'):
        _currentScreen->setCursorYX(1, 1);
        break; // VT52
    case TY_VT52('I'):
        _currentScreen->reverseIndex();
        break; // VT52
    case TY_VT52('J'):
        _currentScreen->clearToEndOfScreen();
        break; // VT52
    case TY_VT52('K'):
        _currentScreen->clearToEndOfLine();
        break; // VT52
    case TY_VT52('Y'):
        _currentScreen->setCursorYX(p - 31, q - 31);
        break; // VT52
    case TY_VT52('Z'):
        reportTerminalType();
        break; // VT52
    case TY_VT52('<'):
        setMode(MODE_Ansi);
        break; // VT52
    case TY_VT52('='):
        setMode(MODE_AppKeyPad);
        break; // VT52
    case TY_VT52('>'):
        resetMode(MODE_AppKeyPad);
        break; // VT52

    case TY_CSI_PG('c'):
        reportSecondaryAttributes();
        break; // VT100

    /**
     * @brief kitty 键盘协议：CSI < [count] u（弹栈）/ CSI = flags ; mode u（设置）。
     */
    case TY_CSI_PL('u'):
        kittyFlagsPop(qMax(1, argv[0]));
        break;
    case TY_CSI_PQ('u'):
        kittyFlagsSet(argv[0], argc >= 1 ? argv[1] : 1);
        break;

    default:
        // Silently ignore all CSI '<' and '=' (private marker) sequences.
        // Token type 12 = TY_CSI_PQ ('='), 13 = TY_CSI_PL ('<');
        // these are consumed but unimplemented.
        if ((token & 0xff) != 12 && (token & 0xff) != 13)
            reportDecodingError();
        break;
    };
}

void Vt102Emulation::clearScreenAndSetColumns(int columnCount) {
    setImageSize(_currentScreen->getLines(), columnCount);
    clearEntireScreen();
    setDefaultMargins();
    _currentScreen->setCursorYX(0, 0);
}

void Vt102Emulation::sendString(const char *s, int length) {
    if (length >= 0)
        emit sendData(s, length);
    else
        emit sendData(s, static_cast<int>(strlen(s)));
}

void Vt102Emulation::reportCursorPosition() {
    const size_t sz = 20;
    char tmp[sz];
    const size_t r =
            snprintf(tmp, sz, "\033[%d;%dR", _currentScreen->getCursorY() + 1,
                             _currentScreen->getCursorX() + 1);
    if (sz <= r) {
        qWarning("Vt102Emulation::reportCursorPosition: Buffer too small\n");
    }
    sendString(tmp);
}

void Vt102Emulation::reportTerminalType() {
    // Primary device attribute response (Request was: ^[[0c or ^[[c (from TT321
    // Users Guide)) VT220:  ^[[?63;1;2;3;6;7;8c   (list deps on emul.
    // capabilities) VT100:  ^[[?1;2c VT101:  ^[[?1;0c VT102:  ^[[?6v
    if (getMode(MODE_Ansi))
        sendString("\033[?1;2c"); // I'm a VT100
    else
        sendString("\033/Z"); // I'm a VT52
}

void Vt102Emulation::reportSecondaryAttributes() {
    // Secondary device attribute response (Request was: ^[[>0c or ^[[>c)
    if (getMode(MODE_Ansi))
        sendString("\033[>0;115;0c"); // Why 115?  ;)
    else
        sendString("\033/Z"); // FIXME I don't think VT52 knows about it but kept
                                                    // for konsoles backward compatibility.
}

void Vt102Emulation::reportTerminalParms(int p)
// DECREPTPARM
{
    const size_t sz = 100;
    char tmp[sz];
    const size_t r =
            snprintf(tmp, sz, "\033[%d;1;1;112;112;1;0x", p); // not really true.
    if (sz <= r) {
        qWarning("Vt102Emulation::reportTerminalParms: Buffer too small\n");
    }
    sendString(tmp);
}

void Vt102Emulation::reportStatus() {
    sendString("\033[0n"); // VT100. Device status report. 0 = Ready.
}

// DECRPM — Report Mode (Terminal To Host), response to DECRQM
// Responds to an ANSI mode query (CSI Pd $ p) with: CSI Pd ; Pm $ y
void Vt102Emulation::reportAnsiMode(int mode, int status)
{
    const size_t sz = 32;
    char tmp[sz];
    const size_t r = snprintf(tmp, sz, "\033[%d;%d$y", mode, status);
    if (sz <= r)
        qWarning("Vt102Emulation::reportAnsiMode: Buffer too small\n");
    sendString(tmp);
}

// DECRPM — Report Mode (Terminal To Host), response to DECRQM
// Responds to a DEC private mode query (CSI ? Pd $ p) with: CSI ? Pd ; Pm $ y
void Vt102Emulation::reportDecMode(int mode, int status)
{
    const size_t sz = 32;
    char tmp[sz];
    const size_t r = snprintf(tmp, sz, "\033[?%d;%d$y", mode, status);
    if (sz <= r)
        qWarning("Vt102Emulation::reportDecMode: Buffer too small\n");
    sendString(tmp);
}

void Vt102Emulation::reportAnswerBack() {
    // FIXME - Test this with VTTEST
    // This is really obsolete VT100 stuff.
    const char *ANSWER_BACK = "";
    sendString(ANSWER_BACK);
}

/*!
        `cx',`cy' are 1-based.
        `cb' indicates the button pressed or released (0-2) or scroll event (4-5).

        eventType represents the kind of mouse action that occurred:
                0 = Mouse button press
                1 = Mouse drag
                2 = Mouse button release
*/

void Vt102Emulation::sendMouseEvent(int cb, int cx, int cy, int eventType) {
    if (cx < 1 || cy < 1)
        return;

    // With the exception of the 1006 mode, button release is encoded in cb.
    // Note that if multiple extensions are enabled, the 1006 is used, so it's
    // okay to check for only that.
    if (eventType == 2 && !getMode(MODE_Mouse1006))
        cb = 3;

    // normal buttons are passed as 0x20 + button,
    // mouse wheel (buttons 4,5) as 0x5c + button
    if (cb >= 4)
        cb += 0x3c;

    // Mouse motion handling
    if ((getMode(MODE_Mouse1002) || getMode(MODE_Mouse1003)) && eventType == 1)
        cb += 0x20; // add 32 to signify motion event

    char command[64];
    command[0] = '\0';
    // Check the extensions in decreasing order of preference. Encoding the
    // release event above assumes that 1006 comes first.
    if (getMode(MODE_Mouse1006)) {
        snprintf(command, sizeof(command), "\033[<%d;%d;%d%c", cb, cx, cy,
                         eventType == 2 ? 'm' : 'M');
    } else if (getMode(MODE_Mouse1015)) {
        snprintf(command, sizeof(command), "\033[%d;%d;%dM", cb + 0x20, cx, cy);
    } else if (getMode(MODE_Mouse1005)) {
        if (cx <= 2015 && cy <= 2015) {
            // The xterm extension uses UTF-8 (up to 2 bytes) to encode
            // coordinate+32, no matter what the locale is. We could easily
            // convert manually, but QString can also do it for us.
            QChar coords[2];
            coords[0] = static_cast<char16_t>(cx + 0x20);
            coords[1] = static_cast<char16_t>(cy + 0x20);
            QString coordsStr = QString(coords, 2);
            QByteArray utf8 = coordsStr.toUtf8();
            snprintf(command, sizeof(command), "\033[M%c%s", cb + 0x20,
                             utf8.constData());
        }
    } else if (cx <= 223 && cy <= 223) {
        snprintf(command, sizeof(command), "\033[M%c%c%c", cb + 0x20, cx + 0x20,
                         cy + 0x20);
    }

    sendString(command);
}

/**
 * The focus lost event can be used by Vim (or other terminal applications)
 * to recognize that the konsole window has lost focus.
 * The escape sequence is also used by iTerm2.
 * Vim needs the following plugin to be installed to convert the escape
 * sequence into the FocusLost autocmd: https://github.com/sjl/vitality.vim
 */
void Vt102Emulation::focusLost(void) {
    if (_reportFocusEvents)
        sendString("\033[O");
}

/**
 * The focus gained event can be used by Vim (or other terminal applications)
 * to recognize that the konsole window has gained focus again.
 * The escape sequence is also used by iTerm2.
 * Vim needs the following plugin to be installed to convert the escape
 * sequence into the FocusGained autocmd: https://github.com/sjl/vitality.vim
 */
void Vt102Emulation::focusGained(void) {
    if (_reportFocusEvents)
        sendString("\033[I");
}

void Vt102Emulation::sendText(const QString &text) {
    if (!text.isEmpty()) {
        QKeyEvent event(QEvent::KeyPress, 0, Qt::NoModifier, text);
        sendKeyEvent(&event, false); // expose as a big fat keypress event
    }
}

void Vt102Emulation::sendKeyEvent(QKeyEvent *event, bool fromPaste) {
    // kitty 键盘协议（级别 1+2）：协商 flags 生效时优先于传统编码；
    // 粘贴文本（fromPaste）与无键码事件（sendText 合成的 key()==0）不参与
    if (_kittyFlags != 0 && !fromPaste && event->key() != 0) {
        QByteArray encoded;
        if (encodeKittyKeyEvent(event, encoded)) {
            if (!encoded.isEmpty()) {
                if (event->type() != QEvent::KeyRelease)
                    emit outputFromKeypressEvent();
                emit sendData(encoded.constData(), encoded.length());
            }
            return;
        }
        // 未命中 kitty 编码的无歧义键（方向键/F1-F12 等）：
        // 按下/重复回落下方传统编码；释放事件按 kitty 规范吞掉，
        // 否则传统路径不区分按下/释放，每次释放会重发按下序列（双发）
        if (event->type() == QEvent::KeyRelease)
            return;
    } else if (event->type() == QEvent::KeyRelease) {
        // 未协商 kitty：释放事件无传统编码，不消费，
        // ignore 以便 TerminalDisplay 恢复事件向上传播的默认语义
        event->ignore();
        return;
    }

    Qt::KeyboardModifiers modifiers = event->modifiers();
    KeyboardTranslator::States states = KeyboardTranslator::NoState;

    // get current states
    if (getMode(MODE_NewLine))
        states |= KeyboardTranslator::NewLineState;
    if (getMode(MODE_Ansi))
        states |= KeyboardTranslator::AnsiState;
    if (getMode(MODE_AppCuKeys))
        states |= KeyboardTranslator::CursorKeysState;
    if (getMode(MODE_AppScreen))
        states |= KeyboardTranslator::AlternateScreenState;
    if (getMode(MODE_AppKeyPad) && (modifiers & Qt::KeypadModifier))
        states |= KeyboardTranslator::ApplicationKeypadState;

    // check flow control state
    if (modifiers & KeyboardTranslator::CTRL_MOD) {
        switch (event->key()) {
        case Qt::Key_S:
            emit flowControlKeyPressed(true);
            break;
        case Qt::Key_Q:
        case Qt::Key_C: // cancel flow control
            emit flowControlKeyPressed(false);
            break;
        }
    }

    // lookup key binding
    if (_keyTranslator) {
        KeyboardTranslator::Entry entry =
                _keyTranslator->findEntry(event->key(), modifiers, states);

        if ((modifiers & Qt::AltModifier) && (event->key() == Qt::Key_Left)) {
            entry = _keyTranslator->findEntry(Qt::Key_Home, Qt::NoModifier, states);
            modifiers = Qt::NoModifier;
        } else if ((modifiers & Qt::AltModifier) &&
                             (event->key() == Qt::Key_Right)) {
            entry = _keyTranslator->findEntry(Qt::Key_End, Qt::NoModifier, states);
            modifiers = Qt::NoModifier;
        }
#if defined(Q_OS_MACOS)
        if ((modifiers & Qt::ControlModifier) &&
                (event->key() == Qt::Key_Backspace)) {
            entry = _keyTranslator->findEntry(Qt::Key_Delete, Qt::NoModifier, states);
            modifiers = Qt::NoModifier;
        }
#endif
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
        if (_enableHandleCtrlC && (modifiers & Qt::ControlModifier) &&
                (event->key() == Qt::Key_C)) {
            bool isSelection = !_currentScreen->isClearSelection();
            if (isSelection) {
                emit handleCtrlC();
                return;
            }
        }
#endif

        // send result to terminal
        QByteArray textToSend;

        // special handling for the Alt (aka. Meta) modifier.  pressing
        // Alt+[Character] results in Esc+[Character] being sent
        // (unless there is an entry defined for this particular combination
        //  in the keyboard modifier)
        bool wantsAltModifier =
                entry.modifiers() & entry.modifierMask() & Qt::AltModifier;
        bool wantsMetaModifier =
                entry.modifiers() & entry.modifierMask() & Qt::MetaModifier;
        bool wantsAnyModifier = entry.state() & entry.stateMask() &
                                                        KeyboardTranslator::AnyModifierState;

        if (modifiers & Qt::AltModifier &&
                !(wantsAltModifier || wantsAnyModifier) && !event->text().isEmpty()) {
            textToSend.prepend("\033");
        }
        if (modifiers & Qt::MetaModifier &&
                !(wantsMetaModifier || wantsAnyModifier) && !event->text().isEmpty()) {
            textToSend.prepend("\030@s");
        }

        if (entry.command() != KeyboardTranslator::NoCommand) {
            if (entry.command() & KeyboardTranslator::EraseCommand) {
                textToSend += eraseChar();
            } else {
                emit handleCommandFromKeyboard(entry.command());
            }

            // TODO command handling
        } else if (!entry.text().isEmpty()) {
            textToSend += entry.text(true, modifiers);
        } else if ((modifiers & KeyboardTranslator::CTRL_MOD) &&
                             event->key() >= 0x40 && event->key() < 0x5f) {
            textToSend += (event->key() & 0x1f);
        } else if (event->key() == Qt::Key_Tab) {
            textToSend += 0x09;
        } else if (event->key() == Qt::Key_PageUp) {
            textToSend += "\033[5~";
        } else if (event->key() == Qt::Key_PageDown) {
            textToSend += "\033[6~";
        } else {
            textToSend += _fromUtf16(event->text());
        }

        if (!fromPaste && textToSend.length()) {
            emit outputFromKeypressEvent();
        }
        emit sendData(textToSend.constData(), textToSend.length());
    } else {
        // print an error message to the terminal if no key translator has been
        // set
        QString translatorError =
                tr("No keyboard translator available.  "
                     "The information needed to convert key presses "
                     "into characters to send to the terminal "
                     "is missing.");
        reset();
        receiveData(translatorError.toUtf8().constData(), translatorError.size());
    }
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                VT100 Charsets                             */
/*                                                                           */
/* ------------------------------------------------------------------------- */

// Character Set Conversion ------------------------------------------------ --

/*
     The processing contains a VT100 specific code translation layer.
     It's still in use and mainly responsible for the line drawing graphics.

     These and some other glyphs are assigned to codes (0x5f-0xfe)
     normally occupied by the latin letters. Since this codes also
     appear within control sequences, the extra code conversion
     does not permute with the tokenizer and is placed behind it
     in the pipeline. It only applies to tokens, which represent
     plain characters.

     This conversion it eventually continued in TerminalDisplay.C, since
     it might involve VT100 enhanced fonts, which have these
     particular glyphs allocated in (0x00-0x1f) in their code page.
*/

#define CHARSET _charset[_currentScreen == _screen[1]]

// Apply current character map.

char32_t Vt102Emulation::applyCharset(char32_t c) {
    // assert for i in [0..31] : vt100extended(vt100_graphics[i]) == i.
    const unsigned short vt100_graphics[32] = {
            // 0/8     1/9    2/10    3/11    4/12    5/13    6/14    7/15
            0x0020, 0x25C6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
            0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
            0xF800, 0xF801, 0x2500, 0xF803, 0xF804, 0x251c, 0x2524, 0x2534,
            0x252c, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00b7};
    if (CHARSET.graphic && 0x5f <= c && c <= 0x7e)
        return vt100_graphics[c - 0x5f];
    if (CHARSET.pound && c == '#')
        return 0xa3; // This mode is obsolete
    return c;
}

/*
     "Charset" related part of the emulation state.
     This configures the VT100 charset filter.

     While most operation work on the current _screen,
     the following two are different.
*/

void Vt102Emulation::resetCharset(int scrno) {
    _charset[scrno].cu_cs = 0;
    qstrncpy(_charset[scrno].charset, "BBBB", 4);
    _charset[scrno].sa_graphic = false;
    _charset[scrno].sa_pound = false;
    _charset[scrno].graphic = false;
    _charset[scrno].pound = false;
}

void Vt102Emulation::setCharset(int n, int cs) // on both screens.
{
    _charset[0].charset[n & 3] = cs;
    useCharset(_charset[0].cu_cs);
    _charset[1].charset[n & 3] = cs;
    useCharset(_charset[1].cu_cs);
}

void Vt102Emulation::setAndUseCharset(int n, int cs) {
    CHARSET.charset[n & 3] = cs;
    useCharset(n & 3);
}

void Vt102Emulation::useCharset(int n) {
    CHARSET.cu_cs = n & 3;
    CHARSET.graphic = (CHARSET.charset[n & 3] == '0');
    CHARSET.pound = (CHARSET.charset[n & 3] == 'A'); // This mode is obsolete
}

void Vt102Emulation::setDefaultMargins() {
    _screen[0]->setDefaultMargins();
    _screen[1]->setDefaultMargins();
}

void Vt102Emulation::setMargins(int t, int b) {
    _screen[0]->setMargins(t, b);
    _screen[1]->setMargins(t, b);
}

void Vt102Emulation::saveCursor() {
    CHARSET.sa_graphic = CHARSET.graphic;
    CHARSET.sa_pound = CHARSET.pound; // This mode is obsolete
    // we are not clear about these
    // sa_charset = charsets[cScreen->_charset];
    // sa_charset_num = cScreen->_charset;
    _currentScreen->saveCursor();
}

void Vt102Emulation::restoreCursor() {
    CHARSET.graphic = CHARSET.sa_graphic;
    CHARSET.pound = CHARSET.sa_pound; // This mode is obsolete
    _currentScreen->restoreCursor();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Mode Operations                            */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/*
     Some of the emulations state is either added to the state of the screens.

     This causes some scoping problems, since different emulations choose to
     located the mode either to the current _screen or to both.

     For strange reasons, the extend of the rendition attributes ranges over
     all screens and not over the actual _screen.

     We decided on the precise precise extend, somehow.
*/

// "Mode" related part of the state. These are all booleans.

void Vt102Emulation::resetModes() {
    // MODE_Allow132Columns is not reset here
    // to match Xterm's behaviour (see Xterm's VTReset() function)

    resetMode(MODE_132Columns);
    saveMode(MODE_132Columns);
    resetMode(MODE_Mouse1000);
    saveMode(MODE_Mouse1000);
    resetMode(MODE_Mouse1001);
    saveMode(MODE_Mouse1001);
    resetMode(MODE_Mouse1002);
    saveMode(MODE_Mouse1002);
    resetMode(MODE_Mouse1003);
    saveMode(MODE_Mouse1003);
    resetMode(MODE_Mouse1005);
    saveMode(MODE_Mouse1005);
    resetMode(MODE_Mouse1006);
    saveMode(MODE_Mouse1006);
    resetMode(MODE_Mouse1015);
    saveMode(MODE_Mouse1015);
    resetMode(MODE_BracketedPaste);
    saveMode(MODE_BracketedPaste);
    resetMode(MODE_SynchronizedOutput);
    saveMode(MODE_SynchronizedOutput);

    resetMode(MODE_AppScreen);
    saveMode(MODE_AppScreen);
    resetMode(MODE_AppCuKeys);
    saveMode(MODE_AppCuKeys);
    resetMode(MODE_AppKeyPad);
    saveMode(MODE_AppKeyPad);
    resetMode(MODE_NewLine);
    setMode(MODE_Ansi);
}

void Vt102Emulation::setMode(int m) {
    _currentModes.mode[m] = true;
    switch (m) {
    case MODE_132Columns:
        if (getMode(MODE_Allow132Columns))
            clearScreenAndSetColumns(132);
        else
            _currentModes.mode[m] = false;
        break;
    case MODE_Mouse1000:
    case MODE_Mouse1001:
    case MODE_Mouse1002:
    case MODE_Mouse1003:
        emit programUsesMouseChanged(false);
        break;

    case MODE_BracketedPaste:
        emit programBracketedPasteModeChanged(true);
        break;

    case MODE_SynchronizedOutput:
        emit synchronizedOutputModeChanged(true);
        break;

    case MODE_AppScreen:
        _screen[1]->clearSelection();
        setScreen(1);
        break;
    }
    if (m < MODES_SCREEN || m == MODE_NewLine) {
        _screen[0]->setMode(m);
        _screen[1]->setMode(m);
    }
}

void Vt102Emulation::resetMode(int m) {
    _currentModes.mode[m] = false;
    switch (m) {
    case MODE_132Columns:
        if (getMode(MODE_Allow132Columns))
            clearScreenAndSetColumns(80);
        break;
    case MODE_Mouse1000:
    case MODE_Mouse1001:
    case MODE_Mouse1002:
    case MODE_Mouse1003:
        emit programUsesMouseChanged(true);
        break;

    case MODE_BracketedPaste:
        emit programBracketedPasteModeChanged(false);
        break;

    case MODE_SynchronizedOutput:
        emit synchronizedOutputModeChanged(false);
        break;

    case MODE_AppScreen:
        _screen[0]->clearSelection();
        setScreen(0);
        break;
    }
    if (m < MODES_SCREEN || m == MODE_NewLine) {
        _screen[0]->resetMode(m);
        _screen[1]->resetMode(m);
    }
}

void Vt102Emulation::saveMode(int m) {
    _savedModes.mode[m] = _currentModes.mode[m];
}

void Vt102Emulation::restoreMode(int m) {
    if (_savedModes.mode[m])
        setMode(m);
    else
        resetMode(m);
}

bool Vt102Emulation::getMode(int m) { return _currentModes.mode[m]; }

char Vt102Emulation::eraseChar() const {
    KeyboardTranslator::Entry entry = _keyTranslator->findEntry(
            Qt::Key_Backspace, Qt::NoModifier, KeyboardTranslator::NoState);
    if (entry.text().size() > 0)
        return entry.text().at(0);
    else
        return '\b';
}

/**
 * @brief 压入新的 kitty flags（CSI > flags u）。
 * @param flags 请求的功能位；未实现的 4/8/16 位按 KITTY_FLAGS_SUPPORTED 掩掉。
 * @note 栈满拒绝（防 DoS）：当前 flags 与栈内容均不变。
 */
void Vt102Emulation::kittyFlagsPush(quint32 flags) {
    if (_kittyFlagsStack.size() >= KITTY_FLAGS_STACK_MAX) {
        qWarning("Vt102Emulation: kitty keyboard flags stack full, push rejected");
        return;
    }
    _kittyFlagsStack.append(_kittyFlags);
    _kittyFlags = flags & KITTY_FLAGS_SUPPORTED;
}

/**
 * @brief 弹出 count 层历史 flags（CSI < [count] u）。
 * @param count 弹出层数；栈已空时所有 flags 复位为 0。
 */
void Vt102Emulation::kittyFlagsPop(int count) {
    for (int i = 0; i < count; i++) {
        if (_kittyFlagsStack.isEmpty()) {
            _kittyFlags = 0;
            break;
        }
        _kittyFlags = _kittyFlagsStack.takeLast();
    }
}

/**
 * @brief 设置 kitty flags（CSI = flags ; mode u）。
 * @param flags 目标功能位（先按 KITTY_FLAGS_SUPPORTED 掩码）。
 * @param mode 1=整体设置（默认）；2=置位指定位；3=复位指定位；其余非法值忽略。
 */
void Vt102Emulation::kittyFlagsSet(quint32 flags, int mode) {
    flags &= KITTY_FLAGS_SUPPORTED;
    switch (mode) {
    case 1: _kittyFlags = flags; break;
    case 2: _kittyFlags |= flags; break;
    case 3: _kittyFlags &= ~flags; break;
    default: break;
    }
}

/**
 * @brief 应答 CSI ? flags u：flags 如实上报（仅含已实现的级别 1+2）。
 */
void Vt102Emulation::reportKittyKeyboardFlags() {
    char tmp[16];
    const int r = snprintf(tmp, sizeof(tmp), "\033[?%uu", _kittyFlags);
    if (r <= 0 || r >= static_cast<int>(sizeof(tmp)))
        return;
    sendString(tmp);
}

void Vt102Emulation::reportDecodingError() {
    if (tokenBufferPos == 0 || (tokenBufferPos == 1 && (tokenBuffer[0] & 0xff) >= 32))
        return;
    //qDebug()<< "Undecodable sequence:" << QString::fromUcs4(tokenBuffer, tokenBufferPos);
}

/**
 * @brief 计算 kitty 键盘协议的修饰键参数。
 * @param modifiers Qt 修饰键。
 * @return 编码值 = 位和 + 1（shift=1 alt=2 ctrl=4 super=8）。
 */
static int kittyModifierParam(Qt::KeyboardModifiers modifiers) {
    int bits = 0;
    if (modifiers & Qt::ShiftModifier)   bits |= 1;
    if (modifiers & Qt::AltModifier)     bits |= 2;
    if (modifiers & Qt::ControlModifier) bits |= 4;
    if (modifiers & Qt::MetaModifier)    bits |= 8; // super
    return bits + 1;
}

bool Vt102Emulation::encodeKittyKeyEvent(QKeyEvent *event, QByteArray &out) {
    out.clear();

    const int key = event->key();
    // 纯修饰键本身不产生事件（级别 8 未实现）：吞掉，不回落传统路径
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt
            || key == Qt::Key_Meta)
        return true;

    // 键码：特殊键取 kitty 功能键码；可打印键取未 shift 形态码点（字母一律小写）
    int codepoint = 0;
    const bool isEnterTabBackspace =
            (key == Qt::Key_Return || key == Qt::Key_Enter
             || key == Qt::Key_Tab || key == Qt::Key_Backspace);
    switch (key) {
    case Qt::Key_Escape:    codepoint = 27;  break;
    case Qt::Key_Return:
    case Qt::Key_Enter:     codepoint = 13;  break;
    case Qt::Key_Tab:       codepoint = 9;   break;
    case Qt::Key_Backspace: codepoint = 127; break;
    default:
        if (key >= 0x20 && key <= 0x10FFFF) {
            codepoint = key;
            if (codepoint >= 'A' && codepoint <= 'Z')
                codepoint += 32; // kitty 要求未 shift（小写）码点
        }
        break;
    }
    if (codepoint == 0)
        return false; // 未覆盖的功能键（方向键等本已无歧义）：回落传统编码

    const int modBits = kittyModifierParam(event->modifiers()) - 1;
    const bool hasCtrlAltSuper = (modBits & (2 | 4 | 8)) != 0;

    // 事件类型：级别 2 才上报重复（2）与释放（3）；按下恒为 1
    int eventType = 1;
    if (_kittyFlags & 2) {
        if (event->type() == QEvent::KeyRelease)
            eventType = 3;
        else if (event->isAutoRepeat())
            eventType = 2;
    } else if (event->type() == QEvent::KeyRelease) {
        return true; // 未协商事件类型：吞掉释放事件
    }

    // 级别 1（消歧义）下需要 CSI u 编码的键：
    // Esc（任意修饰）；Enter/Tab/Backspace 带修饰；可打印键带 ctrl/alt/super
    bool useKittyForm = false;
    if (_kittyFlags & 1) {
        if (key == Qt::Key_Escape)
            useKittyForm = true;
        else if (isEnterTabBackspace)
            useKittyForm = (modBits != 0);
        else
            useKittyForm = hasCtrlAltSuper;
    }

    if (!useKittyForm) {
        // 无歧义键：按下/重复回落传统编码；释放事件按 kitty 规范不上报（级别 8 未实现）
        return event->type() == QEvent::KeyRelease;
    }

    out = "\033[" + QByteArray::number(codepoint);
    const int modParam = modBits + 1;
    if (_kittyFlags & 2)
        out += ";" + QByteArray::number(modParam) + ":" + QByteArray::number(eventType);
    else if (modParam != 1)
        out += ";" + QByteArray::number(modParam);
    out += "u";
    return true;
}
