#include "customizeplayer.h"
#include "chrome.h"
#include "prefs.h"
#include "theme.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {
const char *kMime = "application/x-konos-control";
const int kTileW = 104, kTileH = 92;

bool repeatable(const QString &id) { return id == "spacer" || id == "divider"; }

int chipWidth(const QString &id)
{
    if (id == "spacer")
        return 22;
    if (id == "divider")
        return 14;
    if (id == "volume")
        return 120;
    if (id == "thumbnail")
        return 60;
    const ControlItem *it = controlItem(id);
    return it && it->text ? 116 : 38;
}

// "id\nsource": source is "box", or "left:N" / "right:N" for a chip in the bar.
QMimeData *mimeFor(const QString &id, const QString &source)
{
    auto *m = new QMimeData;
    m->setData(kMime, (id + "\n" + source).toUtf8());
    return m;
}

QPixmap dragPixmap(const QString &id, const QSize &size)
{
    QPixmap pm(size * 2);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(theme().accent(), 1.5));
    p.setBrush(theme().pal().surface);
    p.drawRoundedRect(QRectF(1, 1, size.width() - 2, size.height() - 2), 8, 8);
    const ControlItem *it = controlItem(id);
    if (it)
        p.drawPixmap(QRect((size.width() - 22) / 2, (size.height() - 22) / 2, 22, 22),
                     theme().pixmap(it->icon, theme().pal().text, 22));
    return pm;
}
} // namespace

// ---------------------------------------------------------------------------
// ControlStrip

ControlStrip::ControlStrip(QWidget *parent) : QWidget(parent)
{
    setAcceptDrops(true);
    setFixedHeight(64);
    setMouseTracking(true);
}

QRect ControlStrip::seekRect() const
{
    int lw = 12, rw = 12;
    for (const QString &id : left)
        lw += chipWidth(id) + 6;
    for (const QString &id : right)
        rw += chipWidth(id) + 6;
    return QRect(lw + 8, (height() - 26) / 2, qMax(80, width() - lw - rw - 16), 26);
}

QList<ControlStrip::Chip> ControlStrip::chips() const
{
    QList<Chip> out;
    int x = 12;
    for (int i = 0; i < left.size(); ++i) {
        const int w = chipWidth(left[i]);
        out.append({left[i], true, i, QRect(x, (height() - 40) / 2, w, 40)});
        x += w + 6;
    }
    x = width() - 12;
    for (int i = right.size() - 1; i >= 0; --i) {
        const int w = chipWidth(right[i]);
        x -= w;
        out.prepend({right[i], false, i, QRect(x, (height() - 40) / 2, w, 40)});
        x -= 6;
    }
    return out;
}

void ControlStrip::insert(const QString &id, bool toLeft, int index)
{
    if (!repeatable(id)) {
        for (bool l : {true, false}) {
            QStringList &list = l ? left : right;
            const int at = list.indexOf(id);
            if (at >= 0) {
                list.removeAt(at);
                if (l == toLeft && at < index)
                    --index;
            }
        }
    }
    QStringList &list = toLeft ? left : right;
    list.insert(qBound(0, index, int(list.size())), id);
    update();
    emit changed();
}

void ControlStrip::removeAt(bool fromLeft, int index)
{
    QStringList &list = fromLeft ? left : right;
    if (index >= 0 && index < list.size())
        list.removeAt(index);
    update();
    emit changed();
}

void ControlStrip::dropTarget(const QPoint &pos, bool &toLeft, int &index) const
{
    toLeft = pos.x() < seekRect().center().x();
    index = 0;
    for (const Chip &c : chips())
        if (c.left == toLeft && c.rect.center().x() < pos.x())
            index = c.index + 1;
}

void ControlStrip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &c = theme().pal();
    p.setPen(QPen(c.border, 1));
    p.setBrush(c.chrome);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    // The seek bar stays where it is.
    const QRect seek = seekRect();
    p.setPen(Qt::NoPen);
    p.setBrush(c.input);
    p.drawRoundedRect(seek, 4, 4);
    p.setBrush(theme().accentFill());
    p.drawRoundedRect(QRect(seek.left(), seek.top(), seek.width() / 4, seek.height()), 4, 4);
    p.setPen(c.textMuted);
    p.drawText(seek, Qt::AlignCenter, tr("Time and seek bar"));

    for (const Chip &chip : chips()) {
        const ControlItem *it = controlItem(chip.id);
        if (!it)
            continue;
        const QRectF r = QRectF(chip.rect).adjusted(0.5, 0.5, -0.5, -0.5);
        if (chip.id == "divider") {
            p.setPen(QPen(c.border, 1.5));
            p.drawLine(QPointF(r.center().x(), r.top() + 8), QPointF(r.center().x(), r.bottom() - 8));
            continue;
        }
        if (chip.id == "spacer") {
            p.setPen(QPen(c.textMuted, 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(2, 8, -2, -8), 4, 4);
            continue;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(c.surface);
        p.drawRoundedRect(r, 7, 7);
        if (it->text) {
            p.drawPixmap(QRect(chip.rect.left() + 8, chip.rect.center().y() - 8, 16, 16),
                         theme().pixmap(it->icon, c.textDim, 16));
            p.setPen(c.textDim);
            p.drawText(chip.rect.adjusted(30, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       fontMetrics().elidedText(it->label, Qt::ElideRight, chip.rect.width() - 36));
        } else if (chip.id == "volume") {
            p.drawPixmap(QRect(chip.rect.left() + 8, chip.rect.center().y() - 10, 20, 20),
                         theme().pixmap("volume", c.text, 20));
            p.setBrush(theme().volumeColor());
            p.drawRoundedRect(QRectF(chip.rect.left() + 34, chip.rect.center().y() - 3, 52, 6), 3, 3);
            p.setBrush(c.input);
            p.drawRoundedRect(QRectF(chip.rect.left() + 86, chip.rect.center().y() - 3, 24, 6), 3, 3);
        } else {
            const QColor col = chip.id == "play" ? theme().accent() : c.text;
            p.drawPixmap(QRect(chip.rect.center().x() - 10, chip.rect.center().y() - 10, 20, 20),
                         theme().pixmap(it->icon, col, 20));
        }
    }
    // Where a drop would land.
    if (m_dropIndex >= 0) {
        int x;
        const QList<Chip> all = chips();
        QList<Chip> zone;
        for (const Chip &ch : all)
            if (ch.left == m_dropLeft)
                zone << ch;
        if (zone.isEmpty())
            x = m_dropLeft ? 10 : width() - 10;
        else if (m_dropIndex < zone.size())
            x = zone[m_dropIndex].rect.left() - 3;
        else
            x = zone.last().rect.right() + 3;
        p.setPen(QPen(theme().accent(), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(x, 10, x, height() - 10);
    }
}

void ControlStrip::mousePressEvent(QMouseEvent *e)
{
    m_pressed = -1;
    const QList<Chip> all = chips();
    for (int i = 0; i < all.size(); ++i)
        if (all[i].rect.contains(e->position().toPoint()))
            m_pressed = i;
    m_pressPos = e->position().toPoint();
}

void ControlStrip::mouseMoveEvent(QMouseEvent *e)
{
    const QList<Chip> all = chips();
    bool overChip = false;
    for (const Chip &c : all)
        overChip = overChip || c.rect.contains(e->position().toPoint());
    setCursor(overChip ? Qt::OpenHandCursor : Qt::ArrowCursor);
    if (!(e->buttons() & Qt::LeftButton) || m_pressed < 0 || m_pressed >= all.size() ||
        (e->position().toPoint() - m_pressPos).manhattanLength() < 6)
        return;
    const Chip chip = all[m_pressed];
    m_pressed = -1;
    auto *drag = new QDrag(this);
    drag->setMimeData(mimeFor(chip.id, QString("%1:%2").arg(chip.left ? "left" : "right").arg(chip.index)));
    drag->setPixmap(dragPixmap(chip.id, chip.rect.size()));
    drag->setHotSpot(QPoint(chip.rect.width() / 2, chip.rect.height() / 2));
    drag->exec(Qt::MoveAction);
    // Dropped back on the bar: the drop already moved it. Anywhere else (the
    // box, or outside the window) takes it off the bar.
    if (drag->target() != this)
        removeAt(chip.left, chip.index);
}

void ControlStrip::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasFormat(kMime))
        e->acceptProposedAction();
}

void ControlStrip::dragMoveEvent(QDragMoveEvent *e)
{
    dropTarget(e->position().toPoint(), m_dropLeft, m_dropIndex);
    e->acceptProposedAction();
    update();
}

void ControlStrip::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dropIndex = -1;
    update();
}

void ControlStrip::dropEvent(QDropEvent *e)
{
    const QStringList parts = QString::fromUtf8(e->mimeData()->data(kMime)).split('\n');
    m_dropIndex = -1;
    if (parts.size() != 2)
        return;
    bool toLeft;
    int index;
    dropTarget(e->position().toPoint(), toLeft, index);
    const QString id = parts[0], source = parts[1];
    if (source != "box") { // moving a chip: take it out of its old place first
        const bool fromLeft = source.startsWith("left");
        const int from = source.section(':', 1).toInt();
        QStringList &list = fromLeft ? left : right;
        if (from >= 0 && from < list.size() && list[from] == id) {
            list.removeAt(from);
            if (fromLeft == toLeft && from < index)
                --index;
        }
    }
    insert(id, toLeft, index);
    e->acceptProposedAction();
}

void ControlStrip::contextMenuEvent(QContextMenuEvent *e)
{
    for (const Chip &c : chips()) {
        if (!c.rect.contains(e->pos()))
            continue;
        QMenu menu(this);
        menu.addAction(tr("Remove from the Bar"), this, [this, c] { removeAt(c.left, c.index); });
        menu.exec(e->globalPos());
        return;
    }
}

// ---------------------------------------------------------------------------
// ControlPalette

ControlPalette::ControlPalette(ControlStrip *strip, QWidget *parent) : QWidget(parent), m_strip(strip)
{
    setAcceptDrops(true);
    setMouseTracking(true);
    connect(strip, &ControlStrip::changed, this, qOverload<>(&QWidget::update));
}

int ControlPalette::columns() const
{
    return qMax(1, width() / kTileW);
}

int ControlPalette::heightForWidth(int w) const
{
    const int cols = qMax(1, w / kTileW);
    return (int(controlItems().size()) + cols - 1) / cols * kTileH + 8;
}

QSize ControlPalette::sizeHint() const
{
    return {kTileW * 9, heightForWidth(kTileW * 9)};
}

QRect ControlPalette::tileRect(int i) const
{
    const int cols = columns();
    const int x0 = (width() - cols * kTileW) / 2;
    return QRect(x0 + i % cols * kTileW, i / cols * kTileH + 4, kTileW, kTileH);
}

void ControlPalette::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &c = theme().pal();
    const QList<ControlItem> &items = controlItems();
    for (int i = 0; i < items.size(); ++i) {
        const ControlItem &it = items[i];
        const QRect r = tileRect(i);
        const bool used = !repeatable(it.id) && (m_strip->left.contains(it.id) || m_strip->right.contains(it.id));
        const QRect box(r.center().x() - 17, r.top() + 10, 34, 34);
        p.setPen(QPen(used ? c.border : c.textMuted, 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        const QColor col = used ? c.textMuted : c.text;
        if (it.id != "spacer")
            p.drawPixmap(QRect(box.center().x() - 10, box.center().y() - 10, 20, 20), theme().pixmap(it.icon, col, 20));
        p.setPen(used ? c.textMuted : c.textDim);
        p.drawText(QRect(r.left() + 4, box.bottom() + 6, r.width() - 8, 34), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                   it.label);
    }
}

void ControlPalette::mousePressEvent(QMouseEvent *e)
{
    m_pressed = -1;
    for (int i = 0; i < int(controlItems().size()); ++i)
        if (tileRect(i).contains(e->position().toPoint()))
            m_pressed = i;
    m_pressPos = e->position().toPoint();
}

void ControlPalette::mouseMoveEvent(QMouseEvent *e)
{
    bool over = false;
    for (int i = 0; i < int(controlItems().size()); ++i)
        over = over || tileRect(i).contains(e->position().toPoint());
    setCursor(over ? Qt::OpenHandCursor : Qt::ArrowCursor);
    if (!(e->buttons() & Qt::LeftButton) || m_pressed < 0 ||
        (e->position().toPoint() - m_pressPos).manhattanLength() < 6)
        return;
    const ControlItem it = controlItems().at(m_pressed);
    m_pressed = -1;
    auto *drag = new QDrag(this);
    drag->setMimeData(mimeFor(it.id, "box"));
    drag->setPixmap(dragPixmap(it.id, QSize(chipWidth(it.id), 40)));
    drag->setHotSpot(QPoint(chipWidth(it.id) / 2, 20));
    drag->exec(Qt::CopyAction | Qt::MoveAction);
}

void ControlPalette::dragEnterEvent(QDragEnterEvent *e)
{
    // Chips from the bar can be dropped here to take them off.
    if (e->mimeData()->hasFormat(kMime) && !QString::fromUtf8(e->mimeData()->data(kMime)).endsWith("\nbox"))
        e->acceptProposedAction();
}

void ControlPalette::dropEvent(QDropEvent *e)
{
    e->acceptProposedAction(); // the bar removes it when the drag ends
}

// ---------------------------------------------------------------------------
// CustomizePlayerOverlay

CustomizePlayerOverlay::CustomizePlayerOverlay(QWidget *window)
    : PanelOverlay(tr("Customize Player"), {1080, 680}, window)
{
    auto *intro = new QLabel(tr("Drag items into the bar below to add them, and along it to re-order them. Drag "
                                "them back into the box (or right-click them) to take them off the bar."));
    intro->setProperty("role", "dim");
    intro->setWordWrap(true);
    body()->addWidget(intro);

    m_strip = new ControlStrip;
    m_strip->left = savedControls(true);
    m_strip->right = savedControls(false);
    m_palette = new ControlPalette(m_strip);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(m_palette);
    body()->addWidget(scroll, 1);

    auto *barLabel = new QLabel(tr("The playback bar"));
    barLabel->setProperty("role", "dim");
    body()->addWidget(barLabel);
    body()->addWidget(m_strip);

    auto *foot = new QHBoxLayout;
    auto button = [](const QString &text, bool accent = false) {
        auto *b = new QPushButton(text);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedHeight(36);
        if (accent)
            b->setProperty("role", "accent");
        return b;
    };
    auto *reset = button(tr("Reset to Default"));
    auto *cancel = button(tr("Cancel"));
    auto *save = button(tr("Save"), true);
    foot->addWidget(reset);
    foot->addStretch();
    foot->addWidget(cancel);
    foot->addWidget(save);
    body()->addLayout(foot);

    connect(reset, &QPushButton::clicked, this, [this] {
        m_strip->left = defaultControls(true);
        m_strip->right = defaultControls(false);
        m_strip->update();
        m_palette->update();
    });
    connect(cancel, &QPushButton::clicked, this, &PanelOverlay::close);
    connect(save, &QPushButton::clicked, this, [this] {
        for (bool l : {true, false}) {
            const QStringList list = l ? m_strip->left : m_strip->right;
            prefs().set(l ? "ui/controlsLeft" : "ui/controlsRight",
                        list == defaultControls(l) ? QString("default") : list.join(','));
        }
        close();
    });
}

QStringList CustomizePlayerOverlay::items(bool left) const
{
    return left ? m_strip->left : m_strip->right;
}

void CustomizePlayerOverlay::insert(const QString &id, bool left, int index)
{
    m_strip->insert(id, left, index);
}

void CustomizePlayerOverlay::remove(bool left, int index)
{
    m_strip->removeAt(left, index);
}
