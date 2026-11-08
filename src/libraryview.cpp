#include "libraryview.h"
#include "homepage.h"
#include "albumpanel.h"
#include "musicindex.h"
#include "durations.h"
#include "artistimages.h"
#include "musiceditors.h"
#include "librarydialogs.h"
#include "filedialogs.h"
#include "netfeeds.h"
#include "showpresets.h"
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
#include <QFile>
#include <QFileInfo>
#include <QPointer>
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
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <random>

namespace {
// Rows of the play queue being dragged within it (their positions, comma-separated).
const char *const kQueueRowsMime = "application/x-konos-queue-rows";

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
    AnchorRole, // the open album's songs: the source row they follow
};

// Sort keys. Which ones a view offers depends on what it lists.
enum SortKey { SortDefault, SortName, SortDate, SortSize, SortYear, SortArtist, SortAlbum,
               SortUnwatched, SortWatched, SortDuration, SortProgress };

// Watched state for sorting: 0 not started, 1 part watched, 2 watched.
int watchState(const QModelIndex &idx)
{
    const double p = library().progress(idx.data(PathRole).toString());
    return p >= 0.95 ? 2 : p > 0.01 ? 1 : 0;
}

qint64 lengthOf(const QModelIndex &idx)
{
    const auto kind = EntryKind(idx.data(KindRole).toInt());
    if (kind != EntryKind::Video && kind != EntryKind::Audio)
        return -1;
    const QString path = idx.data(PathRole).toString();
    if (const MusicIndex::Track *t = musicIndex().track(path); t && t->duration > 0)
        return t->duration;
    return Library::isUrl(path) ? -1 : Durations::instance().duration(path);
}

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
    int hideBefore = -1; // rows ordered before this are hidden

protected:
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override
    {
        // An open album's songs sort as their row's last album, just after it.
        const bool pa = a.data(AnchorRole).isValid(), pb = b.data(AnchorRole).isValid();
        if (pa || pb) {
            const QModelIndex A = pa ? sourceModel()->index(a.data(AnchorRole).toInt(), 0) : a;
            const QModelIndex B = pb ? sourceModel()->index(b.data(AnchorRole).toInt(), 0) : b;
            if (A == B)
                return sortOrder() == Qt::DescendingOrder ? pa : pb;
            return lessThan(A, B);
        }
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
        auto byName = [&] {
            const int c = collator().compare(a.data(Qt::DisplayRole).toString(), b.data(Qt::DisplayRole).toString());
            return c ? (desc ? c > 0 : c < 0) : natural();
        };
        switch (key) {
        case SortUnwatched:
        case SortWatched: { // a fixed order (the menu never reverses it), names within each
            static const int unwatchedFirst[] = {0, 1, 2}, watchedFirst[] = {2, 1, 0};
            const int *rank = key == SortUnwatched ? unwatchedFirst : watchedFirst;
            const int ra = rank[watchState(a)], rb = rank[watchState(b)];
            if (ra != rb)
                return desc ? ra > rb : ra < rb;
            return byName(); // A–Z either way
        }
        case SortDuration: {
            const qint64 la = lengthOf(a), lb = lengthOf(b);
            if ((la < 0) != (lb < 0)) // unknown last
                return desc ? la < 0 : lb < 0;
            return la != lb ? la < lb : natural();
        }
        case SortProgress: {
            const double pa = library().progress(a.data(PathRole).toString());
            const double pb = library().progress(b.data(PathRole).toString());
            return pa != pb ? pa < pb : natural();
        }
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
        if (idx.data(OrderRole).toInt() < hideBefore) // the music queue's folded Recently Played
            return false;
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

// The music queue's sizes: a two-line row, and the titles above sections.
namespace {
constexpr int kQueueRow = 58, kRecentTitle = 46, kNowTitle = 40, kNextTitle = 46, kNextFrom = 20, kSectionGap = 18;

struct QueueTitles {
    bool recent = false, now = false, next = false;
    bool gap = false; // a divider above, after an earlier section
};

QueueTitles queueTitles(const EntryDelegate::MusicQueue &q, const QModelIndex &index)
{
    QueueTitles t;
    if (!q.on)
        return t;
    const int order = index.data(OrderRole).toInt();
    t.recent = q.current > 0 && (q.recentFolded ? order == q.current : order == 0);
    t.now = order == q.current;
    t.next = order == q.current + 1;
    t.gap = (t.now || t.next) && (index.row() > 0 || t.recent);
    return t;
}

QString trackCount(int n)
{
    return n == 1 ? QObject::tr("1 track") : QObject::tr("%1 tracks").arg(n);
}
} // namespace

int EntryDelegate::queueHeaderHeight(const QModelIndex &index) const
{
    const QueueTitles t = queueTitles(musicQueue, index);
    return (t.recent ? kRecentTitle : 0) + (t.gap ? kSectionGap : 0) + (t.now ? kNowTitle : 0) +
           (t.next ? kNextTitle + (musicQueue.nextAlbum.isEmpty() ? 0 : kNextFrom) : 0);
}

bool EntryDelegate::onRecentTitle(const QRect &itemRect, const QModelIndex &index, const QPoint &pos) const
{
    if (!queueTitles(musicQueue, index).recent)
        return false;
    return QRect(itemRect.left(), itemRect.top(), itemRect.width(), kRecentTitle).contains(pos);
}

QSize EntryDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const
{
    if (index.data(AnchorRole).isValid()) // an open album's songs: as set
        return index.data(Qt::SizeHintRole).toSize();
    if (!grid && musicQueue.on)
        return QSize(200, kQueueRow + queueHeaderHeight(index));
    // Without a fixed grid (an album's songs are open), a hair narrower so
    // the row wraps where the grid did.
    return grid ? tileSize - QSize(freeLayout ? 4 : 0, 0) : QSize(200, 44);
}

void EntryDelegate::paintQueueRow(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const Palette &c = theme().pal();
    const MusicQueue &q = musicQueue;
    const QueueTitles t = queueTitles(q, index);
    const int order = index.data(OrderRole).toInt();
    const bool shuffle = prefs().flag("playback/shuffle");
    const qreal dpr = p->device()->devicePixelRatioF();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);

    QFont titleFont = option.font;
    titleFont.setPixelSize(17);
    titleFont.setWeight(QFont::DemiBold);
    QFont small = option.font;
    small.setPixelSize(13);
    const QRect full = option.rect;
    int y = full.top();

    if (t.recent) {
        const QRect band(full.left() + 4, y, full.width() - 8, kRecentTitle);
        p->setFont(titleFont);
        p->setPen(c.textDim);
        p->drawText(band, Qt::AlignVCenter | Qt::AlignLeft, shuffle ? QObject::tr("Earlier in the Queue")
                                                                     : QObject::tr("Recently Played"));
        const QRect chevron(band.right() - 18, band.center().y() - 8, 16, 16);
        p->drawPixmap(chevron.topLeft(), theme().pixmap(q.recentFolded ? "chevron-down" : "chevron-up", c.textMuted, 16, dpr));
        p->setFont(small);
        p->setPen(c.textMuted);
        p->drawText(band.adjusted(0, 0, -28, 0), Qt::AlignVCenter | Qt::AlignRight, trackCount(q.current));
        y += kRecentTitle;
    }
    if (t.gap) {
        p->setPen(QPen(c.border, 1));
        p->drawLine(QPointF(full.left() + 4, y + kSectionGap / 2 + 0.5), QPointF(full.right() - 4, y + kSectionGap / 2 + 0.5));
        y += kSectionGap;
    }
    if (t.now) {
        p->setFont(titleFont);
        p->setPen(c.text);
        p->drawText(QRect(full.left() + 4, y, full.width() - 8, kNowTitle), Qt::AlignVCenter | Qt::AlignLeft,
                    QObject::tr("Now Playing"));
        y += kNowTitle;
    }
    if (t.next) {
        const int h = kNextTitle + (q.nextAlbum.isEmpty() ? 0 : kNextFrom);
        QRect band(full.left() + 4, y, full.width() - 8, h);
        // The album's cover on the right, when everything next comes from it.
        if (!q.nextCover.isEmpty() && !q.nextAlbum.isEmpty()) {
            const QPixmap cover = Thumbnailer::instance().get(q.nextCover);
            if (!cover.isNull()) {
                const QRectF tile(band.right() - 40, band.center().y() - 20, 40, 40);
                QPainterPath clip;
                clip.addRoundedRect(tile, 4, 4);
                p->save();
                p->setClipPath(clip);
                p->drawPixmap(tile.topLeft(), Thumbnailer::scaled(cover, tile.size().toSize(), dpr, Qt::KeepAspectRatioByExpanding));
                p->restore();
                band.setRight(band.right() - 52);
            }
        }
        QString right;
        if (!shuffle && q.current + 2 <= q.total)
            right = QObject::tr("%1 of %2").arg(q.current + 2).arg(q.total);
        p->setFont(small);
        p->setPen(c.textMuted);
        const int rw = right.isEmpty() ? 0 : QFontMetrics(small).horizontalAdvance(right) + 12;
        const QRect top(band.left(), band.top() + (q.nextAlbum.isEmpty() ? 0 : 4), band.width(), kNextTitle - (q.nextAlbum.isEmpty() ? 0 : 4));
        if (!right.isEmpty())
            p->drawText(top, Qt::AlignVCenter | Qt::AlignRight, right);
        p->setFont(titleFont);
        p->setPen(c.text);
        p->drawText(top.adjusted(0, 0, -rw, 0), Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(titleFont).elidedText(shuffle ? QObject::tr("Also in the Queue") : QObject::tr("Playing Next"),
                                                       Qt::ElideRight, top.width() - rw));
        if (!q.nextAlbum.isEmpty()) {
            QFont from = option.font;
            from.setPixelSize(14);
            p->setFont(from);
            p->setPen(c.textMuted);
            const QRect line(band.left(), top.bottom() - 10, band.width(), kNextFrom + 6);
            p->drawText(line, Qt::AlignVCenter | Qt::AlignLeft,
                        QFontMetrics(from).elidedText(QObject::tr("From %1").arg(q.nextAlbum), Qt::ElideRight, line.width()));
        }
        y += h;
    }

    // The track itself.
    const QRectF r(full.left(), y + 2, full.width(), kQueueRow - 4);
    const bool current = order == q.current;
    const bool earlier = q.current >= 0 && order < q.current;
    const bool later = order > q.current;
    const bool hover = option.state & QStyle::State_MouseOver;
    const bool selected = option.state & QStyle::State_Selected;
    if (current || hover || selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(selected ? c.selected : c.hover);
        p->drawRoundedRect(r, 8, 8);
    }
    double x = r.left() + 8;
    if (later && !shuffle) { // its place in the queue
        QFont nf = small;
        nf.setFeature(QFont::Tag("tnum"), 1);
        p->setFont(nf);
        p->setPen(c.textMuted);
        p->drawText(QRectF(x - 4, r.top(), 26, r.height()), Qt::AlignVCenter | Qt::AlignRight, QString::number(order + 1));
        x += 30;
    }
    const QRectF tile(x, r.center().y() - 20, 40, 40);
    const QPixmap cover = EntryKind(index.data(KindRole).toInt()) == EntryKind::Audio ? thumbnail(index) : QPixmap();
    if (!cover.isNull()) {
        QPainterPath clip;
        clip.addRoundedRect(tile, 4, 4);
        p->save();
        p->setClipPath(clip);
        p->drawPixmap(tile.topLeft(), Thumbnailer::scaled(cover, tile.size().toSize(), dpr, Qt::KeepAspectRatioByExpanding));
        p->restore();
    } else {
        QColor bg = theme().accent();
        bg.setAlpha(theme().isDark() ? 46 : 36);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(tile, 4, 4);
        const QString icon = EntryKind(index.data(KindRole).toInt()) == EntryKind::Audio ? QString("song")
                                                                                           : iconForKind(EntryKind(index.data(KindRole).toInt()));
        p->drawPixmap(QPointF(tile.center().x() - 10, tile.center().y() - 10), theme().pixmap(icon, theme().accent(), 20, dpr));
    }
    const QRectF text(tile.right() + 14, r.top(), r.right() - tile.right() - 26, r.height());
    QStringList by;
    for (int role : {int(ArtistRole), int(AlbumRole)})
        if (!index.data(role).toString().isEmpty())
            by << index.data(role).toString();
    const QString sub = by.join(QString(" – "));
    QFont f = option.font;
    f.setPixelSize(14);
    p->setFont(f);
    p->setPen(earlier ? c.textDim : c.text);
    const QRectF titleRect = sub.isEmpty() ? text : QRectF(text.left(), text.center().y() - 20, text.width(), 20);
    p->drawText(titleRect, Qt::AlignLeft | (sub.isEmpty() ? Qt::AlignVCenter : Qt::AlignBottom),
                QFontMetrics(f).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, int(text.width())));
    if (!sub.isEmpty()) {
        p->setFont(small);
        p->setPen(c.textMuted);
        p->drawText(QRectF(text.left(), text.center().y() + 2, text.width(), 18), Qt::AlignLeft | Qt::AlignTop,
                    QFontMetrics(small).elidedText(sub, Qt::ElideRight, int(text.width())));
    }
    p->restore();
}

static bool isWebImage(const QString &s)
{
    return s.startsWith("https://") || s.startsWith("http://");
}

QPixmap EntryDelegate::thumbnail(const QModelIndex &index) const
{
    const QString thumb = index.data(ThumbRole).toString();
    const QString src = thumb.isEmpty() ? index.data(PathRole).toString() : thumb;
    if (isWebImage(src))
        return EntryKind(index.data(KindRole).toInt()) == EntryKind::Stream && thumb.isEmpty()
                   ? QPixmap() // a stream's own address isn't a picture
                   : NetFeeds::instance().image(src);
    if (!src.startsWith('/'))
        return {}; // network lists, actions
    return Thumbnailer::instance().get(src);
}

QString EntryDelegate::columnTitle(Column c)
{
    switch (c) {
    case Column::Name: return QObject::tr("Name");
    case Column::Artist: return QObject::tr("Artist");
    case Column::Album: return QObject::tr("Album");
    case Column::Year: return QObject::tr("Year");
    case Column::Duration: return QObject::tr("Length");
    case Column::Size: return QObject::tr("Size");
    case Column::Modified: return QObject::tr("Modified");
    case Column::Progress: return QObject::tr("Progress");
    case Column::Songs: return QObject::tr("Songs");
    case Column::Detail: return QObject::tr("Details");
    }
    return {};
}

QList<QPair<Column, QRectF>> EntryDelegate::columnRects(const QRectF &row) const
{
    auto fixedWidth = [](Column c) {
        switch (c) {
        case Column::Year: return 56;
        case Column::Duration: return 72;
        case Column::Size: return 84;
        case Column::Modified: return 150;
        case Column::Progress: return 118;
        case Column::Songs: return 84;
        default: return 0; // shares what's left
        }
    };
    auto weight = [](Column c) { return c == Column::Name ? 3.0 : 2.0; };
    const double gap = 14;
    double fixed = 0, weights = 0;
    for (Column c : columns) {
        fixed += fixedWidth(c);
        weights += fixedWidth(c) ? 0 : weight(c);
    }
    const double inner = row.width() - 12; // a little room on the right
    const double flexible = qMax(80.0, inner - fixed - gap * (columns.size() - 1));
    QList<QPair<Column, QRectF>> out;
    double x = row.left();
    for (Column c : columns) {
        const double w = fixedWidth(c) ? fixedWidth(c) : flexible * weight(c) / qMax(1.0, weights);
        out << qMakePair(c, QRectF(x, row.top(), w, row.height()));
        x += w + gap;
    }
    return out;
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
    if (index.data(AnchorRole).isValid())
        return; // an open album's songs: a widget of their own
    if (!grid && musicQueue.on) {
        paintQueueRow(p, option, index);
        return;
    }
    // Only videos are "watched"; music just plays.
    const double progress = kind == EntryKind::Video ? library().progress(path) : 0.0;
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
        if (kind == EntryKind::Audio || kind == EntryKind::Group || isWebImage(index.data(ThumbRole).toString()))
            cover = thumbnail(index);
        const QRectF tile(r.left() + 4, r.center().y() - 15, 30, 30);
        if (!cover.isNull()) {
            // Small cover, cropped to a square.
            QPainterPath clip;
            clip.addRoundedRect(tile, 4, 4);
            p->save();
            p->setClipPath(clip);
            p->drawPixmap(tile.topLeft(), Thumbnailer::scaled(cover, tile.size().toSize(), dpr, Qt::KeepAspectRatioByExpanding));
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
        const auto cols = columnRects(r.adjusted(4, 0, 0, 0));
        if (!cols.isEmpty())
            textRect.setRight(cols.first().second.right());
        QString right = cols.isEmpty() ? index.data(DetailRole).toString() : QString();
        if (cols.isEmpty() && right.isEmpty() && kind == EntryKind::Video) {
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
        if (cols.size() > 1)
            paintDetails(p, option, index);
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

    const QPixmap pm = kind == EntryKind::Action || kind == EntryKind::Category ? QPixmap() : thumbnail(index);
    if (!pm.isNull() && isWebImage(index.data(ThumbRole).toString())) {
        // Station logos and podcast artwork: the whole picture, centred.
        const QSizeF box = thumb.size() * (kind == EntryKind::Stream ? 0.62 : 1.0);
        const QPixmap fit = Thumbnailer::scaled(pm, box.toSize(), p->device()->devicePixelRatioF(), Qt::KeepAspectRatio);
        const QSizeF fs = fit.deviceIndependentSize();
        p->drawPixmap(QPointF(thumb.center().x() - fs.width() / 2, thumb.center().y() - fs.height() / 2), fit);
    } else if (!pm.isNull()) {
        // Cover the tile (crop to fill).
        const QPixmap cover = Thumbnailer::scaled(pm, thumb.size().toSize(), p->device()->devicePixelRatioF(),
                                                  Qt::KeepAspectRatioByExpanding);
        p->drawPixmap(thumb.topLeft(), cover);
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

// The columns after the name in a details row.
void EntryDelegate::paintDetails(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const Palette &c = theme().pal();
    const auto kind = EntryKind(index.data(KindRole).toInt());
    const bool media = kind == EntryKind::Video || kind == EntryKind::Audio;
    const QString path = index.data(PathRole).toString();
    QFont f = option.font;
    f.setPixelSize(13);
    p->setFont(f);
    const QFontMetrics fm(f);
    const QRectF r = QRectF(option.rect).adjusted(4, 1, 0, -1);
    const auto cols = columnRects(r);
    for (int i = 1; i < cols.size(); ++i) {
        const auto [col, rect] = cols.at(i);
        QString text;
        Qt::Alignment align = Qt::AlignLeft;
        switch (col) {
        case Column::Artist: text = index.data(ArtistRole).toString(); break;
        case Column::Album: text = index.data(AlbumRole).toString(); break;
        case Column::Year:
            text = index.data(YearRole).toInt() > 0 ? QString::number(index.data(YearRole).toInt()) : QString();
            align = Qt::AlignRight;
            break;
        case Column::Duration: {
            const qint64 ms = lengthOf(index);
            if (ms > 0) {
                const qint64 sec = (ms + 500) / 1000;
                text = sec >= 3600 ? QString("%1:%2:%3").arg(sec / 3600).arg(sec / 60 % 60, 2, 10, QChar('0')).arg(sec % 60, 2, 10, QChar('0'))
                                   : QString("%1:%2").arg(sec / 60).arg(sec % 60, 2, 10, QChar('0'));
            }
            align = Qt::AlignRight;
            break;
        }
        case Column::Size:
            if (media && index.data(SizeRole).toLongLong() > 0)
                text = QLocale().formattedDataSize(index.data(SizeRole).toLongLong(), 1);
            align = Qt::AlignRight;
            break;
        case Column::Modified:
            if (index.data(DateRole).toDateTime().isValid())
                text = QLocale().toString(index.data(DateRole).toDateTime(), QLocale::ShortFormat);
            break;
        case Column::Progress: {
            if (kind != EntryKind::Video)
                break;
            const double pr = library().progress(path);
            if (pr >= 0.95) {
                text = QObject::tr("Watched");
            } else if (pr > 0.01) {
                const QRectF track(rect.left(), rect.center().y() - 2, 64, 4);
                p->setPen(Qt::NoPen);
                p->setBrush(c.hover);
                p->drawRoundedRect(track, 2, 2);
                p->setBrush(theme().accent());
                p->drawRoundedRect(QRectF(track.left(), track.top(), track.width() * pr, track.height()), 2, 2);
                p->setPen(c.textMuted);
                p->drawText(rect.adjusted(72, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, QString("%1%").arg(int(pr * 100)));
            }
            break;
        }
        case Column::Songs: // "Artist · 1990 · 12 songs" → "12 songs"
            text = index.data(DetailRole).toString().section(QString(" · "), -1);
            break;
        case Column::Detail: text = index.data(DetailRole).toString(); break;
        case Column::Name: break;
        }
        if (text.isEmpty())
            continue;
        p->setPen(c.textMuted);
        p->drawText(rect, Qt::AlignVCenter | align, fm.elidedText(text, Qt::ElideRight, int(rect.width())));
    }
}

// Column titles above the details view; a click sorts by that column.
class DetailsHeader : public QWidget {
public:
    DetailsHeader(EntryDelegate *delegate, QListView *view, QWidget *parent)
        : QWidget(parent), m_delegate(delegate), m_view(view)
    {
        setFixedHeight(34);
        setCursor(Qt::PointingHandCursor);
    }
    std::function<void(Column)> clicked;
    Column sorted = Column::Name;
    bool sortedDesc = false;
    bool hasSort = false;

protected:
    QRectF rowRect() const
    {
        // Same horizontal extent as the rows inside the view.
        const QPoint origin = m_view->viewport()->mapTo(parentWidget(), QPoint(0, 0)) - pos();
        return QRectF(origin.x() + 4, 0, m_view->viewport()->width() - 4, height());
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Palette &c = theme().pal();
        QFont f = font();
        f.setPixelSize(12);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        const auto cols = m_delegate->columnRects(rowRect());
        for (int i = 0; i < cols.size(); ++i) {
            auto [col, rect] = cols.at(i);
            if (i == 0)
                rect.setLeft(rect.left() + 38); // over the names, past the icons
            const bool right = col == Column::Year || col == Column::Duration || col == Column::Size;
            const bool active = hasSort && col == sorted;
            p.setPen(active ? c.text : c.textMuted);
            const QString title = EntryDelegate::columnTitle(col);
            p.drawText(rect, Qt::AlignVCenter | (right ? Qt::AlignRight : Qt::AlignLeft), title);
            if (active) {
                const int tw = QFontMetrics(f).horizontalAdvance(title);
                const double x = right ? rect.right() - tw - 14 : rect.left() + tw + 4;
                p.drawPixmap(QPointF(x, rect.center().y() - 6),
                             theme().pixmap(sortedDesc ? "chevron-down" : "chevron-up", c.text, 12, devicePixelRatioF()));
            }
        }
        p.setPen(QPen(c.border, 1));
        p.drawLine(QPointF(0, height() - 0.5), QPointF(width(), height() - 0.5));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        for (const auto &[col, rect] : m_delegate->columnRects(rowRect()))
            if (e->position().x() >= rect.left() - 7 && e->position().x() <= rect.right() + 7) {
                if (clicked)
                    clicked(col);
                return;
            }
    }

private:
    EntryDelegate *m_delegate;
    QListView *m_view;
};

// ---------------------------------------------------------------------------
// LibraryView

LibraryView::LibraryView(Mode mode, QWidget *parent) : QWidget(parent), m_mode(mode)
{
    setObjectName(mode == Full ? "Library" : "SidePanel");
    setAttribute(Qt::WA_StyledBackground);
    m_viewForSection = {{int(Section::Home), 0},     {int(Section::Videos), 0},    {int(Section::Music), 0},
                        {int(Section::Network), 1},  {int(Section::Playlists), 1}, {int(Section::Recent), 0},
                        {int(Section::Queue), 1}};
    if (mode == Full) // "1:2,2:0": section → view, kept between runs
        for (const QString &pair : prefs().string("ui/viewModes").split(',', Qt::SkipEmptyParts))
            m_viewForSection[pair.section(':', 0, 0).toInt()] = qBound(0, pair.section(':', 1).toInt(), 2);
    else // the side panel starts as lists
        for (auto it = m_viewForSection.begin(); it != m_viewForSection.end(); ++it)
            it.value() = 1;

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
    m_viewBtn->setToolTip(mode == Full ? tr("Grid, list or details") : tr("Grid or list"));
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
            connect(&prefs(), &Prefs::changed, this, [this, syncOrder](const QString &key) {
                if (key == "playback/repeat" || key == "playback/shuffle")
                    syncOrder();
                if (key == "playback/shuffle" && m_delegate->musicQueue.on)
                    m_view->viewport()->update(); // the section titles say what shuffle does
                if (key == "ui/queueRecentFolded" && m_delegate->musicQueue.on)
                    reload();
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
    if (mode == Full) {
        m_detailsHeader = new DetailsHeader(m_delegate, m_view, this);
        m_detailsHeader->hide();
        m_detailsHeader->clicked = [this](Column col) {
            const bool music = m_loc.section == Section::Music && !m_loc.music.isEmpty() && m_loc.music != "folders";
            int key = SortName;
            switch (col) {
            case Column::Name: key = music && m_loc.music != "songs" && m_loc.group.isEmpty() ? SortDefault : SortName; break;
            case Column::Artist: key = SortArtist; break;
            case Column::Album: key = SortAlbum; break;
            case Column::Year: key = SortYear; break;
            case Column::Duration: key = SortDuration; break;
            case Column::Size: key = SortSize; break;
            case Column::Modified: key = SortDate; break;
            case Column::Progress: key = SortProgress; break;
            case Column::Songs:
            case Column::Detail: return;
            }
            // The same column again turns the order around.
            setSort(key, key == m_sortKey ? !m_sortDesc : (key == SortDate || key == SortProgress));
        };
        lay->addWidget(m_detailsHeader);
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
    connect(m_viewBtn, &IconButton::clicked, this, [this] {
        applyViewMode(m_mode == Full ? (viewMode() + 1) % 3 : (viewMode() == 0 ? 1 : 0));
    });
    connect(m_sortBtn, &IconButton::clicked, this, &LibraryView::showSortMenu);
    connect(m_moreBtn, &IconButton::clicked, this, &LibraryView::showMoreMenu);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(600);
    connect(m_searchTimer, &QTimer::timeout, this, [this] { reload(); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (m_home && m_home->isVisible()) {
            m_home->setFilter(t);
            return;
        }
        // Radio and Podcasts search online (after a pause in typing).
        if (m_loc.section == Section::Network && (m_loc.path == "radio" || m_loc.path == "podcasts")) {
            const QString q = t.trimmed().length() >= 2 ? t.trimmed() : QString();
            if (q != m_remoteQuery) {
                m_remoteQuery = q;
                m_searchTimer->start();
            }
            return;
        }
        m_proxy->setFilterFixedString(t);
        m_count->setText(itemCount(m_proxy->rowCount()));
    });

    auto repaint = [this] { m_view->viewport()->update(); };
    connect(&Thumbnailer::instance(), &Thumbnailer::ready, this, repaint);
    connect(&NetFeeds::instance(), &NetFeeds::imageReady, this, repaint);
    connect(&NetFeeds::instance(), &NetFeeds::ready, this, [this](const QString &key) {
        if (m_loc.section == Section::Network && key == networkFeedKey()) {
            const int scroll = m_view->verticalScrollBar()->value();
            reload();
            m_view->verticalScrollBar()->setValue(scroll);
        }
    });
    connect(&Durations::instance(), &Durations::ready, this, [this] {
        if (!m_delegate->columns.isEmpty())
            m_view->viewport()->update();
    });
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
        if (m_loc.section == Section::Network && !m_loc.path.startsWith("radio"))
            reload();
    });
    connect(&prefs(), &Prefs::changed, this, [this](const QString &key) {
        if (key == "ui/thumbSize")
            updateGrid();
        if (key.startsWith("library/") || (key == "network/directories" && m_loc.section == Section::Network) ||
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

// The queue tab shows music its own way, when music is what's playing (or,
// before anything plays, all that's queued) and the queue is in its own order.
bool LibraryView::isMusicQueue() const
{
    if (m_mode != QueueTab || m_loc.section != Section::Queue || m_queue.isEmpty() || m_sortKey != SortDefault || m_sortDesc)
        return false;
    if (m_queueCurrent >= 0 && m_queueCurrent < m_queue.size())
        return Library::kindFor(m_queue.at(m_queueCurrent)) == EntryKind::Audio;
    for (const QString &loc : m_queue)
        if (Library::kindFor(loc) != EntryKind::Audio)
            return false;
    return true;
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
    if (m_albumPanel)
        m_albumPanel->setNowPlaying(location);
    for (int i = 0; i < m_model->rowCount(); ++i) {
        QStandardItem *it = m_model->item(i);
        const bool cur = m_loc.section == Section::Queue ? it->data(OrderRole).toInt() == m_queueCurrent
                                                         : it->data(PathRole).toString() == location;
        it->setData(cur, Qt::UserRole + 50);
    }
}

void LibraryView::navigate(const Location &target, bool record)
{
    closeAlbumPanel();
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
    m_remoteQuery.clear();
    m_search->clear();
    loadSort();
    static_cast<EntryProxy *>(m_proxy)->key = m_sortKey;
    const bool menu = loc.section == Section::Music && loc.music.isEmpty();
    applyViewMode(menu ? 1 : m_mode == Full ? m_viewForSection.value(int(loc.section), 0)
                                           : (m_viewForSection.value(int(loc.section), 0) == 0 ? 0 : 1), false);
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
    closeAlbumPanel(false); // opened again below, if the album is still here
    m_model->clear();
    {
        EntryDelegate::MusicQueue mq;
        mq.on = isMusicQueue();
        if (mq.on) {
            mq.current = m_queueCurrent;
            mq.total = int(m_queue.size());
            mq.recentFolded = prefs().flag("ui/queueRecentFolded");
        }
        m_delegate->musicQueue = mq;
        auto *proxy = static_cast<EntryProxy *>(m_proxy);
        const int hide = mq.on && mq.recentFolded ? mq.current : -1;
        if (proxy->hideBefore != hide) {
            proxy->hideBefore = hide;
            proxy->invalidate();
        }
        m_view->setUniformItemSizes(!mq.on);
    }
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
        if (Library::isUrl(loc) && !library().streamName(loc).isEmpty())
            e.name = library().streamName(loc);
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
    case Section::Network:
        reloadNetwork(order, empty);
        break;
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
    case Section::Queue: {
        // Music gets its own layout (EntryDelegate::MusicQueue): titles
        // with the artist and album under them, and no "Watched".
        EntryDelegate::MusicQueue &mq = m_delegate->musicQueue;
        QStringList nextAlbums;
        QString nextCover;
        for (const QString &loc : m_queue) {
            LibraryEntry e = entryFor(loc);
            const MusicIndex::Track *t = mq.on && e.kind == EntryKind::Audio ? musicIndex().track(loc) : nullptr;
            if (t)
                e.name = t->displayTitle();
            QStandardItem *item = addEntry(e, order);
            if (t) {
                item->setData(t->displayArtist(), ArtistRole);
                item->setData(t->album, AlbumRole);
            }
            if (order > m_queueCurrent) {
                nextAlbums << (t ? t->album + '\n' + t->displayAlbumArtist() : QString());
                if (nextCover.isEmpty() && t)
                    nextCover = loc;
            }
            ++order;
        }
        nextAlbums.removeDuplicates();
        if (mq.on && nextAlbums.size() == 1 && !nextAlbums.first().section('\n', 0, 0).isEmpty()) {
            mq.nextAlbum = nextAlbums.first().section('\n', 0, 0);
            mq.nextCover = nextCover;
        }
        empty = tr("The play queue is empty.");
        break;
    }
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
    if (!m_openAlbum.isEmpty())
        openAlbumPanel(m_openAlbum);
}

QString LibraryView::networkFeedKey() const
{
    const QString &p = m_loc.path;
    if (p == "radio")
        return m_remoteQuery.isEmpty() ? "radio:top" : "radio:search:" + m_remoteQuery;
    if (p == "podcasts")
        return m_remoteQuery.isEmpty() ? QString() : "podcasts:search:" + m_remoteQuery;
    if (p.startsWith("m3u:") || p.startsWith("rss:"))
        return p;
    return {};
}

// Network: saved streams and lists, Radio and Podcasts, or one of the online
// lists (stations, a podcast's episodes, a channel list).
void LibraryView::reloadNetwork(int &order, QString &empty)
{
    auto action = [&](const QString &id, const QString &name) {
        LibraryEntry e;
        e.kind = EntryKind::Action;
        e.path = id;
        e.name = name;
        addEntry(e, order++);
    };
    auto list = [&](const QString &path, const QString &name, const QString &icon, const QString &image = {},
                    const QString &detail = {}) {
        LibraryEntry e;
        e.kind = EntryKind::Folder;
        e.path = path;
        e.name = name;
        QStandardItem *item = addEntry(e, order++);
        item->setData(icon, IconRole);
        item->setData(image, ThumbRole);
        item->setData(detail, DetailRole);
        item->setToolTip(path.mid(4));
        return item;
    };
    auto hostName = [](const QString &url) {
        const QUrl u(url);
        return u.host().isEmpty() ? url : u.host() + (u.fileName().isEmpty() ? QString() : " · " + u.fileName());
    };
    const QString &path = m_loc.path;
    if (path.isEmpty()) {
        action("open-network", tr("Open network stream…"));
        for (const auto &[id, name, icon] : {std::tuple{QString("radio"), tr("Radio"), QString("radio")},
                                             std::tuple{QString("podcasts"), tr("Podcasts"), QString("podcast")}}) {
            LibraryEntry e;
            e.kind = EntryKind::Category;
            e.path = id;
            e.name = name;
            addEntry(e, order++)->setData(icon, IconRole);
        }
        for (const QString &url : library().channelLists()) {
            const QString name = library().streamName(url);
            list("m3u:" + url, name.isEmpty() ? hostName(url) : name, "stream", {}, tr("Channel list"));
        }
        for (const QString &url : library().streams()) {
            LibraryEntry e;
            e.kind = EntryKind::Stream;
            e.path = url;
            e.name = library().streamName(url).isEmpty() ? url : library().streamName(url);
            QStandardItem *item = addEntry(e, order++);
            item->setData(library().streamImage(url), ThumbRole);
        }
        return;
    }
    if (path == "podcasts") {
        action("add-podcast", tr("Add a podcast by its feed link…"));
        if (m_remoteQuery.isEmpty()) {
            for (const QString &url : library().podcasts()) {
                const QString name = library().streamName(url);
                list("rss:" + url, name.isEmpty() ? hostName(url) : name, "podcast", library().streamImage(url));
            }
            empty = tr("No podcasts yet.\nSearch for one above, or add one by its feed link.");
            return;
        }
    }
    const QString key = networkFeedKey();
    if (key.isEmpty())
        return;
    const NetFeeds::Feed feed = NetFeeds::instance().feed(key);
    if (key.startsWith("podcasts:")) {
        for (const NetFeeds::Item &it : feed.items)
            list("rss:" + it.url, it.title, "podcast", it.image, it.detail);
    } else {
        // Remember a podcast's title and artwork for the list of podcasts.
        if (key.startsWith("rss:") && !feed.title.isEmpty() && library().podcasts().contains(key.mid(4)))
            library().setStreamInfo(key.mid(4), feed.title, feed.image);
        for (const NetFeeds::Item &it : feed.items) {
            LibraryEntry e;
            e.kind = key.startsWith("rss:") ? EntryKind::Audio : EntryKind::Stream;
            e.path = it.url;
            e.name = it.title;
            e.modified = it.date;
            QStandardItem *item = addEntry(e, order++);
            item->setData(it.image, ThumbRole);
            item->setData(it.detail, DetailRole);
            item->setToolTip(it.detail.isEmpty() ? it.url : it.detail);
        }
    }
    empty = feed.loading && feed.items.isEmpty() ? tr("Loading…") : feed.error;
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
        if (path.startsWith("m3u:") || path.startsWith("rss:")) { // what's loaded of an online list
            QStringList urls;
            for (const NetFeeds::Item &it : NetFeeds::instance().feed(path).items)
                urls << it.url;
            return urls;
        }
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
    if (m_loc.section == Section::Network && !m_loc.path.isEmpty()) {
        const QString &p = m_loc.path;
        if (p.startsWith("rss:") && library().podcasts().contains(p.mid(4))) {
            parts << tr("Podcasts");
            targets << Location{Section::Network, "podcasts"};
        }
        QString name = p == "radio" ? tr("Radio") : p == "podcasts" ? tr("Podcasts") : library().streamName(p.mid(4));
        if (name.isEmpty())
            name = NetFeeds::instance().feed(p).title;
        if (name.isEmpty())
            name = QUrl(p.mid(4)).host();
        parts << name;
        targets << m_loc;
    } else if (!m_loc.playlist.isEmpty()) {
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
            QRect r = m_view->visualRect(idx);
            r.setTop(r.top() + m_delegate->queueHeaderHeight(idx)); // past any section titles
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
    if (obj == m_view->viewport() && m_delegate->musicQueue.on &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick ||
         event->type() == QEvent::MouseButtonRelease)) {
        // The section titles above queue rows aren't part of the rows: a
        // click on Recently Played folds it, the others do nothing.
        const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
        const QModelIndex idx = m_view->indexAt(pos);
        if (idx.isValid() && pos.y() < m_view->visualRect(idx).top() + m_delegate->queueHeaderHeight(idx)) {
            if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton &&
                m_delegate->onRecentTitle(m_view->visualRect(idx), idx, pos)) {
                prefs().set("ui/queueRecentFolded", !prefs().flag("ui/queueRecentFolded")); // reloads
            }
            return true;
        }
    }
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
        if (ke->key() == Qt::Key_Escape && m_albumSpacer) {
            closeAlbumPanel();
            return true;
        }
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

int LibraryView::viewMode() const
{
    return m_delegate->grid ? 0 : m_delegate->columns.isEmpty() ? 1 : 2;
}

void LibraryView::setViewMode(int mode)
{
    applyViewMode(mode);
}

void LibraryView::applyViewMode(int mode, bool remember)
{
    if (m_mode != Full)
        mode = qMin(mode, 1);
    if (remember) {
        m_viewForSection[int(m_loc.section)] = mode;
        if (m_mode == Full) {
            QStringList pairs;
            for (auto it = m_viewForSection.begin(); it != m_viewForSection.end(); ++it)
                pairs << QString("%1:%2").arg(it.key()).arg(it.value());
            pairs.sort();
            prefs().set("ui/viewModes", pairs.join(','));
        }
    }
    m_delegate->columns = mode == 2 ? detailColumns() : QList<Column>();
    if (m_detailsHeader)
        m_detailsHeader->setVisible(mode == 2);
    setGridView(mode == 0, false);
    // The button shows what a click switches to.
    const int nextMode = m_mode == Full ? (mode + 1) % 3 : (mode == 0 ? 1 : 0);
    m_viewBtn->setIconName(nextMode == 0 ? "grid" : nextMode == 1 ? "listview" : "details");
    m_viewBtn->setToolTip(nextMode == 0 ? tr("Show as grid") : nextMode == 1 ? tr("Show as list") : tr("Show details"));
}

// Which columns the details view shows for what's listed here.
QList<Column> LibraryView::detailColumns() const
{
    if (m_loc.section == Section::Music && !m_loc.music.isEmpty() && m_loc.music != "folders") {
        const bool tracks = m_loc.music == "songs" || !m_loc.album.isEmpty() ||
                            (!m_loc.group.isEmpty() && m_loc.music != "artists");
        if (tracks)
            return {Column::Name, Column::Artist, Column::Album, Column::Year, Column::Duration};
        if (m_loc.music == "albums" || (m_loc.music == "artists" && !m_loc.group.isEmpty()))
            return {Column::Name, Column::Artist, Column::Year, Column::Songs};
        return {Column::Name, Column::Songs};
    }
    if (m_loc.section == Section::Network)
        return {Column::Name, Column::Detail, Column::Duration, Column::Progress};
    if (m_loc.section == Section::Playlists && m_loc.playlist.isEmpty())
        return {Column::Name};
    return {Column::Name, Column::Duration, Column::Size, Column::Modified, Column::Progress};
}

void LibraryView::setSort(int key, bool desc)
{
    m_sortKey = key;
    m_sortDesc = desc;
    saveSort();
    applySort();
    if (m_mode == QueueTab && isMusicQueue() != m_delegate->musicQueue.on)
        reload(); // sorted, the queue loses its sections (and gets them back)
}

void LibraryView::setGridView(bool grid, bool remember)
{
    if (!grid)
        closeAlbumPanel();
    m_delegate->grid = grid;
    if (remember)
        m_viewForSection[int(m_loc.section)] = grid ? 0 : 1;
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
    if (m_albumSpacer) {
        m_delegate->tileSize = tile;
        m_view->setGridSize(QSize());
        if (gridColumns() * tile.width() != m_albumWidth) // the window changed: lay the songs out again
            QMetaObject::invokeMethod(this, [this] {
                if (!m_openAlbum.isEmpty())
                    openAlbumPanel(m_openAlbum);
            }, Qt::QueuedConnection);
    } else if (m_delegate->tileSize != tile || m_view->gridSize() != tile) {
        m_delegate->tileSize = tile;
        m_view->setGridSize(tile);
    }
    m_view->doItemsLayout();
}

int LibraryView::gridColumns() const
{
    const int avail = m_view->viewport()->width() - m_view->verticalScrollBar()->sizeHint().width() - 4;
    return qMax(1, avail / qMax(1, m_delegate->tileSize.width()));
}

bool LibraryView::isAlbumTile(const QModelIndex &index) const
{
    if (EntryKind(index.data(KindRole).toInt()) != EntryKind::Group)
        return false;
    const QString path = index.data(PathRole).toString();
    return path.startsWith("a:") || (m_loc.section == Section::Music && m_loc.music == "albums" && m_loc.group.isEmpty());
}

void LibraryView::openAlbumPanel(const QString &albumPath)
{
    closeAlbumPanel(false);
    // The album's tile, and the last tile in its row: the songs go after that.
    int row = -1;
    for (int i = 0; i < m_proxy->rowCount(); ++i)
        if (m_proxy->index(i, 0).data(PathRole).toString() == albumPath) {
            row = i;
            break;
        }
    if (row < 0 || !m_delegate->grid) {
        m_openAlbum.clear();
        return;
    }
    m_openAlbum = albumPath;
    const QModelIndex tile = m_proxy->index(row, 0);
    // Tiles laid out by their own size from now on (the songs' row is
    // taller); then whatever shares the album's row is what the songs follow.
    m_delegate->freeLayout = true;
    m_view->setGridSize(QSize());
    m_view->setUniformItemSizes(false);
    m_view->doItemsLayout();
    auto topOf = [this](int r) { return m_view->visualRect(m_proxy->index(r, 0)).top(); };
    int last = row;
    while (last + 1 < m_proxy->rowCount() && topOf(last + 1) == topOf(row))
        ++last;
    // As wide as a full row, however few albums there are.
    int cols = 1;
    while (cols < m_proxy->rowCount() && topOf(cols) == topOf(0))
        ++cols;
    cols = qMax(cols, gridColumns() - (cols < m_proxy->rowCount() ? 0 : 1));
    const int anchor = m_proxy->mapToSource(m_proxy->index(last, 0)).row();
    const int width = cols * m_delegate->tileSize.width();

    auto *panel = new AlbumTracksPanel(tile.data(Qt::DisplayRole).toString(), tile.data(ArtistRole).toString(),
                                       tracksFor(albumPath));
    panel->setNowPlaying(m_nowPlaying);
    connect(panel, &AlbumTracksPanel::closeRequested, this, [this] { closeAlbumPanel(); });
    connect(panel, &AlbumTracksPanel::playAllRequested, this, [this](QStringList tracks, bool shuffle) {
        if (shuffle)
            std::shuffle(tracks.begin(), tracks.end(), std::mt19937(std::random_device{}()));
        emit playRequested(tracks, 0);
    });
    connect(panel, &AlbumTracksPanel::enqueueRequested, this, [this](const QString &p) { emit enqueueRequested({p}); });
    connect(panel, &AlbumTracksPanel::playNowRequested, this, [this](const QString &p) { emit enqueueAndPlayRequested({p}); });
    connect(panel, &AlbumTracksPanel::playAfterRequested, this, [this](const QString &p) { emit playNextRequested({p}); });
    // The same right-hand gap as the tiles, and some room below.
    auto *host = new QWidget;
    auto *lay = new QHBoxLayout(host);
    lay->setContentsMargins(0, 2, 20, 22);
    lay->addWidget(panel);

    auto *spacer = new QStandardItem;
    spacer->setFlags(Qt::ItemIsEnabled); // not selected, dragged or dropped on
    spacer->setData(int(EntryKind::Action), KindRole);
    spacer->setData(QStringLiteral("album-panel"), PathRole);
    spacer->setData(1 << 30, OrderRole);
    spacer->setData(anchor, AnchorRole);
    spacer->setData(QSize(width - 8, panel->heightForWidth(width - 28) + 24), Qt::SizeHintRole);
    m_model->appendRow(spacer);
    m_albumSpacer = spacer;
    m_albumHost = host;
    m_albumPanel = panel;
    m_albumAnchor = anchor;
    m_albumWidth = gridColumns() * m_delegate->tileSize.width(); // what updateGrid() compares
    const QModelIndex at = m_proxy->mapFromSource(spacer->index());
    m_view->setIndexWidget(at, host);
    m_view->doItemsLayout();
    QMetaObject::invokeMethod(this, [this] {
        if (m_albumSpacer)
            m_view->scrollTo(m_proxy->mapFromSource(m_albumSpacer->index()));
    }, Qt::QueuedConnection);
}

void LibraryView::closeAlbumPanel(bool forget)
{
    if (forget)
        m_openAlbum.clear();
    if (!m_albumSpacer)
        return;
    QStandardItem *spacer = m_albumSpacer;
    m_albumSpacer = nullptr;
    if (m_albumHost)
        m_albumHost->deleteLater();
    m_model->removeRow(spacer->row());
    m_albumAnchor = -1;
    m_albumWidth = 0;
    m_delegate->freeLayout = false;
    m_view->setUniformItemSizes(!m_delegate->musicQueue.on);
    updateGrid(); // the fixed grid size again
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
        if (m_loc.section == Section::Network)
            navigate({Section::Network, path});
        else
            navigate({Section::Music, {}, {}, path});
        break;
    case EntryKind::Group:
        if (m_delegate->grid && isAlbumTile(index)) { // its songs open under it (a second click closes them)
            if (m_openAlbum == path)
                closeAlbumPanel();
            else
                openAlbumPanel(path);
            break;
        }
        if (path.startsWith("a:")) // an album on an artist's page
            navigate({Section::Music, {}, {}, m_loc.music, m_loc.group, path});
        else
            navigate({Section::Music, {}, {}, m_loc.music, path});
        break;
    case EntryKind::Action:
        if (path == "create-playlist") {
            (new NewPlaylistOverlay({}, {}, window()))->open();
        } else if (path == "open-network" || path == "add-podcast") {
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
        if (m_loc.section == Section::Network && Library::isUrl(path)) {
            // Name the station or episode for the queue and the title bar.
            library().setStreamInfo(path, index.data(Qt::DisplayRole).toString(), index.data(ThumbRole).toString());
            if (m_loc.path.startsWith("m3u:")) { // channels: next and previous zap through the list
                const QStringList items = currentPlayable(&at, path);
                emit playRequested(items, at);
                break;
            }
        }
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
        (new NewPlaylistOverlay(items, suggestedName, window()))->open();
    });
}

Library::Pin LibraryView::pinFor(const QModelIndex &index) const
{
    const auto kind = EntryKind(index.data(KindRole).toInt());
    const QString path = index.data(PathRole).toString();
    const QString name = index.data(Qt::DisplayRole).toString();
    switch (kind) {
    case EntryKind::Folder: {
        if (path.startsWith("m3u:"))
            return {"channels", path.mid(4), name};
        if (path.startsWith("rss:"))
            return {"podcast", path.mid(4), name};
        Section s = m_loc.section;
        if (s != Section::Videos && s != Section::Music) {
            s = Section::Videos;
            for (const QString &root : prefs().list("library/musicFolders"))
                if (!root.isEmpty() && (path == root || path.startsWith(QDir::cleanPath(root) + "/")))
                    s = Section::Music;
        }
        return {"folder", path, name, int(s)};
    }
    case EntryKind::Playlist:
        return {"playlist", path, name};
    case EntryKind::Group:
        if (path.startsWith("a:"))
            return {"music", m_loc.music + "\n" + m_loc.group.mid(2) + "\n" + path.mid(2), name};
        return {"music", m_loc.music + "\n" + path.mid(2) + "\n", name};
    case EntryKind::Video:
    case EntryKind::Audio:
    case EntryKind::Stream:
        return {"file", path, name};
    case EntryKind::Category:
        if (m_loc.section == Section::Network)
            return {"network", path, name};
        return {"music", path + "\n\n", name};
    default:
        return {};
    }
}

Library::Pin LibraryView::pinForHere() const
{
    const QString title = m_title ? m_title->text() : QString();
    QString name;
    for (int i = m_crumbs->count() - 1; i >= 0 && name.isEmpty(); --i) // the last breadcrumb
        if (auto *l = qobject_cast<QLabel *>(m_crumbs->itemAt(i)->widget()); l && !l->text().isEmpty())
            name = l->text();
    if (name.isEmpty())
        name = title;
    switch (m_loc.section) {
    case Section::Videos:
    case Section::Music:
        if (m_loc.section == Section::Music && !m_loc.music.isEmpty() && m_loc.music != "folders")
            return {"music", m_loc.music + "\n" + m_loc.group.mid(2) + "\n" + m_loc.album.mid(2), name};
        if (!m_loc.path.isEmpty())
            return {"folder", m_loc.path, QFileInfo(m_loc.path).fileName(), int(m_loc.section)};
        return {};
    case Section::Playlists:
        return m_loc.playlist.isEmpty() ? Library::Pin{} : Library::Pin{"playlist", m_loc.playlist, m_loc.playlist};
    case Section::Network:
        if (m_loc.path.startsWith("m3u:"))
            return {"channels", m_loc.path.mid(4), name};
        if (m_loc.path.startsWith("rss:"))
            return {"podcast", m_loc.path.mid(4), name};
        if (!m_loc.path.isEmpty())
            return {"network", m_loc.path, name};
        return {};
    default:
        return {};
    }
}

void LibraryView::addPinAction(QMenu &menu, const QModelIndex &index)
{
    const Library::Pin pin = pinFor(index);
    if (pin.kind.isEmpty())
        return;
    if (library().isPinned(pin.kind, pin.target))
        menu.addAction(theme().icon("pin", theme().accent()), tr("Unpin from Sidebar"), this,
                       [pin] { library().unpin(pin.kind, pin.target); });
    else
        menu.addAction(theme().icon("pin", theme().accent()), tr("Pin to Sidebar"), this, [pin] { library().pin(pin); });
}

void LibraryView::openPin(const Library::Pin &pin)
{
    if (pin.kind == "folder") {
        navigate({Section(pin.section), pin.target, {}});
    } else if (pin.kind == "playlist") {
        navigate({Section::Playlists, {}, pin.target});
    } else if (pin.kind == "music") {
        const QStringList parts = pin.target.split('\n');
        const QString group = parts.value(1), album = parts.value(2);
        navigate({Section::Music, {}, {}, parts.value(0), group.isEmpty() && album.isEmpty() ? QString() : "g:" + group,
                  album.isEmpty() ? QString() : "a:" + album});
    } else if (pin.kind == "channels") {
        navigate({Section::Network, "m3u:" + pin.target});
    } else if (pin.kind == "podcast") {
        navigate({Section::Network, "rss:" + pin.target});
    } else if (pin.kind == "network") {
        navigate({Section::Network, pin.target});
    } else if (pin.kind == "file") {
        emit playRequested({pin.target}, 0);
    }
}

void LibraryView::showNetwork(const QString &path)
{
    navigate({Section::Network, path});
}

void LibraryView::showContextMenu(const QPoint &pos)
{
    // At the pointer, or under a tile's ⋯ button when that opened it.
    const QPoint menuAt = m_menuAt.isNull() ? m_view->viewport()->mapToGlobal(pos) : m_menuAt;
    const QModelIndex index = m_view->indexAt(pos);
    if (!index.isValid() || index.data(AnchorRole).isValid())
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
        addPlaylistMenu(menu, files);
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
        addPinAction(menu, index);
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
        if (album) {
            // Both write into the files, and say so before they do.
            QMenu *edit = menu.addMenu(theme().icon("edit", accent), tr("Edit"));
            edit->addAction(theme().icon("tune", accent), tr("Edit Metadata…"), this, [this, items] {
                (new AlbumEditorOverlay(items, window()))->open();
            });
            edit->addAction(theme().icon("save-down", accent), tr("Download Album Art…"), this, [this, items] {
                (new AlbumArtOverlay(items, window()))->open();
            });
        }
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
        addPinAction(menu, index);
        menu.addSeparator();
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

    if (kind == EntryKind::Folder && (path.startsWith("m3u:") || path.startsWith("rss:"))) {
        const QString url = path.mid(4);
        const bool podcast = path.startsWith("rss:");
        menu.addAction(theme().icon("play", accent), podcast ? tr("Play Episodes") : tr("Play Channels"), this,
                       [this, index] {
                           const QStringList items = filesFor(index);
                           if (items.isEmpty())
                               navigate({Section::Network, index.data(PathRole).toString()}); // load it first
                           else
                               emit playRequested(items, 0);
                       });
        addPinAction(menu, index);
        menu.addSeparator();
        if (podcast && !library().podcasts().contains(url))
            menu.addAction(theme().icon("plus", accent), tr("Subscribe"), this, [url, index] {
                library().addPodcast(url, index.data(Qt::DisplayRole).toString());
                library().setStreamInfo(url, index.data(Qt::DisplayRole).toString(), index.data(ThumbRole).toString());
            });
        else if (podcast)
            menu.addAction(theme().icon("trash", accent), tr("Unsubscribe"), this, [url] {
                library().removePodcast(url);
                library().unpin("podcast", url);
            });
        else
            menu.addAction(theme().icon("trash", accent), tr("Forget Channel List"), this, [url] {
                library().removeChannelList(url);
                library().unpin("channels", url);
            });
        menu.exec(menuAt);
        return;
    }

    if (kind == EntryKind::Folder) {
        addPinAction(menu, index);
        menu.addSeparator();
        menu.addAction(theme().icon("play", accent), tr("Play folder"), this, [this, path] {
            const QStringList items = library().mediaInFolder(path, filterForSection());
            if (!items.isEmpty())
                emit playRequested(items, 0);
        });
        menu.addAction(theme().icon("plus", accent), tr("Add folder to queue"), this,
                       [this, path] { emit enqueueRequested(library().mediaInFolder(path, filterForSection())); });
        menu.addSeparator();
        if (m_loc.section != Section::Music) {
            menu.addAction(theme().icon("subtitles", accent), tr("Edit Subtitle Tracks…"), this,
                           [this, path] { (new FolderTracksOverlay(path, true, window()))->open(); });
            menu.addAction(theme().icon("audio", accent), tr("Edit Audio Tracks…"), this,
                           [this, path] { (new FolderTracksOverlay(path, false, window()))->open(); });
        }
        menu.addAction(theme().icon("info", accent), tr("Info…"), this, [this, path] { emit infoRequested(path); });
        menu.addAction(theme().icon("external", accent), tr("Open in file manager"), this,
                       [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
        menu.exec(menuAt);
        return;
    }

    if (inQueue)
        menu.addAction(theme().icon("play", accent), tr("Play"), this, [this, index] { activate(index); });

    if (!inQueue) {
        menu.addAction(theme().icon("play-next", accent), tr("Play Next"), this,
                       [this, path] { emit playNextRequested({path}); });
        menu.addSeparator();
    }
    addPlaylistMenu(menu, {path});
    addPinAction(menu, index);
    if (Library::isUrl(path) && !library().streams().contains(path) && m_loc.section == Section::Network)
        menu.addAction(theme().icon("plus", accent), tr("Save to Network"), this, [path, index] {
            library().setStreamInfo(path, index.data(Qt::DisplayRole).toString(), index.data(ThumbRole).toString());
            library().addStream(path, index.data(Qt::DisplayRole).toString());
        });

    if (kind == EntryKind::Video) {
        const bool watched = library().progress(path) >= 0.95;
        menu.addAction(theme().icon(watched ? "eye-off" : "eye", accent),
                       watched ? tr("Mark as unwatched") : tr("Mark as watched"), this,
                       [path, watched] { library().setWatched(path, !watched); });
    }
    if (!Library::isUrl(path))
        menu.addAction(theme().icon("folder", accent), tr("Show in folder"), this, [path] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        });
    menu.addAction(theme().icon("info", accent), tr("Info…"), this, [this, path] { emit infoRequested(path); });

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
    if (m_detailsHeader) {
        static const QHash<int, Column> columnFor = {
            {SortName, Column::Name},         {SortArtist, Column::Artist},  {SortAlbum, Column::Album},
            {SortYear, Column::Year},         {SortDuration, Column::Duration}, {SortSize, Column::Size},
            {SortDate, Column::Modified},     {SortProgress, Column::Progress}};
        const bool music = m_loc.section == Section::Music && !m_loc.music.isEmpty() && m_loc.music != "folders";
        m_detailsHeader->hasSort = columnFor.contains(m_sortKey) || (music && m_sortKey == SortDefault);
        m_detailsHeader->sorted = columnFor.value(m_sortKey, Column::Name);
        m_detailsHeader->sortedDesc = m_sortDesc;
        m_detailsHeader->update();
    }
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
    const QVariant saved = prefs().get("sort/" + ctx);
    const QStringList parts = saved.toString().split(',');
    m_sortKey = parts.value(0).toInt();
    m_sortDesc = parts.value(1) == "1";
}

void LibraryView::saveSort()
{
    const QString ctx = sortContext(m_loc.section, m_loc.music, !m_loc.group.isEmpty(), !m_loc.album.isEmpty());
    const QString value = QString("%1,%2").arg(m_sortKey).arg(m_sortDesc ? 1 : 0);
    prefs().set("sort/" + ctx, value); // every list keeps its order between runs
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

    if (!music) {
        // Videos and other lists: one choice, order included (as in the video).
        struct Choice {
            QString label;
            int key;
            bool desc;
        };
        QList<Choice> choices = {{tr("Default Order"), SortDefault, false}};
        const bool media = m_loc.section != Section::Playlists || !m_loc.playlist.isEmpty();
        if (media && m_loc.section != Section::Network)
            choices << Choice{tr("Unwatched First"), SortUnwatched, false} << Choice{tr("Watched First"), SortWatched, false};
        if (media)
            choices << Choice{tr("Newest First"), SortDate, true} << Choice{tr("Oldest First"), SortDate, false};
        choices << Choice{tr("Title A–Z"), SortName, false} << Choice{tr("Title Z–A"), SortName, true};
        if (media && m_loc.section != Section::Network)
            choices << Choice{tr("Largest First"), SortSize, true} << Choice{tr("Longest First"), SortDuration, true};
        auto *group = new QActionGroup(&menu);
        for (const Choice &c : choices) {
            QAction *a = menu.addAction(c.label, this, [this, c] { setSort(c.key, c.desc); });
            a->setCheckable(true);
            a->setChecked(c.key == m_sortKey && (c.key == SortDefault || c.key == SortUnwatched ||
                                                 c.key == SortWatched || c.desc == m_sortDesc));
            group->addAction(a);
        }
        menu.exec(at);
        return;
    }

    QList<QPair<QString, int>> keys;
    if (m_loc.music == "songs") {
        keys = {{tr("Title"), SortDefault}, {tr("Artist"), SortArtist}, {tr("Album"), SortAlbum},
                {tr("Year"), SortYear}, {tr("Length"), SortDuration}, {tr("Date modified"), SortDate}};
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

// ⋯ → Add Files: into the open playlist, or copied into the open folder.
void LibraryView::addFilesHere()
{
    if (m_loc.section == Section::Playlists && !m_loc.playlist.isEmpty()) {
        const QStringList files = FileDialogs::openFiles(this, tr("Add to playlist"), QDir::homePath(),
                                                         tr("Media files (*.mp4 *.mkv *.avi *.mov *.webm *.mp3 *.flac *.ogg *.opus *.m4a *.wav);;All files (*)"));
        if (!files.isEmpty())
            library().addToPlaylist(m_loc.playlist, files);
        return;
    }
    const QString dir = m_loc.path;
    if (dir.isEmpty())
        return;
    const QStringList files = FileDialogs::openFiles(this, tr("Copy into “%1”").arg(QFileInfo(dir).fileName()),
                                                     QDir::homePath(), tr("All files (*)"));
    if (files.isEmpty())
        return;
    // Copies in the background (videos can be big); names already there are kept.
    m_count->setText(tr("Copying %1…").arg(files.size() == 1 ? QFileInfo(files.first()).fileName()
                                                             : tr("%1 files").arg(files.size())));
    QPointer<LibraryView> self(this);
    QThread *worker = QThread::create([files, dir, self] {
        int copied = 0, skipped = 0;
        for (const QString &f : files) {
            const QString target = dir + "/" + QFileInfo(f).fileName();
            if (QFileInfo::exists(target) || !QFile::copy(f, target))
                ++skipped;
            else
                ++copied;
        }
        QMetaObject::invokeMethod(qApp, [self, copied, skipped] {
            if (!self)
                return;
            self->reload();
            QString text = copied == 1 ? tr("Copied 1 file") : tr("Copied %1 files").arg(copied);
            if (skipped)
                text += tr(" · %1 skipped (already there or unreadable)").arg(skipped);
            self->m_count->setText(text);
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

// ⋯ → Resync: thumbnails, tags and online lists fetched again.
void LibraryView::resync()
{
    if (m_loc.section == Section::Music)
        musicIndex().rescan();
    if (m_loc.section == Section::Network && !networkFeedKey().isEmpty())
        NetFeeds::instance().refresh(networkFeedKey());
    for (int i = 0; i < m_model->rowCount(); ++i) {
        const QString path = m_model->item(i)->data(PathRole).toString();
        if (path.startsWith('/'))
            Thumbnailer::instance().forget(path);
    }
    reload();
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

    const bool folder = (m_loc.section == Section::Videos || m_loc.section == Section::Music) && !m_loc.path.isEmpty();
    const bool playlist = m_loc.section == Section::Playlists && !m_loc.playlist.isEmpty();
    const bool music = m_loc.section == Section::Music && !m_loc.music.isEmpty() && m_loc.music != "folders";
    // Adding: files into this folder or playlist, a folder to the library.
    if (folder || playlist)
        menu.addAction(theme().icon("open-media", accent), folder ? tr("Add Files to This Folder…") : tr("Add Files…"),
                       this, &LibraryView::addFilesHere);
    if (playlist)
        menu.addAction(theme().icon("folder-plus", accent), tr("Add Folder…"), this, [this] {
            const QString dir = FileDialogs::existingDirectory(this, tr("Add folder to playlist"));
            if (!dir.isEmpty())
                library().addToPlaylist(m_loc.playlist, library().mediaInFolder(dir));
        });
    else if (m_loc.section == Section::Videos || m_loc.section == Section::Music || m_loc.section == Section::Home)
        menu.addAction(theme().icon("folder-plus", accent), tr("Add Library Folder…"), this, &LibraryView::addFolderRequested);
    if (m_loc.section == Section::Network)
        menu.addAction(theme().icon("globe", accent), tr("Open Network Stream…"), this, &LibraryView::openNetworkRequested);
    if (m_loc.section == Section::Network && m_loc.path.startsWith("rss:")) {
        const QString url = m_loc.path.mid(4);
        if (library().podcasts().contains(url))
            menu.addAction(theme().icon("trash", accent), tr("Unsubscribe"), this, [this, url] {
                library().removePodcast(url);
                library().unpin("podcast", url);
                navigate({Section::Network, "podcasts"});
            });
        else
            menu.addAction(theme().icon("plus", accent), tr("Subscribe"), this, [url] {
                const NetFeeds::Feed f = NetFeeds::instance().feed("rss:" + url);
                library().addPodcast(url, f.title);
                library().setStreamInfo(url, f.title, f.image);
            });
    }

    // The folder's own subtitle and audio settings (shows, mostly).
    if (folder && m_loc.section == Section::Videos) {
        menu.addSeparator();
        const QString dir = m_loc.path;
        menu.addAction(theme().icon("subtitles", accent), tr("Edit Subtitle Tracks…"), this,
                       [this, dir] { (new FolderTracksOverlay(dir, true, window()))->open(); });
        menu.addAction(theme().icon("audio", accent), tr("Edit Audio Tracks…"), this,
                       [this, dir] { (new FolderTracksOverlay(dir, false, window()))->open(); });
    }

    menu.addSeparator();
    const Library::Pin here = pinForHere();
    if (!here.kind.isEmpty()) {
        if (library().isPinned(here.kind, here.target))
            menu.addAction(theme().icon("pin", accent), tr("Unpin from Sidebar"), this,
                           [here] { library().unpin(here.kind, here.target); });
        else
            menu.addAction(theme().icon("pin", accent), tr("Pin to Sidebar"), this, [here] { library().pin(here); });
    }
    menu.addAction(theme().icon("refresh", accent), tr("Resync"), this, &LibraryView::resync)
        ->setToolTip(tr("Look at the files again: new and removed media, thumbnails, tags"));
    if (folder && !music)
        menu.addAction(theme().icon("info", accent), tr("Info…"), this,
                       [this, dir = m_loc.path] { emit infoRequested(dir); });
    if (folder) {
        const QString dir = m_loc.path;
        menu.addAction(theme().icon("external", accent), tr("Open in File Manager"), this,
                       [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
    }
    menu.exec(m_moreBtn->mapToGlobal(QPoint(0, m_moreBtn->height() + 4)));
}
