#include "appstyle.h"
#include "effects.h"
#include "mainwindow.h"
#include "musicindex.h"
#include "prefs.h"
#include "theme.h"
#include "version.h"

#include <QApplication>
#include <QTimer>
#include <QCommandLineParser>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QIcon>
#include <QSettings>
#include <QStandardPaths>

// VMP was called OmaVideo (0.2–0.4) and "VLC Redesign" before that. On first
// launch, copy the newest older settings and library (not caches, which
// rebuild themselves).
static void migrateOldNames()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString newConf = config + "/vmp/vmp.conf";
    const QString newData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (QFile::exists(newConf) || QDir(newData).exists())
        return;
    for (const QString old : {QStringLiteral("omavideo"), QStringLiteral("vlc-redesign")}) {
        const QString oldConf = config + "/" + old + "/" + old + ".conf";
        const QDir oldData(data + "/" + old + "/" + old);
        if (!QFile::exists(oldConf) && !oldData.exists())
            continue;
        QDir().mkpath(config + "/vmp");
        QFile::copy(oldConf, newConf);
        for (QDirIterator it(oldData.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); it.hasNext();) {
            const QString from = it.next();
            const QString to = newData + "/" + oldData.relativeFilePath(from);
            QDir().mkpath(QFileInfo(to).path());
            QFile::copy(from, to);
        }
        return;
    }
}

int main(int argc, char *argv[])
{
    QApplication::setApplicationName("vmp");
    QApplication::setOrganizationName("vmp");
    QApplication::setApplicationDisplayName("VMP");
    QApplication::setApplicationVersion(VMP_VERSION "-" VMP_CHANNEL);
    QApplication::setDesktopFileName(VMP_APP_ID);
    QApplication app(argc, argv);
    migrateOldNames();
    {
        // 0.6: "Load matching subtitle files" (on/off) became a choice of how
        // closely the names must match; off stays off.
        QSettings s;
        if (s.contains("subs/autoload")) {
            if (!s.value("subs/autoload").toBool() && !s.contains("subs/fuzzy"))
                s.setValue("subs/fuzzy", 0);
            s.remove("subs/autoload");
        }
    }

    for (const char *font : {":/fonts/Inter-Regular.ttf", ":/fonts/Inter-Medium.ttf", ":/fonts/Inter-SemiBold.ttf"})
        QFontDatabase::addApplicationFont(font);
    QFont font("Inter");
    font.setPixelSize(13);
    font.setHintingPreference(QFont::PreferNoHinting);
    app.setFont(font);
    app.setWindowIcon(QIcon(":/vmp.svg"));
    app.setStyle(new AppStyle);

    QCommandLineParser parser;
    parser.setApplicationDescription("Vibes Media Player (VMP): a media player built on libVLC");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption enqueue("enqueue", "Add the given media to the play queue instead of playing it.");
    parser.addOption(enqueue);
    parser.addPositionalArgument("media", "Files, folders or URLs to play.", "[media...]");
    parser.process(app);

    Theme::loadSaved();
    fx().load();
    app.setPalette(theme().palette());
    app.setStyleSheet(theme().styleSheet());

    MainWindow window;
    window.show();
    // Read new or changed music tags in the background once the window is up.
    QTimer::singleShot(2000, &window, [] { musicIndex().ensureScanned(); });
    if (!parser.positionalArguments().isEmpty())
        window.openLocations(parser.positionalArguments(), parser.isSet(enqueue));
    return app.exec();
}
