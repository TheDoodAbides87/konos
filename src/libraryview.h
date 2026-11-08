#pragma once

#include "library.h"

#include <QHash>
#include <QStyledItemDelegate>
#include <QWidget>

class IconButton;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListView;
class QSortFilterProxyModel;
class QStandardItem;
class QStandardItemModel;
class QModelIndex;

enum class Section { Home, Videos, Music, Network, Playlists, Recent, Queue };

QString sectionName(Section s);
QString sectionIcon(Section s);

// Columns of the details view.
enum class Column { Name, Artist, Album, Year, Duration, Size, Modified, Progress, Songs, Detail };

// Paints grid tiles (thumbnail, progress bar, name), list rows and details rows.
class EntryDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit EntryDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    bool grid = true;
    QList<Column> columns; // non-empty: the details view (a list with columns)
    // Where each column sits in a row of this width (the header uses the same).
    QList<QPair<Column, QRectF>> columnRects(const QRectF &row) const;
    static QString columnTitle(Column c);
    QSize tileSize = {300, 214};
    bool square = false; // Music: tiles shaped like album art, with a detail line
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    // Albums, artists, genres and playlists show a play and a ⋯ button on hover.
    enum TileButton { NoButton, PlayButton, MoreButton };
    static bool hasTileButtons(const QModelIndex &index);
    QRectF tileButtonRect(const QRect &itemRect, TileButton b) const;
    TileButton tileButtonAt(const QRect &itemRect, const QModelIndex &index, const QPoint &pos) const;
    QPoint mouse{-1, -1}; // in viewport coordinates, for the buttons' hover look
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    // A play queue of music is laid out like a music app's: Recently Played
    // (can be folded away), Now Playing and Playing Next, each with a title,
    // and two-line rows (title; artist – album). Set by the queue tab.
    struct MusicQueue {
        bool on = false;
        int current = -1;       // queue position playing (-1: none)
        int total = 0;
        bool recentFolded = false;
        QString nextAlbum;      // "From …" when everything next is one album
        QString nextCover;      // a track of that album, for its cover
    };
    MusicQueue musicQueue;
    // The section titles drawn above a queue row (0 if none).
    int queueHeaderHeight(const QModelIndex &index) const;
    // Whether pos (viewport) is on the Recently Played title above this row.
    bool onRecentTitle(const QRect &itemRect, const QModelIndex &index, const QPoint &pos) const;

private:
    void paintQueueRow(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const;
    void paintDetails(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const;
    QPixmap thumbnail(const QModelIndex &index) const;
};

// Media library browser: breadcrumbs, sort/view/search, grid or list.
// Used full-width (library mode) and as the compact side panel in player mode.
class LibraryView : public QWidget {
    Q_OBJECT
public:
    enum Mode { Full, Compact, QueueTab }; // QueueTab: the play queue inside the right-hand panel

    explicit LibraryView(Mode mode, QWidget *parent = nullptr);

    Section section() const { return m_loc.section; }
    void showSection(Section section);
    void showFolder(Section section, const QString &path);
    // Music → view ("songs", "albums", "artists", "genres", "folders"), optionally one group in it.
    void showMusic(const QString &view, const QString &groupKey = QString(), const QString &albumKey = QString());
    void setQueue(const QStringList &items, int current);
    void setNowPlaying(const QString &location);
    void refresh();
    void openPin(const Library::Pin &pin);
    // Dev hooks and tests: the view mode (0 grid, 1 list, 2 details) and sort.
    void setViewMode(int mode);
    void setSort(int key, bool desc);
    void showNetwork(const QString &path); // "", "radio", "podcasts", "m3u:<url>", "rss:<url>"

signals:
    void playRequested(const QStringList &items, int index);
    void enqueueRequested(const QStringList &items);
    void enqueueAndPlayRequested(const QStringList &items); // added to the end of the queue, playing the first
    void playNextRequested(const QStringList &items);       // inserted after what's playing
    void queueJumpRequested(int index);
    void queueRemoveRequested(int index);
    void queueRemoveRowsRequested(QList<int> rows); // several at once (queue positions)
    void queueClearRequested();
    void openNetworkRequested();
    void infoRequested(const QString &location);
    void addFolderRequested();
    void popOutRequested();
    void addFilesRequested();                                // queue tab's +
    void queueMoveRequested(QList<int> rows, int at);       // dragged within the queue; at = position before the drag
    void queueInsertRequested(const QStringList &items, int at); // dropped on the queue (-1: at the end)
    void sectionShown(Section section);

protected:
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    struct Location {
        Section section = Section::Home;
        QString path;     // folder within the section root
        QString playlist; // open playlist name
        QString music;    // Music view: "songs", "albums", "artists", "genres" or "folders"
        QString group;    // open album/artist/genre: "g:" + its key
        QString album;    // an album opened from an artist: "a:" + its key
        Location(Section s = Section::Home, const QString &p = {}, const QString &pl = {}, const QString &m = {},
                 const QString &g = {}, const QString &a = {})
            : section(s), path(p), playlist(pl), music(m), group(g), album(a)
        {
        }
        bool operator==(const Location &o) const
        {
            return section == o.section && path == o.path && playlist == o.playlist && music == o.music &&
                   group == o.group && album == o.album;
        }
    };

    void navigate(const Location &target, bool record = true);
    void reload();
    void rebuildHeader();
    void updateGrid();
    void activate(const QModelIndex &index);
    void showContextMenu(const QPoint &pos);
    void addPlaylistMenu(class QMenu &menu, const QStringList &items, const QString &suggestedName = {});
    bool tileButtonEvent(class QEvent *event);
    void showMoreMenu();
    void showSortMenu();
    void applySort();
    void loadSort();
    void saveSort();
    void setGridView(bool grid, bool remember = true);
    void applyViewMode(int mode, bool remember = true); // 0 grid, 1 list, 2 details
    int viewMode() const;
    QList<Column> detailColumns() const;
    void addPinAction(class QMenu &menu, const QModelIndex &index);
    Library::Pin pinFor(const QModelIndex &index) const;
    Library::Pin pinForHere() const;
    void reloadNetwork(int &order, QString &empty);
    QString networkFeedKey() const; // what the open Network view lists
    void addFilesHere();
    void resync();
    QStringList roots() const;
    QStringList currentPlayable(int *indexOfPath = nullptr, const QString &path = QString()) const;
    int filterForSection() const;
    QStandardItem *addEntry(const LibraryEntry &e, int order);
    void reloadMusic(int &order, QString &empty);
    QStringList tracksFor(const QString &entryPath) const; // songs behind a group entry ("g:…"/"a:…")
    QStringList filesFor(const QModelIndex &idx) const;     // the media an entry stands for (folder, album, playlist…)
    QModelIndexList selectedRows() const;                  // selected entries, in list order
    void playSelection(bool replace);                       // replace the queue with, or add, the selection
    bool artistImages() const;
    void updateQueueCount(); // "3 items · 48m"
    bool isMusicQueue() const;
    bool queueDragEvent(QEvent *event);

    Mode m_mode;
    Location m_loc;
    QList<Location> m_back, m_forward;
    QHash<QObject *, Location> m_crumbTargets;
    QHash<int, int> m_viewForSection; // 0 grid, 1 list, 2 details
    int m_sortKey = 0; // SortKey in libraryview.cpp
    bool m_sortDesc = false;
    bool m_eatRelease = false; // a press on a tile button was handled; keep its release from the list
    QPoint m_menuAt;           // global position for the next context menu (a tile's ⋯ button)
    QHash<QString, QString> m_sortFor; // non-music lists: "key,desc" per section

    QStringList m_queue;
    int m_queueCurrent = -1;
    QString m_nowPlaying;

    QStandardItemModel *m_model;
    QSortFilterProxyModel *m_proxy;
    QListView *m_view;
    EntryDelegate *m_delegate;
    QLabel *m_empty;
    class DetailsHeader *m_detailsHeader = nullptr;
    QString m_remoteQuery;    // Radio / Podcasts: searching online for this
    class QTimer *m_searchTimer = nullptr;
    class HomePage *m_home = nullptr; // full mode's Home

    QWidget *m_header;
    QHBoxLayout *m_crumbs;
    IconButton *m_backBtn, *m_fwdBtn, *m_sortBtn, *m_viewBtn, *m_moreBtn;
    IconButton *m_searchBtn = nullptr, *m_popBtn = nullptr;
    IconButton *m_repeatBtn = nullptr, *m_shuffleBtn = nullptr, *m_removeBtn = nullptr, *m_addBtn = nullptr;
    QWidget *m_dropLine = nullptr; // where a drop on the queue would land
    QLabel *m_count;
    class Dropdown *m_artistArt = nullptr; // Artists: "Artist image" / "Album cover"
    QLabel *m_title = nullptr;
    QLineEdit *m_search;
};
