#include "disc.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QStorageInfo>

namespace Disc {

static QString tr(const char *text) { return QCoreApplication::translate("Disc", text); }

// udev stores device properties as "E:KEY=value" lines in /run/udev/data/b<major>:<minor>.
static QHash<QString, QString> udevProperties(const QString &blockName)
{
    QHash<QString, QString> props;
    QFile dev("/sys/block/" + blockName + "/dev");
    if (!dev.open(QIODevice::ReadOnly))
        return props;
    QFile data("/run/udev/data/b" + QString::fromLatin1(dev.readAll()).trimmed());
    if (!data.open(QIODevice::ReadOnly))
        return props;
    for (const QByteArray &line : data.readAll().split('\n')) {
        if (!line.startsWith("E:"))
            continue;
        const int eq = line.indexOf('=');
        if (eq > 2)
            props.insert(QString::fromUtf8(line.mid(2, eq - 2)), QString::fromUtf8(line.mid(eq + 1)));
    }
    return props;
}

static bool hasPrefixedKey(const QHash<QString, QString> &props, const QString &prefix)
{
    for (auto it = props.cbegin(); it != props.cend(); ++it)
        if (it.key().startsWith(prefix) && it.value() == "1")
            return true;
    return false;
}

QList<Drive> drives()
{
    QList<Drive> out;
    const QStringList names = QDir("/sys/block").entryList({"sr*"}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        Drive d;
        d.device = "/dev/" + name;
        const auto props = udevProperties(name);
        d.model = props.value("ID_MODEL").replace('_', ' ');
        d.label = props.value("ID_FS_LABEL");
        d.audioTracks = props.value("ID_CDROM_MEDIA_TRACK_COUNT_AUDIO").toInt();

        for (const QStorageInfo &vol : QStorageInfo::mountedVolumes()) {
            if (QString::fromUtf8(vol.device()) == d.device) {
                d.mountPoint = vol.rootPath();
                break;
            }
        }
        const QDir root(d.mountPoint);
        const bool mounted = !d.mountPoint.isEmpty();

        if (props.isEmpty()) {
            // No udev data (e.g. a container): guess from the mounted contents.
            if (mounted && root.exists("BDMV"))
                d.type = Type::BluRay;
            else if (mounted && root.exists("VIDEO_TS"))
                d.type = Type::Dvd;
            else if (mounted)
                d.type = Type::Data;
        } else if (props.value("ID_CDROM_MEDIA") != "1") {
            d.type = Type::Empty;
        } else if (hasPrefixedKey(props, "ID_CDROM_MEDIA_BD")) {
            d.type = !mounted || root.exists("BDMV") ? Type::BluRay : Type::Data;
        } else if (hasPrefixedKey(props, "ID_CDROM_MEDIA_DVD") || hasPrefixedKey(props, "ID_CDROM_MEDIA_HDDVD")) {
            d.type = !mounted || root.exists("VIDEO_TS") ? Type::Dvd : Type::Data;
        } else if (d.audioTracks > 0) {
            d.type = Type::AudioCd;
        } else if (mounted && (root.exists("MPEGAV") || root.exists("MPEG2") || root.exists("VCD") || root.exists("SVCD"))) {
            d.type = Type::VideoCd;
        } else {
            d.type = Type::Data;
        }
        out << d;
    }
    return out;
}

QStringList Drive::items() const
{
    switch (type) {
    case Type::Dvd:
        return {"dvd://" + device};
    case Type::BluRay:
        // libbluray reads a mounted disc more reliably than the raw device.
        return {"bluray://" + (mountPoint.isEmpty() ? device : mountPoint)};
    case Type::AudioCd: {
        // The cdda module turns a bare cdda:// into a sub-playlist the player
        // can't follow, so queue each track ("#n" becomes :cdda-track=n).
        QStringList tracks;
        for (int i = 1; i <= audioTracks; ++i)
            tracks << QString("cdda://%1#%2").arg(device).arg(i);
        return tracks;
    }
    case Type::VideoCd:
        return {"vcd://" + device};
    case Type::Data:
        return mountPoint.isEmpty() ? QStringList() : QStringList{mountPoint};
    case Type::Empty:
        break;
    }
    return {};
}

QString Drive::name() const
{
    if (!label.isEmpty())
        return label;
    const QString drive = model.isEmpty() ? device : model;
    switch (type) {
    case Type::Dvd: return tr("DVD in %1").arg(drive);
    case Type::BluRay: return tr("Blu-ray in %1").arg(drive);
    case Type::AudioCd: return tr("Audio CD in %1").arg(drive);
    case Type::VideoCd: return tr("Video CD in %1").arg(drive);
    case Type::Data: return tr("Disc in %1").arg(drive);
    case Type::Empty: break;
    }
    return drive;
}

bool isDiscLocation(const QString &location)
{
    for (const char *scheme : {"dvd://", "dvdsimple://", "bluray://", "cdda://", "vcd://"})
        if (location.startsWith(QLatin1String(scheme)))
            return true;
    return false;
}

QString displayName(const QString &location)
{
    const int sep = location.indexOf("://");
    const QString scheme = location.left(sep);
    QString path = location.mid(sep + 3);
    QString track;
    if (const int hash = path.indexOf('#'); hash >= 0) {
        track = path.mid(hash + 1);
        path.truncate(hash);
    }
    if (scheme == "cdda")
        return track.isEmpty() ? tr("Audio CD") : tr("Audio CD — Track %1").arg(track);

    // Views ask for this on every repaint; don't rescan the drives each time.
    static QList<Drive> cache;
    static QElapsedTimer age;
    if (!age.isValid() || age.elapsed() > 3000) {
        cache = drives();
        age.start();
    }
    for (const Drive &d : std::as_const(cache)) {
        if ((d.device == path || (!d.mountPoint.isEmpty() && d.mountPoint == path)) && !d.label.isEmpty())
            return d.label;
    }
    if (scheme == "bluray")
        return tr("Blu-ray");
    if (scheme == "vcd")
        return tr("Video CD");
    return tr("DVD");
}

QString copyProtection(const QString &location)
{
    if (!location.startsWith(QLatin1String("bluray://")))
        return {};
    QString root = location.mid(int(qstrlen("bluray://")));
    if (const int hash = root.indexOf('#'); hash >= 0)
        root.truncate(hash);
    // A drive (/dev/sr0) is looked at where its disc is mounted.
    if (root.startsWith(QLatin1String("/dev/"))) {
        QString mounted;
        for (const Drive &d : drives())
            if (d.device == root)
                mounted = d.mountPoint;
        if (mounted.isEmpty())
            return {}; // can't look inside; let the player try
        root = mounted;
    }
    // The folders protected discs carry: AACS for its keys, BDSVM for BD+.
    const QDir disc(root);
    const bool aacs = disc.exists("AACS") || disc.exists("aacs");
    const bool bdplus = disc.exists("BDSVM") || disc.exists("bdsvm");
    if (aacs && bdplus)
        return tr("AACS and BD+");
    if (aacs)
        return QStringLiteral("AACS");
    if (bdplus)
        return QStringLiteral("BD+");
    return {};
}

QString copyProtectionMessage(const QString &location)
{
    return tr("This Blu-ray is copy-protected (%1), so Konos can't play it.\n\n"
              "This is a legal limit, not a technical one: playing it would mean getting past its copy "
              "protection, which the law in many countries doesn't allow, so Konos (like VLC) doesn't "
              "include a way to.\n\n"
              "Blu-rays without copy protection play normally, and a digital copy that came with the "
              "disc plays as an ordinary file.")
        .arg(copyProtection(location));
}

} // namespace Disc
