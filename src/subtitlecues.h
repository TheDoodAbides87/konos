#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>

class QProcess;

// When each line of a subtitle track is on screen, for the subtitle timeline.
// Subtitle files (.srt, .vtt, .ass/.ssa) are read directly; text tracks inside
// a video are pulled out with ffmpeg (when installed) and cached on disk.
class SubtitleCues : public QObject {
    Q_OBJECT
public:
    struct Cue {
        qint64 start, end; // ms
        QString text;      // one line, formatting removed
    };
    enum State { Unknown, Loading, Ready, Failed };

    static SubtitleCues &instance();

    // Keys: a subtitle file's path, or embeddedKey() for a track inside a video.
    static QString embeddedKey(const QString &video, int index); // index among the video's subtitle streams
    State state(const QString &key);  // starts loading when Unknown
    QList<Cue> cues(const QString &key) const { return m_cues.value(key); }
    QString failure(const QString &key) const { return m_failures.value(key); }

    static QList<Cue> parse(const QByteArray &data); // SRT, WebVTT or SSA/ASS
    // A plain SRT copy of a Ready track (no fonts, colours or positions),
    // written to the cache; empty if the track isn't ready.
    QString plainFile(const QString &key);

signals:
    void ready(const QString &key); // loaded or failed

private:
    SubtitleCues() = default;
    void loadFile(const QString &key);
    void extract(const QString &key);
    QString cacheFile(const QString &video, int index) const;

    QHash<QString, QList<Cue>> m_cues;
    QHash<QString, QString> m_failures;
    QSet<QString> m_loading;
};
