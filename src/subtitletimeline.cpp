#include "subtitletimeline.h"
#include "player.h"
#include "subtitlecues.h"
#include "theme.h"
#include "waveform.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

SubtitleTimeline::SubtitleTimeline(Player *player, QWidget *parent) : QWidget(parent), m_player(player)
{
    setFixedHeight(92);
    setCursor(Qt::OpenHandCursor);
    setToolTip(tr("Drag sideways to move the subtitles earlier or later. Scroll to zoom."));
    m_tick.setInterval(33);
    connect(&m_tick, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    connect(player, &Player::timeChanged, this, [this](qint64 ms) {
        m_time = ms;
        m_since.restart();
    });
    connect(player, &Player::tracksChanged, this, &SubtitleTimeline::refresh);
    connect(player, &Player::mediaChanged, this, &SubtitleTimeline::refresh);
    connect(&SubtitleCues::instance(), &SubtitleCues::ready, this, [this](const QString &key) {
        if (key == m_key)
            refresh();
    });
    connect(&Waveform::instance(), &Waveform::ready, this, qOverload<>(&QWidget::update));
    m_since.start();
}

void SubtitleTimeline::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refresh();
    m_tick.start();
}

void SubtitleTimeline::hideEvent(QHideEvent *e)
{
    m_tick.stop();
    QWidget::hideEvent(e);
}

void SubtitleTimeline::refresh()
{
    if (!isVisible())
        return;
    m_key.clear();
    m_status.clear();
    const QString location = m_player->location();
    Waveform::instance().peaks(location); // starts decoding the sound if needed
    const int current = m_player->subtitleTrack();
    if (current < 0) {
        m_status = tr("No subtitles on. Pick a track in the Subtitles menu.");
        update();
        return;
    }
    int embeddedIndex = 0;
    for (const Player::SubtitleTrack &t : m_player->subtitleTrackList()) {
        if (t.id == current) {
            m_key = t.external ? t.file : SubtitleCues::embeddedKey(location, embeddedIndex);
            break;
        }
        if (!t.external)
            ++embeddedIndex;
    }
    if (m_key.isEmpty()) {
        m_status = tr("Can't find this subtitle track.");
    } else {
        switch (SubtitleCues::instance().state(m_key)) {
        case SubtitleCues::Loading:
            m_status = tr("Reading subtitle timing…");
            break;
        case SubtitleCues::Failed:
            m_status = SubtitleCues::instance().failure(m_key);
            break;
        default:
            break;
        }
    }
    update();
}

qint64 SubtitleTimeline::now() const
{
    if (m_player->state() != Player::Playing)
        return m_time;
    return m_time + qMin<qint64>(1000, qint64(m_since.elapsed() * m_player->rate()));
}

QRectF SubtitleTimeline::lane() const
{
    return QRectF(rect()).adjusted(12, 28, -12, -10);
}

QRectF SubtitleTimeline::resetRect() const
{
    return QRectF(width() - 70, 4, 60, 20);
}

void SubtitleTimeline::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &c = theme().pal();
    const QColor accent = theme().accent();
    QPainterPath bg;
    bg.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    p.fillPath(bg, QColor(12, 12, 12, 215));
    p.setPen(QColor(255, 255, 255, 30));
    p.drawPath(bg);

    // Header: title, delay, reset.
    const qint64 delay = m_player->subtitleDelay();
    QFont f = font();
    f.setPixelSize(12);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(QColor(0xee, 0xee, 0xee));
    const QString title = tr("Subtitle Timeline");
    p.drawText(QRectF(14, 4, 200, 20), Qt::AlignVCenter | Qt::AlignLeft, title);
    const int tw = QFontMetrics(f).horizontalAdvance(title);
    f.setWeight(QFont::Normal);
    p.setFont(f);
    p.setPen(delay ? accent : QColor(0xa0, 0xa0, 0xa0));
    p.drawText(QRectF(14 + tw + 12, 4, 220, 20), Qt::AlignVCenter | Qt::AlignLeft,
               delay ? tr("Delay %1%2 s").arg(delay > 0 ? "+" : "").arg(delay / 1000.0, 0, 'f', 3) : tr("No delay"));
    p.setPen(QColor(0x90, 0x90, 0x90));
    const QRectF hint(width() / 2.0 - 160, 4, 320, 20);
    p.drawText(hint, Qt::AlignCenter, tr("Drag to shift · Scroll to zoom"));
    if (delay) {
        p.setPen(accent);
        p.drawText(resetRect(), Qt::AlignVCenter | Qt::AlignRight, tr("Reset"));
    }

    const QRectF l = lane();
    const qint64 t = now();
    const double pxPerMs = l.width() / double(m_span);
    const qint64 from = t - m_span / 2, to = t + m_span / 2;
    auto xOf = [&](qint64 ms) { return l.left() + (ms - from) * pxPerMs; };
    p.save();
    p.setClipRect(l);

    // Waveform of the sound (what is said), centred on the lane.
    const QVector<float> wave = Waveform::instance().detail(m_player->location());
    if (!wave.isEmpty()) {
        const double step = 1000.0 / Waveform::kDetailRate;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 60));
        const qint64 first = qMax<qint64>(0, from / qint64(step));
        const qint64 last = qMin<qint64>(wave.size() - 1, to / qint64(step) + 1);
        const double bw = qMax(1.0, step * pxPerMs - 1);
        for (qint64 i = first; i <= last; ++i) {
            const double h = qMax(1.0, wave[int(i)] * (l.height() - 4));
            p.drawRect(QRectF(xOf(qint64(i * step)), l.center().y() - h / 2, bw, h));
        }
    }

    // Seconds ticks.
    const qint64 tick = m_span <= 10000 ? 1000 : m_span <= 30000 ? 2000 : m_span <= 60000 ? 5000 : 10000;
    p.setPen(QColor(255, 255, 255, 40));
    for (qint64 s = (from / tick) * tick; s <= to; s += tick)
        if (s >= 0)
            p.drawLine(QPointF(xOf(s), l.top()), QPointF(xOf(s), l.top() + 5));

    // Subtitle lines, shifted by the delay.
    if (!m_key.isEmpty()) {
        QFont cf = font();
        cf.setPixelSize(12);
        p.setFont(cf);
        const QFontMetrics fm(cf);
        QColor fill = accent;
        fill.setAlpha(80);
        const QRectF cueBand(l.left(), l.top() + 10, l.width(), l.height() - 20);
        for (const SubtitleCues::Cue &cue : SubtitleCues::instance().cues(m_key)) {
            const qint64 a = cue.start + delay, b = cue.end + delay;
            if (b < from)
                continue;
            if (a > to)
                break;
            const QRectF r(xOf(a), cueBand.top(), qMax(3.0, (b - a) * pxPerMs), cueBand.height());
            const bool on = t >= a && t < b;
            QColor fc = fill;
            if (on)
                fc.setAlpha(150);
            p.setPen(QPen(accent, 1));
            p.setBrush(fc);
            p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
            if (r.width() > 24) {
                p.setPen(QColor(0xf4, 0xf4, 0xf4));
                p.drawText(r.adjusted(7, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                           fm.elidedText(cue.text, Qt::ElideRight, int(r.width() - 13)));
            }
        }
    }
    p.restore();

    if (!m_status.isEmpty()) {
        QFont sf = font();
        sf.setPixelSize(13);
        p.setFont(sf);
        p.setPen(c.textMuted.isValid() ? QColor(0xb0, 0xb0, 0xb0) : QColor(Qt::gray));
        p.drawText(l, Qt::AlignCenter, m_status);
    }

    // Now: the playhead in the middle.
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    const double x = l.center().x();
    p.drawRoundedRect(QRectF(x - 1, l.top() - 4, 2, l.height() + 8), 1, 1);
    QPainterPath head;
    head.moveTo(x - 5, l.top() - 6);
    head.lineTo(x + 5, l.top() - 6);
    head.lineTo(x, l.top());
    head.closeSubpath();
    p.drawPath(head);
}

void SubtitleTimeline::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    if (m_player->subtitleDelay() && resetRect().contains(e->position())) {
        m_player->setSubtitleDelay(0);
        update();
        return;
    }
    m_dragging = true;
    m_dragX = e->position().toPoint().x();
    m_dragDelay = m_player->subtitleDelay();
    setCursor(Qt::ClosedHandCursor);
}

void SubtitleTimeline::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_dragging)
        return;
    const double msPerPx = double(m_span) / lane().width();
    // Moving the lines right makes them appear later: a larger delay. 10 ms steps.
    qint64 delay = m_dragDelay + qint64((e->position().x() - m_dragX) * msPerPx);
    delay = qBound(qint64(-600000), (delay / 10) * 10, qint64(600000));
    if (delay != m_player->subtitleDelay())
        m_player->setSubtitleDelay(delay);
    update();
}

void SubtitleTimeline::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
}

void SubtitleTimeline::wheelEvent(QWheelEvent *e)
{
    const int steps = e->angleDelta().y() / 120;
    if (!steps)
        return;
    double span = double(m_span);
    for (int i = 0; i < qAbs(steps); ++i)
        span = steps > 0 ? span / 1.25 : span * 1.25; // up: closer
    m_span = qBound(qint64(4000), qint64(span), qint64(120000));
    update();
    e->accept();
}
