#include "albumpanel.h"
#include "musicindex.h"
#include "theme.h"
#include "widgets.h"

#include <QFileInfo>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QtMath>

namespace {
constexpr int kSide = 32;       // left and right margin
constexpr int kTracksTop = 104; // where the songs start
constexpr int kRow = 40;
constexpr int kColumnGap = 48;
constexpr int kFooter = 56; // the year line and the bottom margin

QString length(qint64 ms)
{
    const qint64 s = (ms + 500) / 1000;
    return s >= 3600 ? QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                     : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
} // namespace

AlbumTracksPanel::AlbumTracksPanel(const QString &album, const QString &artist, const QStringList &tracks, QWidget *parent)
    : QWidget(parent), m_album(album), m_artist(artist)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    int year = 0;
    qint64 total = 0;
    for (const QString &p : tracks) {
        Song s;
        s.path = p;
        if (const MusicIndex::Track *t = musicIndex().track(p)) {
            s.title = t->displayTitle();
            s.number = t->number;
            s.ms = t->duration;
            if (t->year && (!year || t->year < year))
                year = t->year;
        } else {
            s.title = QFileInfo(p).completeBaseName();
        }
        total += s.ms;
        m_songs << s;
    }
    QStringList foot;
    if (year)
        foot << QString::number(year);
    foot << (m_songs.size() == 1 ? tr("1 song") : tr("%1 songs").arg(m_songs.size()));
    if (total > 0) {
        const qint64 mins = (total + 30000) / 60000;
        foot << (mins < 60 ? tr("%1 min").arg(mins) : tr("%1 h %2 min").arg(mins / 60).arg(mins % 60));
    }
    m_footer = foot.join(QString(" · "));

    m_play = new IconButton("play-fill", this);
    m_play->setToolTip(tr("Play “%1”").arg(album));
    m_shuffle = new IconButton("shuffle", this);
    m_shuffle->setToolTip(tr("Shuffle “%1”").arg(album));
    m_close = new IconButton("close", this);
    m_close->setToolTip(tr("Close"));
    QStringList paths = tracks;
    connect(m_play, &IconButton::clicked, this, [this, paths] { emit playAllRequested(paths, false); });
    connect(m_shuffle, &IconButton::clicked, this, [this, paths] { emit playAllRequested(paths, true); });
    connect(m_close, &IconButton::clicked, this, &AlbumTracksPanel::closeRequested);
    connect(&theme(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void AlbumTracksPanel::setNowPlaying(const QString &path)
{
    if (m_now != path) {
        m_now = path;
        update();
    }
}

int AlbumTracksPanel::columns(int width) const
{
    return width >= 760 && m_songs.size() > 1 ? 2 : 1;
}

int AlbumTracksPanel::rowsPerColumn(int width) const
{
    const int cols = columns(width);
    return (int(m_songs.size()) + cols - 1) / cols;
}

int AlbumTracksPanel::heightForWidth(int width) const
{
    return kTracksTop + rowsPerColumn(width) * kRow + kFooter;
}

QRect AlbumTracksPanel::songRect(int i) const
{
    const int cols = columns(width()), rows = rowsPerColumn(width());
    const int colWidth = (width() - 2 * kSide + 16 - (cols - 1) * kColumnGap) / cols;
    const int col = i / rows, row = i % rows; // down the first column, then the second
    return QRect(kSide - 8 + col * (colWidth + kColumnGap), kTracksTop + row * kRow, colWidth, kRow);
}

QRect AlbumTracksPanel::moreRect(int i) const
{
    const QRect r = songRect(i);
    return QRect(r.right() - 34, r.top() + 6, 28, 28);
}

int AlbumTracksPanel::songAt(const QPoint &pos) const
{
    for (int i = 0; i < m_songs.size(); ++i)
        if (songRect(i).contains(pos))
            return i;
    return -1;
}

void AlbumTracksPanel::resizeEvent(QResizeEvent *)
{
    QFont f = font();
    f.setPixelSize(22);
    f.setWeight(QFont::DemiBold);
    const int titleWidth = qMin(qCeil(QFontMetricsF(f).horizontalAdvance(m_album)) + 4, width() - 2 * kSide - 160);
    const int x = kSide + titleWidth + 18;
    m_play->setGeometry(x, 26, 34, 34);
    m_shuffle->setGeometry(x + 38, 26, 34, 34);
    m_close->setGeometry(width() - 52, 18, 34, 34);
}

void AlbumTracksPanel::paintEvent(QPaintEvent *)
{
    const Palette &c = theme().pal();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal dpr = devicePixelRatioF();
    p.setPen(QPen(c.border, 1));
    p.setBrush(c.surface);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);

    QFont title = font();
    title.setPixelSize(22);
    title.setWeight(QFont::DemiBold);
    p.setFont(title);
    p.setPen(c.text);
    const int titleWidth = qMin(qCeil(QFontMetricsF(title).horizontalAdvance(m_album)) + 4, width() - 2 * kSide - 160);
    p.drawText(QRect(kSide, 26, titleWidth, 34), Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetrics(title).elidedText(m_album, Qt::ElideRight, titleWidth));
    QFont sub = font();
    sub.setPixelSize(15);
    p.setFont(sub);
    p.setPen(c.textDim);
    p.drawText(QRect(kSide, 60, width() - 2 * kSide, 24), Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetrics(sub).elidedText(m_artist, Qt::ElideRight, width() - 2 * kSide));

    QFont f = font();
    f.setPixelSize(14);
    QFont small = font();
    small.setPixelSize(13);
    small.setFeature(QFont::Tag("tnum"), 1); // digits line up
    for (int i = 0; i < m_songs.size(); ++i) {
        const Song &s = m_songs[i];
        const QRect r = songRect(i);
        const bool now = s.path == m_now;
        if (i == m_hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(c.hover);
            p.drawRoundedRect(QRectF(r).adjusted(0, 2, 0, -2), 6, 6);
        }
        p.setFont(small);
        p.setPen(now ? theme().accent() : c.textMuted);
        const QRect num(r.left() + 4, r.top(), 26, r.height());
        if (now)
            p.drawPixmap(QPointF(num.right() - 14, r.center().y() - 7), theme().pixmap("volume", theme().accent(), 14, dpr));
        else
            p.drawText(num, Qt::AlignVCenter | Qt::AlignRight, QString::number(s.number ? s.number : i + 1));
        const QRect more = moreRect(i);
        const QString len = s.ms > 0 ? length(s.ms) : QString();
        const int lenWidth = QFontMetrics(small).horizontalAdvance("00:00") + 8;
        const QRect lenRect(more.left() - lenWidth - 6, r.top(), lenWidth, r.height());
        if (!len.isEmpty())
            p.drawText(lenRect, Qt::AlignVCenter | Qt::AlignRight, len);
        p.drawPixmap(QPointF(more.left() + 4, more.top() + 4),
                     theme().pixmap("more", i == m_hover ? c.text : c.textMuted, 20, dpr));
        p.setFont(f);
        p.setPen(now ? theme().accent() : c.text);
        const QRect text(num.right() + 16, r.top(), lenRect.left() - num.right() - 28, r.height());
        p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(f).elidedText(s.title, Qt::ElideRight, text.width()));
    }

    p.setFont(sub);
    p.setPen(c.textMuted);
    p.drawText(QRect(kSide, height() - kFooter + 8, width() - 2 * kSide, 24), Qt::AlignVCenter | Qt::AlignLeft, m_footer);
}

void AlbumTracksPanel::mouseMoveEvent(QMouseEvent *e)
{
    const int i = songAt(e->position().toPoint());
    if (i != m_hover) {
        m_hover = i;
        setCursor(i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void AlbumTracksPanel::leaveEvent(QEvent *)
{
    m_hover = -1;
    update();
}

void AlbumTracksPanel::mousePressEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    const int i = songAt(pos);
    if (i < 0)
        return;
    if (e->button() == Qt::RightButton || (e->button() == Qt::LeftButton && moreRect(i).contains(pos)))
        showMenu(i, e->globalPosition().toPoint());
    else if (e->button() == Qt::LeftButton) {
        emit enqueueRequested(m_songs[i].path);
    }
}

void AlbumTracksPanel::showMenu(int i, const QPoint &globalPos)
{
    const QString path = m_songs[i].path;
    const QColor accent = theme().accent();
    QMenu menu(this);
    menu.addAction(theme().icon("play-fill", accent), tr("Play Now"), this, [this, path] { emit playNowRequested(path); });
    menu.addAction(theme().icon("play-next", accent), tr("Play After"), this, [this, path] { emit playAfterRequested(path); })
        ->setToolTip(tr("After what's playing now"));
    m_hover = i;
    menu.exec(globalPos);
    m_hover = songAt(mapFromGlobal(QCursor::pos()));
    update();
}
