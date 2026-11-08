#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QStringList>
#include <QTimer>

struct libvlc_instance_t;
struct libvlc_media_t;

// Tags (title, artist, album, genre, cover) of everything in the music folders,
// read with libVLC in the background and kept in music.json so only new or
// changed files are read again. Powers Music → Songs / Albums / Artists / Genres.
class MusicIndex : public QObject {
    Q_OBJECT
public:
    struct Track {
        QString path, title, artist, albumArtist, album, genre, art;
        // No album artist tag: who the album's other songs say it is, else
        // its main artist without "feat. …" (worked out, not saved).
        QString impliedAlbumArtist;
        int number = 0;
        int year = 0;        // release year, 0 if unknown
        qint64 duration = 0; // ms
        qint64 mtime = 0;    // file's modification time when read
        QString displayTitle() const;
        QString displayArtist() const;
        QString displayAlbum() const;
        QString displayGenre() const;
        QString displayAlbumArtist() const; // album artist, else the implied one, else the artist
        QString label() const; // "title - artist", as in the list
    };
    enum Field { Album, Artist, Genre, AlbumArtist };
    struct Group {
        QString key;  // identifies the group (albums: name + album artist)
        QString name;
        QString cover; // a track whose cover stands for the group
        QString artist; // albums: the album artist, or "Various Artists"
        int count = 0;
        int year = 0; // albums: earliest year among their tracks
    };

    static MusicIndex &instance();

    // Loads the saved index and checks the music folders for changes (once).
    void ensureScanned();
    void rescan();
    bool isScanning() const { return m_scanning || m_inFlight > 0 || !m_queue.isEmpty(); }

    QList<Track> tracks() const; // title order
    // Optionally only what's inside another group (e.g. one artist's albums).
    struct Within {
        Field field = Album;
        QString key;
        bool active = false; // false: everything
    };
    QList<Group> groups(Field field, const Within &within) const; // name order, "Unknown …" last
    QList<Group> groups(Field field) const { return groups(field, Within()); }
    QList<Track> tracksIn(Field field, const QString &key, const Within &within) const;
    QList<Track> tracksIn(Field field, const QString &key) const { return tracksIn(field, key, Within()); }
    QString groupName(Field field, const QString &key) const;
    // Up to n song paths per group, in one pass (for finding artist folders).
    QHash<QString, QStringList> samplePaths(Field field, int n) const;
    const Track *track(const QString &path) const;
    static QString fieldName(Field field);

signals:
    void changed();

private slots:
    void onParsedQueued(void *media); // libVLC's "parsed" event, moved to the UI thread

private:
    MusicIndex();
    void load();
    void save() const;
    void scanned(const QHash<QString, qint64> &files);
    void pump();
    void parsed(libvlc_media_t *media);
    static QString groupKey(const Track &t, Field field);
    void resolveAlbumArtists(); // fills in impliedAlbumArtist

    libvlc_instance_t *m_vlc = nullptr;
    QHash<QString, Track> m_tracks;
    QQueue<QPair<QString, qint64>> m_queue;
    QHash<libvlc_media_t *, QPair<QString, qint64>> m_parsing;
    int m_inFlight = 0;
    bool m_loaded = false, m_scanning = false, m_rescanAgain = false;
    QTimer m_notify, m_save;
};

inline MusicIndex &musicIndex() { return MusicIndex::instance(); }
