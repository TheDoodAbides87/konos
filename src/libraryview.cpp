#include "libraryview.h"
#include "homepage.h"
#include "musicindex.h"
#include "durations.h"
#include "artistimages.h"
#include "musiceditors.h"
#include "prefs.h"
#include "theme.h"
#include "widgets.h"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCollator>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QMimeData>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <random>

namespace {
// Rows of the play queue being dragged within it (their positions, comma-separated).
const char *const kQueueRowsMime = "application/x-vmp-queue-rows";

enum Roles {
    KindRole = Qt::UserRole + 1,
    PathRole,
    OrderRole,
    SizeRole,
    DateRole,
    IconRole,   // icon name, for categories and groups
    ThumbRole,  // file whose thumbnail/cover to show (groups: one of their tracks)
    DetailRole, // right-hand text in list rows ("3 songs")
    YearRole,   // music: release year (0 = unknown)
    ArtistRole, // music: artist (albums: album artist)
    AlbumRole,  // music: album
};

// Sort keys. Which ones a view offers depends on what it lists.
enum SortKey { SortDefault, SortName, SortDate, SortSize, SortYear, SortArtist, SortAlbum };

QString itemCount(int n) { return n == 1 ? QObject::tr("1 item") : QObject::tr("%1 items").arg(n); }

QString songCount(int n) { return n == 1 ? QObject::tr("1 song") : QObject::tr("%1 songs").arg(n); }

MusicIndex::Field musicField(const QString &view)
{
    if (view == "artists")
        return prefs().get("music/artistMode").toString() == "albumartist" ? MusicIndex::AlbumArtist
                                                                            : MusicIndex::Artist;
    return view == "genres" ? MusicIndex::Genre : MusicIndex::Album;
}

QString musicViewName(const QString &view)
{
    if (view == "songs")
        return QObject::tr("Songs");
    if (view == "folders")
        return QObject::tr("Folders");
    if (view == "artists")
        return QObject::tr("Artists"); // whichever tag it's grouped by
    return MusicIndex::fieldName(musicField(view));
}

// Sorting is remembered per kind of list ("music/albums", "music/artists/group", "videos"…).
QString sortContext(Section section, const QString &music, bool inGroup, bool inAlbum = false)
{
    if (section != Section::Music || music.isEmpty() || music == "folders")
        return QString::number(int(section));
    return "music/" + music + (inAlbum ? "/album" : inGroup ? "/group" : "");
}

MusicIndex::Within within(MusicIndex::Field field, const QString &key)
{
    return {field, key, true};
}

QCollator &collator()
{
    static QCollator c = [] {
        QCollator x;
        x.setNumericMode(true);
        x.setCaseSensitivity(Qt::CaseInsensitive);
        return x;
    }();
    return c;
}

static bool isMediaEntry(const QModelIndex &idx)
{
    const auto kind = EntryKind(idx.data(KindRole).toInt());
    return kind == EntryKind::Video || kind == EntryKind::Audio || kind == EntryKind::Stream;
}

class EntryProxy : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    int key = 0;

protected:
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override
    {
        const auto ka = EntryKind(a.data(KindRole).toInt());
        const auto kb = EntryKind(b.data(KindRole).toInt());
        // Actions ("Create playlist") stay on top, then folders.
        const bool desc = sortOrder() == Qt::DescendingOrder;
        auto rank = [](EntryKind k) {
            return k == EntryKind::Action || k == EntryKind::Category ? 0 : (k == EntryKind::Folder ? 1 : 2);
        };
        if (rank(ka) != rank(kb))
            return desc ? rank(ka) > rank(kb) : rank(ka) < rank(kb);
        if (rank(ka) == 0) // the Music menu keeps its order
            return desc ? a.data(OrderRole).toInt() > b.data(OrderRole).toInt()
                        : a.data(OrderRole).toInt() < b.data(OrderRole).toInt();
        // Ties keep the natural order, whichever way the list is sorted.
        auto natural = [&] {
            const int oa = a.data(OrderRole).toInt(), ob = b.data(OrderRole).toInt();
            return desc ? oa > ob : oa < ob;
        };
        auto byText = [&](int role) {
            const QString ta = a.data(role).toString(), tb = b.data(role).toString();
            if (ta.isEmpty() != tb.isEmpty()) // untagged last
                return desc ? ta.isEmpty() : tb.isEmpty();
            const int c = collator().compare(ta, tb);
            return c ? c < 0 : natural();
        };
        switch (key) {
        case SortYear: {
            const int ya = a.data(YearRole).toInt(), yb = b.data(YearRole).toInt();
            if ((ya == 0) != (yb == 0)) // unknown year last
                return desc ? ya == 0 : yb == 0;
            return ya != yb ? ya < yb : natural();
        }
        case SortArtist:
            return byText(ArtistRole);
        case SortAlbum:
            return byText(AlbumRole);
        case SortName:
            return collator().compare(a.data(Qt::DisplayRole).toString(), b.data(Qt::DisplayRole).toString()) < 0;
        case 2:
            return a.data(DateRole).toDateTime() < b.data(DateRole).toDateTime();
        case 3:
            return a.data(SizeRole).toLongLong() < b.data(SizeRole).toLongLong();
        default:
            return a.data(OrderRole).toInt() < b.data(OrderRole).toInt();
        }
    }
public:
    // Turns a dragged entry into the files behind it (folders, music groups, playlists).
    std::function<QStringList(const QModelIndex &)> resolve;

    QStringList mimeTypes() const override { return {"text/uri-list", kQueueRowsMime}; }
    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        QList<QUrl> urls;
        QStringList rows;
        for (const QModelIndex &idx : indexes) {
            const auto kind = EntryKind(idx.data(KindRole).toInt());
            if (kind == EntryKind::Action || kind == EntryKind::Category)
                continue;
            const QString path = idx.data(PathRole).toString();
            const QStringList files = resolve ? resolve(idx) : QStringList{path};
            for (const QString &f : files)
                urls << (Library::isUrl(f) ? QUrl(f) : QUrl::fromLocalFile(f));
            rows << QString::number(idx.data(OrderRole).toInt());
        }
        if (urls.isEmpty())
            return nullptr;
        auto *mime = new QMimeData;
        mime->setUrls(urls);
        mime->setData(kQueueRowsMime, rows.join(',').toUtf8());
        return mime;
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        Qt::ItemFlags f = QSortFilterProxyModel::flags(index) & ~Qt::ItemIsDropEnabled;
        const auto kind = EntryKind(index.data(KindRole).toInt());
        if (kind == EntryKind::Action || kind == EntryKind::Category)
            f &= ~Qt::ItemIsDragEnabled;
        return f;
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const QModelIndex idx = sourceModel()->index(row, 0, parent);
        if (filterRegularExpression().pattern().isEmpty())
            return true;
        if (EntryKind(idx.data(KindRole).toInt()) == EntryKind::Action)
            return false;
        return QSortFilterProxyModel::filterAcceptsRow(row, parent);
    }
};
} // namespace

QString sectionName(Section s)
{
    switch (s) {
    case Section::Home: return QObject::tr("Home");
    case Section::Videos: return QObject::tr("Videos");
    case Section::Music: return QObject::tr("Music");
    case Section::Network: return QObject::tr("Network");
    case Section::Playlists: return QObject::tr("Playlists");
    case Section::Recent: return QObject::tr("Recent");
    case Section::Queue: return QObject::tr("Play Queue");
    }
    return {};
}

QString sectionIcon(Section s)
{
    switch (s) {
    case Section::Home: return "home";
    case Section::Videos: return "videos";
    case Section::Music: return "music";
    case Section::Network: return "network";
    case Section::Playlists: return "playlist";
    case Section::Recent: return "recent";
    case Section::Queue: return "queue";
    }
    return {};
}

// ---------------------------------------------------------------------------
// EntryDelegate

static QString iconForKind(EntryKind k)
{
    switch (k) {
    case EntryKind::Folder: return "folder";
    case EntryKind::Video: return "film";
    case EntryKind::Audio: return "note";
    case EntryKind::Playlist: return "playlist";
    case EntryKind::Stream: return "stream";
    case EntryKind::Action: return "plus";
    case EntryKind::Category: return "song";
    case EntryKind::Group: return "album";
    }
    return "file";
}

bool EntryDelegate::hasTileButtons(const QModelIndex &index)
{
    const auto kind = EntryKind(index.data(KindRole).toInt());
    return kind == EntryKind::Group || kind == EntryKind::Playlist;
}

QRectF EntryDelegate::tileButtonRect(const QRect &itemRect, TileButton b) const
{
    // Same thumbnail rectangle as paint().
    const QRectF cell = QRectF(itemRect).adjusted(0, 0, -20, 0);
    const QRectF thumb(cell.left(), cell.top(), cell.width(), square ? cell.width() : cell.width() * 9.0 / 16.0);
    const double d = qBound(28.0, thumb.width() * 0.19, 44.0), m = qMax(8.0, d * 0.25);
    const double y = thumb.bottom() - m - d;
    return b == PlayButton ? QRectF(thumb.left() + m, y, d, d) : QRectF(thumb.right() - m - d, y, d, d);
}

EntryDelegate::TileButton EntryDelegate::tileButtonAt(const QRect &itemRect, const QModelIndex &index,
                                                      const QPoint &pos) const
{
    if (!grid || !hasTileButtons(index))
        return NoButton;
    for (TileButton b : {PlayButton, MoreButton}) {
        const QRectF r = tileButtonRect(itemRect, b);
        const QPointF d = QPointF(pos) - r.center();
        if (d.x() * d.x() + d.y() * d.y() <= (r.width() / 2 + 2) * (r.width() / 2 + 2))
            return b;
    }
    return NoButton;
}

QSize EntryDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return grid ? tileSize : QSize(200, 44);
}

void EntryDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const Palette &c = theme().pal();
    const auto kind = EntryKind(index.data(KindRole).toInt());
    const QString path = index.data(PathRole).toString();
    const QString name = index.data(Qt::DisplayRole).toString();
    const bool hover = option.state & QStyle::State_MouseOver;
    const bool selected = option.state & QStyle::State_Selected;
    const bool current = index.data(Qt::UserRole + 50).toBool();
    const double progress = (kind == EntryKind::Video || kind == EntryKind::Audio) ? library().progress(path) : 0.0;
    const bool watched = progress >= 0.95;

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);

    if (!grid) {
        const QRectF r = QRectF(option.rect).adjusted(0, 1, 0, -1);
        if (hover || selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? c.selected : c.hover);
            p->drawRoundedRect(r, 6, 6);
        }
        const QColor iconColor = current ? theme().accent() : c.textDim;
        const QString iconName = index.data(IconRole).toString().isEmpty() ? iconForKind(kind) : index.data(IconRole).toString();
        const qreal dpr = p->device()->devicePixelRatioF();
        QPixmap cover;
        if (kind == EntryKind::Audio || kind == EntryKind::Group) {
            const QString thumbPath = index.data(ThumbRole).toString();
            cover = Thumbnailer::instance().get(thumbPath.isEmpty() ? path : thumbPath);
        }
        const QRectF tile(r.left() + 4, r.center().y() - 15, 30, 30);
        if (!cover.isNull()) {
            // Small cover, cropped to a square.
            QPainterPath clip;
            clip.addRoundedRect(tile, 4, 4);
            p->save();
            p->setClipPath(clip);
            const QSizeF sc = QSizeF(cover.size()).scaled(tile.size(), Qt::KeepAspectRatioByExpanding);
            p->drawPixmap(QRectF(tile.center().x() - sc.width() / 2, tile.center().y() - sc.height() / 2, sc.width(),
                                 sc.height()),
                          cover, QRectF(cover.rect()));
            p->restore();
            if (current) {
                p->setPen(QPen(theme().accent(), 1.5));
                p->setBrush(Qt::NoBrush);
                p->drawRoundedRect(tile.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
            }
        } else if (kind == EntryKind::Audio || kind == EntryKind::Group) {
            // No cover: an accent-tinted tile, like Listen Again on Home.
            QColor bg = theme().accent();
            bg.setAlpha(theme().isDark() ? 46 : 36);
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(tile, 4, 4);
            p->drawPixmap(QPointF(tile.center().x() - 9, tile.center().y() - 9),
                          theme().pixmap(kind == EntryKind::Audio ? "song" : iconName, theme().accent(), 18, dpr));
        } else {
            p->drawPixmap(QPointF(r.left() + 9, r.center().y() - 10), theme().pixmap(iconName, iconColor, 20, dpr));
        }
        QFont f = option.font;
        f.setPixelSize(14);
        p->setFont(f);
        QColor textColor = current ? theme().accent() : (kind == EntryKind::Action ? c.textDim : c.text);
        if (watched && !current)
            textColor = c.textDim;
        p->setPen(textColor);
        QRectF textRect = r.adjusted(42, 0, -12, 0);
        QString right = index.data(DetailRole).toString();
        if (right.isEmpty() && (kind == EntryKind::Video || kind == EntryKind::Audio)) {
            if (progress > 0.01 && !watched)
                right = QString("%1%").arg(int(progress * 100));
            else if (watched)
                right = QObject::tr("Watched");
        }
        if (!right.isEmpty()) {
            QFont sf = f;
            sf.setPixelSize(12);
            const int rw = QFontMetrics(sf).horizontalAdvance(right) + 6;
            p->setFont(sf);
            p->setPen(c.textMuted);
            p->drawText(textRect, Qt::AlignVCenter | Qt::AlignRight, right);
            textRect.setRight(textRect.right() - rw - 8);
            p->setFont(f);
            p->setPen(textColor);
        }
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(f).elidedText(name, Qt::ElideRight, int(textRect.width())));
        p->restore();
        return;
    }

    // Grid tile: 16:9 thumbnail + name underneath.
    const QRectF cell = QRectF(option.rect).adjusted(0, 0, -20, 0);
    const QRectF thumb(cell.left(), cell.top(), cell.width(), square ? cell.width() : cell.width() * 9.0 / 16.0);
    if (selected) { // a tinted backdrop behind the whole tile, name included
        p->setPen(Qt::NoPen);
        p->setBrush(c.selected);
        p->drawRoundedRect(QRectF(cell.left(), cell.top(), cell.width(), option.rect.height() - 4), 6, 6);
    }
    QPainterPath clip;
    clip.addRoundedRect(thumb, 6, 6);
    p->setClipPath(clip);
    p->fillRect(thumb, kind == EntryKind::Video ? QColor(0, 0, 0) : (theme().isDark() ? theme().tone(QColor(0x1c, 0x1c, 0x1c)) : c.surface));

    QPixmap pm;
    if (kind == EntryKind::Video || kind == EntryKind::Audio || kind == EntryKind::Folder || kind == EntryKind::Group) {
        const QString thumbPath = index.data(ThumbRole).toString();
        pm = Thumbnailer::instance().get(thumbPath.isEmpty() ? path : thumbPath);
    }
    if (!pm.isNull()) {
        // Cover the tile (crop to fill).
        const QSizeF scaled = QSizeF(pm.size()).scaled(thumb.size(), Qt::KeepAspectRatioByExpanding);
        const QRectF target(thumb.center().x() - scaled.width() / 2, thumb.center().y() - scaled.height() / 2,
                            scaled.width(), scaled.height());
        p->drawPixmap(target, pm, QRectF(pm.rect()));
    } else {
        const int s = qMin(48, int(thumb.height() / 3));
        const QColor ic = kind == EntryKind::Action ? c.textDim : c.textMuted;
        const QString iconName = index.data(IconRole).toString().isEmpty() ? iconForKind(kind) : index.data(IconRole).toString();
        p->drawPixmap(QPointF(thumb.center().x() - s / 2.0, thumb.center().y() - s / 2.0),
                      theme().pixmap(iconName, ic, s, p->device()->devicePixelRatioF()));
    }
    if (hover) {
        p->fillRect(thumb, QColor(255, 255, 255, 18));
        if (kind == EntryKind::Video || kind == EntryKind::Audio || kind == EntryKind::Stream) {
            const QRectF circle(thumb.center().x() - 24, thumb.center().y() - 24, 48, 48);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 150));
            p->drawEllipse(circle);
            p->drawPixmap(QPointF(circle.center().x() - 10, circle.center().y() - 11),
                          theme().pixmap("play-fill", QColor(0xf0, 0xf0, 0xf0), 22, p->device()->devicePixelRatioF()));
        } else if (hasTileButtons(index)) {
            // Play (bottom left) and ⋯ (bottom right), lighter under the pointer.
            const qreal dpr = p->device()->devicePixelRatioF();
            const TileButton under = tileButtonAt(option.rect, index, mouse);
            for (TileButton b : {PlayButton, MoreButton}) {
                const QRectF r = tileButtonRect(option.rect, b);
                p->setPen(QPen(QColor(255, 255, 255, under == b ? 70 : 30), 1));
                p->setBrush(under == b ? QColor(48, 48, 48, 235) : QColor(20, 20, 20, 205));
                p->drawEllipse(r.adjusted(0.5, 0.5, -0.5, -0.5));
                const int s = int(r.width() * 0.46);
                // The triangle's weight sits left of its box: nudge it right.
                const double nudge = b == PlayButton ? r.width() * 0.04 : 0;
                p->drawPixmap(QPointF(r.center().x() - s / 2.0 + nudge, r.center().y() - s / 2.0),
                              theme().pixmap(b == PlayButton ? "play-fill" : "more", QColor(0xf4, 0xf4, 0xf4), s, dpr));
            }
        }
    }
    if (progress > 0.01) {
        const QRectF bar(thumb.left() + 3, thumb.bottom() - 4, (thumb.width() - 6) * progress, 3);
        p->setPen(Qt::NoPen);
        p->setBrush(theme().accent());
        p->drawRoundedRect(bar, 1.5, 1.5);
    }
    if (current) {
        p->setClipping(false);
        p->setPen(QPen(theme().accent(), 2));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(thumb.adjusted(1, 1, -1, -1), 6, 6);
    }
    p->setClipping(false);
    if (selected) { // check badge in the corner
        const QRectF badge(thumb.left() + 8, thumb.top() + 8, 24, 24);
        p->setPen(QPen(QColor(0, 0, 0, 90), 1));
        p->setBrush(theme().accent());
        p->drawEllipse(badge);
        p->drawPixmap(QPointF(badge.left() + 3, badge.top() + 3),
                      theme().pixmap("check", QColor(0xff, 0xff, 0xff), 18, p->device()->devicePixelRatioF()));
    }

    QFont f = option.font;
    f.setPixelSize(15);
    p->setFont(f);
    p->setPen(current ? theme().accent() : (watched ? c.textMuted : c.text));
    const QRectF textRect(cell.left(), thumb.bottom() + (square ? 8 : 10), cell.width(), 24);
    p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(f).elidedText(name, square ? Qt::ElideRight : Qt::ElideMiddle, int(textRect.width())));
    const QString detail = index.data(DetailRole).toString();
    if (square && !detail.isEmpty()) {
        QFont df = option.font;
        df.setPixelSize(13);
        p->setFont(df);
        p->setPen(c.textMuted);
        const QRectF detailRect(cell.left(), textRect.bottom(), cell.width(), 20);
        p->drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(df).elidedText(detail, Qt::ElideRight, int(detailRect.width())));
    }
    p->restore();
}

// ---------------------------------------------------------------------------
// LibraryView

LibraryView::LibraryView(Mode mode, QWidget *parent) : QWidget(parent), m_mode(mode)
{
    setObjectName(mode == Full ? "Library" : "SidePanel");
    setAttribute(Qt::WA_StyledBackground);
    m_gridForSection = {{int(Section::Home), true},   {int(Section::Videos), true},
                        {int(Section::Music), true},  {int(Section::Network), false},
                        {int(Section::Playlists), false}, {int(Section::Recent), true},
                        {int(Section::Queue), false}};

    auto *lay = new QVBoxLayout(this);
    lay->setSpacing(0);

    m_backBtn = new IconButton("arrow-left", this);
    m_fwdBtn = new IconButton("arrow-right", this);
    m_sortBtn = new IconButton("sort", this);
    m_viewBtn = new IconButton("grid", this);
    m_moreBtn = new IconButton("more", this);
    m_backBtn->setToolTip(tr("Back"));
    m_fwdBtn->setToolTip(tr("Forward"));
    m_sortBtn->setToolTip(tr("Sort"));
    m_viewBtn->setToolTip(tr("Toggle grid / list"));
    m_moreBtn->setToolTip(tr("More"));
    m_count = new QLabel(this);
    m_count->setProperty("role", "dim");
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search"));
    m_search->setProperty("role", "search");
    m_search->setClearButtonEnabled(true);
    m_search->setFocusPolicy(Qt::ClickFocus);
    auto *searchIcon = new QLabel(m_search);
    searchIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto updateSearchIcon = [searchIcon] {
        searchIcon->setPixmap(theme().pixmap("search", theme().pal().textDim, 18, 2.0));
    };
    updateSearchIcon();
    connect(&theme(), &Theme::changed, searchIcon, updateSearchIcon);
    searchIcon->move(12, 9);
    searchIcon->resize(18, 18);

    m_artistArt = new Dropdown(this);
    m_artistArt->addItem(tr("Album cover"), "cover");
    m_artistArt->addItem(tr("Artist image"), "artist");
    m_artistArt->setToolTip(tr("Picture shown for each artist"));
    m_artistArt->setFixedSize(mode == Full ? 170 : 150, 32);
    m_artistArt->setCurrentIndex(qMax(0, m_artistArt->findData(prefs().get("music/artistArt").toString())));
    m_artistArt->hide();
    connect(m_artistArt, &QComboBox::activated, this, [this](int i) {
        prefs().set("music/artistArt", m_artistArt->itemData(i).toString()); // reloads
    });

    m_header = new QWidget(this);
    auto *crumbHost = new QWidget(m_header);
    m_crumbs = new QHBoxLayout(crumbHost);
    m_crumbs->setContentsMargins(0, 0, 0, 0);
    m_crumbs->setSpacing(4);

    if (mode == Full) {
        lay->setContentsMargins(28, 14, 8, 0);
        auto *h = new QHBoxLayout(m_header);
        h->setContentsMargins(0, 0, 20, 12);
        h->setSpacing(4);
        h->addWidget(m_backBtn);
        h->addWidget(m_fwdBtn);
        h->addSpacing(10);
        h->addWidget(crumbHost);
        h->addStretch();
        h->addWidget(m_artistArt);
        h->addSpacing(12);
        h->addWidget(m_count);
        h->addSpacing(8);
        h->addWidget(m_sortBtn);
        h->addWidget(m_viewBtn);
        h->addSpacing(8);
        m_search->setMinimumWidth(160);
        m_search->setMaximumWidth(340);
        m_search->setFixedHeight(36);
        h->addWidget(m_search);
        h->addSpacing(8);
        h->addWidget(m_moreBtn);
        lay->addWidget(m_header);
        lay->addWidget(hline(this));
        lay->addSpacing(20);
    } else {
        lay->setContentsMargins(18, 14, 18, 0);
        m_popBtn = new IconButton("external", this);
        m_popBtn->setToolTip(tr("Open in library"));
        m_searchBtn = new IconButton("search", this);
        m_searchBtn->setToolTip(tr("Search"));
        m_title = new QLabel(this);
        m_title->setProperty("role", "title");
        auto *v = new QVBoxLayout(m_header);
        v->setContentsMargins(0, 0, 0, 10);
        v->setSpacing(10);
        auto *row1 = new QHBoxLayout;
        row1->setSpacing(4);
        row1->addWidget(m_backBtn);
        row1->addWidget(m_fwdBtn);
        row1->addSpacing(6);
        row1->addWidget(m_title, 1);
        row1->addWidget(m_moreBtn);
        row1->addWidget(m_popBtn);
        auto *row2 = new QHBoxLayout;
        row2->setContentsMargins(6, 0, 0, 0);
        row2->setSpacing(4);
        row2->addWidget(m_count);
        row2->addStretch();
        row2->addWidget(m_artistArt);
        row2->addWidget(m_sortBtn);
        row2->addWidget(m_viewBtn);
        row2->addWidget(m_searchBtn);
        if (mode == QueueTab) {
            // Only the queue, under the panel's tabs: no navigation or title.
            lay->setContentsMargins(18, 16, 18, 0);
            for (QWidget *w : std::initializer_list<QWidget *>{m_backBtn, m_fwdBtn, m_title, m_popBtn})
                w->hide();
            row1->removeWidget(m_moreBtn);
            m_viewBtn->hide();
            m_searchBtn->hide();
            // Like the video's queue: sort, repeat, shuffle, remove, add, more.
            m_repeatBtn = new IconButton("repeat", this);
            m_shuffleBtn = new IconButton("shuffle", this);
            m_removeBtn = new IconButton("minus", this);
            m_removeBtn->setToolTip(tr("Remove the selected item"));
            m_addBtn = new IconButton("plus", this);
            m_addBtn->setToolTip(tr("Add files to the queue"));
            for (IconButton *b : {m_repeatBtn, m_shuffleBtn, m_removeBtn, m_addBtn})
                row2->addWidget(b);
            row2->addWidget(m_moreBtn);
            delete row1;
            auto syncOrder = [this] {
                const int repeat = prefs().integer("playback/repeat");
                m_repeatBtn->setIconName(repeat == 1 ? "repeat-one" : "repeat");
                m_repeatBtn->setActive(repeat != 0);
                m_repeatBtn->setToolTip(repeat == 0 ? tr("Repeat: off") : repeat == 1 ? tr("Repeat: this item")
                                                                                        : tr("Repeat: whole queue"));
                m_shuffleBtn->setActive(prefs().flag("playback/shuffle"));
                m_shuffleBtn->setToolTip(prefs().flag("playback/shuffle") ? tr("Shuffle: on") : tr("Shuffle: off"));
            };
            syncOrder();
            connect(&prefs(), &Prefs::changed, this, [syncOrder](const QString &key) {
                if (key == "playback/repeat" || key == "playback/shuffle")
                    syncOrder();
            });
            connect(m_repeatBtn, &IconButton::clicked, this, [] {
                static const int next[] = {2, 0, 1}; // off → all → one → off
                prefs().set("playback/repeat", next[qBound(0, prefs().integer("playback/repeat"), 2)]);
            });
            connect(m_shuffleBtn, &IconButton::clicked, this,
                    [] { prefs().set("playback/shuffle", !prefs().flag("playback/shuffle")); });
            connect(m_addBtn, &IconButton::clicked, this, &LibraryView::addFilesRequested);
            connect(m_removeBtn, &IconButton::clicked, this, [this] {
                QList<int> rows;
                for (const QModelIndex &idx : selectedRows())
                    rows << idx.data(OrderRole).toInt();
                if (!rows.isEmpty())
                    emit queueRemoveRowsRequested(rows);
            });
        } else {
            v->addLayout(row1);
        }
        v->addLayout(row2);
        v->addWidget(m_search);
        m_search->setVisible(false);
        crumbHost->hide();
        lay->addWidget(m_header);
        lay->addWidget(hline(this));
        lay->addSpacing(12);
        connect(m_searchBtn, &IconButton::clicked, this, [this] {
            m_search->setVisible(!m_search->isVisible());
            if (m_search->isVisible())
                m_search->setFocus();
            else
                m_search->clear();
        });
        connect(m_popBtn, &IconButton::clicked, this, &LibraryView::popOutRequested);
    }

    m_model = new QStandardItemModel(this);
    auto *proxy = new EntryProxy(this);
    m_proxy = proxy;
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setSortRole(Qt::DisplayRole);
    m_proxy->sort(0);

    m_view = new QListView(this);
    m_view->setObjectName("entries");
    m_delegate = new EntryDelegate(m_view);
    m_view->setItemDelegate(m_delegate);
    m_view->setModel(m_proxy);
    m_view->setMouseTracking(true);
    m_view->viewport()->setAttribute(Qt::WA_Hover);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection); // Ctrl/Shift-click, Ctrl+A
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->verticalScrollBar()->setSingleStep(24);
    m_view->setUniformItemSizes(true);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->viewport()->setCursor(Qt::PointingHandCursor);
    m_view->installEventFilter(this);
    m_view->viewport()->installEventFilter(this); // tile buttons; drops on the queue
    // Drag entries out (to the queue); the queue also takes drops and reorders.
    proxy->resolve = [this](const QModelIndex &idx) { return filesFor(idx); };
    m_view->setDragEnabled(true);
    m_view->setDragDropMode(QAbstractItemView::DragOnly);
    m_view->setDefaultDropAction(Qt::CopyAction);
    if (mode == QueueTab) {
        m_view->setAcceptDrops(true);
        m_view->viewport()->setAcceptDrops(true);
        m_view->setDragDropMode(QAbstractItemView::DragDrop);
        m_dropLine = new QWidget(m_view->viewport());
        m_dropLine->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_dropLine->hide();
        connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this] { m_removeBtn->setEnabled(m_view->selectionModel()->hasSelection()); });
        connect(&Durations::instance(), &Durations::ready, this, [this](const QString &path) {
            if (m_queue.contains(path))
                updateQueueCount();
        });
    }
    lay->addWidget(m_view, 1);
    if (mode == Full) {
        m_home = new HomePage(this);
        m_home->hide();
        lay->addWidget(m_home, 1);
        connect(m_home, &HomePage::playRequested, this, &LibraryView::playRequested);
        connect(m_home, &HomePage::enqueueRequested, this, &LibraryView::enqueueRequested);
        connect(m_home, &HomePage::sectionRequested, this, [this](Section s) { navigate({s, {}, {}}); });
        connect(m_home, &HomePage::folderRequested, this, [this](const QString &path) {
            // Open it under Music if it's inside a music folder, else Videos.
            Section section = Section::Videos;
            for (const QString &root : prefs().list("library/musicFolders"))
                if (!root.isEmpty() && (path == root || path.startsWith(QDir::cleanPath(root) + "/")))
                    section = Section::Music;
            navigate({section, path, {}});
        });
    }

    m_empty = new QLabel(m_view);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setProperty("role", "muted");
    m_empty->setWordWrap(true);
    m_empty->hide();

    // A click opens folders, albums and playlists but only selects media;
    // media plays on double-click or Enter. (Not QListView::activated: some
    // styles fire it on a single click.)
    connect(m_view, &QListView::clicked, this, [this](const QModelIndex &idx) {
        if (QApplication::keyboardModifiers() == Qt::NoModifier && !isMediaEntry(idx))
            activate(idx);
    });
    connect(m_view, &QListView::doubleClicked, this, [this](const QModelIndex &idx) {
        if (QApplication::keyboardModifiers() == Qt::NoModifier && isMediaEntry(idx))
            activate(idx);
    });
    connect(m_view, &QListView::customContextMenuRequested, this, &LibraryView::showContextMenu);
    connect(m_backBtn, &IconButton::clicked, this, [this] {
        if (m_back.isEmpty())
            return;
        m_forward.prepend(m_loc);
        navigate(m_back.takeLast(), false);
    });
    connect(m_fwdBtn, &IconButton::clicked, this, [this] {
        if (m_forward.isEmpty())
            return;
        m_back.append(m_loc);
        navigate(m_forward.takeFirst(), false);
    });
    connect(m_viewBtn, &IconButton::clicked, this, [this] { setGridView(!m_delegate->grid); });
    connect(m_sortBtn, &IconButton::clicked, this, &LibraryView::showSortMenu);
    connect(m_moreBtn, &IconButton::clicked, this, &LibraryView::showMoreMenu);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (m_home && m_home->isVisible()) {
            m_home->setFilter(t);
            return;
        }
        m_proxy->setFilterFixedString(t);
        m_count->setText(itemCount(m_proxy->rowCount()));
    });

    auto repaint = [this] { m_view->viewport()->update(); };
    connect(&Thumbnailer::instance(), &Thumbnailer::ready, this, repaint);
    connect(&library(), &Library::positionsChanged, this, repaint);
    connect(&theme(), &Theme::changed, this, repaint);
    connect(&library(), &Library::recentChanged, this, [this] {
        if (m_loc.section == Section::Recent || m_loc.section == Section::Home)
            reload();
    });
    connect(&ArtistImages::instance(), &ArtistImages::ready, this, [this](const QString &name, const QString &file) {
        if (m_loc.music != "artists" || !m_loc.group.isEmpty() || !artistImages())
            return;
        if (file.isEmpty()) { // a chosen picture was dropped: back to the album cover
            reload();
            return;
        }
        for (int i = 0; i < m_model->rowCount(); ++i) {
            QStandardItem *it = m_model->item(i);
            if (EntryKind(it->data(KindRole).toInt()) == EntryKind::Group && it->data(ArtistRole).toString() == name)
                it->setData(file, ThumbRole);
        }
        m_view->viewport()->update(); // same file name when a picture is replaced
    });
    connect(&musicIndex(), &MusicIndex::changed, this, [this] {
        if (m_loc.section != Section::Music || m_loc.music.isEmpty() || m_loc.music == "folders")
            return;
        const int scroll = m_view->verticalScrollBar()->value();
        const QString filter = m_search->text();
        reload();
        m_proxy->setFilterFixedString(filter);
        m_view->verticalScrollBar()->setValue(scroll);
    });
    connect(&library(), &Library::playlistsChanged, this, [this] {
        if (m_loc.section == Section::Playlists)
            reload();
    });
    connect(&library(), &Library::streamsChanged, this, [this] {
        if (m_loc.section == Section::Network)
            reload();
    });
    connect(&prefs(), &Prefs::changed, this, [this](const QString &key) {
        if (key == "ui/thumbSize")
            updateGrid();
        if (key.startsWith("library/") ||
            ((key == "music/artistMode" || key == "music/artistArt") && m_loc.music == "artists"))
            reload();
        if (key == "music/artistArt")
            m_artistArt->setCurrentIndex(qMax(0, m_artistArt->findData(prefs().get(key).toString())));
        if (key == "music/albumYearDesc" && m_loc.music == "artists" && !m_loc.group.isEmpty() && m_loc.album.isEmpty()) {
            loadSort();
            applySort();
        }
    });

    navigate({Section::Home, {}, {}}, false);
}

void LibraryView::showSection(Section section)
{
    if (m_loc == Location{section}) {
        reload();
        return;
    }
    navigate({section, {}, {}});
}

void LibraryView::showFolder(Section section, const QString &path)
{
    navigate({section, path, {}});
}

void LibraryView::showMusic(const QString &view, const QString &groupKey, const QString &albumKey)
{
    navigate({Section::Music, {}, {}, view, groupKey.isNull() ? QString() : "g:" + groupKey,
              albumKey.isNull() ? QString() : "a:" + albumKey});
}

void LibraryView::refresh()
{
    reload();
}

void LibraryView::setQueue(const QStringList &items, int current)
{
    m_queue = items;
    m_queueCurrent = current;
    if (m_loc.section == Section::Queue)
        reload();
}

void LibraryView::setNowPlaying(const QString &location)
{
    m_nowPlaying = location;
    for (int i = 0; i < m_model->rowCount(); ++i) {
        QStandardItem *it = m_model->item(i);
        const bool cur = m_loc.section == Section::Queue ? it->data(OrderRole).toInt() == m_queueCurrent
                                                         : it->data(PathRole).toString() == location;
        it->setData(cur, Qt::UserRole + 50);
    }
}

void LibraryView::navigate(const Location &target, bool record)
{
    Location loc = target;
    // A music folder (e.g. from Home) opens under Music → Folders.
    if (loc.section == Section::Music && !loc.path.isEmpty())
        loc.music = "folders";
    if (record && !(loc == m_loc)) {
        m_back.append(m_loc);
        m_forward.clear();
    }
    m_loc = loc;
    if ((loc.section == Section::Videos || loc.section == Section::Music) && !loc.path.isEmpty())
        library().visitFolder(loc.path);
    m_search->clear();
    loadSort();
    static_cast<EntryProxy *>(m_proxy)->key = m_sortKey;
    const bool menu = loc.section == Section::Music && loc.music.isEmpty();
    setGridView(m_mode == Full && !menu ? m_gridForSection.value(int(loc.section), true) : false, !menu);
    reload();
    emit sectionShown(loc.section);
}

QStringList LibraryView::roots() const
{
    QStringList r;
    const QStringList folders = prefs().list(m_loc.section == Section::Music ? "library/musicFolders"
                                                                             : "library/videoFolders");
    for (const QString &f : folders)
        if (QFileInfo(f).isDir())
            r << f;
    return r;
}

int LibraryView::filterForSection() const
{
    switch (m_loc.section) {
    case Section::Videos: return 0;
    case Section::Music: return 1;
    default: return 2;
    }
}

QStandardItem *LibraryView::addEntry(const LibraryEntry &e, int order)
{
    auto *item = new QStandardItem(e.name);
    item->setData(int(e.kind), KindRole);
    item->setData(e.path, PathRole);
    item->setData(order, OrderRole);
    item->setData(e.size, SizeRole);
    item->setData(e.modified, DateRole);
    const bool cur = m_loc.section == Section::Queue ? order == m_queueCurrent : e.path == m_nowPlaying;
    item->setData(cur, Qt::UserRole + 50);
    if (e.kind != EntryKind::Category && e.kind != EntryKind::Group && e.kind != EntryKind::Action)
        item->setToolTip(e.path);
    m_model->appendRow(item);
    return item;
}

void LibraryView::reload()
{
    m_model->clear();
    const bool home = m_home && m_loc.section == Section::Home;
    // The Music menu is a fixed list: no sorting or grid.
    const bool menu = m_loc.section == Section::Music && m_loc.music.isEmpty();
    if (m_home) {
        m_home->setVisible(home);
        m_view->setVisible(!home);
    }
    m_sortBtn->setVisible(!home && !menu);
    m_viewBtn->setVisible(!home && !menu && m_mode != QueueTab);
    m_artistArt->setVisible(m_loc.section == Section::Music && m_loc.music == "artists" && m_loc.group.isEmpty());
    if (home) {
        m_home->reload();
        m_count->setText(m_home->itemCount() == 1 ? tr("1 item") : tr("%1 items").arg(m_home->itemCount()));
        m_empty->hide();
        m_backBtn->setEnabled(!m_back.isEmpty());
        m_fwdBtn->setEnabled(!m_forward.isEmpty());
        rebuildHeader();
        return;
    }
    QString empty;
    int order = 0;
    auto entryFor = [](const QString &loc) {
        LibraryEntry e;
        e.kind = Library::kindFor(loc);
        e.path = loc;
        e.name = Library::displayName(loc);
        if (const MusicIndex::Track *t = e.kind == EntryKind::Audio ? musicIndex().track(loc) : nullptr)
            e.name = t->label();
        const QFileInfo fi(loc);
        if (!Library::isUrl(loc)) {
            e.size = fi.size();
            e.modified = fi.lastModified();
        }
        return e;
    };

    switch (m_loc.section) {
    case Section::Home: {
        for (const QString &loc : library().recent()) {
            const double pr = library().progress(loc);
            if (pr > 0.01 && pr < 0.95 && order < 10)
                addEntry(entryFor(loc), order++);
        }
        for (const QString &f : prefs().list("library/videoFolders") + prefs().list("library/musicFolders")) {
            if (!QFileInfo(f).isDir())
                continue;
            LibraryEntry e;
            e.kind = EntryKind::Folder;
            e.path = f;
            e.name = QFileInfo(f).fileName();
            addEntry(e, order++);
        }
        empty = tr("Nothing here yet.\nOpen a file with Ctrl+O, drop media onto the window,\nor add folders in Settings → Library.");
        break;
    }
    case Section::Videos:
    case Section::Music: {
        if (m_loc.section == Section::Music && m_loc.music != "folders") {
            reloadMusic(order, empty);
            break;
        }
        const QStringList rs = roots();
        if (m_loc.path.isEmpty() && rs.size() == 1)
            m_loc.path = rs.first();
        if (m_loc.path.isEmpty()) {
            for (const QString &r : rs) {
                LibraryEntry e;
                e.kind = EntryKind::Folder;
                e.path = r;
                e.name = QFileInfo(r).fileName();
                addEntry(e, order++);
            }
        } else {
            for (const LibraryEntry &e : library().listFolder(m_loc.path, filterForSection()))
                addEntry(e, order++);
        }
        empty = rs.isEmpty() ? tr("No %1 folder configured.\nAdd one in Settings → Library.").arg(sectionName(m_loc.section).toLower())
                             : tr("This folder has no %1.").arg(m_loc.section == Section::Music ? tr("music") : tr("videos"));
        break;
    }
    case Section::Network: {
        LibraryEntry open;
        open.kind = EntryKind::Action;
        open.path = "open-network";
        open.name = tr("Open network stream…");
        addEntry(open, order++);
        for (const QString &url : library().streams()) {
            LibraryEntry e;
            e.kind = EntryKind::Stream;
            e.path = url;
            e.name = url;
            addEntry(e, order++);
        }
        break;
    }
    case Section::Playlists: {
        if (m_loc.playlist.isEmpty()) {
            LibraryEntry create;
            create.kind = EntryKind::Action;
            create.path = "create-playlist";
            create.name = tr("Create playlist");
            addEntry(create, order++);
            for (const QString &name : library().playlistNames()) {
                LibraryEntry e;
                e.kind = EntryKind::Playlist;
                e.path = name;
                e.name = name;
                addEntry(e, order++);
            }
        } else {
            for (const QString &loc : library().playlist(m_loc.playlist))
                addEntry(entryFor(loc), order++);
            empty = tr("This playlist is empty.\nRight-click media and choose “Add to playlist”.");
        }
        break;
    }
    case Section::Recent:
        for (const QString &loc : library().recent())
            addEntry(entryFor(loc), order++);
        empty = tr("Media you play will show up here.");
        break;
    case Section::Queue:
        for (const QString &loc : m_queue)
            addEntry(entryFor(loc), order++);
        empty = tr("The play queue is empty.");
        break;
    }

    m_proxy->sort(0, m_sortDesc ? Qt::DescendingOrder : Qt::AscendingOrder);
    int realItems = 0;
    for (int i = 0; i < m_model->rowCount(); ++i)
        if (EntryKind(m_model->item(i)->data(KindRole).toInt()) != EntryKind::Action)
            ++realItems;
    m_count->setText(itemCount(realItems));
    if (m_loc.section == Section::Queue)
        updateQueueCount();
    if (m_removeBtn)
        m_removeBtn->setEnabled(false);
    m_empty->setText(empty);
    m_empty->setVisible(realItems == 0 && !empty.isEmpty());
    m_empty->setGeometry(m_view->rect().adjusted(20, 0, -20, -m_view->height() / 4));
    m_backBtn->setEnabled(!m_back.isEmpty());
    m_fwdBtn->setEnabled(!m_forward.isEmpty());
    rebuildHeader();
    updateGrid();
}

// Music: the menu (Add, Songs, Albums, Artists, Genres, Folders), or a view
// built from the tags in the music index.
void LibraryView::reloadMusic(int &order, QString &empty)
{
    MusicIndex &index = musicIndex();
    index.ensureScanned();
    if (m_loc.music.isEmpty()) {
        LibraryEntry add;
        add.kind = EntryKind::Action;
        add.path = "add-music-folder";
        add.name = tr("Add");
        addEntry(add, order++);
        const QList<QPair<QString, QString>> views = {
            {"songs", "song"}, {"albums", "album"}, {"artists", "artist"}, {"genres", "genre"}, {"folders", "folder"}};
        for (const auto &v : views) {
            LibraryEntry e;
            e.kind = EntryKind::Category;
            e.path = v.first;
            e.name = musicViewName(v.first);
            addEntry(e, order++)->setData(v.second, IconRole);
        }
        return;
    }

    auto addTrack = [this, &order](const MusicIndex::Track &t) {
        LibraryEntry e;
        e.kind = EntryKind::Audio;
        e.path = t.path;
        e.name = t.label();
        const QFileInfo fi(t.path);
        e.size = fi.size();
        e.modified = fi.lastModified();
        QStandardItem *item = addEntry(e, order++);
        item->setData(t.year, YearRole);
        item->setData(t.artist, ArtistRole);
        item->setData(t.album, AlbumRole);
    };
    const MusicIndex::Field field = musicField(m_loc.music);
    auto addGroup = [this, &order](const MusicIndex::Group &g, const QString &path, const QString &icon,
                                   bool showArtist) {
        LibraryEntry e;
        e.kind = EntryKind::Group;
        e.path = path;
        e.name = g.name;
        QStandardItem *item = addEntry(e, order++);
        item->setData(icon, IconRole);
        item->setData(g.cover, ThumbRole);
        item->setData(g.year, YearRole);
        QStringList detail;
        if (icon == "album") {
            item->setData(g.artist, ArtistRole);
            if (showArtist && !g.artist.isEmpty())
                detail << g.artist;
            if (g.year)
                detail << QString::number(g.year);
        }
        detail << songCount(g.count);
        item->setData(detail.join(" · "), DetailRole);
        return item;
    };
    const bool artists = m_loc.music == "artists";
    if (m_loc.music == "songs") {
        for (const MusicIndex::Track &t : index.tracks())
            addTrack(t);
    } else if (artists && !m_loc.album.isEmpty()) {
        // Artist → album: its songs (only this artist's, on a compilation).
        for (const MusicIndex::Track &t : index.tracksIn(MusicIndex::Album, m_loc.album.mid(2),
                                                         within(field, m_loc.group.mid(2))))
            addTrack(t);
    } else if (artists && !m_loc.group.isEmpty()) {
        // Artist: their albums.
        for (const MusicIndex::Group &g : index.groups(MusicIndex::Album, within(field, m_loc.group.mid(2))))
            addGroup(g, "a:" + g.key, "album", false);
    } else if (!m_loc.group.isEmpty()) {
        for (const MusicIndex::Track &t : index.tracksIn(field, m_loc.group.mid(2)))
            addTrack(t);
    } else {
        const QString icon = artists ? "artist" : field == MusicIndex::Genre ? "genre" : "album";
        const bool pictures = artists && artistImages();
        const QHash<QString, QStringList> samples = pictures ? index.samplePaths(field, 6) : QHash<QString, QStringList>();
        for (const MusicIndex::Group &g : index.groups(field)) {
            QStandardItem *item = addGroup(g, "g:" + g.key, icon, true);
            if (pictures) {
                // The artist's picture, or the album cover until (unless) there is one.
                const QString picture = ArtistImages::instance().image(g.key, samples.value(g.key));
                if (!picture.isEmpty())
                    item->setData(picture, ThumbRole);
                item->setData(g.key, ArtistRole);
            }
        }
    }
    if (prefs().list("library/musicFolders").isEmpty())
        empty = tr("No music folder configured.\nGo back to Music and choose “Add”.");
    else if (index.isScanning())
        empty = tr("Looking for music…");
    else
        empty = tr("No music found in your music folders.");
}

QStringList LibraryView::tracksFor(const QString &entryPath) const
{
    QStringList out;
    const MusicIndex::Field field = musicField(m_loc.music);
    const QList<MusicIndex::Track> tracks =
        entryPath.startsWith("a:")
            ? musicIndex().tracksIn(MusicIndex::Album, entryPath.mid(2), within(field, m_loc.group.mid(2)))
            : musicIndex().tracksIn(field, entryPath.mid(2));
    for (const MusicIndex::Track &t : tracks)
        out << t.path;
    return out;
}

QStringList LibraryView::filesFor(const QModelIndex &idx) const
{
    const QString path = idx.data(PathRole).toString();
    switch (EntryKind(idx.data(KindRole).toInt())) {
    case EntryKind::Group:
        return tracksFor(path);
    case EntryKind::Folder:
        return library().mediaInFolder(path, filterForSection());
    case EntryKind::Playlist:
        return library().expand(library().playlist(path));
    case EntryKind::Video:
    case EntryKind::Audio:
    case EntryKind::Stream:
        return {path};
    default:
        return {};
    }
}

QModelIndexList LibraryView::selectedRows() const
{
    QModelIndexList sel = m_view->selectionModel()->selectedIndexes();
    std::sort(sel.begin(), sel.end(), [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
    return sel;
}

void LibraryView::playSelection(bool replace)
{
    QStringList files;
    for (const QModelIndex &idx : selectedRows())
        files << filesFor(idx);
    if (files.isEmpty())
        return;
    if (replace)
        emit playRequested(files, 0);
    else
        emit enqueueRequested(files);
}

bool LibraryView::artistImages() const
{
    return prefs().get("music/artistArt").toString() == "artist";
}

void LibraryView::rebuildHeader()
{
    // Title (compact) or breadcrumbs (full).
    QStringList parts;
    QList<Location> targets;
    parts << sectionName(m_loc.section);
    targets << Location{m_loc.section, {}, {}};
    if (m_loc.section == Section::Music && !m_loc.music.isEmpty()) {
        parts << musicViewName(m_loc.music);
        targets << Location{Section::Music, {}, {}, m_loc.music};
        if (!m_loc.group.isEmpty()) {
            parts << musicIndex().groupName(musicField(m_loc.music), m_loc.group.mid(2));
            targets << Location{Section::Music, {}, {}, m_loc.music, m_loc.group};
        }
        if (!m_loc.album.isEmpty()) {
            parts << musicIndex().groupName(MusicIndex::Album, m_loc.album.mid(2));
            targets << m_loc;
        }
    }
    if (!m_loc.playlist.isEmpty()) {
        parts << m_loc.playlist;
        targets << m_loc;
    } else if (!m_loc.path.isEmpty()) {
        QString root;
        for (const QString &r : roots())
            if (m_loc.path == r || m_loc.path.startsWith(r + "/"))
                root = r;
        if (root.isEmpty()) {
            parts << QFileInfo(m_loc.path).fileName();
            targets << m_loc;
        } else {
            QString acc = root;
            if (roots().size() > 1) {
                parts << QFileInfo(root).fileName();
                targets << Location{m_loc.section, root, {}};
            }
            const QString rel = m_loc.path.mid(root.size());
            for (const QString &seg : rel.split('/', Qt::SkipEmptyParts)) {
                acc += "/" + seg;
                parts << seg;
                targets << Location{m_loc.section, acc, {}};
            }
        }
    }

    if (m_title)
        m_title->setText(parts.last());

    m_crumbTargets.clear();
    while (QLayoutItem *item = m_crumbs->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (m_mode != Full)
        return;
    for (int i = 0; i < parts.size(); ++i) {
        const bool last = i == parts.size() - 1;
        auto *crumb = new QLabel(parts[i]);
        crumb->setProperty("role", last ? "crumbCurrent" : "crumb");
        if (!last) {
            crumb->setCursor(Qt::PointingHandCursor);
            crumb->setProperty("crumbIndex", i);
            crumb->installEventFilter(this);
            m_crumbTargets.insert(crumb, targets[i]);
        }
        m_crumbs->addWidget(crumb);
        if (!last) {
            auto *caret = new QLabel;
            caret->setPixmap(theme().pixmap("caret-right", theme().pal().textMuted, 16, devicePixelRatioF()));
            m_crumbs->addWidget(caret);
        }
    }
}

void LibraryView::updateQueueCount()
{
    const int n = int(m_queue.size());
    QString text = itemCount(n);
    qint64 total = 0;
    bool known = n > 0;
    for (const QString &loc : m_queue) {
        const qint64 ms = Durations::instance().duration(loc);
        if (ms < 0)
            known = false; // still being read; ready() updates this
        else
            total += ms;
    }
    if (known && total > 0) {
        const qint64 mins = (total + 30000) / 60000;
        const QString len = total < 60000 ? tr("%1s").arg(total / 1000)
                            : mins < 60   ? tr("%1m").arg(mins)
                                          : tr("%1h %2m").arg(mins / 60).arg(mins % 60);
        text += QString(" · ") + len;
    }
    m_count->setText(text);
}

// Drops on the queue: files from the library or a file manager go in where
// they're dropped; queue rows dragged within it move.
bool LibraryView::queueDragEvent(QEvent *event)
{
    auto rowAt = [this](const QPoint &pos, int *lineY) {
        const QModelIndex idx = m_view->indexAt(pos);
        int row = m_proxy->rowCount();
        int y = 0;
        if (idx.isValid()) {
            const QRect r = m_view->visualRect(idx);
            const bool below = pos.y() > r.center().y();
            row = idx.row() + (below ? 1 : 0);
            y = below ? r.bottom() + 1 : r.top();
        } else if (row > 0) {
            y = m_view->visualRect(m_proxy->index(row - 1, 0)).bottom() + 1;
        }
        if (lineY)
            *lineY = y;
        // Position in the queue: the order of the row the drop lands before.
        return row < m_proxy->rowCount() ? m_proxy->index(row, 0).data(OrderRole).toInt() : int(m_queue.size());
    };
    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto *e = static_cast<QDragMoveEvent *>(event);
        if (!e->mimeData()->hasUrls()) {
            e->ignore();
            return true;
        }
        int y = 0;
        rowAt(e->position().toPoint(), &y);
        m_dropLine->setStyleSheet(QString("background: %1; border-radius: 1px;").arg(theme().accent().name()));
        m_dropLine->setGeometry(6, qMax(0, y - 1), m_view->viewport()->width() - 12, 2);
        m_dropLine->show();
        m_dropLine->raise();
        e->setDropAction(Qt::CopyAction); // never Move: the view would delete the dragged rows itself
        e->accept();
        return true;
    }
    case QEvent::DragLeave:
        m_dropLine->hide();
        return true;
    case QEvent::Drop: {
        auto *e = static_cast<QDropEvent *>(event);
        m_dropLine->hide();
        const int at = rowAt(e->position().toPoint(), nullptr);
        if (e->source() == m_view && e->mimeData()->hasFormat(kQueueRowsMime)) {
            QList<int> rows;
            for (const QString &r : QString::fromUtf8(e->mimeData()->data(kQueueRowsMime)).split(',', Qt::SkipEmptyParts))
                rows << r.toInt();
            if (!rows.isEmpty())
                emit queueMoveRequested(rows, at);
        } else {
            QStringList items;
            for (const QUrl &u : e->mimeData()->urls())
                items << (u.isLocalFile() ? u.toLocalFile() : u.toString());
            if (!items.isEmpty())
                emit queueInsertRequested(items, at);
        }
        e->setDropAction(Qt::CopyAction); // never Move: the view would delete the dragged rows itself
        e->accept();
        return true;
    }
    default:
        return false;
    }
}

// The play and ⋯ buttons on album/artist/genre/playlist tiles. Presses on them
// are kept from the list, so they don't select, open or start a drag.
bool LibraryView::tileButtonEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseMove: {
        const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
        const QModelIndex idx = m_view->indexAt(pos);
        const auto before = m_delegate->tileButtonAt(m_view->visualRect(idx), idx, m_delegate->mouse);
        m_delegate->mouse = pos;
        if (m_delegate->tileButtonAt(m_view->visualRect(idx), idx, pos) != before && idx.isValid())
            m_view->viewport()->update(m_view->visualRect(idx));
        return false;
    }
    case QEvent::Leave:
        m_delegate->mouse = {-1, -1};
        return false;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton)
            return false;
        const QPoint pos = me->position().toPoint();
        const QModelIndex idx = m_view->indexAt(pos);
        const QRect rect = m_view->visualRect(idx);
        const auto b = idx.isValid() ? m_delegate->tileButtonAt(rect, idx, pos) : EntryDelegate::NoButton;
        if (b == EntryDelegate::NoButton)
            return false;
        m_eatRelease = true;
        if (event->type() == QEvent::MouseButtonDblClick)
            return true; // the first press already acted
        const QPersistentModelIndex target(idx);
        if (b == EntryDelegate::PlayButton) {
            const QStringList files = filesFor(idx);
            if (!files.isEmpty())
                emit enqueueAndPlayRequested(files);
        } else {
            // The menu opens under the button, after this press is done.
            const QRectF r = m_delegate->tileButtonRect(rect, EntryDelegate::MoreButton);
            m_menuAt = m_view->viewport()->mapToGlobal(QPoint(int(r.left()), int(r.bottom()) + 4));
            QMetaObject::invokeMethod(this, [this, target, pos] {
                if (target.isValid())
                    showContextMenu(pos);
                m_menuAt = {};
            }, Qt::QueuedConnection);
        }
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (m_eatRelease) {
            m_eatRelease = false;
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool LibraryView::eventFilter(QObject *obj, QEvent *event)
{
    if (m_dropLine && obj == m_view->viewport() &&
        (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
         event->type() == QEvent::DragLeave || event->type() == QEvent::Drop))
        return queueDragEvent(event);
    if (obj == m_view->viewport() && tileButtonEvent(event))
        return true;
    if (event->type() == QEvent::MouseButtonRelease) {
        if (auto *label = qobject_cast<QLabel *>(obj); label && m_crumbTargets.contains(label)) {
            const Location target = m_crumbTargets.value(label);
            QMetaObject::invokeMethod(this, [this, target] { navigate(target); }, Qt::QueuedConnection);
            return true;
        }
    }
    if (obj == m_view && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Backspace) {
            m_backBtn->click();
            return true;
        }
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && ke->modifiers() == Qt::NoModifier) {
            const QModelIndexList sel = selectedRows();
            if (sel.size() > 1 && m_loc.section != Section::Queue)
                playSelection(true);
            else if (m_view->currentIndex().isValid())
                activate(m_view->currentIndex());
            return true;
        }
    }
    if (obj == m_view && event->type() == QEvent::Resize)
        QMetaObject::invokeMethod(this, [this] { updateGrid(); }, Qt::QueuedConnection);
    return QWidget::eventFilter(obj, event);
}

void LibraryView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    updateGrid();
}

void LibraryView::setGridView(bool grid, bool remember)
{
    m_delegate->grid = grid;
    if (remember)
        m_gridForSection[int(m_loc.section)] = grid;
    m_viewBtn->setIconName(grid ? "listview" : "grid");
    if (grid) {
        m_view->setViewMode(QListView::IconMode);
        m_view->setFlow(QListView::LeftToRight);
        m_view->setWrapping(true);
        m_view->setResizeMode(QListView::Adjust);
        m_view->setMovement(QListView::Static);
        m_view->setSpacing(0);
    } else {
        m_view->setViewMode(QListView::ListMode);
        m_view->setFlow(QListView::TopToBottom);
        m_view->setWrapping(false);
        m_view->setGridSize(QSize());
        m_view->setSpacing(0);
    }
    updateGrid();
}

void LibraryView::updateGrid()
{
    m_empty->setGeometry(m_view->rect().adjusted(20, 0, -20, -m_view->height() / 4));
    if (!m_delegate->grid) {
        m_view->doItemsLayout();
        return;
    }
    // Music shows album art, which is square: smaller, square tiles.
    const bool square = m_loc.section == Section::Music;
    static const int minWidths[] = {200, 280, 380};
    static const int squareWidths[] = {150, 190, 250};
    const int size = qBound(0, prefs().integer("ui/thumbSize"), 2);
    const int minW = square ? squareWidths[size] : minWidths[size];
    const int gap = 20;
    // Each cell includes a gap on its right; the last column's gap is the right margin.
    const int avail = m_view->viewport()->width() - m_view->verticalScrollBar()->sizeHint().width() - 4;
    const int cols = qMax(1, avail / (minW + gap));
    const int tileW = avail / cols;
    const int thumbH = square ? tileW - gap : int((tileW - gap) * 9.0 / 16.0);
    const QSize tile(tileW, thumbH + (square ? 64 : 50));
    m_delegate->square = square;
    if (m_delegate->tileSize != tile || m_view->gridSize() != tile) {
        m_delegate->tileSize = tile;
        m_view->setGridSize(tile);
    }
    m_view->doItemsLayout();
}

QStringList LibraryView::currentPlayable(int *indexOfPath, const QString &path) const
{
    QStringList items;
    for (int i = 0; i < m_proxy->rowCount(); ++i) {
        const QModelIndex idx = m_proxy->index(i, 0);
        const auto kind = EntryKind(idx.data(KindRole).toInt());
        if (kind == EntryKind::Video || kind == EntryKind::Audio || kind == EntryKind::Stream) {
            const QString p = idx.data(PathRole).toString();
            if (indexOfPath && p == path)
                *indexOfPath = int(items.size());
            items << p;
        }
    }
    return items;
}

void LibraryView::activate(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    const auto kind = EntryKind(index.data(KindRole).toInt());
    const QString path = index.data(PathRole).toString();
    switch (kind) {
    case EntryKind::Folder: {
        Section target = m_loc.section;
        if (m_loc.section == Section::Home)
            target = prefs().list("library/musicFolders").contains(path) ? Section::Music : Section::Videos;
        navigate({target, path, {}});
        break;
    }
    case EntryKind::Playlist:
        navigate({Section::Playlists, {}, path});
        break;
    case EntryKind::Category:
        navigate({Section::Music, {}, {}, path});
        break;
    case EntryKind::Group:
        if (path.startsWith("a:")) // an album on an artist's page
            navigate({Section::Music, {}, {}, m_loc.music, m_loc.group, path});
        else
            navigate({Section::Music, {}, {}, m_loc.music, path});
        break;
    case EntryKind::Action:
        if (path == "create-playlist") {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Create playlist"), tr("Playlist name:"),
                                                       QLineEdit::Normal, tr("New playlist"), &ok);
            if (ok)
                library().createPlaylist(name);
        } else if (path == "open-network") {
            emit openNetworkRequested();
        } else if (path == "add-music-folder") {
            emit addFolderRequested();
        }
        break;
    case EntryKind::Video:
    case EntryKind::Audio:
    case EntryKind::Stream: {
        if (m_loc.section == Section::Queue) {
            emit queueJumpRequested(index.data(OrderRole).toInt());
            break;
        }
        int at = 0;
        // Videos, streams and anything on Home/Recent play on their own; music
        // tracks queue the rest of their album/folder/playlist so it plays on.
        if (kind != EntryKind::Audio || m_loc.section == Section::Home || m_loc.section == Section::Recent
            || m_loc.section == Section::Network) {
            emit playRequested({path}, 0);
        } else {
            const QStringList items = currentPlayable(&at, path);
            emit playRequested(items, at);
        }
        break;
    }
    }
}

void LibraryView::addPlaylistMenu(QMenu &menu, const QStringList &items, const QString &suggestedName)
{
    QMenu *addTo = menu.addMenu(theme().icon("playlist", theme().accent()), tr("Add to Playlist"));
    for (const QString &name : library().playlistNames())
        addTo->addAction(name, this, [items, name] { library().addToPlaylist(name, items); });
    if (!library().playlistNames().isEmpty())
        addTo->addSeparator();
    addTo->addAction(tr("New Playlist…"), this, [this, items, suggestedName] {
        bool ok = false;
        const QString n = QInputDialog::getText(this, tr("Create playlist"), tr("Playlist name:"), QLineEdit::Normal,
                                                suggestedName.isEmpty() ? tr("New playlist") : suggestedName, &ok);
        if (ok)
            library().addToPlaylist(library().createPlaylist(n), items);
    });
}

void LibraryView::showContextMenu(const QPoint &pos)
{
    // At the pointer, or under a tile's ⋯ button when that opened it.
    const QPoint menuAt = m_menuAt.isNull() ? m_view->viewport()->mapToGlobal(pos) : m_menuAt;
    const QModelIndex index = m_view->indexAt(pos);
    if (!index.isValid())
        return;
    const auto kind = EntryKind(index.data(KindRole).toInt());
    const QString path = index.data(PathRole).toString();
    const QColor accent = theme().accent();
    QMenu menu(this);

    if (kind == EntryKind::Action || kind == EntryKind::Category)
        return;

    // Right-clicking outside the selection selects just that entry; a tile's
    // ⋯ button acts on its tile alone and leaves the selection as it is.
    const bool fromButton = !m_menuAt.isNull();
    const bool inSelection = m_view->selectionModel()->isSelected(index);
    if (!inSelection && !fromButton)
        m_view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    const QModelIndexList sel = selectedRows();
    const bool many = sel.size() > 1 && (inSelection || !fromButton);
    const bool inQueue = m_loc.section == Section::Queue;

    // Several entries, or a single media file: queue actions for the selection.
    if ((many || isMediaEntry(index)) && !inQueue) {
        menu.addAction(theme().icon("play", accent), tr("Replace Queue"), this, [this] { playSelection(true); });
        menu.addAction(theme().icon("plus", accent), tr("Add to Queue"), this, [this] { playSelection(false); });
    }
    if (many) {
        QStringList files;
        for (const QModelIndex &i : sel)
            files << filesFor(i);
        QMenu *addTo = menu.addMenu(theme().icon("playlist", accent), tr("Add to playlist"));
        for (const QString &name : library().playlistNames())
            addTo->addAction(name, this, [files, name] { library().addToPlaylist(name, files); });
        if (!library().playlistNames().isEmpty())
            addTo->addSeparator();
        addTo->addAction(tr("New playlist…"), this, [this, files] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Create playlist"), tr("Playlist name:"),
                                                       QLineEdit::Normal, tr("New playlist"), &ok);
            if (ok)
                library().addToPlaylist(library().createPlaylist(name), files);
        });
        QList<int> orders; // descending, so earlier removals don't shift later ones
        QStringList paths;
        for (const QModelIndex &i : sel) {
            orders << i.data(OrderRole).toInt();
            paths << i.data(PathRole).toString();
        }
        std::sort(orders.begin(), orders.end(), std::greater<int>());
        menu.addSeparator();
        if (inQueue)
            menu.addAction(theme().icon("trash", accent), tr("Remove %1 from queue").arg(sel.size()), this,
                           [this, orders] { emit queueRemoveRowsRequested(orders); });
        if (m_loc.section == Section::Recent || m_loc.section == Section::Home)
            menu.addAction(theme().icon("trash", accent), tr("Remove %1 from recent").arg(sel.size()), this, [paths] {
                for (const QString &p : paths)
                    library().removeRecent(p);
            });
        if (m_loc.section == Section::Playlists && !m_loc.playlist.isEmpty())
            menu.addAction(theme().icon("trash", accent), tr("Remove %1 from playlist").arg(sel.size()), this,
                           [pl = m_loc.playlist, orders] {
                               for (int o : orders)
                                   library().removeFromPlaylist(pl, o);
                           });
        menu.exec(menuAt);
        return;
    }

    if (kind == EntryKind::Group) {
        const QStringList items = tracksFor(path);
        const QString name = index.data(Qt::DisplayRole).toString();
        const bool album = path.startsWith("a:") || m_loc.music == "albums";
        const bool artist = m_loc.music == "artists" && m_loc.group.isEmpty();
        addPlaylistMenu(menu, items, name);
        menu.addSeparator();
        const QString quoted = QFontMetrics(menu.font()).elidedText(name, Qt::ElideRight, 260);
        menu.addAction(theme().icon("play", accent), tr("Play “%1”").arg(quoted), this,
                       [this, items] { emit playRequested(items, 0); });
        menu.addAction(theme().icon("shuffle", accent), tr("Shuffle “%1”").arg(quoted), this, [this, items] {
            QStringList shuffled = items;
            std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937(std::random_device{}()));
            emit playRequested(shuffled, 0);
        });
        menu.addAction(theme().icon("play-next", accent), tr("Play Next"), this,
                       [this, items] { emit playNextRequested(items); });
        menu.addAction(theme().icon("play-later", accent), tr("Play Later"), this,
                       [this, items] { emit enqueueRequested(items); });
        if (album || (artist && artistImages() && !path.mid(2).isEmpty()))
            menu.addSeparator();
        if (album)
            menu.addAction(theme().icon("tune", accent), tr("Edit Album Info…"), this, [this, items] {
                (new AlbumEditorOverlay(items, window()))->open();
            });
        if (artist && artistImages() && !path.mid(2).isEmpty())
            menu.addAction(theme().icon("artist", accent), tr("Choose Artist Image…"), this, [this, name = path.mid(2)] {
                (new ArtistPictureOverlay(name, window()))->open();
            });
        const MusicIndex::Track *first = items.isEmpty() ? nullptr : musicIndex().track(items.first());
        const bool goArtist = album && first && m_loc.music != "artists";
        if (goArtist || first)
            menu.addSeparator();
        if (goArtist) {
            // The artist's key in the Artists view (as MusicIndex groups them).
            const QString key = musicField("artists") == MusicIndex::Artist || first->albumArtist.isEmpty()
                                    ? first->artist
                                    : first->albumArtist;
            menu.addAction(theme().icon("artist", accent), tr("Go to Artist"), this,
                           [this, key] { navigate({Section::Music, {}, {}, "artists", "g:" + key}); });
        }
        if (album && first)
            menu.addAction(theme().icon("folder", accent), tr("Show in Folder"), this, [dir = QFileInfo(first->path).absolutePath()] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
            });
        menu.exec(menuAt);
        return;
    }

    if (kind == EntryKind::Playlist) {
        menu.addAction(theme().icon("play", accent), tr("Play"), this, [this, path] {
            const QStringList items = library().expand(library().playlist(path));
            if (!items.isEmpty())
                emit playRequested(items, 0);
        });
        menu.addAction(theme().icon("play-next", accent), tr("Play Next"), this,
                       [this, path] { emit playNextRequested(library().expand(library().playlist(path))); });
        menu.addAction(theme().icon("play-later", accent), tr("Play Later"), this,
                       [this, path] { emit enqueueRequested(library().expand(library().playlist(path))); });
        menu.addAction(theme().icon("settings", accent), tr("Rename…"), this, [this, path] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename playlist"), tr("Name:"), QLineEdit::Normal, path, &ok);
            if (ok)
                library().renamePlaylist(path, name);
        });
        menu.addAction(theme().icon("trash", accent), tr("Delete playlist"), this,
                       [path] { library().deletePlaylist(path); });
        menu.exec(menuAt);
        return;
    }

    if (kind == EntryKind::Folder) {
        menu.addAction(theme().icon("play", accent), tr("Play folder"), this, [this, path] {
            const QStringList items = library().mediaInFolder(path, filterForSection());
            if (!items.isEmpty())
                emit playRequested(items, 0);
        });
        menu.addAction(theme().icon("plus", accent), tr("Add folder to queue"), this,
                       [this, path] { emit enqueueRequested(library().mediaInFolder(path, filterForSection())); });
        menu.addAction(theme().icon("external", accent), tr("Open in file manager"), this,
                       [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
        menu.exec(menuAt);
        return;
    }

    if (inQueue)
        menu.addAction(theme().icon("play", accent), tr("Play"), this, [this, index] { activate(index); });

    QMenu *addTo = menu.addMenu(theme().icon("playlist", accent), tr("Add to playlist"));
    for (const QString &name : library().playlistNames())
        addTo->addAction(name, this, [path, name] { library().addToPlaylist(name, {path}); });
    if (!library().playlistNames().isEmpty())
        addTo->addSeparator();
    addTo->addAction(tr("New playlist…"), this, [this, path] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Create playlist"), tr("Playlist name:"),
                                                   QLineEdit::Normal, tr("New playlist"), &ok);
        if (ok)
            library().addToPlaylist(library().createPlaylist(name), {path});
    });

    if (kind == EntryKind::Video || kind == EntryKind::Audio) {
        const bool watched = library().progress(path) >= 0.95;
        menu.addAction(theme().icon(watched ? "eye-off" : "eye", accent),
                       watched ? tr("Mark as unwatched") : tr("Mark as watched"), this,
                       [path, watched] { library().setWatched(path, !watched); });
    }
    if (!Library::isUrl(path))
        menu.addAction(theme().icon("folder", accent), tr("Show in folder"), this, [path] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        });

    menu.addSeparator();
    if (m_loc.section == Section::Recent || m_loc.section == Section::Home)
        menu.addAction(theme().icon("trash", accent), tr("Remove from recent"), this,
                       [path] { library().removeRecent(path); });
    if (m_loc.section == Section::Playlists && !m_loc.playlist.isEmpty()) {
        const int order = index.data(OrderRole).toInt();
        const QString pl = m_loc.playlist;
        menu.addAction(theme().icon("trash", accent), tr("Remove from playlist"), this,
                       [pl, order] { library().removeFromPlaylist(pl, order); });
    }
    if (m_loc.section == Section::Queue) {
        const int order = index.data(OrderRole).toInt();
        menu.addAction(theme().icon("trash", accent), tr("Remove from queue"), this,
                       [this, order] { emit queueRemoveRequested(order); });
    }
    if (m_loc.section == Section::Network)
        menu.addAction(theme().icon("trash", accent), tr("Forget stream"), this,
                       [path] { library().removeStream(path); });
    menu.exec(menuAt);
}

void LibraryView::applySort()
{
    static_cast<EntryProxy *>(m_proxy)->key = m_sortKey;
    m_proxy->invalidate();
    m_proxy->sort(0, m_sortDesc ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void LibraryView::loadSort()
{
    const QString ctx = sortContext(m_loc.section, m_loc.music, !m_loc.group.isEmpty(), !m_loc.album.isEmpty());
    if (ctx == "music/artists/group") {
        // An artist's albums go by year; the Sort menu picks the direction.
        m_sortKey = SortYear;
        m_sortDesc = prefs().get("music/albumYearDesc").toBool();
        return;
    }
    const QVariant saved = ctx.startsWith("music/") ? prefs().get("sort/" + ctx) : m_sortFor.value(ctx);
    const QStringList parts = saved.toString().split(',');
    m_sortKey = parts.value(0).toInt();
    m_sortDesc = parts.value(1) == "1";
}

void LibraryView::saveSort()
{
    const QString ctx = sortContext(m_loc.section, m_loc.music, !m_loc.group.isEmpty(), !m_loc.album.isEmpty());
    const QString value = QString("%1,%2").arg(m_sortKey).arg(m_sortDesc ? 1 : 0);
    if (ctx.startsWith("music/"))
        prefs().set("sort/" + ctx, value); // music views keep their order between runs
    else
        m_sortFor.insert(ctx, value);
}

void LibraryView::showSortMenu()
{
    QMenu menu(this);
    const QPoint at = m_sortBtn->mapToGlobal(QPoint(0, m_sortBtn->height() + 4));
    const bool music = m_loc.section == Section::Music && !m_loc.music.isEmpty() && m_loc.music != "folders";
    const bool inGroup = !m_loc.group.isEmpty();

    // Artists, and an artist's albums: two segments, who counts as the artist
    // and which way album years run.
    if (music && m_loc.music == "artists" && m_loc.album.isEmpty()) {
        menu.addSection(tr("Group by"));
        const bool albumArtist = prefs().get("music/artistMode").toString() == "albumartist";
        auto *mode = new QActionGroup(&menu);
        for (bool aa : {false, true}) {
            QAction *a = menu.addAction(aa ? tr("Album Artist") : tr("Artist"), this, [this, aa] {
                if (!m_loc.group.isEmpty()) // that artist may not exist the other way
                    navigate({Section::Music, {}, {}, "artists"});
                prefs().set("music/artistMode", aa ? "albumartist" : "artist"); // reloads
            });
            a->setCheckable(true);
            a->setChecked(aa == albumArtist);
            mode->addAction(a);
        }
        menu.addSection(tr("Album Year"));
        const bool desc = prefs().get("music/albumYearDesc").toBool();
        auto *order = new QActionGroup(&menu);
        for (bool d : {false, true}) {
            QAction *a = menu.addAction(d ? tr("Descending") : tr("Ascending"), this, [this, d] {
                prefs().set("music/albumYearDesc", d);
                if (!m_loc.group.isEmpty()) {
                    loadSort();
                    applySort();
                }
            });
            a->setCheckable(true);
            a->setChecked(d == desc);
            order->addAction(a);
        }
        menu.exec(at);
        return;
    }

    QList<QPair<QString, int>> keys;
    if (!music) {
        keys = {{tr("Default order"), SortDefault}, {tr("Name"), SortName}, {tr("Date modified"), SortDate}, {tr("Size"), SortSize}};
    } else if (m_loc.music == "songs") {
        keys = {{tr("Title"), SortDefault}, {tr("Artist"), SortArtist}, {tr("Album"), SortAlbum},
                {tr("Year"), SortYear}, {tr("Date modified"), SortDate}};
    } else if (inGroup) {
        keys = {{m_loc.music == "genres" ? tr("Album order") : tr("Track order"), SortDefault}, {tr("Title"), SortName}};
        if (m_loc.music == "genres")
            keys << qMakePair(tr("Album"), int(SortAlbum)) << qMakePair(tr("Artist"), int(SortArtist))
                 << qMakePair(tr("Year"), int(SortYear));
    } else if (m_loc.music == "albums") {
        keys = {{tr("Name"), SortDefault}, {tr("Album artist"), SortArtist}, {tr("Year"), SortYear}};
    } else {
        keys = {{tr("Name"), SortDefault}};
    }

    auto *group = new QActionGroup(&menu);
    for (const auto &k : keys) {
        QAction *a = menu.addAction(k.first, this, [this, key = k.second] {
            m_sortKey = key;
            saveSort();
            applySort();
        });
        a->setCheckable(true);
        a->setChecked(k.second == m_sortKey);
        group->addAction(a);
    }
    menu.addSeparator();
    auto *order = new QActionGroup(&menu);
    for (bool d : {false, true}) {
        const QString label = m_sortKey == SortYear ? (d ? tr("Newest first") : tr("Oldest first"))
                                                    : (d ? tr("Descending") : tr("Ascending"));
        QAction *a = menu.addAction(label, this, [this, d] {
            m_sortDesc = d;
            saveSort();
            applySort();
        });
        a->setCheckable(true);
        a->setChecked(m_sortDesc == d);
        order->addAction(a);
    }
    menu.exec(at);
}

void LibraryView::showMoreMenu()
{
    const QColor accent = theme().accent();
    QMenu menu(this);
    const QStringList items = currentPlayable();
    QAction *play = menu.addAction(theme().icon("play", accent), tr("Play all"), this,
                                   [this, items] { emit playRequested(items, 0); });
    play->setEnabled(!items.isEmpty() && m_loc.section != Section::Queue);
    QAction *shuffle = menu.addAction(theme().icon("shuffle", accent), tr("Shuffle all"), this, [this, items] {
        QStringList shuffled = items;
        std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937(std::random_device{}()));
        emit playRequested(shuffled, 0);
    });
    shuffle->setEnabled(!items.isEmpty() && m_loc.section != Section::Queue);
    QAction *enqueue = menu.addAction(theme().icon("plus", accent), tr("Add all to queue"), this,
                                      [this, items] { emit enqueueRequested(items); });
    enqueue->setEnabled(!items.isEmpty() && m_loc.section != Section::Queue);
    menu.addSeparator();
    if (m_loc.section == Section::Queue) {
        menu.addAction(theme().icon("playlist", accent), tr("Save queue as playlist…"), this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Save queue"), tr("Playlist name:"),
                                                       QLineEdit::Normal, tr("Queue"), &ok);
            if (ok)
                library().addToPlaylist(library().createPlaylist(name), m_queue);
        });
        menu.addAction(theme().icon("trash", accent), tr("Clear queue"), this, &LibraryView::queueClearRequested);
    }
    if (m_loc.section == Section::Recent)
        menu.addAction(theme().icon("trash", accent), tr("Clear history"), this, [] { library().clearRecent(); });
    if ((m_loc.section == Section::Videos || m_loc.section == Section::Music) && !m_loc.path.isEmpty()) {
        const QString dir = m_loc.path;
        menu.addAction(theme().icon("external", accent), tr("Open in file manager"), this,
                       [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
    }
    if (m_loc.section == Section::Videos || m_loc.section == Section::Music || m_loc.section == Section::Home)
        menu.addAction(theme().icon("folder", accent), tr("Add a library folder…"), this, &LibraryView::addFolderRequested);
    menu.addAction(theme().icon("refresh", accent), tr("Refresh"), this, [this] {
        if (m_loc.section == Section::Music)
            musicIndex().rescan();
        reload();
    });
    menu.exec(m_moreBtn->mapToGlobal(QPoint(0, m_moreBtn->height() + 4)));
}
