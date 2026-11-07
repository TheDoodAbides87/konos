#include "updatecheck.h"
#include "prefs.h"
#include "version.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>

UpdateCheck &UpdateCheck::instance()
{
    static UpdateCheck u;
    return u;
}

QString UpdateCheck::currentVersion()
{
    return QStringLiteral(VMP_VERSION) + (QStringLiteral(VMP_CHANNEL).isEmpty() ? QString() : "-" VMP_CHANNEL);
}

int UpdateCheck::compare(const QString &a, const QString &b)
{
    static const QRegularExpression re(R"(^v?(\d+(?:\.\d+)*)(?:-?([a-z]+)\.?(\d*))?)", QRegularExpression::CaseInsensitiveOption);
    auto parse = [](const QString &s, QList<int> &nums, int &rank, int &n) {
        const auto m = re.match(s.trimmed());
        for (const QString &p : m.captured(1).split('.', Qt::SkipEmptyParts))
            nums << p.toInt();
        while (nums.size() < 3)
            nums << 0;
        // A release without a channel comes after its betas and release candidates.
        const QString ch = m.captured(2).toLower();
        rank = ch.isEmpty() ? 4 : ch == "rc" ? 3 : ch == "beta" ? 2 : ch == "alpha" ? 1 : 0;
        n = m.captured(3).toInt();
    };
    QList<int> na, nb;
    int ra, rb, xa, xb;
    parse(a, na, ra, xa);
    parse(b, nb, rb, xb);
    for (int i = 0; i < qMax(na.size(), nb.size()); ++i) {
        const int x = na.value(i), y = nb.value(i);
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (ra != rb)
        return ra < rb ? -1 : 1;
    return xa == xb ? 0 : xa < xb ? -1 : 1;
}

bool UpdateCheck::updatedByPacman()
{
    return QFileInfo::exists("/usr/bin/pacman") &&
           QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath() == "/usr/bin/vmp";
}

void UpdateCheck::checkAutomatically()
{
    if (!prefs().flag("updates/check"))
        return;
    const qint64 last = prefs().get("updates/lastCheck").toLongLong();
    if (QDateTime::currentSecsSinceEpoch() - last < 20 * 3600)
        return;
    run(false);
}

void UpdateCheck::checkNow()
{
    run(true);
}

void UpdateCheck::run(bool manual)
{
    if (m_running)
        return;
    m_running = true;
    if (!m_net)
        m_net = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/" VMP_RELEASES_REPO "/releases/latest")));
    req.setHeader(QNetworkRequest::UserAgentHeader, "VMP/" + currentVersion());
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, manual] {
        reply->deleteLater();
        m_running = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404) { // the repository has no releases yet
            emit finished(false, {}, {}, tr("No releases have been published yet."), manual);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            emit finished(false, {}, {}, tr("Couldn't reach GitHub (%1).").arg(reply->errorString()), manual);
            return;
        }
        prefs().set("updates/lastCheck", QDateTime::currentSecsSinceEpoch());
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        QString version = o.value("tag_name").toString();
        if (version.startsWith('v'))
            version.remove(0, 1);
        const QString page = o.value("html_url").toString();
        if (version.isEmpty()) {
            emit finished(false, {}, {}, tr("GitHub's answer didn't include a version."), manual);
            return;
        }
        emit finished(compare(version, currentVersion()) > 0, version, page, {}, manual);
    });
}
