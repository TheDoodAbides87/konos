#include "widgets.h"
#include <QStyle>
#include "theme.h"

#include <QAbstractItemView>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QPainterPathStroker>
#include <QPushButton>
#include <QStyleOptionComboBox>
#include <QToolTip>
#include <QVBoxLayout>

QString formatTime(qint64 ms, bool forceHours)
{
    if (ms < 0)
        ms = 0;
    const qint64 s = ms / 1000;
    const int h = int(s / 3600), m = int((s / 60) % 60), sec = int(s % 60);
    if (h > 0 || forceHours)
        return QString("%1:%2:%3").arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
}

static void connectTheme(QWidget *w)
{
    QObject::connect(&theme(), &Theme::changed, w, qOverload<>(&QWidget::update));
}

// ---------------------------------------------------------------------------
// IconButton

IconButton::IconButton(const QString &icon, QWidget *parent) : QAbstractButton(parent), m_icon(icon)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    connectTheme(this);
}

void IconButton::setIconName(const QString &name)
{
    m_icon = name;
    update();
}

void IconButton::setIconPixelSize(int size)
{
    m_iconSize = size;
    updateGeometry();
    update();
}

void IconButton::setLabel(const QString &label)
{
    m_label = label;
    updateGeometry();
    update();
}

void IconButton::setActive(bool active)
{
    m_active = active;
    update();
}

void IconButton::setSelectedLook(bool selected)
{
    m_selected = selected;
    update();
}

void IconButton::setIconColor(const QColor &color)
{
    m_iconColor = color;
    update();
}

QSize IconButton::sizeHint() const
{
    if (m_label.isEmpty())
        return QSize(m_iconSize + 14, m_iconSize + 14);
    QFont f = font();
    f.setPixelSize(m_labelPx);
    return QSize(m_iconSize + 30 + QFontMetrics(f).horizontalAdvance(m_label), m_iconSize + 20);
}

void IconButton::enterEvent(QEnterEvent *)
{
    m_hover = true;
    update();
}

void IconButton::leaveEvent(QEvent *)
{
    m_hover = false;
    update();
}

void IconButton::paintEvent(QPaintEvent *)
{
    const Palette &c = theme().pal();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_selected) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.selected);
        p.drawRoundedRect(r, 6, 6);
    } else if ((m_hover || isDown()) && m_hoverBg && isEnabled()) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.hover);
        p.drawRoundedRect(r, 6, 6);
    }

    QColor iconColor = m_iconColor.isValid() ? m_iconColor : (m_hover ? c.text : c.textDim);
    if (m_active || (isCheckable() && isChecked()) || m_selected)
        iconColor = theme().accent();
    if (!isEnabled())
        iconColor = c.textMuted;

    const QPixmap pm = theme().pixmap(m_icon, iconColor, m_iconSize, devicePixelRatioF());
    if (m_label.isEmpty()) {
        p.drawPixmap(QPointF((width() - m_iconSize) / 2.0, (height() - m_iconSize) / 2.0), pm);
        return;
    }
    const int x = 12;
    p.drawPixmap(QPointF(x, (height() - m_iconSize) / 2.0), pm);
    QFont f = font();
    f.setPixelSize(m_labelPx);
    p.setFont(f);
    p.setPen(m_selected || m_hover ? c.text : c.textDim);
    p.drawText(QRect(x + m_iconSize + 14, 0, width(), height()), Qt::AlignVCenter | Qt::AlignLeft, m_label);
}

// ---------------------------------------------------------------------------
// PlayButton

PlayButton::PlayButton(QWidget *parent) : QAbstractButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(56, 28);
    connectTheme(this);
}

void PlayButton::setPlaying(bool playing)
{
    m_playing = playing;
    update();
}

void PlayButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor bg = theme().accent();
    if (m_hover)
        bg = bg.lighter(110);
    if (isDown())
        bg = bg.darker(110);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(rect()), 4, 4);
    const int s = 16;
    p.drawPixmap(QPointF((width() - s) / 2.0, (height() - s) / 2.0),
                 theme().pixmap(m_playing ? "pause-fill" : "play-fill", theme().onAccent(), s, devicePixelRatioF()));
}

// ---------------------------------------------------------------------------
// SliderBar

SliderBar::SliderBar(Qt::Orientation orientation, QWidget *parent) : QWidget(parent), m_orientation(orientation)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    if (orientation == Qt::Horizontal)
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    else
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    connectTheme(this);
}

void SliderBar::setRange(double min, double max)
{
    m_min = min;
    m_max = max > min ? max : min + 1;
    m_value = qBound(m_min, m_value, m_max);
    update();
}

void SliderBar::setValue(double value)
{
    value = qBound(m_min, value, m_max);
    if (qFuzzyCompare(value + 1.0, m_value + 1.0))
        return;
    m_value = value;
    update();
}

void SliderBar::setThickness(int px)
{
    m_thickness = px;
    updateGeometry();
    update();
}

QSize SliderBar::sizeHint() const
{
    const int t = m_thickness + 2 * m_overshoot;
    return m_orientation == Qt::Horizontal ? QSize(200, t) : QSize(t, 200);
}

QSize SliderBar::minimumSizeHint() const
{
    const int t = m_thickness + 2 * m_overshoot;
    return m_orientation == Qt::Horizontal ? QSize(40, t) : QSize(t, 40);
}

QRectF SliderBar::trackRect() const
{
    if (m_orientation == Qt::Horizontal)
        return QRectF(0.5, (height() - m_thickness) / 2.0, width() - 1.0, m_thickness);
    return QRectF((width() - m_thickness) / 2.0, 0.5, m_thickness, height() - 1.0);
}

double SliderBar::valueAt(const QPointF &pos) const
{
    const QRectF t = trackRect();
    const qreal handle = 4;
    double ratio;
    if (m_orientation == Qt::Horizontal)
        ratio = (pos.x() - t.left() - handle) / (t.width() - 2 * handle);
    else
        ratio = 1.0 - (pos.y() - t.top() - handle) / (t.height() - 2 * handle);
    ratio = qBound(0.0, ratio, 1.0);
    double v = m_min + ratio * (m_max - m_min);
    if (m_step > 0)
        v = m_min + qRound((v - m_min) / m_step) * m_step;
    return qBound(m_min, v, m_max);
}

void SliderBar::userSet(double v)
{
    v = qBound(m_min, v, m_max);
    if (qFuzzyCompare(v + 1.0, m_value + 1.0))
        return;
    m_value = v;
    update();
    emit valueChanged(v);
}

void SliderBar::paintEvent(QPaintEvent *)
{
    const Palette &c = theme().pal();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF t = trackRect();
    const bool horizontal = m_orientation == Qt::Horizontal;
    const double ratio = (m_value - m_min) / (m_max - m_min);
    const QColor accent = m_neutral || !isEnabled() ? QColor(0x80, 0x80, 0x80) : theme().accent();
    const QColor fill = m_neutral || !isEnabled() ? c.surface : theme().accentFill();

    if (horizontal && !m_wave.isEmpty()) {
        // Waveform: thin bars, played part in the accent colour, with a slim
        // accent line for the position.
        const qreal pitch = 3.5, barW = 2;
        const int bars = qMax(1, int((t.width() + pitch - barW) / pitch));
        const qreal x0 = t.left() + (t.width() - ((bars - 1) * pitch + barW)) / 2;
        const qreal posX = t.left() + ratio * t.width();
        const qreal full = t.height() - 2;
        QColor rest = c.textMuted;
        QColor hoverRest = c.textDim;
        p.setPen(Qt::NoPen);
        for (int i = 0; i < bars; ++i) {
            const int from = int(qint64(i) * m_wave.size() / bars);
            const int to = qMax(from + 1, int(qint64(i + 1) * m_wave.size() / bars));
            float v = 0;
            for (int j = from; j < to && j < m_wave.size(); ++j)
                v = qMax(v, m_wave[j]);
            const qreal h = qMax<qreal>(2, v * full);
            const qreal x = x0 + i * pitch;
            const bool played = x + barW / 2 <= posX;
            const bool hovered = m_hoverX >= 0 && ((x >= posX && x <= m_hoverX) || (x <= posX && x >= m_hoverX));
            p.setBrush(played ? (hovered ? accent.lighter(130) : accent) : (hovered ? hoverRest : rest));
            p.drawRoundedRect(QRectF(x, t.center().y() - h / 2, barW, h), 1, 1);
        }
        p.setBrush(accent);
        p.drawRoundedRect(QRectF(qBound(t.left(), posX - 1.5, t.right() - 3), t.top() - 2, 3, t.height() + 4), 1.5, 1.5);
        return;
    }

    // Track
    p.setPen(QPen(theme().isDark() ? QColor(0, 0, 0, 120) : QColor(0, 0, 0, 30), 1));
    p.setBrush(c.input);
    const qreal radius = qMin<qreal>(m_radius, m_thickness / 2.0);
    p.drawRoundedRect(t, radius, radius);

    // Handle position
    const qreal handleLen = 8;
    QPointF centre;
    if (horizontal)
        centre = QPointF(t.left() + handleLen / 2 + ratio * (t.width() - handleLen), t.center().y());
    else
        centre = QPointF(t.center().x(), t.bottom() - handleLen / 2 - ratio * (t.height() - handleLen));

    // Fill from the start of the track up to the handle
    const qreal inset = m_thickness >= 16 ? 3 : 2;
    QRectF fillRect = t.adjusted(inset, inset, -inset, -inset);
    if (horizontal)
        fillRect.setRight(centre.x());
    else
        fillRect.setTop(centre.y());
    if (fillRect.width() > 0 && fillRect.height() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(fillRect, radius - inset / 2, radius - inset / 2);
    }

    // Ticks
    if (m_ticks > 1) {
        for (int i = 1; i < m_ticks; ++i) {
            const qreal f = double(i) / m_ticks;
            const bool filled = f <= ratio;
            QColor tick = filled ? accent.darker(220) : theme().tone(theme().isDark() ? QColor(0x2c, 0x2c, 0x2c) : QColor(0xbb, 0xbb, 0xbb));
            if (filled)
                tick.setAlpha(150);
            p.setPen(Qt::NoPen);
            p.setBrush(tick);
            const qreal len = m_thickness * 0.38;
            if (horizontal) {
                const qreal x = t.left() + handleLen / 2 + f * (t.width() - handleLen);
                p.drawRoundedRect(QRectF(x - 1, t.center().y() - len / 2, 2, len), 1, 1);
            } else {
                const qreal y = t.bottom() - handleLen / 2 - f * (t.height() - handleLen);
                p.drawRoundedRect(QRectF(t.center().x() - len / 2, y - 1, len, 2), 1, 1);
            }
        }
    }

    // Marker
    if (!qIsNaN(m_marker)) {
        const double mf = (m_marker - m_min) / (m_max - m_min);
        p.setPen(Qt::NoPen);
        p.setBrush(c.textMuted);
        if (horizontal) {
            const qreal x = t.left() + handleLen / 2 + mf * (t.width() - handleLen);
            p.drawRect(QRectF(x - 0.5, t.top() + 4, 1, t.height() - 8));
        }
    }

    // Handle
    const qreal along = handleLen;
    const qreal across = m_thickness + 2 * m_overshoot;
    QRectF handle = horizontal ? QRectF(centre.x() - along / 2, centre.y() - across / 2, along, across)
                               : QRectF(centre.x() - across / 2, centre.y() - along / 2, across, along);
    p.setBrush(accent);
    p.drawRoundedRect(handle, 4, 4);
}

void SliderBar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    emit sliderPressed();
    userSet(valueAt(e->position()));
    if (m_hoverTracking)
        emit hovered(valueAt(e->position()), e->position().toPoint());
    if (!m_wave.isEmpty() && !m_dragging) {
        m_hoverX = e->position().x();
        update();
    }
}

void SliderBar::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging)
        userSet(valueAt(e->position()));
    if (m_hoverTracking)
        emit hovered(valueAt(e->position()), e->position().toPoint());
}

void SliderBar::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || !m_dragging)
        return;
    m_dragging = false;
    emit sliderReleased();
    if (m_hoverTracking && !rect().contains(e->position().toPoint()))
        emit hoverEnded();
}

void SliderBar::leaveEvent(QEvent *)
{
    if (m_hoverX >= 0) {
        m_hoverX = -1;
        update();
    }
    if (m_hoverTracking && !m_dragging)
        emit hoverEnded();
}

void SliderBar::wheelEvent(QWheelEvent *e)
{
    const double step = m_step > 0 ? m_step : (m_max - m_min) / 50.0;
    const int dir = e->angleDelta().y() > 0 ? 1 : (e->angleDelta().y() < 0 ? -1 : 0);
    if (dir) {
        emit sliderPressed();
        userSet(m_value + dir * step);
        emit sliderReleased();
    }
    e->accept();
}

void SliderBar::keyPressEvent(QKeyEvent *e)
{
    const double step = m_step > 0 ? m_step : (m_max - m_min) / 50.0;
    switch (e->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        userSet(m_value - step);
        break;
    case Qt::Key_Right:
    case Qt::Key_Up:
        userSet(m_value + step);
        break;
    case Qt::Key_Home:
        userSet(m_min);
        break;
    case Qt::Key_End:
        userSet(m_max);
        break;
    default:
        QWidget::keyPressEvent(e);
        return;
    }
    emit sliderReleased();
}

// ---------------------------------------------------------------------------
// VolumeBar

VolumeBar::VolumeBar(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFixedSize(92, 24);
    setToolTip(tr("Volume"));
    connectTheme(this);
}

void VolumeBar::setValue(int value)
{
    m_value = qBound(0, value, m_max);
    update();
}

void VolumeBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal w = width(), h = height();
    // A wedge, thin on the left and full height on the right, centred
    // vertically, with rounded corners (a core shape grown by a round stroke).
    const qreal round = 3, outline = 1.5;
    const qreal top = outline + round, bottom = h - outline - round;
    const qreal mid = h / 2, thin = (bottom - top) * 0.17;
    QPainterPath core;
    core.moveTo(outline + round, mid - thin);
    core.lineTo(w - outline - round, top);
    core.lineTo(w - outline - round, bottom);
    core.lineTo(outline + round, mid + thin);
    core.closeSubpath();
    QPainterPathStroker grow;
    grow.setWidth(2 * round);
    grow.setJoinStyle(Qt::RoundJoin);
    const QPainterPath wedge = core.united(grow.createStroke(core)).simplified();

    // Empty part.
    p.setPen(Qt::NoPen);
    p.setBrush(theme().isDark() ? QColor(0x2a, 0x2a, 0x2a) : theme().pal().input);
    p.drawPath(wedge);

    // Filled part: the colour runs green → olive → orange → red along the
    // whole bar, so loud (and boosted, past 100 %) shows warmer.
    const qreal x = w * m_value / m_max;
    QLinearGradient grad(0, 0, w, 0);
    if (m_muted) {
        grad.setColorAt(0, QColor(0x5a, 0x5a, 0x5a));
        grad.setColorAt(1, QColor(0x80, 0x80, 0x80));
    } else {
        grad.setColorAt(0.0, QColor(0x5c, 0xb8, 0x0c));
        grad.setColorAt(0.45, QColor(0x7a, 0x9a, 0x10));
        grad.setColorAt(0.78, QColor(0xc0, 0x5a, 0x0c));
        grad.setColorAt(1.0, QColor(0xe0, 0x0a, 0x1e));
    }
    p.save();
    p.setClipRect(QRectF(0, 0, x, h));
    p.setBrush(grad);
    p.drawPath(wedge);
    p.restore();

    // Dark outline around the whole wedge.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(theme().isDark() ? QColor(0, 0, 0) : QColor(0, 0, 0, 120), outline));
    p.drawPath(wedge);
}

void VolumeBar::setFromX(qreal x)
{
    int v = qBound(0, int(qRound(x / width() * m_max)), m_max);
    if (qAbs(v - 100) <= 3)
        v = 100; // gentle snap to 100 %
    if (v != m_value) {
        m_value = v;
        update();
        emit valueChanged(v);
    }
    QToolTip::showText(mapToGlobal(QPoint(int(x), -30)), QString("%1 %").arg(m_value), this);
}

void VolumeBar::mousePressEvent(QMouseEvent *e)
{
    setFromX(e->position().x());
}

void VolumeBar::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton)
        setFromX(e->position().x());
}

void VolumeBar::wheelEvent(QWheelEvent *e)
{
    const int dir = e->angleDelta().y() > 0 ? 5 : -5;
    m_value = qBound(0, m_value + dir, m_max);
    update();
    emit valueChanged(m_value);
    e->accept();
}

// ---------------------------------------------------------------------------
// ToggleSwitch

ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(60, 28);
    connectTheme(this);
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    const Palette &c = theme().pal();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(Qt::NoPen);
    p.setBrush(c.input);
    p.drawRoundedRect(r, 5, 5);

    QFont f = font();
    f.setPixelSize(12);
    f.setLetterSpacing(QFont::PercentageSpacing, 104);
    p.setFont(f);

    const QRectF left(r.left() + 3, r.top() + 3, r.width() / 2 - 3, r.height() - 6);
    const QRectF right(r.center().x(), r.top() + 3, r.width() / 2 - 3, r.height() - 6);
    if (isChecked()) {
        p.setBrush(theme().accentFill());
        p.drawRoundedRect(left, 3, 3);
        p.setPen(theme().accent());
        p.drawText(left, Qt::AlignCenter, tr("ON"));
        p.setPen(Qt::NoPen);
        p.setBrush(theme().accent());
        p.drawRoundedRect(right, 3, 3);
    } else {
        p.setBrush(c.knobOff);
        p.drawRoundedRect(left, 3, 3);
        p.setPen(c.textDim);
        p.drawText(right, Qt::AlignCenter, tr("OFF"));
    }
    if (!isEnabled()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 90));
        p.drawRoundedRect(r, 5, 5);
    }
}

// ---------------------------------------------------------------------------
// SegmentedControl

SegmentedControl::SegmentedControl(const QStringList &options, QWidget *parent)
    : QWidget(parent), m_options(options)
{
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(34);
    connectTheme(this);
}

QSize SegmentedControl::sizeHint() const
{
    return QSize(qMax(300, int(m_options.size()) * 96), 34);
}

void SegmentedControl::setCurrentIndex(int index)
{
    m_current = index;
    update();
}

QRectF SegmentedControl::segmentRect(int i) const
{
    const qreal gap = 3;
    const qreal w = (width() - gap * (m_options.size() - 1)) / m_options.size();
    return QRectF(i * (w + gap), 0, w, height());
}

void SegmentedControl::paintEvent(QPaintEvent *)
{
    const Palette &c = theme().pal();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QFont f = font();
    f.setPixelSize(15);
    p.setFont(f);
    for (int i = 0; i < m_options.size(); ++i) {
        const QRectF r = segmentRect(i).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool sel = i == m_current;
        p.setPen(Qt::NoPen);
        p.setBrush(sel ? theme().accentFill() : (theme().isDark() ? theme().tone(QColor(0x1b, 0x1b, 0x1b)) : c.surface));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(sel ? theme().accent() : c.textDim);
        p.drawText(r, Qt::AlignCenter, m_options[i]);
    }
}

void SegmentedControl::mousePressEvent(QMouseEvent *e)
{
    for (int i = 0; i < m_options.size(); ++i) {
        if (segmentRect(i).contains(e->position()) && i != m_current) {
            m_current = i;
            update();
            emit currentChanged(i);
        }
    }
}

// ---------------------------------------------------------------------------
// AccentSwatches

AccentSwatches::AccentSwatches(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
    connectTheme(this);
}

QSize AccentSwatches::sizeHint() const
{
    return QSize(int(Theme::accentChoices().size()) * 33 + 6, 38);
}

QRectF AccentSwatches::cell(int i) const
{
    return QRectF(3 + i * 33, 3, 32, 32);
}

void AccentSwatches::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(theme().isDark() ? theme().tone(QColor(0x0e, 0x0e, 0x0e)) : theme().pal().surface);
    p.drawRoundedRect(QRectF(rect()), 5, 5);
    const auto colors = Theme::accentChoices();
    for (int i = 0; i < colors.size(); ++i) {
        const QRectF cr = cell(i);
        if (i == theme().accentIndex()) {
            p.setBrush(mixColor(theme().tone(QColor(0x0b, 0x0b, 0x0b)), colors[i], 0.3));
            p.drawRoundedRect(cr, 4, 4);
        }
        p.setBrush(colors[i]);
        p.drawRoundedRect(cr.adjusted(6, 6, -6, -6), 2, 2);
    }
}

void AccentSwatches::mousePressEvent(QMouseEvent *e)
{
    for (int i = 0; i < Theme::accentChoices().size(); ++i)
        if (cell(i).contains(e->position()))
            emit picked(i);
}

// ---------------------------------------------------------------------------
// Dropdown

Dropdown::Dropdown(QWidget *parent) : QComboBox(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(38);
    setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connectTheme(this);
}

void Dropdown::showPopup()
{
    QComboBox::showPopup();
    QWidget *popup = view()->window();
    if (!popup || popup == window())
        return;
    int widest = 0;
    for (int i = 0; i < count(); ++i)
        widest = qMax(widest, view()->fontMetrics().horizontalAdvance(itemText(i)));
    bool icons = false;
    for (int i = 0; i < count() && !icons; ++i)
        icons = !itemIcon(i).isNull();
    const int needed = widest + 110 + (icons ? iconSize().width() + 10 : 0); // margins, check mark, frame
    if (popup->width() < needed)
        popup->resize(needed, popup->height());
}

QSize Dropdown::sizeHint() const
{
    QSize s = QComboBox::sizeHint();
    return QSize(qMax(s.width() + 40, 260), 38);
}

QSize Dropdown::minimumSizeHint() const
{
    return QSize(120, 38);
}

void Dropdown::paintEvent(QPaintEvent *)
{
    const Palette &c = theme().pal();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(theme().isDark() ? theme().tone(QColor(0x14, 0x14, 0x14)) : c.border, 1));
    p.setBrush(theme().isDark() ? theme().tone(QColor(0x1c, 0x1c, 0x1c)) : c.surface);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    QFont f = font();
    f.setPixelSize(15);
    p.setFont(f);
    p.setPen(isEnabled() ? c.text : c.textMuted);
    QRect textRect = rect().adjusted(16, 0, -44, 0);
    if (const QIcon icon = itemIcon(currentIndex()); !icon.isNull()) { // e.g. equalizer curves
        const QSize is = iconSize();
        p.drawPixmap(QPoint(textRect.left(), (height() - is.height()) / 2), icon.pixmap(is, devicePixelRatioF()));
        textRect.setLeft(textRect.left() + is.width() + 10);
    }
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetrics(f).elidedText(currentText(), Qt::ElideRight, textRect.width()));
    p.drawPixmap(QPointF(width() - 34, (height() - 20) / 2.0),
                 theme().pixmap("chevron-down", c.textDim, 20, devicePixelRatioF()));
}

// ---------------------------------------------------------------------------
// ColorField

ColorField::ColorField(const QColor &color, QWidget *parent) : QWidget(parent), m_color(color)
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_edit = new QLineEdit(color.name().toUpper(), this);
    m_edit->setProperty("role", "value");
    m_edit->setFixedHeight(38);
    m_edit->setMaxLength(7);
    m_swatch = new QPushButton(this);
    m_swatch->setCursor(Qt::PointingHandCursor);
    m_swatch->setFixedSize(26, 26);
    lay->addWidget(m_edit);
    // Overlay the swatch inside the right end of the field.
    m_edit->setTextMargins(0, 0, 34, 0);
    auto *inner = new QHBoxLayout(m_edit);
    inner->setContentsMargins(0, 0, 8, 0);
    inner->addStretch();
    inner->addWidget(m_swatch);
    updateSwatch();
    setFixedWidth(200);

    connect(m_edit, &QLineEdit::editingFinished, this, [this] {
        const QColor c(m_edit->text().trimmed());
        if (c.isValid() && c != m_color) {
            m_color = c;
            updateSwatch();
            emit colorChanged(c);
        } else {
            m_edit->setText(m_color.name().toUpper());
        }
    });
    connect(m_swatch, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(m_color, this, tr("Choose colour"));
        if (c.isValid() && c != m_color) {
            setColor(c);
            emit colorChanged(c);
        }
    });
}

void ColorField::setColor(const QColor &color)
{
    m_color = color;
    m_edit->setText(color.name().toUpper());
    updateSwatch();
}

void ColorField::updateSwatch()
{
    m_swatch->setStyleSheet(QString("QPushButton { background: %1; border: 1px solid #555; border-radius: 3px; padding: 0; }")
                                .arg(m_color.name()));
}

// ---------------------------------------------------------------------------
// InfoIcon

InfoIcon::InfoIcon(const QString &tip, QWidget *parent) : QLabel(parent)
{
    setToolTip(tip);
    auto refresh = [this] { setPixmap(theme().pixmap("info", theme().pal().textMuted, 18, devicePixelRatioF())); };
    refresh();
    connect(&theme(), &Theme::changed, this, refresh);
    setFixedSize(20, 20);
}

// ---------------------------------------------------------------------------
// ThemeCard

ThemeCard::ThemeCard(const QString &label, int kind, QWidget *parent) : QAbstractButton(parent), m_kind(kind)
{
    setText(label);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
    connectTheme(this);
}

void ThemeCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF box(4, 4, width() - 8, width() - 8);
    if (isChecked()) {
        p.setPen(QPen(theme().accent(), 3));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box.adjusted(-1.5, -1.5, 1.5, 1.5), 14, 14);
    } else {
        p.setPen(QPen(theme().pal().border, 1.5));
        p.drawRoundedRect(box.adjusted(-1, -1, 1, 1), 14, 14);
    }
    const QRectF inner = box.adjusted(5, 5, -5, -5);
    QPainterPath path;
    path.addRoundedRect(inner, 9, 9);
    p.setPen(Qt::NoPen);
    if (m_kind == Theme::Omarchy) {
        // Desktop theme preview: background, a text line and an accent bar.
        const Palette *om = theme().omarchyPalette();
        p.fillPath(path, om ? om->window : QColor(0x22, 0x22, 0x22));
        if (om) {
            p.setBrush(om->text);
            p.drawRoundedRect(QRectF(inner.left() + 10, inner.top() + 14, inner.width() * 0.55, 5), 2.5, 2.5);
            p.setBrush(om->textMuted);
            p.drawRoundedRect(QRectF(inner.left() + 10, inner.top() + 25, inner.width() * 0.35, 5), 2.5, 2.5);
            p.setBrush(theme().omarchyAccent());
            p.drawRoundedRect(QRectF(inner.left() + 10, inner.bottom() - 18, inner.width() - 20, 7), 3.5, 3.5);
        }
    } else if (m_kind == 0) {
        p.fillPath(path, QColor(0x22, 0x22, 0x22));
    } else if (m_kind == 1) {
        p.fillPath(path, QColor(0xee, 0xee, 0xee));
    } else {
        p.save();
        p.setClipPath(path);
        p.fillRect(inner, QColor(0xee, 0xee, 0xee));
        QPainterPath half;
        half.moveTo(inner.topRight());
        half.lineTo(inner.bottomRight());
        half.lineTo(inner.bottomLeft());
        half.closeSubpath();
        p.fillPath(half, QColor(0x22, 0x22, 0x22));
        p.restore();
    }
    QFont f = font();
    f.setPixelSize(13);
    p.setFont(f);
    p.setPen(isChecked() ? theme().pal().text : theme().pal().textMuted);
    p.drawText(QRectF(0, box.bottom() + 6, width(), height() - box.bottom() - 6), Qt::AlignHCenter | Qt::AlignTop,
               text());
}

// ---------------------------------------------------------------------------
// RadioDot

RadioDot::RadioDot(const QString &text, QWidget *parent) : QRadioButton(text, parent)
{
    setCursor(Qt::PointingHandCursor);
    connectTheme(this);
}

QSize RadioDot::sizeHint() const
{
    QFont f = font();
    f.setPixelSize(15);
    return QSize(32 + QFontMetrics(f).horizontalAdvance(text()) + 8, 28);
}

void RadioDot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(11, height() / 2.0);
    const QColor accent = theme().accent();
    if (isChecked()) {
        p.setPen(QPen(mixColor(accent, Qt::white, 0.45), 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 9, 9);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(c, 6, 6);
    } else {
        p.setPen(QPen(theme().pal().textMuted, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 8, 8);
    }
    QFont f = font();
    f.setPixelSize(15);
    p.setFont(f);
    p.setPen(isChecked() ? theme().pal().text : theme().pal().textDim);
    p.drawText(QRect(30, 0, width() - 30, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
}

// ---------------------------------------------------------------------------
// CollapsiblePanel

namespace {
// Header row: "Collapse Settings" with a chevron, left aligned or centred.
class PanelHeader : public QAbstractButton {
public:
    explicit PanelHeader(QWidget *parent) : QAbstractButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(52);
        connectTheme(this);
    }
    bool centered = false;
    bool expanded = true;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QFont f = font();
        f.setPixelSize(15);
        p.setFont(f);
        const QColor col = underMouse() ? theme().pal().text : theme().pal().textDim;
        p.setPen(col);
        const QPixmap chev = theme().pixmap(expanded ? "chevron-down" : "chevron-right", col, 20, devicePixelRatioF());
        if (centered) {
            const int tw = QFontMetrics(f).horizontalAdvance(text());
            const int x = (width() - tw - 28) / 2;
            p.drawText(QRect(x, 0, tw + 2, height()), Qt::AlignVCenter, text());
            p.drawPixmap(QPointF(x + tw + 8, (height() - 20) / 2.0), chev);
        } else {
            p.drawText(rect().adjusted(20, 0, -50, 0), Qt::AlignVCenter | Qt::AlignLeft, text());
            p.drawPixmap(QPointF(width() - 40, (height() - 20) / 2.0), chev);
        }
    }
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }
};
} // namespace

CollapsiblePanel::CollapsiblePanel(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_header = new PanelHeader(this);
    lay->addWidget(m_header);
    m_content = new QWidget(this);
    m_body = new QVBoxLayout(m_content);
    m_body->setContentsMargins(20, 4, 20, 22);
    m_body->setSpacing(16);
    lay->addWidget(m_content);
    connect(m_header, &QAbstractButton::clicked, this, [this] {
        setExpanded(!m_expanded);
        emit toggled(m_expanded);
    });
    connectTheme(this);
    refreshHeader();
}

void CollapsiblePanel::setHeaderCentered(bool centered)
{
    static_cast<PanelHeader *>(m_header)->centered = centered;
    m_header->update();
}

void CollapsiblePanel::setTexts(const QString &expanded, const QString &collapsed)
{
    m_expandedText = expanded;
    m_collapsedText = collapsed;
    refreshHeader();
}

void CollapsiblePanel::setExpanded(bool expanded)
{
    m_expanded = expanded;
    m_content->setVisible(expanded);
    refreshHeader();
}

void CollapsiblePanel::refreshHeader()
{
    auto *h = static_cast<PanelHeader *>(m_header);
    h->expanded = m_expanded;
    h->setText(m_expanded ? m_expandedText : m_collapsedText);
    h->update();
}

void CollapsiblePanel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(theme().tone(theme().isDark() ? QColor(0x1b, 0x1b, 0x1b) : QColor(0xec, 0xec, 0xec)));
    p.drawRoundedRect(QRectF(rect()), 10, 10);
}

// ---------------------------------------------------------------------------
// ValueSlider

ValueSlider::ValueSlider(const QString &label, double min, double max, double value,
                         std::function<QString(double)> format, QWidget *parent)
    : QWidget(parent), m_format(std::move(format))
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);
    auto *top = new QHBoxLayout;
    auto *name = new QLabel(label, this);
    name->setProperty("role", "dim");
    name->setStyleSheet("font-size: 15px;");
    m_value = new QLabel(this);
    m_value->setProperty("role", "dim");
    m_value->setStyleSheet("font-size: 15px;");
    top->addWidget(name);
    top->addStretch();
    top->addWidget(m_value);
    lay->addLayout(top);
    m_slider = new SliderBar(Qt::Horizontal, this);
    m_slider->setRange(min, max);
    m_slider->setValue(value);
    lay->addWidget(m_slider);
    m_value->setText(m_format(value));
    connect(m_slider, &SliderBar::valueChanged, this, [this](double v) {
        m_value->setText(m_format(v));
        emit valueChanged(v);
    });
}

void ValueSlider::setValue(double v)
{
    m_slider->setValue(v);
    m_value->setText(m_format(m_slider->value()));
}

double ValueSlider::value() const
{
    return m_slider->value();
}

// ---------------------------------------------------------------------------
// Helpers

QWidget *settingRow(const QString &label, QWidget *control, const QString &info, QWidget *parent)
{
    auto *row = new QWidget(parent);
    row->setObjectName("SettingRow");
    row->setProperty("searchText", label);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 4, 0, 4);
    lay->setSpacing(10);
    auto *l = new QLabel(label, row);
    l->setProperty("role", "dim");
    l->setStyleSheet("font-size: 15px;");
    lay->addWidget(l);
    if (!info.isEmpty())
        lay->addWidget(new InfoIcon(info, row));
    lay->addStretch();
    if (control) {
        control->setParent(row);
        lay->addWidget(control);
    }
    row->setMinimumHeight(44);
    return row;
}

QLabel *sectionHeading(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setProperty("role", "heading");
    l->setStyleSheet("font-size: 17px; font-weight: 500; padding-top: 6px;");
    return l;
}

namespace Dialog {
QPushButton *button(const QString &text, bool accent)
{
    auto *b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(36);
    if (accent) {
        b->setProperty("role", "accent");
        b->style()->unpolish(b);
        b->style()->polish(b);
    }
    return b;
}

QLineEdit *field(const QString &placeholder)
{
    auto *e = new QLineEdit;
    e->setFixedHeight(36);
    e->setPlaceholderText(placeholder);
    return e;
}

QLabel *caption(const QString &text, bool wrap)
{
    auto *l = new QLabel(text);
    l->setProperty("role", "dim");
    l->setWordWrap(wrap);
    return l;
}
} // namespace Dialog

QWidget *hline(QWidget *parent)
{
    auto *f = new QFrame(parent);
    f->setProperty("role", "hline");
    f->setFixedHeight(1);
    return f;
}

// ---------------------------------------------------------------------------
// TabButton

TabButton::TabButton(const QString &icon, const QString &text, QWidget *parent) : QAbstractButton(parent), m_icon(icon)
{
    setText(text);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(54);
    connect(&theme(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

QSize TabButton::sizeHint() const
{
    QFont f = font();
    f.setPixelSize(14);
    return QSize(QFontMetrics(f).horizontalAdvance(text()) + 44, 54);
}

void TabButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &c = theme().pal();
    const bool on = isChecked();
    QFont f = font();
    f.setPixelSize(14);
    p.setFont(f);
    const int tw = QFontMetrics(f).horizontalAdvance(text());
    const int total = 20 + 8 + tw;
    const int x = qMax(0, (width() - total) / 2);
    p.drawPixmap(QPointF(x, (height() - 20) / 2.0 - 1),
                 theme().pixmap(m_icon, on ? theme().accent() : c.textMuted, 20, devicePixelRatioF()));
    p.setPen(on ? c.text : c.textDim);
    p.drawText(QRect(x + 28, 0, tw + 4, height() - 2), Qt::AlignVCenter, text());
    if (on) {
        p.setPen(Qt::NoPen);
        p.setBrush(theme().accent());
        p.drawRoundedRect(QRectF(4, height() - 3, width() - 8, 3), 1.5, 1.5);
    }
}

// ---------------------------------------------------------------------------
// ChoiceList

ChoiceList::ChoiceList(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(&theme(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void ChoiceList::setItems(const QList<QPair<QString, QVariant>> &items, const QVariant &current)
{
    m_items = items;
    m_current = current;
    m_hover = -1;
    setCursor(items.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
    updateGeometry();
    update();
}

void ChoiceList::setEmptyText(const QString &text)
{
    m_empty = text;
    update();
}

QSize ChoiceList::sizeHint() const
{
    return QSize(200, kRow * int(qMax<qsizetype>(1, m_items.size())));
}

void ChoiceList::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &c = theme().pal();
    QFont f = font();
    f.setPixelSize(14);
    p.setFont(f);
    if (m_items.isEmpty()) {
        p.setPen(c.textMuted);
        p.drawText(rect().adjusted(34, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m_empty);
        return;
    }
    for (int i = 0; i < m_items.size(); ++i) {
        const QRect r(0, i * kRow, width(), kRow);
        const bool on = m_current.isValid() && m_items[i].second == m_current;
        if (i == m_hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(c.hover);
            p.drawRoundedRect(r.adjusted(0, 2, 0, -2), 6, 6);
        }
        if (on)
            p.drawPixmap(QPointF(8, r.top() + (kRow - 18) / 2.0),
                         theme().pixmap("check", theme().accent(), 18, devicePixelRatioF()));
        p.setPen(on ? c.text : c.textDim);
        const QRect tr = r.adjusted(34, 0, -8, 0);
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(f).elidedText(m_items[i].first, Qt::ElideRight, tr.width()));
    }
}

void ChoiceList::mouseMoveEvent(QMouseEvent *e)
{
    const int row = m_items.isEmpty() ? -1 : int(e->position().y()) / kRow;
    const int hover = row >= 0 && row < m_items.size() ? row : -1;
    if (hover != m_hover) {
        m_hover = hover;
        update();
    }
}

void ChoiceList::mousePressEvent(QMouseEvent *e)
{
    const int row = int(e->position().y()) / kRow;
    if (e->button() != Qt::LeftButton || m_items.isEmpty() || row < 0 || row >= m_items.size())
        return;
    m_current = m_items[row].second;
    update();
    emit chosen(m_current);
}

void ChoiceList::leaveEvent(QEvent *)
{
    m_hover = -1;
    update();
}
