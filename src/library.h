#pragma once

#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QTimer>

// Category: a Music view (Songs, Albums…); Group: an album, artist or genre.
enum class EntryKind { Folder, Video, Audio, Playlist, Stream, Action, Category, Group };

struct LibraryEntry {
    EntryKind kind = EntryKind::Video;
    QString path;     // file path, folder path, URL, playlist name or action id
    QString name;     // display name
    qint64 size = 0;
    QDateTime modified;
};

// Persistent library state: recents, resume positions, playlists, streams.
class Library : public QObject {
    Q_OBJECT
public:
    static Library &instance();

    static bool isVideoFile(const QString &path);
    static bool isAudioFile(const QString &path);
    static bool isPlaylistFile(const QString &path);
    static bool isSubtitleFile(const QString &path);
    static bool isMedia(const QString &path) { return isVideoFile(path) || isAudioFile(path); }
    static bool isUrl(const QString &location);
    static QString displayName(const QString &location);
    static EntryKind kindFor(const QString &location);

    // Lists sub-folders and media in dir. filter: 0 = video, 1 = audio, 2 = any.
    QList<LibraryEntry> listFolder(const QString &dir, int filter) const;
    // All playable files in a folder (non-recursive), sorted by name.
    QStringList mediaInFolder(const QString &dir, int filter = 2) const;
    // Expands folders and playlist files into playable items.
    QStringList expand(const QStringList &locations) const;

    QStringList recent() const { return m_recent; }
    void addRecent(const QString &location);
    void removeRecent(const QString &location);
    void clearRecent();

    // Resume position (ms) and progress ratio for a location.
    qint64 resumeTime(const QString &location) const;
    double progress(const QString &location) const;
    void setPosition(const QString &location, qint64 timeMs, qint64 lengthMs);
    void setWatched(const QString &location, bool watched);
    void clearPositions();

    QStringList playlistNames() const;
    QStringList playlist(const QString &name) const { return m_playlists.value(name); }
    QString createPlaylist(const QString &name);
    void renamePlaylist(const QString &from, const QString &to);
    void deletePlaylist(const QString &name);
    void addToPlaylist(const QString &name, const QStringList &items);
    void removeFromPlaylist(const QString &name, int index);

    // Title, artist and cover remembered from playback (for the home page).
    struct Meta {
        QString title, artist, art;
    };
    Meta meta(const QString &location) const { return m_meta.value(location); }
    void setMeta(const QString &location, const Meta &meta);

    // Folders ranked by how often media in them is played or they're opened.
    void visitFolder(const QString &dir);
    QStringList frequentFolders(int count) const;

    QStringList streams() const { return m_streams; }
    void addStream(const QString &url);
    void removeStream(const QString &url);

signals:
    void recentChanged();
    void positionsChanged();
    void playlistsChanged();
    void streamsChanged();

private:
    Library();
    void load();
    void scheduleSave();
    void save() const;

    QStringList m_recent;
    QHash<QString, QPair<qint64, qint64>> m_positions; // location -> (time, length)
    QHash<QString, QStringList> m_playlists;
    QStringList m_playlistOrder;
    QStringList m_streams;
    QHash<QString, Meta> m_meta;
    QHash<QString, int> m_folderVisits;
    QTimer m_saveTimer;
};

inline Library &library() { return Library::instance(); }

// Generates and caches thumbnails in the background.
class Thumbnailer : public QObject {
    Q_OBJECT
public:
    static Thumbnailer &instance();

    // Returns a cached thumbnail, or a null pixmap and queues generation.
    QPixmap get(const QString &path);
    // Look again next time (e.g. a cover turned up for this file).
    void forget(const QString &path);
    void clearCache();

signals:
    void ready(const QString &path);

private:
    Thumbnailer();
    QString cacheFile(const QString &path) const;
    void pump();
    void finished(const QString &path, const QString &out, bool ok);

    QCache<QString, QPixmap> m_memory;
    QQueue<QString> m_queue;
    QSet<QString> m_pending;
    QSet<QString> m_failed;
    int m_running = 0;
    QString m_tool; // "ffmpegthumbnailer", "ffmpeg" or empty
};
