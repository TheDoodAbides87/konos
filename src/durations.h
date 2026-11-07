#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>

struct libvlc_instance_t;
struct libvlc_media_t;

// How long media files are (for "3 items · 48m" in the queue). Known lengths
// come from the music index or playback; others are read with libVLC in the
// background and kept in AppData/durations.json.
class Durations : public QObject {
    Q_OBJECT
public:
    static Durations &instance();
    // Length in ms, or -1 while unknown (ready() follows once it's read).
    qint64 duration(const QString &location);

signals:
    void ready(const QString &location);

private slots:
    void onParsedQueued(void *media);

private:
    Durations();
    void pump();
    void save();
    QString key(const QString &location) const;

    libvlc_instance_t *m_vlc = nullptr;
    QHash<QString, qint64> m_known; // key (path + mtime) -> ms; 0 = couldn't tell
    QQueue<QString> m_queue;
    QSet<QString> m_queued;
    QHash<libvlc_media_t *, QString> m_parsing;
    QTimer m_saveTimer;
};
