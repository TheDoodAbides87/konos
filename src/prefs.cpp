#include "prefs.h"

#include <QDir>
#include <QHash>
#include <QSettings>
#include <QStandardPaths>

static const QHash<QString, QVariant> &defaults()
{
    static const QHash<QString, QVariant> d = {
        {"ui/theme", 0},
        {"ui/accent", 0},
        {"omarchy/theme", 3},  // Omarchy build: follow the desktop theme
        {"omarchy/accent", 9}, // ...and its accent
        {"ui/language", "English"},
        {"ui/startInLibrary", true},
        {"ui/autoHideMs", 2500},
        {"ui/pauseOnMinimize", false},
        {"ui/remainingTime", false},
        {"ui/clickPause", false},
        {"ui/thumbSize", 1},
        {"ui/sidePanel", true},
        {"ui/miniPlayer", true},  // the player in a corner of the library while something plays
        {"ui/miniCorner", 3},     // 0 top left, 1 top right, 2 bottom left, 3 bottom right
        {"ui/libraryQueue", false}, // the queue beside the library
        {"ui/viewModes", ""},
        {"ui/rightPanel", false},
        {"audio/device", ""},
        {"osd/showKeys", true},
        {"ui/rightTab", 0},
        {"startup/confirmDialogs", true},
        {"startup/restoreSession", false},
        {"updates/check", true},
        {"updates/lastCheck", 0},
        {"diag/developer", false},
        {"diag/saveLog", false},
        {"diag/logFile", QDir::homePath() + "/vmp.log"},
        {"diag/logLevel", 0},
        {"privacy/history", true},
        {"privacy/positions", true},
        {"privacy/metaNetwork", false},
        {"controls/seekStep", 10},
        {"controls/volumeStep", 5},
        {"controls/wheel", 0},
        {"playback/resume", 0},
        {"playback/autoNext", true},
        {"playback/speed", 1.0},
        {"playback/repeat", 0},
        {"playback/shuffle", false},
        {"video/hw", true},
        {"video/deinterlace", "off"},
        {"video/aspect", "Auto"},
        {"video/streamResolution", -1}, // adaptive streams: preferred height, -1 = best
        {"playback/subTimeline", false}, // subtitle timeline above the controls
        {"playback/waveform", false},    // audio waveform behind the seek bar
        {"playback/metadata", false},    // title and media details over the video
        {"audio/visualizer", ""},        // audio-only media: spectrum, scope, … instead of the cover
        {"subs/default", ""},            // track picked on open: "", "off", "track:N" or "lang:xx"
        {"audio/default", ""},           // the same for audio
        {"audio/volume", 80},
        {"audio/muted", false},
        {"subs/autoload", true},
        {"subs/language", ""},
        {"library/videoFolders", QStringList{QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)}},
        {"library/musicFolders", QStringList{QStandardPaths::writableLocation(QStandardPaths::MusicLocation)}},
        {"network/caching", 1000},
        {"performance/fileCaching", 300},
        {"performance/skipLoop", 0},
        {"performance/fastSeek", false},
        {"record/dir", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/VMP Recordings"},
        {"snapshot/dir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/VMP Snapshots"},
        {"snapshot/format", "png"},
        {"advanced/args", ""},
        {"opensubtitles/apiKey", ""},
        {"opensubtitles/user", ""},
        {"opensubtitles/token", ""},   // session token only; the password is never stored
        {"opensubtitles/baseUrl", ""},
        {"opensubtitles/language", ""},
        {"music/artistMode", "artist"}, // or "albumartist"
        {"music/artistArt", "cover"},   // Artists show album covers, or "artist" pictures
        {"music/albumYearDesc", false},
    };
    return d;
}

Prefs &Prefs::instance()
{
    static Prefs p;
    return p;
}

QVariant Prefs::get(const QString &key) const
{
    QSettings s;
    return s.value(key, defaults().value(key));
}

void Prefs::set(const QString &key, const QVariant &value)
{
    QSettings s;
    if (s.value(key, defaults().value(key)) == value)
        return;
    s.setValue(key, value);
    emit changed(key);
}
