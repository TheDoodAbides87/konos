#include "miniplayer.h"
#include "player.h"
#include "prefs.h"
#include "theme.h"
#include "widgets.h"

#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace {
const int kWidth = 344;
const int kMargin = 18; // from the parent's edges
} // namespace

MiniPlayer::MiniPlayer(Player *player, QWidget *parent) : QWidget(parent), m_player(player)
{
    setAttribute(Qt::WA_StyledBackground, false);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 6);
    v->setSpacing(6);

    m_host = new QWidget(this);
    m_host->setFixedSize(kWidth - 16, (kWidth - 16) * 9 / 16);
    m_hostLayout = new QVBoxLayout(m_host);
    m_hostLayout->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_host);

    m_strip = new QWidget(this);
    m_strip->setCursor(Qt::OpenHandCursor);
    m_strip->installEventFilter(this);
    auto *h = new QHBoxLayout(m_strip);
    h->setContentsMargins(4, 0, 0, 0);
    h->setSpacing(2);
    auto *text = new QVBoxLayout;
    text->setSpacing(0);
    m_title = new QLabel(m_strip);
    m_artist = new QLabel(m_strip);
    m_artist->setProperty("role", "dim");
    for (QLabel *l : {m_title, m_artist})
        l->setAttribute(Qt::WA_TransparentForMouseEvents);
    text->addWidget(m_title);
    text->addWidget(m_artist);
    h->addLayout(text, 1);
    m_expand = new IconButton("maximize", m_strip);
    m_expand->setToolTip(tr("Back to the player"));
    m_close = new IconButton("close", m_strip);
    m_close->setToolTip(tr("Hide until the next item"));
    h->addWidget(m_expand);
    h->addWidget(m_close);
    v->addWidget(m_strip);

    connect(m_expand, &IconButton::clicked, this, &MiniPlayer::expandRequested);
    connect(m_close, &IconButton::clicked, this, &MiniPlayer::closeRequested);
    connect(m_player, &Player::metaChanged, this, &MiniPlayer::updateText);
    connect(m_player, &Player::mediaChanged, this, &MiniPlayer::updateText);
    connect(&theme(), &Theme::changed, this, qOverload<>(&QWidget::update));
    parent->installEventFilter(this);
    setFixedWidth(kWidth);
    adjustSize();
    updateText();
    hide();
}

void MiniPlayer::updateText()
{
    QString title = m_player->title();
    if (title.isEmpty())
        title = QFileInfo(m_player->location()).fileName();
    const int w = kWidth - 110;
    m_title->setText(m_title->fontMetrics().elidedText(title, Qt::ElideRight, w));
    m_title->setToolTip(title);
    m_artist->setText(m_artist->fontMetrics().elidedText(m_player->artist(), Qt::ElideRight, w));
    m_artist->setVisible(!m_player->artist().isEmpty());
    adjustSize();
    place();
}

void MiniPlayer::place()
{
    if (!parentWidget())
        return;
    const int corner = qBound(0, prefs().integer("ui/miniCorner"), 3); // 0 TL, 1 TR, 2 BL, 3 BR
    const QRect r = parentWidget()->rect();
    const int x = corner % 2 ? r.right() - width() - kMargin : r.left() + kMargin + 10;
    const int y = corner >= 2 ? r.bottom() - height() - kMargin : r.top() + 70;
    move(x, y);
    raise();
}

void MiniPlayer::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &c = theme().pal();
    // A soft shadow, then the card.
    for (int i = 4; i > 0; --i) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 14 * i));
        p.drawRoundedRect(QRectF(rect()).adjusted(4 - i, 6 - i, i - 4, i - 2), 12, 12);
    }
    p.setBrush(c.panel);
    p.setPen(QPen(c.border, 1));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
}

bool MiniPlayer::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == parentWidget() && event->type() == QEvent::Resize && isVisible())
        place();
    if (obj != m_strip)
        return QWidget::eventFilter(obj, event);
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->button() == Qt::LeftButton) {
            m_dragging = true;
            m_dragFrom = mapFromGlobal(e->globalPosition().toPoint());
            m_strip->setCursor(Qt::ClosedHandCursor);
            return true;
        }
        break;
    }
    case QEvent::MouseMove:
        if (m_dragging) {
            const QPoint at = parentWidget()->mapFromGlobal(static_cast<QMouseEvent *>(event)->globalPosition().toPoint());
            move(at - m_dragFrom);
            return true;
        }
        break;
    case QEvent::MouseButtonRelease:
        if (m_dragging) {
            m_dragging = false;
            m_strip->setCursor(Qt::OpenHandCursor);
            // Settle in the nearest corner.
            const QPoint centre = geometry().center();
            const QRect r = parentWidget()->rect();
            const int corner = (centre.x() > r.center().x() ? 1 : 0) + (centre.y() > r.center().y() ? 2 : 0);
            prefs().set("ui/miniCorner", corner);
            place();
            return true;
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(obj, event);
}
