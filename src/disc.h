#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Optical drives and the disc in them, read from sysfs and the udev database
// (no extra dependencies). Used by "Play Disc".
namespace Disc {

enum class Type { Empty, Dvd, BluRay, AudioCd, VideoCd, Data };

struct Drive {
    QString device;     // /dev/sr0
    QString model;      // BD-RE BP50NB40
    Type type = Type::Empty;
    QString label;      // volume label, if any
    QString mountPoint; // where the disc is mounted, if it is
    int audioTracks = 0;

    // Queue entries that play this disc (one per track for audio CDs).
    QStringList items() const;
    QString name() const; // "NEXTGEN" or "DVD in BD-RE BP50NB40"
};

QList<Drive> drives();

// True for dvd://, bluray://, cdda:// and vcd:// locations.
bool isDiscLocation(const QString &location);
// Human-readable name for a disc location ("NEXTGEN", "Audio CD — Track 3").
QString displayName(const QString &location);

// The copy protection a Blu-ray carries: "AACS", "AACS and BD+", or empty
// for an unprotected disc (or anything that isn't a Blu-ray). Konos doesn't
// play protected discs: getting past the protection isn't allowed in many
// countries, so it isn't included (as in VLC).
QString copyProtection(const QString &location);
// What Konos says instead of playing one.
QString copyProtectionMessage(const QString &location);

} // namespace Disc
