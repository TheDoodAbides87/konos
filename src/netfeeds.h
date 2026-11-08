#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;

// The online lists under Network, fetched when they're opened:
//   "m3u:<url>"            an M3U channel list (IPTV, radio bundles)
//   "rss:<url>"            a podcast feed and its episodes
//   "radio:top"            popular stations from radio-browser.info
//   "radio:search:<text>"  stations matching a name
//   "podcasts:search:<text>"  podcasts matching a name (Apple's public search)
// Only these requests are sent; nothing about the user or their media.
class NetFeeds : public QObject {
    Q_OBJECT
public:
    struct Item {
        QString title;
        QString url;    // stream or episode; for podcast search results, the feed
        QString detail; // "Jazz · Germany", "12 Mar 2026 · 54m"
        QString image;
        QDateTime date;
    };
    struct Feed {
        QString title, image, description;
        QList<Item> items;
        QString error;
        bool loading = false;
        bool loaded = false;
    };

    static NetFeeds &instance();

    // What's known about a list. Starts fetching it when it hasn't been
    // (or was fetched over an hour ago); ready(key) follows.
    Feed feed(const QString &key);
    void refresh(const QString &key);

    // Pictures for stations, channels and podcasts: null until fetched,
    // then imageReady(url).
    QPixmap image(const QString &url);

    // What a link is, from its first bytes: "channels" (an M3U list of
    // streams), "podcast" (an RSS feed), or "stream" (anything else, played as is).
    void probe(const QString &url, std::function<void(const QString &kind, const QString &title)> done);

    static bool isChannelList(const QByteArray &data);
    static bool isFeed(const QByteArray &data);
    static QList<Item> parseM3u(const QByteArray &data, const QUrl &base);
    static Feed parseRss(const QByteArray &data);

signals:
    void ready(const QString &key);
    void imageReady(const QString &url);

private:
    static bool blocked(const QString &key);
    NetFeeds() = default;
    QNetworkAccessManager *net();
    void fetch(const QString &key);

    QNetworkAccessManager *m_net = nullptr;
    QHash<QString, Feed> m_feeds;
    QHash<QString, QDateTime> m_fetched;
    QHash<QString, QPixmap> m_images;
    QSet<QString> m_imagePending;
};
