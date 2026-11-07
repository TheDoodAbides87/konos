#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QTimer>
#include <QUrlQuery>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

// Client for the OpenSubtitles.com REST API (the opensubtitles.org XML-RPC API
// is closed to third-party apps). Every request needs an API key: Settings →
// Subtitles, or one baked in at build time (OPENSUBTITLES_API_KEY). Signing in
// is optional and raises the daily download allowance.
class OpenSubtitles : public QObject {
    Q_OBJECT
public:
    struct Query {
        QString path;     // local video, for the movie hash
        QString text;     // title to search for
        QString language; // "en", "pt-br"…
        int season = -1;
        int episode = -1;
        int year = -1;
    };
    struct Result {
        qint64 fileId = 0;
        QString fileName;
        QString release;
        QString language;
        QString title; // "The Wand S01E03 - Pilot"
        int downloads = 0;
        int season = -1, episode = -1;
        bool hashMatch = false;
        bool hearingImpaired = false;
    };
    using SearchDone = std::function<void(const QList<Result> &results, const QString &error)>;
    // remaining: downloads left today (-1 unknown).
    using DownloadDone = std::function<void(const QByteArray &data, const QString &fileName, int remaining,
                                            const QString &error)>;

    static OpenSubtitles &instance();

    QString apiKey() const;
    bool hasKey() const { return !apiKey().isEmpty(); }
    bool signedIn() const;
    QString user() const;

    void search(const Query &query, SearchDone done);
    void download(qint64 fileId, DownloadDone done);
    void signIn(const QString &user, const QString &password, std::function<void(const QString &error)> done);
    void signOut();

    // OSDb hash: file size plus the 64-bit little-endian sums of the first and
    // last 64 KiB, as 16 hex digits. Empty for files under 128 KiB.
    static QString movieHash(const QString &path);
    // Title, season, episode and year guessed from a file name
    // ("The_Wand_S01E03.1080p.mkv" → "The Wand", 1, 3).
    static Query guess(const QString &path);

signals:
    void accountChanged();

private:
    OpenSubtitles();
    QNetworkRequest request(const QString &path, const QUrlQuery &query = {}) const;
    // Requests go out at most four a second (the API allows five).
    void enqueue(std::function<void()> send);
    QString errorFor(QNetworkReply *reply, const QByteArray &body) const;

    QNetworkAccessManager *m_net;
    QQueue<std::function<void()>> m_queue;
    QTimer m_pace;
};
