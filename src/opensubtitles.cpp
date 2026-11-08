#include "opensubtitles.h"
#include "prefs.h"
#include "version.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QtEndian>

static const char *kUserAgent = "Konos v" KONOS_VERSION;

OpenSubtitles &OpenSubtitles::instance()
{
    static OpenSubtitles o;
    return o;
}

OpenSubtitles::OpenSubtitles() : m_net(new QNetworkAccessManager(this))
{
    m_pace.setInterval(260);
    connect(&m_pace, &QTimer::timeout, this, [this] {
        if (m_queue.isEmpty()) {
            m_pace.stop();
            return;
        }
        m_queue.dequeue()();
    });
}

QString OpenSubtitles::apiKey() const
{
    const QString key = prefs().string("opensubtitles/apiKey").trimmed();
#ifdef KONOS_OPENSUBTITLES_KEY
    if (key.isEmpty())
        return QStringLiteral(KONOS_OPENSUBTITLES_KEY);
#endif
    return key;
}

bool OpenSubtitles::signedIn() const { return !prefs().string("opensubtitles/token").isEmpty(); }
QString OpenSubtitles::user() const { return prefs().string("opensubtitles/user"); }

void OpenSubtitles::enqueue(std::function<void()> send)
{
    m_queue.enqueue(std::move(send));
    if (!m_pace.isActive()) {
        m_queue.dequeue()();
        m_pace.start();
    }
}

QNetworkRequest OpenSubtitles::request(const QString &path, const QUrlQuery &query) const
{
    // Signed-in accounts may be sent to another host (VIP).
    const QString host = prefs().string("opensubtitles/baseUrl").isEmpty() ? QStringLiteral("api.opensubtitles.com")
                                                                            : prefs().string("opensubtitles/baseUrl");
#ifdef KONOS_DEVHOOKS
    // Test builds can point at a mock server ("http://127.0.0.1:8765").
    QUrl url((host.contains("://") ? host : "https://" + host) + "/api/v1" + path);
#else
    QUrl url("https://" + host + "/api/v1" + path);
#endif
    if (!query.isEmpty()) {
        // The API redirects anything but its canonical form: parameters sorted,
        // lower-case values, "+" for spaces.
        QList<QPair<QString, QString>> items = query.queryItems(QUrl::FullyDecoded);
        std::sort(items.begin(), items.end());
        QStringList parts;
        for (const auto &[k, v] : items)
            parts << k + '=' + QString::fromLatin1(QUrl::toPercentEncoding(v.toLower(), " ")).replace(' ', '+');
        url.setQuery(parts.join('&'), QUrl::StrictMode);
    }
    QNetworkRequest req(url);
    req.setRawHeader("Api-Key", apiKey().toUtf8());
    req.setRawHeader("User-Agent", kUserAgent);
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (signedIn())
        req.setRawHeader("Authorization", "Bearer " + prefs().string("opensubtitles/token").toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(20000);
    return req;
}

QString OpenSubtitles::errorFor(QNetworkReply *reply, const QByteArray &body) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject json = QJsonDocument::fromJson(body).object();
    QString message = json.value("message").toString();
    if (message.isEmpty() && json.value("errors").isArray())
        message = json.value("errors").toArray().first().toString();
    switch (status) {
    case 401:
        return tr("OpenSubtitles didn't accept the sign-in. Sign in again in Settings → Subtitles.");
    case 403:
        return tr("OpenSubtitles didn't accept the API key. Check it in Settings → Subtitles.");
    case 406:
        return message.isEmpty() ? tr("Today's download limit is used up.") : message;
    case 429:
        return tr("OpenSubtitles is busy; try again in a moment.");
    default:
        break;
    }
    if (reply->error() != QNetworkReply::NoError && status == 0)
        return tr("Couldn't reach OpenSubtitles (%1).").arg(reply->errorString());
    return message.isEmpty() ? tr("OpenSubtitles answered with an error (%1).").arg(status) : message;
}

void OpenSubtitles::search(const Query &q, SearchDone done)
{
    if (!hasKey()) {
        done({}, tr("Add an OpenSubtitles API key in Settings → Subtitles."));
        return;
    }
    QUrlQuery query;
    const QString hash = q.path.isEmpty() ? QString() : movieHash(q.path);
    if (!hash.isEmpty())
        query.addQueryItem("moviehash", hash);
    if (!q.text.trimmed().isEmpty())
        query.addQueryItem("query", q.text.trimmed());
    if (!q.language.isEmpty())
        query.addQueryItem("languages", q.language);
    if (q.season >= 0)
        query.addQueryItem("season_number", QString::number(q.season));
    if (q.episode >= 0)
        query.addQueryItem("episode_number", QString::number(q.episode));
    if (q.year > 0 && q.episode < 0)
        query.addQueryItem("year", QString::number(q.year));

    enqueue([this, query, q, done] {
        QNetworkReply *reply = m_net->get(request("/subtitles", query));
        connect(reply, &QNetworkReply::finished, this, [this, reply, q, done] {
            reply->deleteLater();
            const QByteArray body = reply->readAll();
            if (reply->error() != QNetworkReply::NoError) {
                done({}, errorFor(reply, body));
                return;
            }
            QList<Result> out;
            for (const QJsonValue &item : QJsonDocument::fromJson(body).object().value("data").toArray()) {
                const QJsonObject a = item.toObject().value("attributes").toObject();
                const QJsonObject feature = a.value("feature_details").toObject();
                for (const QJsonValue &f : a.value("files").toArray()) {
                    Result r;
                    r.fileId = f.toObject().value("file_id").toInteger();
                    r.fileName = f.toObject().value("file_name").toString();
                    r.release = a.value("release").toString();
                    r.language = a.value("language").toString();
                    r.downloads = a.value("download_count").toInt();
                    r.hashMatch = a.value("moviehash_match").toBool();
                    r.hearingImpaired = a.value("hearing_impaired").toBool();
                    r.title = feature.value("title").toString();
                    if (r.title.isEmpty())
                        r.title = feature.value("movie_name").toString();
                    r.season = feature.value("season_number").toInt(-1);
                    r.episode = feature.value("episode_number").toInt(-1);
                    if (r.fileId > 0)
                        out << r;
                }
            }
            // Best first: same file (hash), right episode, most downloaded.
            auto score = [&q](const Result &r) {
                int s = r.hashMatch ? 4 : 0;
                if (q.episode >= 0 && r.episode == q.episode && (q.season < 0 || r.season == q.season))
                    s += 2;
                return s;
            };
            std::stable_sort(out.begin(), out.end(), [&](const Result &a, const Result &b) {
                return score(a) != score(b) ? score(a) > score(b) : a.downloads > b.downloads;
            });
            if (q.episode >= 0) {
                // Drop other episodes of the show when the right one is there.
                QList<Result> exact;
                for (const Result &r : std::as_const(out))
                    if (r.hashMatch || (r.episode == q.episode && (q.season < 0 || r.season == q.season)))
                        exact << r;
                if (!exact.isEmpty())
                    out = exact;
            }
            done(out, {});
        });
    });
}

void OpenSubtitles::download(qint64 fileId, DownloadDone done)
{
    if (!hasKey()) {
        done({}, {}, -1, tr("Add an OpenSubtitles API key in Settings → Subtitles."));
        return;
    }
    enqueue([this, fileId, done] {
        const QByteArray body = QJsonDocument(QJsonObject{{"file_id", fileId}}).toJson(QJsonDocument::Compact);
        QNetworkReply *reply = m_net->post(request("/download"), body);
        connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
            reply->deleteLater();
            const QByteArray data = reply->readAll();
            const QJsonObject json = QJsonDocument::fromJson(data).object();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 401 && signedIn()) {
                // Expired sign-in: drop it so the next try goes out anonymously.
                prefs().set("opensubtitles/token", QString());
                emit accountChanged();
            }
            if (reply->error() != QNetworkReply::NoError || json.value("link").toString().isEmpty()) {
                QString error = errorFor(reply, data);
                if (status == 406 && !json.value("reset_time").toString().isEmpty())
                    error = tr("Today's download limit is used up (resets in %1).").arg(json.value("reset_time").toString());
                done({}, {}, json.value("remaining").toInt(-1), error);
                return;
            }
            const int remaining = json.value("remaining").toInt(-1);
            const QString fileName = json.value("file_name").toString();
            QNetworkRequest fileReq{QUrl(json.value("link").toString())};
            fileReq.setRawHeader("User-Agent", kUserAgent);
            fileReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            fileReq.setTransferTimeout(30000);
            QNetworkReply *file = m_net->get(fileReq);
            connect(file, &QNetworkReply::finished, this, [file, fileName, remaining, done] {
                file->deleteLater();
                if (file->error() != QNetworkReply::NoError)
                    done({}, {}, remaining, tr("The subtitle download failed (%1).").arg(file->errorString()));
                else
                    done(file->readAll(), fileName, remaining, {});
            });
        });
    });
}

void OpenSubtitles::signIn(const QString &user, const QString &password, std::function<void(const QString &)> done)
{
    if (!hasKey()) {
        done(tr("Add an OpenSubtitles API key first."));
        return;
    }
    // Sign in against the default host; the reply names the one to use.
    // (A full URL is a test server; keep it.)
    if (!prefs().string("opensubtitles/baseUrl").contains("://"))
        prefs().set("opensubtitles/baseUrl", QString());
    prefs().set("opensubtitles/token", QString());
    enqueue([this, user, password, done] {
        const QByteArray body =
            QJsonDocument(QJsonObject{{"username", user}, {"password", password}}).toJson(QJsonDocument::Compact);
        QNetworkReply *reply = m_net->post(request("/login"), body);
        connect(reply, &QNetworkReply::finished, this, [this, reply, user, done] {
            reply->deleteLater();
            const QByteArray data = reply->readAll();
            const QJsonObject json = QJsonDocument::fromJson(data).object();
            const QString token = json.value("token").toString();
            if (reply->error() != QNetworkReply::NoError || token.isEmpty()) {
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                done(status == 401 ? tr("Wrong username or password.") : errorFor(reply, data));
                return;
            }
            // Only the session token is kept, never the password.
            prefs().set("opensubtitles/user", user);
            prefs().set("opensubtitles/token", token);
            prefs().set("opensubtitles/baseUrl", json.value("base_url").toString());
            emit accountChanged();
            done({});
        });
    });
}

void OpenSubtitles::signOut()
{
    prefs().set("opensubtitles/token", QString());
    prefs().set("opensubtitles/baseUrl", QString());
    emit accountChanged();
}

QString OpenSubtitles::movieHash(const QString &path)
{
    QFile f(path);
    const qint64 size = f.size();
    constexpr qint64 chunk = 64 * 1024;
    if (size < 2 * chunk || !f.open(QIODevice::ReadOnly))
        return {};
    quint64 hash = quint64(size);
    auto sum = [&hash](const QByteArray &block) {
        for (qsizetype i = 0; i + 8 <= block.size(); i += 8)
            hash += qFromLittleEndian<quint64>(block.constData() + i);
    };
    sum(f.read(chunk));
    f.seek(size - chunk);
    sum(f.read(chunk));
    return QStringLiteral("%1").arg(hash, 16, 16, QLatin1Char('0'));
}

OpenSubtitles::Query OpenSubtitles::guess(const QString &path)
{
    Query q;
    q.path = path;
    QString name = QFileInfo(path).completeBaseName();
    name.replace(QRegularExpression("[._]+"), " ");
    static const QRegularExpression episode(R"(\b[Ss](\d{1,2})[ ._-]?[Ee](\d{1,3})\b|\b(\d{1,2})x(\d{2,3})\b)");
    static const QRegularExpression year(R"(\(?\b((?:19|20)\d{2})\b\)?)");
    static const QRegularExpression junk(
        R"(\b(2160p|1080p|720p|480p|4k|uhd|hdr|bluray|blu-ray|brrip|bdrip|web-?dl|webrip|web|hdtv|dvdrip|x264|x265|h264|h265|hevc|aac|ac3|dts|remux|proper|repack|extended)\b.*$)",
        QRegularExpression::CaseInsensitiveOption);
    if (const auto m = episode.match(name); m.hasMatch()) {
        q.season = (m.captured(1).isEmpty() ? m.captured(3) : m.captured(1)).toInt();
        q.episode = (m.captured(2).isEmpty() ? m.captured(4) : m.captured(2)).toInt();
        name.truncate(m.capturedStart());
    } else {
        // The last year-like number is the year ("Blade Runner 2049 (2017)").
        QRegularExpressionMatch last;
        for (auto it = year.globalMatch(name); it.hasNext();)
            last = it.next();
        if (last.hasMatch() && last.capturedStart() > 0) {
            q.year = last.captured(1).toInt();
            name.truncate(last.capturedStart());
        }
    }
    name.remove(junk);
    name.remove(QRegularExpression(R"(\[[^\]]*\])")); // [group] tags
    q.text = name.simplified().remove(QRegularExpression("[ -]+$"));
    return q;
}
