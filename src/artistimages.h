#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <functional>

class QNetworkAccessManager;

// Pictures of artists for Music → Artists ("Artist image"). Uses an
// artist.jpg (or .png/.webp) in the artist's folder if there is one, otherwise
// looks the name up on Deezer's public catalogue and caches the picture.
// Nothing is looked up unless the user picks "Artist image".
class ArtistImages : public QObject {
    Q_OBJECT
public:
    static ArtistImages &instance();

    // Image file for the artist, or empty: either there's none, or it's being
    // looked up and ready() follows. trackPaths: some of the artist's songs,
    // to find their folder.
    QString image(const QString &name, const QStringList &trackPaths);

    // Choosing a picture by hand (Artists → right-click → Choose Artist Image).
    struct Candidate {
        QString name, thumb, picture; // thumb: small image URL, picture: large
        int fans = 0;
    };
    void search(const QString &query, std::function<void(const QList<Candidate> &, const QString &error)> done);
    // Fetches a URL (or reads a file) and keeps it as the artist's picture,
    // ahead of artist.jpg and the automatic one.
    void choose(const QString &name, const QString &urlOrFile, std::function<void(const QString &error)> done);
    void resetChoice(const QString &name); // back to automatic
    bool hasChoice(const QString &name) const;
    QNetworkAccessManager *network();

signals:
    void ready(const QString &name, const QString &file);

private:
    ArtistImages();
    QString cacheBase(const QString &name) const;
    void store(const QString &name, const QByteArray &data, std::function<void(const QString &)> done);
    void next();
    void done(const QString &name, const QByteArray &data);

    QNetworkAccessManager *m_net = nullptr;
    QQueue<QString> m_queue;
    QSet<QString> m_queued;
    QHash<QString, QString> m_known; // name -> file ("" = none)
    bool m_busy = false;
    QTimer m_pace;
};
