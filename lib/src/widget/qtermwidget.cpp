/*  
 Copyright (C) 2008 e_k (e_k@users.sourceforge.net)

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License as published by the Free Software Foundation; either
 version 2 of the License, or (at your option) any later version.
 
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.
 
 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
*/
#include <QLayout>
#include <QBoxLayout>
#include <QtDebug>
#include <QDir>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTranslator>
#include <QLocale>
#include <QCoreApplication>

#include "CharacterColor.h"
#include "Screen.h"
#include "ScreenWindow.h"
#include "Emulation.h"
#include "TerminalDisplay.h"
#include "Vt102Emulation.h"
#include "KeyboardTranslator.h"
#include "ColorScheme.h"
#include "SearchBar.h"
#include "qtermwidget.h"

#define QTERMW_HLIGHT "qtermw_hlight"


/**
 * @brief 强制初始化内嵌在静态库中的 res.qrc 资源（配色方案、键位布局）。
 *
 * 静态库中的 qrc 对象文件若无人引用会被链接器丢弃，必须显式初始化。
 * 本函数所在的 qtermwidget.cpp 编译单元必然随 QTermWidget 一起被链接，
 * 因此对 qInitResources_res() 的引用能把 res 资源对象从归档中拉入最终二进制。
 */
static void initEmbeddedResources()
{
    Q_INIT_RESOURCE(res);
}

// 按系统 locale 从 qrc 加载库自身翻译；QTranslator 有意不释放，生命周期与 QCoreApplication 一致。
static void installQTermWidgetTranslator()
{
#ifdef QTERMWIDGET_EMBED_TRANSLATIONS
    // 静态库中的 qrc 对象文件若无人引用会被链接器丢弃，必须显式初始化。
    Q_INIT_RESOURCE(qtermwidget_translations);
#endif
    static QTranslator *translator = [] {
        auto *t = new QTranslator(QCoreApplication::instance());
        if (t->load(QLocale::system(), QStringLiteral("qtermwidget"), QStringLiteral("_"),
                    QStringLiteral(":/lib/qtermwidget/translations"))) {
            QCoreApplication::installTranslator(t);
        } else {
            delete t;
            t = nullptr;
        }
        return t;
    }();
    Q_UNUSED(translator);
}

QTermWidget::QTermWidget(QWidget *msgParent, QWidget *parent)
    : QWidget(parent) {
    initEmbeddedResources();
    installQTermWidgetTranslator();
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    setLayout(m_layout);

    m_terminalDisplay = new TerminalDisplay(this);
    m_emulation = new Vt102Emulation();
    m_terminalDisplay->setBellMode(TerminalDisplay::SystemBeepBell);
    m_terminalDisplay->setTerminalSizeHint(true);
    m_terminalDisplay->setTripleClickMode(TerminalDisplay::SelectWholeLine);
    m_terminalDisplay->setTerminalSizeStartup(true);

    connect(m_terminalDisplay, &TerminalDisplay::keyPressedSignal, m_emulation, &Emulation::sendKeyEvent);
    connect(m_terminalDisplay, &TerminalDisplay::keyReleasedSignal, this, [this](QKeyEvent *e) {
        m_emulation->sendKeyEvent(e, false);
    });
    connect(m_terminalDisplay, &TerminalDisplay::mouseSignal, m_emulation, &Emulation::sendMouseEvent);
    connect(m_terminalDisplay, &TerminalDisplay::sendStringToEmu, this, [this](const char* s){
        m_emulation->sendString(s);
    });
    connect(m_emulation, &Emulation::stateSet, this, &QTermWidget::activityStateSet);
    //setup timer for monitoring session activity
    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setSingleShot(true);
    connect(m_monitorTimer, &QTimer::timeout, this, &QTermWidget::monitorTimerDone);
    // allow emulation to notify view when the foreground process
    // indicates whether or not it is interested in mouse signals
    connect(m_emulation, &Emulation::programUsesMouseChanged, m_terminalDisplay, &TerminalDisplay::setUsesMouse);
    m_terminalDisplay->setUsesMouse(m_emulation->programUsesMouse());
    connect(m_emulation, &Emulation::programBracketedPasteModeChanged, m_terminalDisplay, &TerminalDisplay::setBracketedPasteMode);
    m_terminalDisplay->setBracketedPasteMode(m_emulation->programBracketedPasteMode());
    connect(m_emulation, &Emulation::synchronizedOutputModeChanged, m_terminalDisplay, &TerminalDisplay::setSynchronizedOutputMode);
    // sixel 图像锚定需要真实单元格像素尺寸：字体度量变化时同步给仿真层，并立即同步一次初值
    connect(m_terminalDisplay, &TerminalDisplay::changedFontMetricSignal, this, [this](int height, int width){
        m_emulation->setCellPixelSize(width, height);
    });
    m_emulation->setCellPixelSize(m_terminalDisplay->cellPixelWidth(), m_terminalDisplay->cellPixelHeight());
    m_terminalDisplay->setScreenWindow(m_emulation->createWindow());
    connect(m_emulation, &Emulation::primaryScreenInUse, m_terminalDisplay, &TerminalDisplay::usingPrimaryScreen);
    connect(m_emulation, &Emulation::imageSizeChanged, this, [this](int /*height*/, int /*width*/){
        updateTerminalSize();
    });
    connect(m_terminalDisplay, &TerminalDisplay::changedContentSizeSignal, this, [this](int /*height*/, int /*width*/){
        updateTerminalSize();
    });

    setFlowControlEnabled(true);
    m_emulation->setCodec(QStringEncoder{QStringConverter::Encoding::Utf8});
    m_emulation->setHistory(HistoryTypeBuffer(1000));
    m_emulation->setKeyBindings(QString());

    m_layout->addWidget(m_terminalDisplay);
    m_terminalDisplay->setObjectName("terminalDisplay");
    setMessageParentWidget(msgParent ? msgParent : this);

    connect(m_terminalDisplay, &TerminalDisplay::notifyBell, this, &QTermWidget::notifyBell);
    connect(m_terminalDisplay, &TerminalDisplay::handleCtrlC, this, &QTermWidget::handleCtrlC);
    connect(m_terminalDisplay, &TerminalDisplay::changedContentCountSignal, this, &QTermWidget::termSizeChange);
    connect(m_terminalDisplay, &TerminalDisplay::mousePressEventForwarded, this, &QTermWidget::mousePressEventForwarded);
    connect(m_emulation, &Emulation::profileChangeCommandReceived, this, &QTermWidget::profileChanged);
    connect(m_emulation, &Emulation::zmodemRecvDetected, this, &QTermWidget::zmodemRecvDetected);
    connect(m_emulation, &Emulation::zmodemSendDetected, this, &QTermWidget::zmodemSendDetected);
    connect(m_emulation, &Emulation::titleChanged, this, &QTermWidget::titleChanged);
    // redirect data from TTY to external recipient
    connect(m_emulation, &Emulation::sendData, this, [this](const char *buff, int len) {
        if (m_echo) {
            recvData(buff, len);
        }
        emit sendData(buff, len);
    });
    connect( m_emulation, &Emulation::dupDisplayOutput, this, &QTermWidget::dupDisplayOutput);
    // 滚动越顶（含滚轮路径）→ 向历史提供者读回更老的行注入显示层
    connect(m_terminalDisplay, &TerminalDisplay::historyTopReached, this, [this]() {
        fetchOlderHistory();
    });
    connect( m_emulation, &Emulation::changeTabTextColorRequest, this, &QTermWidget::changeTabTextColorRequest);
    connect( m_emulation, &Emulation::cursorChanged, this, &QTermWidget::cursorChanged);

    // That's OK, FilterChain's dtor takes care of UrlFilter.
    m_urlFilter = new UrlFilter();
    connect(m_urlFilter, &UrlFilter::activated, this, &QTermWidget::urlActivated);
    m_terminalDisplay->filterChain()->addFilter(m_urlFilter);
    m_UrlFilterEnable = true;

    m_searchBar = new SearchBar(this);
    m_searchBar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Maximum);
    connect(m_searchBar, &SearchBar::searchCriteriaChanged, this, [this](){
        search(true, false);
    });
    connect(m_searchBar, &SearchBar::findNext, this, [this](){
        search(true, true);
    });
    connect(m_searchBar, &SearchBar::findPrevious, this, [this](){
        search(false, false);
    });
    m_layout->addWidget(m_searchBar);
    m_searchBar->hide();
    connect(m_searchBar, &SearchBar::madeHidden, this, [this]() {
        if (auto regexFilter = m_terminalDisplay->filterChain()->getRegExpFilter(QLatin1String(QTERMW_HLIGHT)))
        {
            m_terminalDisplay->filterChain()->removeFilter(regexFilter);
            delete regexFilter;
        }
    });

    QString style_sheet = qApp->styleSheet();
    m_searchBar->setStyleSheet(style_sheet);
    
    this->setFocus( Qt::OtherFocusReason );
    this->setFocusPolicy( Qt::WheelFocus );
    m_terminalDisplay->resize(this->size());

    this->setFocusProxy(m_terminalDisplay);
    connect(m_terminalDisplay, &TerminalDisplay::copyAvailable,
            this, &QTermWidget::copyAvailable);
    connect(m_terminalDisplay, &TerminalDisplay::termGetFocus,
            this, &QTermWidget::termGetFocus);
    connect(m_terminalDisplay, &TerminalDisplay::termLostFocus,
            this, &QTermWidget::termLostFocus);
    connect(m_terminalDisplay, &TerminalDisplay::keyPressedSignal, this, [this] (QKeyEvent* e, bool) { 
        emit termKeyPressed(e); 
    });

    setScrollBarPosition(NoScrollBar);
    setKeyboardCursorShape(Emulation::KeyboardCursorShape::BlockCursor);

    connect(m_emulation, &Emulation::imageResizeRequest, this, [this](const QSize& size){
        if ((size.width() <= 1) || (size.height() <= 1)) {
            return;
        }
        setSize(size);
    });
}

QTermWidget::~QTermWidget() {
    setUrlFilterEnabled(false);
    clearHighLightTexts();
    delete m_urlFilter;
    delete m_searchBar;
    delete m_emulation;
}

void QTermWidget::selectionChanged(bool textSelected) {
    emit copyAvailable(textSelected);
}

void QTermWidget::search(bool forwards, bool next) {
    int startColumn, startLine;

    if (next) {
        // search from just after current selection
        m_terminalDisplay->screenWindow()->screen()->getSelectionEnd(startColumn, startLine);
        startColumn++;
    } else {
        // search from start of current selection
        m_terminalDisplay->screenWindow()->screen()->getSelectionStart(startColumn, startLine);
    }

    //qDebug() << "current selection starts at: " << startColumn << startLine;
    //qDebug() << "current cursor position: " << m_terminalDisplay->screenWindow()->cursorPosition();

    QRegularExpression regExp;
    if (m_searchBar->useRegularExpression()) {
        regExp.setPattern(m_searchBar->searchText());
    } else {
        regExp.setPattern(QRegularExpression::escape(m_searchBar->searchText()));
    }
    regExp.setPatternOptions(m_searchBar->matchCase() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);

    HistorySearch *historySearch =
            new HistorySearch(m_emulation, regExp, forwards, startColumn, startLine, this);
    connect(historySearch, &HistorySearch::matchFound, this, [this](int startColumn, int startLine, int endColumn, int endLine){
        ScreenWindow* sw = m_terminalDisplay->screenWindow();
        //qDebug() << "Scroll to" << startLine;
        sw->scrollTo(startLine);
        sw->setTrackOutput(false);
        sw->notifyOutputChanged();
        sw->setSelectionStart(startColumn, startLine - sw->currentLine(), false);
        sw->setSelectionEnd(endColumn, endLine - sw->currentLine());
    });
    connect(historySearch, &HistorySearch::noMatchFound, this, [this](){
        m_terminalDisplay->screenWindow()->clearSelection();
    });
    connect(historySearch, &HistorySearch::noMatchFound, m_searchBar, &SearchBar::noMatchFound);
    historySearch->search();

    // Highlighting all matches.
    auto regexFilter = m_terminalDisplay->filterChain()->getRegExpFilter(QLatin1String(QTERMW_HLIGHT));
    if (regexFilter) {
        if (m_searchBar->highlightAllMatches() && regexFilter->regExp() == regExp) {
            return;
        }
        m_terminalDisplay->filterChain()->removeFilter(regexFilter);
        delete regexFilter;
        m_terminalDisplay->update();
    }
    if (m_searchBar->highlightAllMatches() && !regExp.pattern().isEmpty()) {
        regexFilter = new RegExpFilter();
        regexFilter->setObjectName(QLatin1String(QTERMW_HLIGHT));
        regexFilter->setRegExp(regExp);
        m_terminalDisplay->filterChain()->addFilter(regexFilter);
        m_terminalDisplay->update();
    }
}

QSize QTermWidget::sizeHint() const {
    const QSize size = m_terminalDisplay->sizeHint();
    // TerminalDisplay 未给出有效高度时回退到历史默认值 150
    return (size.isValid() && size.height() > 0) ? size : QSize(size.width(), 150);
}

void QTermWidget::setTerminalSizeHint(bool enabled) {
    m_terminalDisplay->setTerminalSizeHint(enabled);
}

bool QTermWidget::terminalSizeHint() {
    return m_terminalDisplay->terminalSizeHint();
}

void QTermWidget::setTerminalFont(const QFont &font) {
    m_terminalDisplay->setVTFont(font);
}

QFont QTermWidget::getTerminalFont() {
    return m_terminalDisplay->getVTFont();
}

void QTermWidget::setTerminalOpacity(qreal level) {
    m_terminalDisplay->setOpacity(level);
}

void QTermWidget::setTerminalBackgroundImage(const QString& backgroundImage) {
    m_terminalDisplay->setBackgroundImage(backgroundImage);
}

void QTermWidget::setTerminalBackgroundMovie(const QString& backgroundMovie) {
    m_terminalDisplay->setBackgroundMovie(backgroundMovie);
}

void QTermWidget::setTerminalBackgroundVideo(const QString& backgroundVideo) {
    m_terminalDisplay->setBackgroundVideo(backgroundVideo);
}

void QTermWidget::setTerminalBackgroundMode(int mode) {
    m_terminalDisplay->setBackgroundMode(static_cast<BackgroundMode>(mode));
}

void QTermWidget::setTextCodec(QStringEncoder codec) {
    m_emulation->setCodec(std::move(codec));
}

void QTermWidget::setColorScheme(const QString& origName) {
    const ColorScheme *cs = nullptr;

    const bool isFile = QFile::exists(origName);
    const QString& name = isFile ? QFileInfo(origName).baseName() : origName;

    // avoid legacy (int) solution
    if (!availableColorSchemes().contains(name)) {
        if (isFile) {
            if (ColorSchemeManager::instance()->loadCustomColorScheme(origName))
                cs = ColorSchemeManager::instance()->findColorScheme(name);
            else
                qWarning () << Q_FUNC_INFO << "cannot load color scheme from" << origName;
        }

        if (!cs)
            cs = ColorSchemeManager::instance()->defaultColorScheme();
    } else {
        cs = ColorSchemeManager::instance()->findColorScheme(name);
    }

    if (! cs) {
        QMessageBox::information(messageParentWidget,
                                 tr("Color Scheme Error"),
                                 tr("Cannot load color scheme: %1").arg(name));
        return;
    }
    ColorEntry table[TABLE_COLORS];
    cs->getColorTable(table);
    m_terminalDisplay->setColorTable(table);
    m_hasDarkBackground = cs->hasDarkBackground();
}

QStringList QTermWidget::getAvailableColorSchemes() {
   return QTermWidget::availableColorSchemes();
}

QStringList QTermWidget::availableColorSchemes() {
    // 静态调用路径（构造实例前）也需保证配色资源已初始化；Q_INIT_RESOURCE 幂等，开销可忽略
    initEmbeddedResources();
    QStringList ret;
    const auto allColorSchemes = ColorSchemeManager::instance()->allColorSchemes();
    for (const ColorScheme* cs : allColorSchemes)
        ret.append(cs->name());
    return ret;
}

void QTermWidget::setBackgroundColor(const QColor &color) {
    m_terminalDisplay->setBackgroundColor(color);
}

void QTermWidget::setForegroundColor(const QColor &color) {
    m_terminalDisplay->setForegroundColor(color);
}

void QTermWidget::setANSIColor(const int ansiColorId, const QColor &color) {
    m_terminalDisplay->setColorTableColor(ansiColorId, color);
}

void QTermWidget::setPreeditColorIndex(int index) {
    m_terminalDisplay->setPreeditColorIndex(index);
}

void QTermWidget::setSize(const QSize &size) {
    m_terminalDisplay->setSize(size.width(), size.height());
}

void QTermWidget::setHistorySize(int lines) {
    if (lines < 0)
        m_emulation->setHistory(HistoryTypeFile());
    else if (lines == 0)
        m_emulation->setHistory(HistoryTypeNone());
    else
        m_emulation->setHistory(HistoryTypeBuffer(lines));
}

int QTermWidget::historySize() const {
    const HistoryType& currentHistory = m_emulation->history();

     if (currentHistory.isEnabled()) {
         if (currentHistory.isUnlimited()) {
             return -1;
         } else {
             return currentHistory.maximumLineCount();
         }
     } else {
         return 0;
     }
}

void QTermWidget::setHistoryProvider(std::function<QStringList(qint64 beforeLine, int maxLines)> provider) {
    m_historyProvider = std::move(provider);
    m_historyProviderExhausted = false;
}

void QTermWidget::fetchOlderHistory() {
    if (!m_historyProvider || m_historyProviderExhausted || m_historyFetching)
        return;

    const qint64 base = m_emulation->historyBaseLine();
    if (base <= 0) {
        // 内存历史已含会话全部输出（或提供者误用注入了超量行），无更老行可读
        m_historyProviderExhausted = true;
        return;
    }

    // 防重入置位走 RAII：提供者回调抛异常时也能复位，避免取数通道永久闭锁
    m_historyFetching = true;
    const auto fetchingGuard = qScopeGuard([this] { m_historyFetching = false; });
    // 拷贝到局部再调用：回调内若重入 setHistoryProvider()，销毁/替换的是成员本体，
    // 直接对成员 std::function 发起调用则属未定义行为
    const auto provider = m_historyProvider;
    const QStringList texts = provider(base, HISTORY_FETCH_LINES);

    if (texts.isEmpty()) {
        m_historyProviderExhausted = true;
        return;
    }

    // QString → UCS-4 → Character（默认属性）：与 receiveData / dupDisplayCharacter
    // 同一 char32_t 字符管线；读回行的折行关系不可知，统一按非折行整行处理
    QVector<QVector<Character>> lines;
    QVector<bool> wrapped;
    lines.reserve(texts.size());
    wrapped.reserve(texts.size());
    for (const QString &text : texts) {
        const QVector<uint> ucs4 = text.toUcs4();
        QVector<Character> line;
        line.reserve(ucs4.size());
        for (const char32_t c : ucs4)
            line.append(Character(c));
        lines.append(std::move(line));
        wrapped.append(false);
    }

    const int n = m_emulation->prependHistoryLines(lines, wrapped);
    if (n <= 0) {
        // 底层滚动类型不支持前插，或前插区已满（回看深度达内存历史上限）：
        // 两种结局都无法再注入，标记耗尽终止滚动事件空转
        m_historyProviderExhausted = true;
        return;
    }

    m_terminalDisplay->scrollAfterHistoryPrepend(n);
}

void QTermWidget::setOsc52Enabled(bool enabled) {
    m_emulation->setOsc52Enabled(enabled);
}

bool QTermWidget::osc52Enabled() const {
    return m_emulation->osc52Enabled();
}

void QTermWidget::setScrollBarPosition(ScrollBarPosition pos) {
    m_terminalDisplay->setScrollBarPosition(pos);
}

void QTermWidget::scrollToEnd() {
    m_terminalDisplay->scrollToEnd();
}

void QTermWidget::sendText(const QString &text) {
    m_emulation->sendText(text);
}

void QTermWidget::sendKeyEvent(QKeyEvent *e) {
    m_emulation->sendKeyEvent(e, false);
}

void QTermWidget::resizeEvent(QResizeEvent*) {
    //qDebug("global window resizing...with %d %d", this->size().width(), this->size().height());
    m_terminalDisplay->resize(this->size());
}

void QTermWidget::updateTerminalSize() {
    int minLines = -1;
    int minColumns = -1;

    // minimum number of lines and columns that views require for
    // their size to be taken into consideration ( to avoid problems
    // with new view widgets which haven't yet been set to their correct size )
    const int VIEW_LINES_THRESHOLD = 2;
    const int VIEW_COLUMNS_THRESHOLD = 2;

    //select largest number of lines and columns that will fit in all visible views
    if ( m_terminalDisplay->isHidden() == false &&
            m_terminalDisplay->lines() >= VIEW_LINES_THRESHOLD &&
            m_terminalDisplay->columns() >= VIEW_COLUMNS_THRESHOLD ) {
        minLines = (minLines == -1) ? m_terminalDisplay->lines() : qMin( minLines , m_terminalDisplay->lines() );
        minColumns = (minColumns == -1) ? m_terminalDisplay->columns() : qMin( minColumns , m_terminalDisplay->columns() );
    }

    // backend emulation must have a _terminal of at least 1 column x 1 line in size
    if ( minLines > 0 && minColumns > 0 ) {
        m_emulation->setImageSize( minLines , minColumns );
    }
}

void QTermWidget::monitorTimerDone() {
    //FIXME: The idea here is that the notification popup will appear to tell the user than output from
    //the terminal has stopped and the popup will disappear when the user activates the session.
    //
    //This breaks with the addition of multiple views of a session.  The popup should disappear
    //when any of the views of the session becomes active


    //FIXME: Make message text for this notification and the activity notification more descriptive.
    if (m_monitorSilence) {
        emit silence();
        emit stateChanged(NOTIFYSILENCE);
    } else {
        emit stateChanged(NOTIFYNORMAL);
    }

    m_notifiedActivity=false;
}

void QTermWidget::activityStateSet(int state) {
    if (state==NOTIFYBELL) {
        m_terminalDisplay->bell();
    } else if (state==NOTIFYACTIVITY) {
        if (m_monitorSilence) {
            m_monitorTimer->start(m_silenceSeconds*1000);
        }

        if ( m_monitorActivity ) {
            //FIXME:  See comments in monitorTimerDone()
            if (!m_notifiedActivity) {
                m_notifiedActivity=true;
                emit activity();
            }
        }
    }

    if ( state==NOTIFYACTIVITY && !m_monitorActivity ) {
        state = NOTIFYNORMAL;
    }
    if ( state==NOTIFYSILENCE && !m_monitorSilence ) {
        state = NOTIFYNORMAL;
    }

    emit stateChanged(state);
}

void QTermWidget::setMonitorActivity(bool enabled) {
    m_monitorActivity=enabled;
    m_notifiedActivity=false;

    activityStateSet(NOTIFYNORMAL);
}

void QTermWidget::setMonitorSilence(bool enabled) {
    if (m_monitorSilence==enabled) {
        return;
    }

    m_monitorSilence=enabled;
    if (m_monitorSilence) {
        m_monitorTimer->start(m_silenceSeconds*1000);
    } else {
        m_monitorTimer->stop();
    }

    activityStateSet(NOTIFYNORMAL);
}

void QTermWidget::setSilenceTimeout(int seconds) {
    m_silenceSeconds=seconds;
    if (m_monitorSilence) {
        m_monitorTimer->start(m_silenceSeconds*1000);
    }
}

void QTermWidget::bracketText(QString& text) {
    m_terminalDisplay->bracketText(text);
}

void QTermWidget::disableBracketedPasteMode(bool disable) {
    m_terminalDisplay->disableBracketedPasteMode(disable);
}

bool QTermWidget::bracketedPasteModeIsDisabled() const {
    return m_terminalDisplay->bracketedPasteModeIsDisabled();
}

void QTermWidget::copyClipboard() {
    m_terminalDisplay->copyClipboard(QClipboard::Clipboard);
}

void QTermWidget::copySelection() {
    m_terminalDisplay->copyClipboard(QClipboard::Selection);
}

void QTermWidget::pasteClipboard() {
    m_terminalDisplay->pasteClipboard();
}

void QTermWidget::pasteSelection() {
    m_terminalDisplay->pasteSelection();
}

void QTermWidget::selectAll() {
    m_terminalDisplay->selectAll();
}

int QTermWidget::setZoom(int step) {
    QFont font = m_terminalDisplay->getVTFont();

    font.setPointSize(font.pointSize() + step);
    setTerminalFont(font);
    return font.pointSize();
}

int QTermWidget::zoomIn() {
    return setZoom(STEP_ZOOM);
}

int QTermWidget::zoomOut() {
    return setZoom(-STEP_ZOOM);
}

void QTermWidget::setKeyBindings(const QString & kb) {
    m_emulation->setKeyBindings(kb);
}

void QTermWidget::clear() {
    clearScreen();
    clearScrollback();
}

void QTermWidget::clearScrollback() {
    m_emulation->clearHistory();
    m_historyProviderExhausted = false; // 历史清空后允许提供者重新应答
}

void QTermWidget::clearScreen() {
    m_emulation->reset();
    /**
     * TODO:
     * Attempts to get the shell program to redraw the current display area.
     * This can be used after clearing the screen, for example, to get the
     * shell to redraw the prompt line.
     */
}

void QTermWidget::setFlowControlEnabled(bool enabled) {
    if (m_flowControl == enabled) {
        return;
    }

    m_flowControl = enabled;

    emit flowControlEnabledChanged(enabled);
}

bool QTermWidget::flowControlEnabled() const {
    return m_flowControl;
}

void QTermWidget::setFlowControlWarningEnabled(bool enabled) {
    if (flowControlEnabled()) {
        // Do not show warning label if flow control is disabled
        m_terminalDisplay->setFlowControlWarningEnabled(enabled);
    }
}

QStringList QTermWidget::availableKeyBindings() {
    // 静态调用路径（构造实例前）也需保证键位资源已初始化；Q_INIT_RESOURCE 幂等，开销可忽略
    initEmbeddedResources();
    return KeyboardTranslatorManager::instance()->allTranslators();
}

QString QTermWidget::keyBindings() const {
    return m_emulation->keyBindings();
}

void QTermWidget::toggleShowSearchBar() {
    if(m_searchBar->isHidden()) {
        m_searchBar->setText(selectedText(true));
        m_searchBar->show();
    } else {
        m_searchBar->hide();
    }
}

void QTermWidget::setMotionAfterPasting(int action) {
    m_terminalDisplay->setMotionAfterPasting(static_cast<MotionAfterPasting>(action));
}

int QTermWidget::historyLinesCount() const {
    return m_terminalDisplay->screenWindow()->screen()->getHistLines();
}

int QTermWidget::screenColumnsCount() const {
    return m_terminalDisplay->screenWindow()->screen()->getColumns();
}

int QTermWidget::screenLinesCount() const {
    return m_terminalDisplay->screenWindow()->screen()->getLines();
}

void QTermWidget::setSelectionStart(int row, int column) {
    m_terminalDisplay->screenWindow()->screen()->setSelectionStart(column, row, true);
}

void QTermWidget::setSelectionEnd(int row, int column) {
    m_terminalDisplay->screenWindow()->screen()->setSelectionEnd(column, row);
}

void QTermWidget::getSelectionStart(int& row, int& column) {
    m_terminalDisplay->screenWindow()->screen()->getSelectionStart(column, row);
}

void QTermWidget::getSelectionEnd(int& row, int& column) {
    m_terminalDisplay->screenWindow()->screen()->getSelectionEnd(column, row);
}

QString QTermWidget::selectedText(bool preserveLineBreaks) const {
    return m_terminalDisplay->screenWindow()->screen()->selectedText(preserveLineBreaks);
}

Filter::HotSpot* QTermWidget::getHotSpotAt(const QPoint &pos) const {
    int row = 0, column = 0;
    m_terminalDisplay->getCharacterPosition(pos, row, column);
    return getHotSpotAt(row, column);
}

Filter::HotSpot* QTermWidget::getHotSpotAt(int row, int column) const {
    return m_terminalDisplay->filterChain()->hotSpotAt(row, column);
}

QList<QAction*> QTermWidget::filterActions(const QPoint& position) {
    return m_terminalDisplay->filterActions(position);
}

int QTermWidget::recvData(const char *buff, int len) const {
    m_emulation->receiveData( buff, len );
    return len;
}

void QTermWidget::setKeyboardCursorShape(KeyboardCursorShape shape) {
    m_terminalDisplay->setKeyboardCursorShape(shape);
}

void QTermWidget::setKeyboardCursorShape(uint32_t shape) {
    m_terminalDisplay->setKeyboardCursorShape(static_cast<QTermWidget::KeyboardCursorShape>(shape));
}

void QTermWidget::setBlinkingCursor(bool blink) {
    m_terminalDisplay->setBlinkingCursor(blink);
}

void QTermWidget::setBidiEnabled(bool enabled) {
    m_terminalDisplay->setBidiEnabled(enabled);
}

bool QTermWidget::isBidiEnabled() {
    return m_terminalDisplay->isBidiEnabled();
}

void QTermWidget::cursorChanged(Emulation::KeyboardCursorShape cursorShape, bool blinkingCursorEnabled) {
    // TODO: A switch to enable/disable DECSCUSR?
    setKeyboardCursorShape(cursorShape);
    setBlinkingCursor(blinkingCursorEnabled);
}

void QTermWidget::setMargin(int margin) {
    m_terminalDisplay->setMargin(margin);
}

int QTermWidget::getMargin() const {
    return m_terminalDisplay->margin();
}

void QTermWidget::saveHistory(QTextStream *stream, int format, int start, int end) {
    TerminalCharacterDecoder *decoder;
    if(format == 0) {
        decoder = new PlainTextDecoder;
    } else {
        decoder = new HTMLDecoder;
    }
    decoder->begin(stream);
    if(start < 0) {
        start = 0;
    }
    if(end < 0) {
        end = m_emulation->lineCount();
    }
    m_emulation->writeToStream(decoder, start, end);
    delete decoder;
}

void QTermWidget::saveHistory(QIODevice *device, int format, int start, int end) {
    QTextStream stream(device);
    saveHistory(&stream, format, start, end);
}

void QTermWidget::screenShot(QPixmap *pixmap) {
    QPixmap currPixmap(m_terminalDisplay->size());
    m_terminalDisplay->render(&currPixmap);
    *pixmap = currPixmap.scaled(pixmap->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

void QTermWidget::repaintDisplay() {
    m_terminalDisplay->repaintDisplay();
}

void QTermWidget::screenShot(const QString &fileName) {
    qreal deviceratio = m_terminalDisplay->devicePixelRatio();
    deviceratio = deviceratio*2;
    QPixmap pixmap(m_terminalDisplay->size() * deviceratio);
    pixmap.setDevicePixelRatio(deviceratio);
    m_terminalDisplay->render(&pixmap);
    pixmap.save(fileName);
}

void QTermWidget::setLocked(bool enabled) {
    this->setEnabled(!enabled);
    m_terminalDisplay->setLocked(enabled);
}

void QTermWidget::setDrawLineChars(bool drawLineChars) {
    m_terminalDisplay->setDrawLineChars(drawLineChars);
}

void QTermWidget::setBoldIntense(bool boldIntense) {
    m_terminalDisplay->setBoldIntense(boldIntense);
}

void QTermWidget::setConfirmMultilinePaste(bool confirmMultilinePaste) {
    m_terminalDisplay->setConfirmMultilinePaste(confirmMultilinePaste);
}

void QTermWidget::setTrimPastedTrailingNewlines(bool trimPastedTrailingNewlines) {
    m_terminalDisplay->setTrimPastedTrailingNewlines(trimPastedTrailingNewlines);
}

void QTermWidget::setEcho(bool echo) {
    m_echo = echo;
}

void QTermWidget::setKeyboardCursorColor(bool useForegroundColor, const QColor& color) {
    m_terminalDisplay->setKeyboardCursorColor(useForegroundColor, color);
}

void QTermWidget::addHighLightText(const QString &text, const QColor &color) {
    for (int i = 0; i < m_highLightTexts.size(); i++) {
        if (m_highLightTexts.at(i)->text == text) {
            return;
        }
    }
    HighLightText *highLightText = new HighLightText(text,color);
    m_highLightTexts.append(highLightText);
    m_terminalDisplay->filterChain()->addFilter(highLightText->regExpFilter);
    m_terminalDisplay->updateFilters();
    m_terminalDisplay->repaint();
}

QMap<QString, QColor> QTermWidget::getHighLightTexts() const {
    QMap<QString, QColor> highLightTexts;
    for (int i = 0; i < m_highLightTexts.size(); i++) {
        highLightTexts.insert(m_highLightTexts.at(i)->text, m_highLightTexts.at(i)->color);
    }
    return highLightTexts;
}

bool QTermWidget::isContainHighLightText(const QString &text) {
    for (int i = 0; i < m_highLightTexts.size(); i++) {
        if (m_highLightTexts.at(i)->text == text) {
            return true;
        }
    }
    return false;
}

void QTermWidget::removeHighLightText(const QString &text) {
    for (int i = 0; i < m_highLightTexts.size(); i++) {
        if (m_highLightTexts.at(i)->text == text) {
            m_terminalDisplay->filterChain()->removeFilter(m_highLightTexts.at(i)->regExpFilter);
            delete m_highLightTexts.at(i);
            m_highLightTexts.removeAt(i);
            m_terminalDisplay->updateFilters();
            break;
        }
    }
    m_terminalDisplay->repaint();
}

void QTermWidget::clearHighLightTexts() {
    for (int i = 0; i < m_highLightTexts.size(); i++) {
        m_terminalDisplay->filterChain()->removeFilter(m_highLightTexts.at(i)->regExpFilter);
        delete m_highLightTexts.at(i);
    }
    m_terminalDisplay->updateFilters();
    m_highLightTexts.clear();
    m_terminalDisplay->repaint();
}

void QTermWidget::setWordCharacters(const QString &wordCharacters) {
    m_terminalDisplay->setWordCharacters(wordCharacters);
}

QString QTermWidget::wordCharacters() const {
    return m_terminalDisplay->wordCharacters();
}

void QTermWidget::autoHideMouseAfter(int delay)
{
    m_terminalDisplay->autoHideMouseAfter(delay);
}

void QTermWidget::setShowResizeNotificationEnabled(bool enabled) {
    m_terminalDisplay->setShowResizeNotificationEnabled(enabled);
}

void QTermWidget::setEnableHandleCtrlC(bool enable) {
    m_emulation->setEnableHandleCtrlC(enable);
}

int QTermWidget::lines() {
    return m_terminalDisplay->lines();
}

int QTermWidget::columns() {
    return m_terminalDisplay->columns();
}

int QTermWidget::getCursorX() {
    return m_terminalDisplay->getCursorX();
}

int QTermWidget::getCursorY() {
    return m_terminalDisplay->getCursorY();
}

void QTermWidget::setCursorX(int x) {
    m_terminalDisplay->setCursorX(x);
}

void QTermWidget::setCursorY(int y) {
    m_terminalDisplay->setCursorY(y);
}

QString QTermWidget::screenGet(int row1, int col1, int row2, int col2, int mode) {
    return m_terminalDisplay->screenGet(row1, col1, row2, col2, mode);
}

void QTermWidget::setSelectionOpacity(qreal opacity) {
    m_terminalDisplay->setSelectionOpacity(opacity);
}

void QTermWidget::setUrlFilterEnabled(bool enable) {
    if(m_UrlFilterEnable == enable) {
        return;
    }
    m_UrlFilterEnable = enable;
    if(enable) {
        m_terminalDisplay->filterChain()->addFilter(m_urlFilter);
    } else {
        m_terminalDisplay->filterChain()->removeFilter(m_urlFilter);
    }
}

void QTermWidget::setMessageParentWidget(QWidget *parent) {
    messageParentWidget = parent;
    m_terminalDisplay->setMessageParentWidget(messageParentWidget);
}

void QTermWidget::reTranslateUi() {
    m_searchBar->retranslateUi();
}

void QTermWidget::set_fix_quardCRT_issue33(bool fix) {
    m_terminalDisplay->set_fix_quardCRT_issue33(fix);
}
