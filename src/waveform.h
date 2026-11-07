#pragma once

#include <QCache>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QVector>

struct libvlc_instance_t;
struct libvlc_media_player_t;

// Loudness outline of an audio (or video) file for the seek bar and the
// subtitle timeline. libVLC decodes the file
// to 8 kHz mono as fast as it can (about a second per ten minutes of audio);
// the result is cached on disk so each file is only decoded once.
class Waveform : public QObject {
    Q_OBJECT
public:
    static Waveform &instance();
    static bool supports(const QString &location); // local audio and video files

    // Peaks (0..1, kBins of them) if known; otherwise empty, and ready() follows.
    QVector<float> peaks(const QString &path);
    // The same at kDetailRate peaks per second (for the subtitle timeline);
    // empty until peaks() has finished for this path.
    QVector<float> detail(const QString &path);

    static constexpr int kBins = 1000;
    static constexpr int kDetailRate = 20;

    // Decoder side, filled from libVLC's thread.
    struct Job;

signals:
    void ready(const QString &path);

private slots:
    void jobEnded(); // libVLC's end/error event, moved to the UI thread

private:
    Waveform();
    QString cacheFile(const QString &path, const char *suffix = ".bin") const;
    void start();
    void finish(bool ok);

    libvlc_instance_t *m_vlc = nullptr;
    libvlc_media_player_t *m_mp = nullptr;
    Job *m_job = nullptr;
    QQueue<QString> m_queue;
    QCache<QString, QVector<float>> m_memory;
    QString m_detailPath; // the one detailed outline kept in memory
    QVector<float> m_detail;
    QTimer m_timeout;
};
