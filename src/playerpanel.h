#pragma once

#include <QHash>
#include <QList>
#include <QWidget>

class IconButton;
class LibraryView;
class Player;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class TabButton;

// Saved moments in media files ("Bookmarks" tab), kept per file in
// AppData/bookmarks.json.
class Bookmarks {
public:
    struct Mark {
        QString name;
        qint64 ms = 0;
    };
    static Bookmarks &instance();
    QList<Mark> marks(const QString &location) const; // sorted by time
    void set(const QString &location, QList<Mark> marks);

private:
    Bookmarks();
    void save() const;
    QHash<QString, QList<Mark>> m_marks;
};

// The panel on the right of the player: the play queue, the current media's
// bookmarks and its chapters, one tab each.
class PlayerPanel : public QWidget {
    Q_OBJECT
public:
    enum Tab { QueueTab, BookmarksTab, ChaptersTab };

    PlayerPanel(Player *player, QWidget *parent = nullptr);
    LibraryView *queue() const { return m_queue; }
    Tab tab() const { return m_tab; }
    void setTab(Tab tab);
    void addBookmark(); // at the current time

signals:
    void tabChanged(PlayerPanel::Tab tab);

protected:
    void showEvent(QShowEvent *) override;

private:
    QWidget *buildBookmarks();
    QWidget *buildChapters();
    void refreshBookmarks();
    void refreshChapters();
    void removeBookmark();
    void renameBookmark(QListWidgetItem *item);
    void storeBookmarks();

    Player *m_player;
    Tab m_tab = QueueTab;
    QList<TabButton *> m_tabs;
    QStackedWidget *m_stack;
    LibraryView *m_queue;

    QListWidget *m_marks;
    QLabel *m_markCount, *m_markEmpty;
    IconButton *m_markAdd, *m_markRemove;
    QString m_markLocation; // whose bookmarks m_marks shows

    QListWidget *m_chapters;
    QLabel *m_chapterCount, *m_chapterEmpty;
};
