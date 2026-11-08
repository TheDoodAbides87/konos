#pragma once

#include <QObject>

class QNetworkAccessManager;

// Asks GitHub for the latest release in the public releases repository
// (KONOS_RELEASES_REPO) and says whether it's newer than this build. Only the
// request itself is sent; nothing about the user or their media.
//
// The Arch package is updated by pacman from the same releases, so there the
// app only points to `pacman -Syu`. (The Mac and Windows builds will download
// and install the update themselves.)
class UpdateCheck : public QObject {
    Q_OBJECT
public:
    static UpdateCheck &instance();

    // automatic: at most once a day, and only when "Check for updates" is on.
    void checkAutomatically();
    void checkNow();

    static QString currentVersion(); // "0.4.0-beta"
    // <0, 0, >0 like strcmp; "0.4.0-beta" < "0.4.0-rc" < "0.4.0" < "0.4.1-beta".
    static int compare(const QString &a, const QString &b);
    static bool updatedByPacman(); // installed from the Arch package
    // Running as an AppImage whose file can be replaced: the update can be
    // downloaded and swapped in (it takes effect on the next start).
    static bool canSelfUpdate();
    QString appImageUrl() const { return m_appImageUrl; } // of the latest release, once checked
    void installUpdate(); // progress() and installed() follow
    void setAppImageUrl(const QString &url) { m_appImageUrl = url; } // tests

signals:
    // newer: a newer release exists (version like "0.5.0-beta", page = its web page).
    // error: empty when the check worked. manual: started by checkNow().
    void finished(bool newer, const QString &version, const QString &page, const QString &error, bool manual);
    void progress(int percent);
    void installed(bool ok, const QString &error);

private:
    UpdateCheck() = default;
    void run(bool manual);
    QNetworkAccessManager *m_net = nullptr;
    bool m_running = false;
    QString m_appImageUrl;
};
