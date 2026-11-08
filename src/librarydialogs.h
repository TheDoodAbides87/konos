#pragma once

#include "musiceditors.h" // PanelOverlay

#include <functional>

class Dropdown;
class QPlainTextEdit;
class QTreeWidget;

// "New Playlist": a name and the files in it, dragged in or picked.
class NewPlaylistOverlay : public PanelOverlay {
    Q_OBJECT
public:
    NewPlaylistOverlay(const QStringList &items, const QString &suggestedName, QWidget *window);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void addItems(const QStringList &items);
    void updateCount();
    void create();

    QLineEdit *m_name;
    QListWidget *m_list;
    QLabel *m_count;
    QPushButton *m_create;
};

// "Open Network Stream": one or more addresses, optionally saved together as
// a playlist. A link to an M3U channel list or a podcast feed opens it under
// Network instead of playing it.
class NetworkStreamOverlay : public PanelOverlay {
    Q_OBJECT
public:
    using OpenFn = std::function<void(const QStringList &urls, bool play)>;
    using BrowseFn = std::function<void(const QString &kind, const QString &url)>; // "channels" / "podcast"
    NetworkStreamOverlay(QWidget *window, OpenFn open, BrowseFn browse);

private:
    void go(bool play);

    QPlainTextEdit *m_urls;
    QLineEdit *m_playlist;
    QLabel *m_status;
    QPushButton *m_play, *m_queue;
    OpenFn m_open;
    BrowseFn m_browse;
};

// "Info": what's known about a file, stream or folder.
class MediaInfoOverlay : public PanelOverlay {
    Q_OBJECT
public:
    MediaInfoOverlay(const QString &location, QWidget *window);

private:
    void addRow(const QString &label, const QString &value);
    class QGridLayout *m_grid;
    int m_rows = 0;
};

// Library ⋯ → "Edit Subtitle Tracks" / "Edit Audio Tracks": the folder's
// default track, and a subtitle or audio file attached to each video (found
// by episode number, or picked by hand).
class FolderTracksOverlay : public PanelOverlay {
    Q_OBJECT
public:
    FolderTracksOverlay(const QString &folder, bool subtitles, QWidget *window);

private:
    void findAutomatically(const QString &searchIn);
    void chooseFor(class QTreeWidgetItem *item);
    void setFile(QTreeWidgetItem *item, const QString &file);
    void save();
    bool candidate(const QString &file) const;
    bool autodetected(const QString &video, const QString &file) const; // VLC finds it on its own

    QString m_folder;
    bool m_subtitles;
    Dropdown *m_rule;
    QTreeWidget *m_table;
    QLabel *m_status;
};
