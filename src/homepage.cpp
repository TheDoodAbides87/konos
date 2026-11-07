#include "homepage.h"
#include "musicindex.h"
#include "library.h"
#include "prefs.h"
#include "theme.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// Card sizes for the Small / Medium / Large thumbnail setting.
qreal cardScale()
{
    static const qreal scales[] = {0.82, 1.0, 1.25};
    return scales[qBound(0, prefs().integer("ui/thumbSize"), 2)];
}

} // namespace

// One card: a thumbnail (16:9 for video, square for audio and folders), a
// title and, for audio, the artist.
class HomeCard : public QWidget {
public:
    QString path, title, subtitle;
    EntryKind kind = EntryKind::Video;
    std::function<void()> activated;
    std::function<void(const QPoint &)> menuRequested;

    HomeCard(QWidget *parent) : QWidget(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }

    QSize thumbSize() const
    {
        const qreal s = cardScale();
        return kind == EntryKind::Video ? QSize(qRound(216 * s), qRound(122 * s)) : QSize(qRound(137 * s), qRound(137 * s));
    }
    QSize sizeHint() const override
    {
        const QSize t = thumbSize();
        return {t.width(), t.height() + (subtitle.isEmpty() ? 40 : 62)};
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const Palette &c = theme().pal();
        const qreal dpr = devicePixelRatioF();
        const QRectF thumb(QPointF(0, 0), QSizeF(thumbSize()));
        const bool hover = underMouse();

        QPainterPath clip;
        clip.addRoundedRect(thumb, 8, 8);
        p.save();
        p.setClipPath(clip);
        QPixmap pm = Thumbnailer::instance().get(path);
        if (kind == EntryKind::Video) {
            p.fillRect(thumb, Qt::black);
        } else if (kind == EntryKind::Audio && pm.isNull()) {
            // No cover: an accent-tinted tile with a note, like a blank sleeve.
            p.fillRect(thumb, mixColor(c.surface, theme().accent(), 0.18));
        } else {
            p.fillRect(thumb, theme().isDark() ? theme().tone(QColor(0x1c, 0x1c, 0x1c)) : c.surface);
        }
        if (!pm.isNull()) {
            const QSizeF scaled = QSizeF(pm.size()).scaled(thumb.size(), Qt::KeepAspectRatioByExpanding);
            p.drawPixmap(QRectF(thumb.center().x() - scaled.width() / 2, thumb.center().y() - scaled.height() / 2,
                                scaled.width(), scaled.height()),
                         pm, QRectF(pm.rect()));
        } else if (kind != EntryKind::Video) {
            const int s = kind == EntryKind::Audio ? int(thumb.height() * 0.55) : 34;
            const QColor ic = kind == EntryKind::Audio ? theme().accent() : c.textDim;
            p.drawPixmap(QPointF(thumb.center().x() - s / 2.0, thumb.center().y() - s / 2.0),
                         theme().pixmap(kind == EntryKind::Audio ? "note" : "folder", ic, s, dpr));
        }
        if (hover) {
            p.fillRect(thumb, QColor(255, 255, 255, 16));
            if (kind != EntryKind::Folder) {
                const QRectF circle(thumb.center().x() - 22, thumb.center().y() - 22, 44, 44);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 150));
                p.drawEllipse(circle);
                p.drawPixmap(QPointF(circle.center().x() - 10, circle.center().y() - 11),
                             theme().pixmap("play-fill", QColor(0xf0, 0xf0, 0xf0), 22, dpr));
            }
        }
        const double progress = kind == EntryKind::Video ? library().progress(path) : 0;
        if (progress > 0.01 && progress < 0.95) {
            p.setPen(Qt::NoPen);
            p.setBrush(theme().accent());
            p.drawRect(QRectF(thumb.left(), thumb.bottom() - 4, thumb.width() * progress, 4));
        }
        p.restore();

        QFont f = font();
        f.setPixelSize(14);
        p.setFont(f);
        p.setPen(c.text);
        const QRectF titleRect(0, thumb.bottom() + 10, width(), 22);
        p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(f).elidedText(title, Qt::ElideMiddle, width()));
        if (!subtitle.isEmpty()) {
            f.setPixelSize(13);
            p.setFont(f);
            p.setPen(c.textMuted);
            p.drawText(QRectF(0, titleRect.bottom() + 2, width(), 20), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(f).elidedText(subtitle, Qt::ElideRight, width()));
        }
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && activated)
            activated();
    }
    void contextMenuEvent(QContextMenuEvent *e) override
    {
        if (menuRequested)
            menuRequested(e->globalPos());
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave)
            update();
        return QWidget::event(e);
    }
};

// "Title ›" header plus one row of cards; cards that don't fit are hidden.
class HomeRow : public QWidget {
public:
    QList<HomeCard *> cards;
    std::function<void()> headerClicked;

    HomeRow(const QString &title, QWidget *parent) : QWidget(parent)
    {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(14);
        m_header = new QPushButton(title + QStringLiteral("  ›"));
        m_header->setProperty("role", "homeHeader");
        m_header->setCursor(Qt::PointingHandCursor);
        m_header->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        connect(m_header, &QPushButton::clicked, this, [this] {
            if (headerClicked)
                headerClicked();
        });
        v->addWidget(m_header);
        m_strip = new QWidget;
        m_cards = new QHBoxLayout(m_strip);
        m_cards->setContentsMargins(0, 0, 0, 0);
        m_cards->setSpacing(16);
        m_cards->addStretch();
        v->addWidget(m_strip);
    }

    void clear()
    {
        qDeleteAll(cards);
        cards.clear();
    }
    HomeCard *add()
    {
        auto *card = new HomeCard(m_strip);
        m_cards->insertWidget(m_cards->count() - 1, card);
        cards << card;
        return card;
    }
    // Shows the cards matching the filter that fit the width; hides the row
    // when there are none.
    void relayout(const QString &filter)
    {
        int x = 0, shown = 0;
        const int avail = width();
        for (HomeCard *card : std::as_const(cards)) {
            card->setFixedSize(card->sizeHint());
            const bool match = filter.isEmpty() || card->title.contains(filter, Qt::CaseInsensitive) ||
                               card->subtitle.contains(filter, Qt::CaseInsensitive);
            const bool fits = x + card->width() <= qMax(avail, card->width());
            card->setVisible(match && fits);
            if (match && fits) {
                x += card->width() + m_cards->spacing();
                ++shown;
            }
        }
        setVisible(shown > 0);
    }
    int count() const { return int(cards.size()); }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        relayout(filter);
    }

public:
    QString filter;

private:
    QPushButton *m_header;
    QWidget *m_strip;
    QHBoxLayout *m_cards;
};

HomePage::HomePage(QWidget *parent) : QScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    content->setObjectName("HomeContent");
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(0, 6, 20, 24);
    v->setSpacing(36);
    m_continue = new HomeRow(tr("Continue Watching"), content);
    m_listen = new HomeRow(tr("Listen Again"), content);
    m_folders = new HomeRow(tr("Frequently Visited Folders"), content);
    m_continue->headerClicked = [this] { emit sectionRequested(Section::Recent); };
    m_listen->headerClicked = [this] { emit sectionRequested(Section::Music); };
    m_folders->headerClicked = [this] { emit sectionRequested(Section::Videos); };
    v->addWidget(m_continue);
    v->addWidget(m_listen);
    v->addWidget(m_folders);
    m_empty = new QLabel(tr("Nothing here yet.\nOpen a file with Ctrl+O, drop media onto the window,\n"
                            "or add folders in Settings → Library."));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setProperty("role", "muted");
    v->addWidget(m_empty, 1);
    v->addStretch();
    setWidget(content);

    connect(&Thumbnailer::instance(), &Thumbnailer::ready, this, [this] { widget()->update(); });
    m_reload.setSingleShot(true);
    m_reload.setInterval(1500);
    connect(&m_reload, &QTimer::timeout, this, &HomePage::reload);
    connect(&library(), &Library::positionsChanged, this, [this] {
        for (HomeCard *c : std::as_const(m_continue->cards))
            c->update();
        if (isVisible())
            m_reload.start();
    });
    connect(&theme(), &Theme::changed, this, [this] { widget()->update(); });
    connect(&prefs(), &Prefs::changed, this, [this](const QString &key) {
        if (key == "ui/thumbSize")
            reload();
    });
}

void HomePage::reload()
{
    for (HomeRow *row : {m_continue, m_listen, m_folders})
        row->clear();

    // Continue Watching: unfinished videos, most recent first.
    QStringList videos, audio;
    for (const QString &loc : library().recent()) {
        const EntryKind kind = Library::kindFor(loc);
        const double progress = library().progress(loc);
        if (kind == EntryKind::Video && progress > 0.01 && progress < 0.95 && videos.size() < 12)
            videos << loc;
        else if (kind == EntryKind::Audio && audio.size() < 16)
            audio << loc;
    }
    for (int i = 0; i < videos.size(); ++i) {
        HomeCard *card = m_continue->add();
        card->kind = EntryKind::Video;
        card->path = videos[i];
        card->title = Library::displayName(videos[i]);
        card->activated = [this, loc = videos[i]] { emit playRequested({loc}, 0); };
        card->menuRequested = [this, loc = videos[i]](const QPoint &pos) { showMenu(loc, EntryKind::Video, pos); };
    }
    // Listen Again: recent audio, played back as a list from the card picked.
    for (int i = 0; i < audio.size(); ++i) {
        Library::Meta meta = library().meta(audio[i]);
        if (const MusicIndex::Track *t = musicIndex().track(audio[i])) {
            if (meta.title.isEmpty())
                meta.title = t->title;
            if (meta.artist.isEmpty())
                meta.artist = t->artist;
        }
        HomeCard *card = m_listen->add();
        card->kind = EntryKind::Audio;
        card->path = audio[i];
        card->title = !meta.title.isEmpty() ? meta.title
                      : Library::isUrl(audio[i]) ? Library::displayName(audio[i])
                                                 : QFileInfo(audio[i]).completeBaseName();
        card->subtitle = meta.artist.isEmpty() ? QFileInfo(QFileInfo(audio[i]).absolutePath()).fileName() : meta.artist;
        card->activated = [this, audio, i] { emit playRequested(audio, i); };
        card->menuRequested = [this, loc = audio[i]](const QPoint &pos) { showMenu(loc, EntryKind::Audio, pos); };
    }
    // Frequently Visited Folders, topped up with the library folders.
    QStringList folders = library().frequentFolders(12);
    for (const QString &f : prefs().list("library/videoFolders") + prefs().list("library/musicFolders"))
        if (folders.size() < 12 && QFileInfo(f).isDir() && !folders.contains(QDir::cleanPath(f)))
            folders << QDir::cleanPath(f);
    for (const QString &f : std::as_const(folders)) {
        HomeCard *card = m_folders->add();
        card->kind = EntryKind::Folder;
        card->path = f;
        card->title = QFileInfo(f).fileName();
        card->activated = [this, f] { emit folderRequested(f); };
        card->menuRequested = [this, f](const QPoint &pos) { showMenu(f, EntryKind::Folder, pos); };
    }
    setFilter(m_filter);
}

void HomePage::setFilter(const QString &text)
{
    m_filter = text.trimmed();
    for (HomeRow *row : {m_continue, m_listen, m_folders}) {
        row->filter = m_filter;
        row->relayout(m_filter);
    }
    m_empty->setVisible(itemCount() == 0);
    if (itemCount() == 0)
        m_empty->setText(m_filter.isEmpty() ? tr("Nothing here yet.\nOpen a file with Ctrl+O, drop media onto the window,\n"
                                                 "or add folders in Settings → Library.")
                                            : tr("Nothing on Home matches “%1”.").arg(m_filter));
}

int HomePage::itemCount() const
{
    int n = 0;
    for (HomeRow *row : {m_continue, m_listen, m_folders})
        n += row->count();
    return n;
}

void HomePage::showMenu(const QString &path, EntryKind kind, const QPoint &globalPos)
{
    const QColor accent = theme().accent();
    QMenu menu(this);
    if (kind == EntryKind::Folder) {
        menu.addAction(theme().icon("folder", accent), tr("Open"), this, [this, path] { emit folderRequested(path); });
        menu.addAction(theme().icon("play", accent), tr("Play folder"), this, [this, path] {
            const QStringList items = library().mediaInFolder(path);
            if (!items.isEmpty())
                emit playRequested(items, 0);
        });
        menu.addAction(theme().icon("plus", accent), tr("Add folder to queue"), this,
                       [this, path] { emit enqueueRequested(library().mediaInFolder(path)); });
        menu.addAction(theme().icon("external", accent), tr("Open in file manager"), this,
                       [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    } else {
        menu.addAction(theme().icon("play", accent), tr("Play"), this, [this, path] { emit playRequested({path}, 0); });
        menu.addAction(theme().icon("plus", accent), tr("Add to queue"), this, [this, path] { emit enqueueRequested({path}); });
        if (kind == EntryKind::Video)
            menu.addAction(theme().icon("eye", accent), tr("Mark as watched"), this,
                           [path] { library().setWatched(path, true); });
        if (!Library::isUrl(path))
            menu.addAction(theme().icon("folder", accent), tr("Show in folder"), this, [path] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
            });
        menu.addSeparator();
        menu.addAction(theme().icon("trash", accent), tr("Remove from recent"), this,
                       [path] { library().removeRecent(path); });
    }
    menu.exec(globalPos);
}
