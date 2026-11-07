#include "artistimages.h"
#include "version.h"
#include "library.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

namespace {
// "AC/DC" and "ac-dc" are the same artist; so are "Sigur Rós" and "sigur rós".
QString normalised(const QString &name)
{
    QString out;
    for (const QChar ch : name.toCaseFolded())
        if (ch.isLetterOrNumber())
            out += ch;
    return out;
}

QNetworkRequest request(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "VMP/" VMP_VERSION);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(15000);
    return req;
}
} // namespace

ArtistImages &ArtistImages::instance()
{
    static ArtistImages images;
    return images;
}

ArtistImages::ArtistImages()
{
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/artists");
    // Stay well under Deezer's limit (50 requests per 5 s).
    m_pace.setSingleShot(true);
    m_pace.setInterval(150);
    connect(&m_pace, &QTimer::timeout, this, &ArtistImages::next);
}

QString ArtistImages::cacheBase(const QString &name) const
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/artists/" +
           QCryptographicHash::hash(normalised(name).toUtf8(), QCryptographicHash::Md5).toHex();
}

QString ArtistImages::image(const QString &name, const QStringList &trackPaths)
{
    if (normalised(name).isEmpty())
        return {};
    if (const auto it = m_known.constFind(name); it != m_known.constEnd())
        return *it;
    // 0. One the user picked.
    if (QFile::exists(cacheBase(name) + ".user.jpg"))
        return m_known[name] = cacheBase(name) + ".user.jpg";

    // 1. A picture in the artist's own folder (Artist/Album/track → Artist/artist.jpg).
    static const QStringList names = {"artist.jpg", "artist.png", "artist.webp", "Artist.jpg", "Artist.png"};
    QSet<QString> dirs;
    for (const QString &p : trackPaths) {
        const QDir album = QFileInfo(p).dir();
        dirs.insert(album.absolutePath());
        dirs.insert(QFileInfo(album.absolutePath()).absolutePath());
        if (dirs.size() >= 8)
            break;
    }
    for (const QString &d : dirs)
        for (const QString &n : names)
            if (QFile::exists(d + "/" + n))
                return m_known[name] = d + "/" + n;

    // 2. One fetched earlier. A ".none" marker means Deezer had nothing; ask
    //    again after a month in case it has since.
    const QString base = cacheBase(name);
    if (QFile::exists(base + ".jpg"))
        return m_known[name] = base + ".jpg";
    const QFileInfo none(base + ".none");
    if (none.exists() && none.lastModified().daysTo(QDateTime::currentDateTime()) < 30)
        return m_known[name] = QString();

    // 3. Look it up.
    if (!m_queued.contains(name)) {
        m_queued.insert(name);
        m_queue.enqueue(name);
        if (!m_busy && !m_pace.isActive())
            m_pace.start(0);
    }
    return {};
}

QNetworkAccessManager *ArtistImages::network()
{
    if (!m_net)
        m_net = new QNetworkAccessManager(this);
    return m_net;
}

bool ArtistImages::hasChoice(const QString &name) const
{
    return QFile::exists(cacheBase(name) + ".user.jpg");
}

void ArtistImages::search(const QString &query,
                          std::function<void(const QList<Candidate> &, const QString &error)> done)
{
    QUrl url("https://api.deezer.com/search/artist");
    url.setQuery(QUrlQuery{{"q", query}, {"limit", "24"}});
    QNetworkReply *reply = network()->get(request(url));
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, tr("Couldn't reach Deezer (%1).").arg(reply->errorString()));
            return;
        }
        QList<Candidate> out;
        for (const QJsonValue &v : QJsonDocument::fromJson(reply->readAll()).object().value("data").toArray()) {
            const QJsonObject o = v.toObject();
            Candidate c;
            c.name = o.value("name").toString();
            c.thumb = o.value("picture_medium").toString();
            c.picture = o.value("picture_xl").toString();
            c.fans = o.value("nb_fan").toInt();
            if (!c.thumb.isEmpty() && !c.thumb.contains("/artist//"))
                out << c;
        }
        done(out, QString());
    });
}

void ArtistImages::choose(const QString &name, const QString &urlOrFile, std::function<void(const QString &)> done)
{
    if (!urlOrFile.startsWith("http")) {
        QFile f(urlOrFile);
        if (!f.open(QIODevice::ReadOnly)) {
            done(tr("Couldn't read “%1”.").arg(QFileInfo(urlOrFile).fileName()));
            return;
        }
        store(name, f.readAll(), done);
        return;
    }
    QNetworkReply *reply = network()->get(request(QUrl(urlOrFile)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, name, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(tr("Couldn't download the picture (%1).").arg(reply->errorString()));
            return;
        }
        store(name, reply->readAll(), done);
    });
}

void ArtistImages::store(const QString &name, const QByteArray &data, std::function<void(const QString &)> done)
{
    // Re-encode as JPEG so any picture format works and the file stays small.
    QImage image = QImage::fromData(data);
    if (image.isNull()) {
        done(tr("That isn't a picture VMP can read."));
        return;
    }
    if (image.width() > 1000 || image.height() > 1000)
        image = image.scaled(1000, 1000, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QString file = cacheBase(name) + ".user.jpg";
    if (!image.save(file, "JPG", 90)) {
        done(tr("Couldn't save the picture."));
        return;
    }
    Thumbnailer::instance().forget(file);
    m_known[name] = file;
    emit ready(name, file);
    done(QString());
}

void ArtistImages::resetChoice(const QString &name)
{
    const QString base = cacheBase(name);
    Thumbnailer::instance().forget(base + ".user.jpg");
    QFile::remove(base + ".user.jpg");
    QFile::remove(base + ".none"); // and give the automatic lookup another go
    m_known.remove(name);
    emit ready(name, image(name, {}));
}

void ArtistImages::next()
{
    if (m_busy || m_queue.isEmpty())
        return;
    network();
    m_busy = true;
    const QString name = m_queue.dequeue();
    QUrl url("https://api.deezer.com/search/artist");
    url.setQuery(QUrlQuery{{"q", name}, {"limit", "5"}});
    QNetworkReply *reply = m_net->get(request(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, name] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // Offline or refused: try again next time the list is shown.
            m_queued.remove(name);
            m_busy = false;
            m_pace.start();
            return;
        }
        // Only take an exact name match, and skip Deezer's blank placeholder.
        QString picture;
        const QJsonArray data = QJsonDocument::fromJson(reply->readAll()).object().value("data").toArray();
        for (const QJsonValue &v : data) {
            const QJsonObject o = v.toObject();
            if (normalised(o.value("name").toString()) != normalised(name))
                continue;
            const QString pic = o.value("picture_big").toString();
            if (!pic.isEmpty() && !pic.contains("/artist//"))
                picture = pic;
            break;
        }
        if (picture.isEmpty()) {
            done(name, {});
            return;
        }
        QNetworkReply *img = m_net->get(request(QUrl(picture)));
        connect(img, &QNetworkReply::finished, this, [this, img, name] {
            img->deleteLater();
            done(name, img->error() == QNetworkReply::NoError ? img->readAll() : QByteArray());
        });
    });
}

void ArtistImages::done(const QString &name, const QByteArray &data)
{
    const QString base = cacheBase(name);
    QString file;
    if (!data.isEmpty()) {
        QFile f(base + ".jpg");
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size())
            file = f.fileName();
    } else {
        QFile marker(base + ".none");
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
            marker.close();
    }
    m_known[name] = file;
    m_queued.remove(name);
    m_busy = false;
    if (!file.isEmpty())
        emit ready(name, file);
    m_pace.start();
}
