/*
 * Copyright (C) 2012 Rajendran Thirupugalsamy
 * See LICENSE for full copyright and license information.
 * See COPYING for distribution information.
 */

#include "QtLogDbg.h"
#include <QDebug>
#include <QApplication>
#include <QPainter>
#include <QClipboard>
#include <QScrollBar>
extern "C" {
#include "putty.h"
}
#include "GuiTerminalWindow.h"

GuiTerminalWindow::GuiTerminalWindow(QWidget *parent) :
    QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setWindowState(Qt::WindowMaximized);
    setWindowTitle(tr("QuTTY"));

    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    connect(verticalScrollBar(), SIGNAL(actionTriggered(int)), this, SLOT(vertScrollBarAction(int)));
    connect(verticalScrollBar(), SIGNAL(sliderMoved(int)), this, SLOT(vertScrollBarMoved(int)));

    QPalette pal(palette());
    // set black background // not working as expected
    pal.setColor(QPalette::Background, Qt::black);
    pal.setColor(QPalette::Foreground, Qt::white);
    viewport()->setAutoFillBackground(true);
    viewport()->setPalette(pal);

    setFocusPolicy(Qt::StrongFocus);
    _any_update = false;

    termrgn = QRegion();
    term = NULL;
    ldisc = NULL;
    backend = NULL;
    backhandle = NULL;
    qtsock = NULL;
    _font = NULL;
    _fontMetrics = NULL;

    mouseButtonAction = MA_NOTHING;
    setMouseTracking(true);
    viewport()->setCursor(Qt::IBeamCursor);

    _tmuxMode = TMUX_MODE_NONE;
    _tmuxGateway = NULL;
}

GuiTerminalWindow::~GuiTerminalWindow()
{
    if (_tmuxMode==TMUX_MODE_GATEWAY && _tmuxGateway) {
        _tmuxGateway->initiateDetach();
        delete _tmuxGateway;
    }
}

extern "C" Socket get_ssh_socket(void *handle);
extern "C" Socket get_telnet_socket(void *handle);

int GuiTerminalWindow::initTerminal()
{
    char *realhost = NULL;
    char *ip_addr = cfg.host;

    memset(&ucsdata, 0, sizeof(struct unicode_data));
    init_ucs(&cfg, &ucsdata);
    setTermFont(&cfg.font);
    cfgtopalette(&cfg);

    term = term_init(&cfg, &ucsdata, this);
    term_size(term, cfg.height, cfg.width, cfg.savelines);
    resize(cfg.width*fontWidth, cfg.height*fontHeight);
    // resize according to config if window is smaller
    if ( !(mainWindow->windowState() & Qt::WindowMaximized) &&
          ( mainWindow->size().width() < cfg.width*fontWidth ||
            mainWindow->size().height() < cfg.height*fontHeight))
        mainWindow->resize(cfg.width*fontWidth,
                           cfg.height*fontHeight);

    backend = backend_from_proto(cfg.protocol);
    backend->init(this, &backhandle, &cfg, (char*)ip_addr, cfg.port, &realhost, 1, 0);
    if (realhost)
        sfree(realhost);

    switch(cfg.protocol) {
    case PROT_TELNET:
        as = (Actual_Socket)get_telnet_socket(backhandle);
        break;
    case PROT_SSH:
        as = (Actual_Socket)get_ssh_socket(backhandle);
        break;
    default:
        assert(0);
    }
    qtsock = as->qtsock;
    QObject::connect(as->qtsock, SIGNAL(readyRead()), this, SLOT(readyRead()));

    /*
     * Connect the terminal to the backend for resize purposes.
     */
    term_provide_resize_fn(term, backend->size, backhandle);

    /*
     * Set up a line discipline.
     */
    ldisc = ldisc_create(&cfg, term, backend, backhandle, this);
    return 0;
}

TmuxWindowPane *GuiTerminalWindow::initTmuxClientTerminal(TmuxGateway *gateway,
                                        int id, int width, int height)
{
    TmuxWindowPane *tmuxPane = NULL;

    memset(&ucsdata, 0, sizeof(struct unicode_data));
    init_ucs(&cfg, &ucsdata);
    setTermFont(&cfg.font);
    cfgtopalette(&cfg);

    term = term_init(&cfg, &ucsdata, this);
    term_size(term, cfg.height, cfg.width, cfg.savelines);
    resize(cfg.width*fontWidth, cfg.height*fontHeight);
    // resize according to config if window is smaller
    if ( !(mainWindow->windowState() & Qt::WindowMaximized) &&
          ( mainWindow->size().width() < cfg.width*fontWidth ||
            mainWindow->size().height() < cfg.height*fontHeight))
        mainWindow->resize(cfg.width*fontWidth,
                           cfg.height*fontHeight);

    _tmuxMode = TMUX_MODE_CLIENT;
    _tmuxGateway = gateway;
    cfg.protocol = PROT_TMUX_CLIENT;
    cfg.port = -1;
    cfg.width = width;
    cfg.height = height;

    backend = backend_from_proto(cfg.protocol);
    // HACK - pass paneid in port
    backend->init(this, &backhandle, &cfg, NULL, id, NULL, 0, 0);
    tmuxPane = new TmuxWindowPane(gateway, this);
    tmuxPane->id = id;
    tmuxPane->width = width;
    tmuxPane->height = height;

    as = NULL;
    qtsock = NULL;

    /*
     * Connect the terminal to the backend for resize purposes.
     */
    term_provide_resize_fn(term, backend->size, backhandle);

    /*
     * Set up a line discipline.
     */
    ldisc = ldisc_create(&cfg, term, backend, backhandle, this);
    return tmuxPane;
}

void GuiTerminalWindow::keyPressEvent ( QKeyEvent *e )
{
    noise_ultralight(e->key());
    if (!term) return;

    // skip ALT SHIFT CTRL keypress events
    switch(e->key()) {
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Control:
    case Qt::Key_Shift:
        return;
    }

    char buf[16];
    int len = TranslateKey(e, buf);
    assert(len<16);
    if (len>0 || len==-2) {
        term_nopaste(term);
        term_seen_key_event(term);
        ldisc_send(ldisc, buf, len, 1);
        //show_mouseptr(0);
    } else if(len==-1) {
        wchar_t bufwchar[16];
        len = 0;
        // Treat Alt by just inserting an Esc before everything else
        if (e->modifiers() & Qt::AltModifier){
            bufwchar[len++] = 0x1b;
        }
        len += e->text().toWCharArray(bufwchar+len);
        assert(len<16);
        term_nopaste(term);
        term_seen_key_event(term);
        luni_send(ldisc, bufwchar, len, 1);
        //ldisc_send(ldisc, buf, 1, 1);
    }
}

void GuiTerminalWindow::keyReleaseEvent ( QKeyEvent * e )
{
    noise_ultralight(e->key());
}

void GuiTerminalWindow::readyRead ()
{
    char buf[20480];
    int len = qtsock->read(buf, sizeof(buf));
    noise_ultralight(len);
    (*as->plug)->receive(as->plug, 0, buf, len);

    if(qtsock->bytesAvailable()>0) readyRead();
}

void GuiTerminalWindow::paintEvent (QPaintEvent *e)
{
    QPainter painter(viewport());

    painter.fillRect(e->rect(), Qt::black);
    if(!term)
        return;

    for(int i=0; i<e->region().rects().size(); i++) {
        const QRect &r = e->region().rects().at(i);
        int row = r.top()/fontHeight;
        int colstart = r.left()/fontWidth;
        int rowend = (r.bottom()+1)/fontHeight;
        int colend = (r.right()+1)/fontWidth;
        for(; row<rowend; row++) {
            for(int col=colstart; col<colend; ) {
                uint attr = term->dispstr_attr[row][col];
                int coldiff = col+1;
                for(;attr==term->dispstr_attr[row][coldiff]; coldiff++);
                QString str = QString::fromWCharArray(&term->dispstr[row][col], coldiff-col);
                paintText(painter, row, col, str, attr);

                // paint cursor
                if (attr & (TATTR_ACTCURS | TATTR_PASCURS))
                    paintCursor(painter, row, col, str, attr);
                col = coldiff;
            }
        }
    }
}

void GuiTerminalWindow::paintText(QPainter &painter, int row, int col,
                                  const QString &str, unsigned long attr)
{
    if ((attr & TATTR_ACTCURS) && (cfg.cursor_type == 0 || term->big_cursor)) {
    attr &= ~(ATTR_REVERSE|ATTR_BLINK|ATTR_COLOURS);
    if (bold_mode == BOLD_COLOURS)
        attr &= ~ATTR_BOLD;

    /* cursor fg and bg */
    attr |= (260 << ATTR_FGSHIFT) | (261 << ATTR_BGSHIFT);
    }

    int nfg = ((attr & ATTR_FGMASK) >> ATTR_FGSHIFT);
    int nbg = ((attr & ATTR_BGMASK) >> ATTR_BGSHIFT);
    if (attr & ATTR_REVERSE) {
        int t = nfg;
        nfg = nbg;
        nbg = t;
    }
    if (bold_mode == BOLD_COLOURS && (attr & ATTR_BOLD)) {
        if (nfg < 16) nfg |= 8;
        else if (nfg >= 256) nfg |= 1;
    }
    if (bold_mode == BOLD_COLOURS && (attr & ATTR_BLINK)) {
        if (nbg < 16) nbg |= 8;
        else if (nbg >= 256) nbg |= 1;
    }
    painter.fillRect(QRect(col*fontWidth, row*fontHeight,
                          fontWidth*str.length(), fontHeight),
                          colours[nbg]);
    painter.setPen(colours[nfg]);
    painter.drawText(col*fontWidth,
                     row*fontHeight+_fontMetrics->ascent(),
                     str);
}

void GuiTerminalWindow::paintCursor(QPainter &painter, int row, int col,
                                  const QString &str, unsigned long attr)
{
    int fnt_width;
    int char_width;
    int ctype = cfg.cursor_type;

    if ((attr & TATTR_ACTCURS) && (ctype == 0 || term->big_cursor)) {
        return paintText(painter, row, col, str, attr);
    }

    fnt_width = char_width = fontWidth;
    if (attr & ATTR_WIDE)
    char_width *= 2;
    int x = col*fnt_width;
    int y = row*fontHeight;

    if ((attr & TATTR_PASCURS) && (ctype == 0 || term->big_cursor)) {
        QPoint points[] = {
            QPoint(x, y),
            QPoint(x, y+fontHeight-1),
            QPoint(x+char_width-1, y+fontHeight-1),
            QPoint(x+char_width-1, y)
        };
        painter.setPen(colours[261]);
        painter.drawPolygon(points, 4);
    } else if ((attr & (TATTR_ACTCURS | TATTR_PASCURS)) && ctype != 0) {
        int startx, starty, dx, dy, length, i;
        if (ctype == 1) {
            startx = x;
            starty = y + fontMetrics().descent();
            dx = 1;
            dy = 0;
            length = char_width;
        } else {
            int xadjust = 0;
            if (attr & TATTR_RIGHTCURS)
            xadjust = char_width - 1;
            startx = x + xadjust;
            starty = y;
            dx = 0;
            dy = 1;
            length = fontHeight;
        }
        if (attr & TATTR_ACTCURS) {
            assert(0); // NOT_YET_IMPLEMENTED
        } else {
            painter.setPen(colours[261]);
            for (i = 0; i < length; i++) {
                if (i % 2 == 0) {
                    painter.drawPoint(startx, starty);
                }
                startx += dx;
                starty += dy;
            }
        }
    }
}

int GuiTerminalWindow::from_backend(int is_stderr, const char *data, int len)
{
    if (_tmuxMode==TMUX_MODE_GATEWAY && _tmuxGateway)
        return _tmuxGateway->fromBackend(is_stderr, data, len);
    return term_data(term, is_stderr, data, len);
}

void GuiTerminalWindow::preDrawTerm()
{
    termrgn = QRegion();
}

void GuiTerminalWindow::drawTerm()
{
    this->viewport()->update(termrgn);
}

void GuiTerminalWindow::drawText(int row, int col, wchar_t *ch, int len, unsigned long attr, int lattr)
{
    if (attr & TATTR_COMBINING) {
        // TODO NOT_YET_IMPLEMENTED
        return;
    }
    termrgn |= QRect(col*fontWidth, row*fontHeight, fontWidth*len, fontHeight);
}

void GuiTerminalWindow::setTermFont(FontSpec *f)
{
    if (_font) delete _font;
    if (_fontMetrics) delete _fontMetrics;

    _font = new QFont(f->name, f->height);
    _font->setStyleHint(QFont::TypeWriter);
    setFont(*_font);
    _fontMetrics = new QFontMetrics(*_font);

    fontWidth = _fontMetrics->width(QChar('a'));
    fontHeight = _fontMetrics->height();

    qDebug()<<__FUNCTION__<<_font->family()<<_font->styleHint()<<_font->style();
}

void GuiTerminalWindow::cfgtopalette(Config *cfg)
{
    int i;
    static const int ww[] = {
    256, 257, 258, 259, 260, 261,
    0, 8, 1, 9, 2, 10, 3, 11,
    4, 12, 5, 13, 6, 14, 7, 15
    };

    for (i = 0; i < 22; i++) {
        colours[ww[i]] = QColor::fromRgb(cfg->colours[i][0], cfg->colours[i][1], cfg->colours[i][2]);
    }
    for (i = 0; i < NEXTCOLOURS; i++) {
    if (i < 216) {
        int r = i / 36, g = (i / 6) % 6, b = i % 6;
        r = r ? r * 40 + 55 : 0;
        g = g ? g * 40 + 55 : 0;
        b = b ? b * 40 + 55 : 0;
        colours[i+16] = QColor::fromRgb(r, g, b);
    } else {
        int shade = i - 216;
        shade = shade * 10 + 8;
        colours[i+16] = QColor::fromRgb(shade, shade, shade);
    }
    }

    /* Override with system colours if appropriate * /
    if (cfg.system_colour)
        systopalette();*/
}

/*
 * Translate a raw mouse button designation (LEFT, MIDDLE, RIGHT)
 * into a cooked one (SELECT, EXTEND, PASTE).
 */
static Mouse_Button translate_button(Config *cfg, Mouse_Button button)
{
    if (button == MBT_LEFT)
    return MBT_SELECT;
    if (button == MBT_MIDDLE)
    return cfg->mouse_is_xterm == 1 ? MBT_PASTE : MBT_EXTEND;
    if (button == MBT_RIGHT)
    return cfg->mouse_is_xterm == 1 ? MBT_EXTEND : MBT_PASTE;
    assert(0);
    return MBT_NOTHING;			       /* shouldn't happen */
}

void 	GuiTerminalWindow::mouseDoubleClickEvent ( QMouseEvent * e )
{
    noise_ultralight(e->x()<<16 | e->y());
    if (!term) return;

    if(e->button()==Qt::RightButton &&
            ((e->modifiers() & Qt::ControlModifier) || (cfg.mouse_is_xterm == 2))) {
        // TODO right click menu
    }
    Mouse_Button button, bcooked;
    button = e->button()==Qt::LeftButton ? MBT_LEFT :
             e->button()==Qt::RightButton ? MBT_RIGHT :
             e->button()==Qt::MidButton ? MBT_MIDDLE : MBT_NOTHING;
    assert(button!=MBT_NOTHING);
    int x = e->x()/fontWidth, y = e->y()/fontHeight, mod=e->modifiers();
    bcooked = translate_button(&cfg, button);

    // detect single/double/triple click
    mouseClickTimer.start();
    mouseButtonAction = MA_2CLK;

    qDebug()<<__FUNCTION__<<x<<y<<mod<<button<<bcooked<<mouseButtonAction;
    term_mouse(term, button, bcooked, mouseButtonAction,
               x,y, mod&Qt::ShiftModifier, mod&Qt::ControlModifier, mod&Qt::AltModifier);
    e->accept();
}

//#define (e) e->button()&Qt::LeftButton
void 	GuiTerminalWindow::mouseMoveEvent ( QMouseEvent * e )
{
    noise_ultralight(e->x()<<16 | e->y());
    if (e->buttons()==Qt::NoButton || !term) return;

    Mouse_Button button, bcooked;
    button = e->buttons()&Qt::LeftButton ? MBT_LEFT :
             e->buttons()&Qt::RightButton ? MBT_RIGHT :
             e->buttons()&Qt::MidButton ? MBT_MIDDLE : MBT_NOTHING;
    assert(button!=MBT_NOTHING);
    int x = e->x()/fontWidth, y = e->y()/fontHeight, mod=e->modifiers();
    bcooked = translate_button(&cfg, button);
    term_mouse(term, button, bcooked, MA_DRAG,
               x,y, mod&Qt::ShiftModifier, mod&Qt::ControlModifier, mod&Qt::AltModifier);
    e->accept();
}

// Qt 5.0 supports qApp->styleHints()->mouseDoubleClickInterval()
#define CFG_MOUSE_TRIPLE_CLICK_INTERVAL 500

void 	GuiTerminalWindow::mousePressEvent ( QMouseEvent * e )
{
    noise_ultralight(e->x()<<16 | e->y());
    if (!term) return;

    if(e->button()==Qt::RightButton &&
            ((e->modifiers() & Qt::ControlModifier) || (cfg.mouse_is_xterm == 2))) {
        // TODO right click menu
    }
    Mouse_Button button, bcooked;
    button = e->button()==Qt::LeftButton ? MBT_LEFT :
             e->button()==Qt::RightButton ? MBT_RIGHT :
             e->button()==Qt::MidButton ? MBT_MIDDLE : MBT_NOTHING;
    assert(button!=MBT_NOTHING);
    int x = e->x()/fontWidth, y = e->y()/fontHeight, mod=e->modifiers();
    bcooked = translate_button(&cfg, button);

    // detect single/double/triple click
    if (button == MBT_LEFT &&
            !mouseClickTimer.hasExpired(CFG_MOUSE_TRIPLE_CLICK_INTERVAL)) {
        mouseButtonAction =
                mouseButtonAction==MA_CLICK ? MA_2CLK :
                    mouseButtonAction==MA_2CLK ? MA_3CLK :
                        mouseButtonAction==MA_3CLK ? MA_CLICK : MA_NOTHING;
        qDebug()<<__FUNCTION__<<"not expired"<<mouseButtonAction;
    } else
        mouseButtonAction = MA_CLICK;

    term_mouse(term, button, bcooked, mouseButtonAction,
               x,y, mod&Qt::ShiftModifier, mod&Qt::ControlModifier, mod&Qt::AltModifier);
    e->accept();
}

void 	GuiTerminalWindow::mouseReleaseEvent ( QMouseEvent * e )
{
    noise_ultralight(e->x()<<16 | e->y());
    if (!term) return;

    Mouse_Button button, bcooked;
    button = e->button()==Qt::LeftButton ? MBT_LEFT :
             e->button()==Qt::RightButton ? MBT_RIGHT :
             e->button()==Qt::MidButton ? MBT_MIDDLE : MBT_NOTHING;
    assert(button!=MBT_NOTHING);
    int x = e->x()/fontWidth, y = e->y()/fontHeight, mod=e->modifiers();
    bcooked = translate_button(&cfg, button);
    term_mouse(term, button, bcooked, MA_RELEASE,
               x,y, mod&Qt::ShiftModifier, mod&Qt::ControlModifier, mod&Qt::AltModifier);
    e->accept();
}

void GuiTerminalWindow::getClip(wchar_t **p, int *len)
{
    static wchar_t *clipboard_contents = NULL;
    static int clipboard_length = 0;
    if (p && len) {
        if (clipboard_contents) delete clipboard_contents;
        QString s = QApplication::clipboard()->text();
        qDebug()<<"clipboard"<<s;
        clipboard_length = s.length()+1;
        clipboard_contents = new wchar_t[clipboard_length];
        clipboard_length = s.toWCharArray(clipboard_contents);
        clipboard_contents[clipboard_length] = 0;
        *p = clipboard_contents;
        *len = clipboard_length;
    } else {
        qDebug()<<"clipboard clear";
        if (clipboard_contents) delete clipboard_contents;
        clipboard_contents = NULL;
        clipboard_length = 0;
    }
}

void GuiTerminalWindow::requestPaste()
{
    term_do_paste(term);
}

void GuiTerminalWindow::writeClip(wchar_t * data, int *attr, int len, int must_deselect)
{
    data[len] = 0;
    QString s = QString::fromWCharArray(data);
    QApplication::clipboard()->setText(s);
}

void 	GuiTerminalWindow::resizeEvent ( QResizeEvent * e )
{
    if (term)
        term_size(term, viewport()->size().height()/fontHeight,
                  viewport()->size().width()/fontWidth, cfg.savelines);
    if (_tmuxMode==TMUX_MODE_CLIENT) {
        wchar_t cmd_resize[128];
        int cmd_resize_len = wsprintf(cmd_resize, L"control set-client-size %d,%d\n",
                                      viewport()->size().width()/fontWidth,
                                      viewport()->size().height()/fontHeight);
        _tmuxGateway->sendCommand(_tmuxGateway, CB_NULL,
                                  cmd_resize, cmd_resize_len);
    }
}

bool GuiTerminalWindow::event(QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Tab) {
            keyPressEvent(keyEvent);
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

void GuiTerminalWindow::focusInEvent ( QFocusEvent * e )
{
    if (!term) return;
    term_set_focus(term, TRUE);
    term_update(term);
}

void GuiTerminalWindow::focusOutEvent ( QFocusEvent * e )
{
    if (!term) return;
    term_set_focus(term, FALSE);
    term_update(term);
}

void GuiTerminalWindow::setScrollBar(int total, int start, int page)
{
    verticalScrollBar()->setPageStep(page);
    verticalScrollBar()->setRange(0, total-page);
    if (verticalScrollBar()->value()!=start)
        verticalScrollBar()->setValue(start);
}

void GuiTerminalWindow::vertScrollBarAction(int action)
{
    if (!term) return;
    switch(action) {
    case QAbstractSlider::SliderSingleStepAdd:
        term_scroll(term, 0, +1);
        break;
    case QAbstractSlider::SliderSingleStepSub:
        term_scroll(term, 0, -1);
        break;
    case QAbstractSlider::SliderPageStepAdd:
        term_scroll(term, 0, +term->rows/2);
        break;
    case QAbstractSlider::SliderPageStepSub:
        term_scroll(term, 0, -term->rows/2);
        break;
    }
}

void GuiTerminalWindow::vertScrollBarMoved(int value)
{
    if (!term) return;
    term_scroll(term, 1, value);
}

int GuiTerminalWindow::initTmuxContollerMode(char *tmux_version)
{
    // TODO version check
    assert(_tmuxMode == TMUX_MODE_NONE);

    qDebug()<<"TMUX mode entered";
    _tmuxMode = TMUX_MODE_GATEWAY;
    _tmuxGateway = new TmuxGateway(this);

    return 0;
}

void GuiTerminalWindow::detachTmuxContollerMode()
{
    assert(_tmuxMode == TMUX_MODE_GATEWAY);

    _tmuxGateway->detach();
    delete _tmuxGateway;
    _tmuxGateway= NULL;
    _tmuxMode = TMUX_MODE_NONE;
}
