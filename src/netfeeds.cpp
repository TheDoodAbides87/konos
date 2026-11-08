#include "netfeeds.h"
#include "prefs.h"
#include "updatecheck.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace {
QNetworkRequest request(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Konos/" + UpdateCheck::currentVersion());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(20000);
    // Some feed hosts (NPR's CDN) stall Qt's HTTP/2 requests; HTTP/1.1 works everywhere.
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    return req;
}

QString imageCacheFile(const QString &url)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/netimages";
    QDir().mkpath(dir);
    return dir + "/" + QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex() + ".png";
}

// "54:12", "3252", "1:02:03" → "54m", "1h 2m"
QString durationText(const QString &s)
{
    qint64 secs = 0;
    for (const QString &part : s.trimmed().split(':'))
        secs = secs * 60 + part.toLongLong();
    if (secs <= 0)
        return {};
    const qint64 mins = (secs + 30) / 60;
    return mins < 60 ? QObject::tr("%1m").arg(qMax<qint64>(1, mins)) : QObject::tr("%1h %2m").arg(mins / 60).arg(mins % 60);
}

// "Tue, 03 Nov 2026 10:00:00 GMT", "3 Nov 2026 10:00 +0100", ISO 8601.
QDateTime feedDate(const QString &text)
{
    QString d = text.trimmed();
    QDateTime t = QDateTime::fromString(d, Qt::RFC2822Date);
    if (t.isValid())
        return t;
    t = QDateTime::fromString(d, Qt::ISODate);
    if (t.isValid())
        return t;
    if (d.indexOf(',') >= 0 && d.indexOf(',') < 5)
        d = d.mid(d.indexOf(',') + 1).trimmed(); // the day name
    const QStringList parts = d.split(' ', Qt::SkipEmptyParts);
    if (parts.size() < 3)
        return {};
    const QString date = parts.mid(0, 3).join(' ');
    const QString time = parts.value(3, "00:00:00");
    for (const char *fmt : {"d MMM yyyy hh:mm:ss", "d MMM yyyy hh:mm"}) {
        t = QLocale::c().toDateTime(date + " " + time, fmt);
        if (t.isValid()) {
            t.setTimeZone(QTimeZone::UTC); // close enough for a date
            return t;
        }
    }
    return {};
}

QString stripHtml(QString s)
{
    static const QRegularExpression tags("<[^>]*>");
    s.remove(tags);
    s.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&#39;", "'");
    return s.simplified();
}
} // namespace

NetFeeds &NetFeeds::instance()
{
    static NetFeeds n;
    return n;
}

QNetworkAccessManager *NetFeeds::net()
{
    if (!m_net)
        m_net = new QNetworkAccessManager(this);
    return m_net;
}

// Radio and podcast search ask outside directories; Settings → Privacy can turn that off.
bool NetFeeds::blocked(const QString &key)
{
    return (key.startsWith("radio:") || key.startsWith("podcasts:")) && !prefs().flag("network/directories");
}

NetFeeds::Feed NetFeeds::feed(const QString &key)
{
    if (blocked(key)) {
        Feed off;
        off.loaded = true;
        off.error = tr("Radio and podcast directories are turned off in Settings → Privacy → Advanced Options.");
        return off;
    }
    Feed &f = m_feeds[key];
    const QDateTime when = m_fetched.value(key);
    if (!f.loading && (!f.loaded || when.secsTo(QDateTime::currentDateTime()) > 3600))
        fetch(key);
    return f;
}

void NetFeeds::refresh(const QString &key)
{
    if (!m_feeds.value(key).loading)
        fetch(key);
}

void NetFeeds::fetch(const QString &key)
{
    QUrl url;
    const QString kind = key.section(':', 0, 0);
    const QString rest = key.section(':', 1);
    if (blocked(key))
        return;
    if (kind == "m3u" || kind == "rss") {
        url = QUrl(rest);
    } else if (kind == "radio") {
        // all.api resolves to one of the community's servers.
        if (rest == "top") {
            url = QUrl("https://all.api.radio-browser.info/json/stations/topvote");
            url.setQuery("limit=150&hidebroken=true");
        } else {
            url = QUrl("https://all.api.radio-browser.info/json/stations/search");
            QUrlQuery q;
            q.addQueryItem("name", rest.section(':', 1));
            q.addQueryItem("limit", "150");
            q.addQueryItem("hidebroken", "true");
            q.addQueryItem("order", "votes");
            q.addQueryItem("reverse", "true");
            url.setQuery(q);
        }
    } else if (kind == "podcasts") {
        url = QUrl("https://itunes.apple.com/search");
        QUrlQuery q;
        q.addQueryItem("media", "podcast");
        q.addQueryItem("limit", "60");
        q.addQueryItem("term", rest.section(':', 1));
        url.setQuery(q);
    }
    Feed &f = m_feeds[key];
    if (!url.isValid() || url.scheme().isEmpty()) {
        f.error = tr("That isn't a web address.");
        f.loaded = true;
        emit ready(key);
        return;
    }
    f.loading = true;
    QNetworkReply *reply = net()->get(request(url));
    limitSize(reply, 24 << 20); // a channel list or feed; a link to a big file stops here
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, kind, url] {
        reply->deleteLater();
        Feed out;
        out.loaded = true;
        const QByteArray data = reply->readAll();
        if (reply->property("tooBig").toBool()) {
            out = m_feeds.value(key);
            out.error = tr("This link is too big to be a channel list or feed.");
        } else if (reply->error() != QNetworkReply::NoError) {
            out = m_feeds.value(key); // keep what was there
            out.error = tr("Couldn't load it (%1).").arg(reply->errorString());
        } else if (kind == "m3u") {
            out.items = parseM3u(data, url);
            if (out.items.isEmpty())
                out.error = tr("No channels found in this list.");
        } else if (kind == "rss") {
            out = parseRss(data);
            if (out.items.isEmpty() && out.error.isEmpty())
                out.error = tr("This feed has no episodes.");
        } else if (kind == "radio") {
            for (const QJsonValue &v : QJsonDocument::fromJson(data).array()) {
                const QJsonObject o = v.toObject();
                Item it;
                it.title = o.value("name").toString().trimmed();
                it.url = o.value("url_resolved").toString();
                if (it.url.isEmpty())
                    it.url = o.value("url").toString();
                it.image = o.value("favicon").toString();
                QStringList detail;
                const QString tags = o.value("tags").toString().section(',', 0, 1).replace(',', ", ");
                if (!tags.isEmpty())
                    detail << tags;
                if (!o.value("country").toString().isEmpty())
                    detail << o.value("country").toString();
                if (o.value("bitrate").toInt() > 0)
                    detail << tr("%1 kbps").arg(o.value("bitrate").toInt());
                it.detail = detail.join(" · ");
                if (!it.title.isEmpty() && !it.url.isEmpty())
                    out.items << it;
            }
            if (out.items.isEmpty())
                out.error = tr("No stations found.");
        } else if (kind == "podcasts") {
            for (const QJsonValue &v : QJsonDocument::fromJson(data).object().value("results").toArray()) {
                const QJsonObject o = v.toObject();
                Item it;
                it.title = o.value("collectionName").toString();
                it.url = o.value("feedUrl").toString();
                it.image = o.value("artworkUrl600").toString();
                if (it.image.isEmpty())
                    it.image = o.value("artworkUrl100").toString();
                it.detail = o.value("artistName").toString();
                if (!it.title.isEmpty() && !it.url.isEmpty())
                    out.items << it;
            }
            if (out.items.isEmpty())
                out.error = tr("No podcasts found.");
        }
        // Done, even when it failed (an error kept the old entry, flags and all).
        out.loading = false;
        out.loaded = true;
        m_feeds.insert(key, out);
        m_fetched.insert(key, QDateTime::currentDateTime());
        emit ready(key);
    });
}

void NetFeeds::limitSize(QNetworkReply *reply, qint64 bytes)
{
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply, bytes](qint64 got, qint64 total) {
        if (got > bytes || total > bytes) {
            reply->setProperty("tooBig", true);
            reply->abort();
        }
    });
}

void NetFeeds::cacheImage(const QString &url, const QPixmap &pm)
{
    if (pm.isNull()) {
        m_imageFailed.insert(url);
        return;
    }
    m_images.insert(url, new QPixmap(pm), qMax<qsizetype>(1, qsizetype(pm.width()) * pm.height() * 4 / 1024));
}

QPixmap NetFeeds::image(const QString &url)
{
    if (url.isEmpty())
        return {};
    if (QPixmap *hit = m_images.object(url))
        return *hit;
    if (m_imageFailed.contains(url))
        return {};
    const QString file = imageCacheFile(url);
    if (QFile::exists(file)) {
        QPixmap pm(file);
        cacheImage(url, pm);
        return pm;
    }
    if (m_imagePending.contains(url) || m_imagePending.size() > 12)
        return {};
    m_imagePending.insert(url);
    QNetworkReply *reply = net()->get(request(QUrl(url)));
    limitSize(reply, 8 << 20); // artwork, not a film
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, file] {
        reply->deleteLater();
        m_imagePending.remove(url);
        QByteArray data = reply->readAll();
        QBuffer buf(&data);
        QImageReader reader(&buf);
        reader.setAutoTransform(true);
        // Big podcast artwork: decode at tile size.
        if (reader.size().isValid() && reader.size().width() > 400)
            reader.setScaledSize(reader.size().scaled(400, 400, Qt::KeepAspectRatio));
        const QImage img = reader.read();
        QPixmap pm;
        if (!img.isNull()) {
            pm = QPixmap::fromImage(img);
            img.save(file, "PNG");
        }
        cacheImage(url, pm);
        emit imageReady(url);
    });
    return {};
}

void NetFeeds::probe(const QString &url, std::function<void(const QString &, const QString &)> done)
{
    const QUrl u(url);
    if (u.scheme() != "http" && u.scheme() != "https") {
        done("stream", {});
        return;
    }
    QNetworkRequest req = request(u);
    req.setTransferTimeout(10000);
    QNetworkReply *reply = net()->get(req);
    auto data = std::make_shared<QByteArray>();
    auto finished = std::make_shared<bool>(false);
    auto decide = [reply, data, done, finished] {
        if (*finished)
            return;
        *finished = true;
        const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        QString kind = "stream", title;
        if (isChannelList(*data)) {
            kind = "channels";
        } else if (isFeed(*data) || type.contains("rss") || type.contains("xml")) {
            if (isFeed(*data)) {
                kind = "podcast";
                QXmlStreamReader x(*data);
                while (!x.atEnd() && !x.hasError()) // the channel's title comes first
                    if (x.readNext() == QXmlStreamReader::StartElement && x.name() == u"title") {
                        title = x.readElementText().trimmed();
                        break;
                    }
            }
        }
        reply->abort();
        reply->deleteLater();
        done(kind, title);
    };
    // Audio and video streams never end: decide from the first 64 KB.
    connect(reply, &QNetworkReply::readyRead, reply, [reply, data, decide] {
        data->append(reply->readAll());
        const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        if (data->size() > 65536 || type.startsWith("audio/") || type.startsWith("video/"))
            decide();
    });
    connect(reply, &QNetworkReply::finished, reply, [reply, data, decide] {
        data->append(reply->readAll());
        decide();
    });
}

bool NetFeeds::isChannelList(const QByteArray &data)
{
    const QByteArray head = data.left(65536);
    // HLS playlists (#EXT-X-…) are one stream in pieces, not a list of channels.
    return head.trimmed().startsWith("#EXTM3U") && head.contains("#EXTINF") && !head.contains("#EXT-X-");
}

bool NetFeeds::isFeed(const QByteArray &data)
{
    const QByteArray head = data.left(4096);
    return head.contains("<rss") || head.contains("<feed");
}

QList<NetFeeds::Item> NetFeeds::parseM3u(const QByteArray &data, const QUrl &base)
{
    static const QRegularExpression attr(R"re((tvg-logo|group-title)="([^"]*)")re");
    QList<Item> out;
    Item pending;
    for (QString line : QString::fromUtf8(data).split('\n')) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith("#EXTINF")) {
            pending = Item();
            // #EXTINF:-1 tvg-logo="…" group-title="News",Channel Name
            const int comma = line.indexOf(',', line.lastIndexOf('"') + 1);
            pending.title = comma >= 0 ? line.mid(comma + 1).trimmed() : QString();
            auto it = attr.globalMatch(line);
            while (it.hasNext()) {
                const auto m = it.next();
                if (m.captured(1) == "tvg-logo")
                    pending.image = m.captured(2);
                else
                    pending.detail = m.captured(2);
            }
        } else if (!line.startsWith('#')) {
            Item it = pending;
            it.url = base.resolved(QUrl(line)).toString();
            if (it.title.isEmpty())
                it.title = QUrl(it.url).fileName().isEmpty() ? it.url : QUrl(it.url).fileName();
            out << it;
            pending = Item();
        }
    }
    return out;
}

NetFeeds::Feed NetFeeds::parseRss(const QByteArray &data)
{
    Feed f;
    QXmlStreamReader x(data);
    Item item;
    bool inItem = false;
    QString itemDuration;
    while (!x.atEnd()) {
        const auto token = x.readNext();
        const auto name = x.name();
        if (token == QXmlStreamReader::StartElement) {
            if (name == u"item" || name == u"entry") {
                inItem = true;
                item = Item();
                itemDuration.clear();
            } else if (name == u"title") {
                const QString t = x.readElementText().trimmed();
                if (inItem)
                    item.title = t;
                else if (f.title.isEmpty())
                    f.title = t;
            } else if (name == u"enclosure" && inItem) {
                item.url = x.attributes().value("url").toString();
            } else if (name == u"link" && inItem && item.url.isEmpty() &&
                       x.attributes().value("rel") == u"enclosure") {
                item.url = x.attributes().value("href").toString();
            } else if ((name == u"pubDate" || name == u"published") && inItem) {
                item.date = feedDate(x.readElementText());
            } else if (name == u"duration" && inItem) {
                itemDuration = durationText(x.readElementText());
            } else if (name == u"image") {
                // <itunes:image href="…"/> or <image><url>…</url></image>
                const QString href = x.attributes().value("href").toString();
                if (!href.isEmpty()) {
                    if (inItem)
                        item.image = href;
                    else if (f.image.isEmpty())
                        f.image = href;
                }
            } else if (name == u"url" && !inItem && f.image.isEmpty()) {
                f.image = x.readElementText().trimmed();
            } else if ((name == u"description" || name == u"summary") && !inItem && f.description.isEmpty()) {
                f.description = stripHtml(x.readElementText());
            }
        } else if (token == QXmlStreamReader::EndElement && (name == u"item" || name == u"entry")) {
            inItem = false;
            QStringList detail;
            if (item.date.isValid())
                detail << QLocale().toString(item.date.date(), QLocale::ShortFormat);
            if (!itemDuration.isEmpty())
                detail << itemDuration;
            item.detail = detail.join(" · ");
            if (!item.url.isEmpty())
                f.items << item;
        }
    }
    if (x.hasError() && f.items.isEmpty())
        f.error = QObject::tr("This doesn't look like a podcast feed.");
    for (Item &it : f.items)
        if (it.image.isEmpty())
            it.image = f.image;
    return f;
}
