#include "datafile.h"
#include "player.h"
#include "musicindex.h"
#include "library.h"
#include "prefs.h"

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <vlc/vlc.h>

namespace {
const int kMaxInFlight = 4;

QCollator &collator()
{
    static QCollator c = [] {
        QCollator x;
        x.setNumericMode(true);
        x.setCaseSensitivity(Qt::CaseInsensitive);
        return x;
    }();
    return c;
}

bool before(const QString &a, const QString &b) { return collator().compare(a, b) < 0; }

QString indexFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/music.json";
}

QStringList musicRoots()
{
    QStringList out;
    for (const QString &r : prefs().list("library/musicFolders"))
        if (!r.isEmpty() && QFileInfo(r).isDir())
            out << QDir::cleanPath(r);
    return out;
}
} // namespace

QString MusicIndex::Track::displayTitle() const
{
    return title.isEmpty() ? QFileInfo(path).completeBaseName() : title;
}

QString MusicIndex::Track::displayArtist() const
{
    return artist.isEmpty() ? QObject::tr("Unknown Artist") : artist;
}

QString MusicIndex::Track::displayAlbum() const
{
    return album.isEmpty() ? QObject::tr("Unknown Album") : album;
}

QString MusicIndex::Track::displayGenre() const
{
    return genre.isEmpty() ? QObject::tr("Unknown Genre") : genre;
}

QString MusicIndex::Track::displayAlbumArtist() const
{
    return albumArtist.isEmpty() ? displayArtist() : albumArtist;
}

QString MusicIndex::Track::label() const
{
    return artist.isEmpty() ? displayTitle() : displayTitle() + " - " + artist;
}

MusicIndex &MusicIndex::instance()
{
    // Never destroyed: libVLC may still be reading files when the app quits.
    static MusicIndex *index = new MusicIndex;
    return *index;
}

MusicIndex::MusicIndex()
{
    m_notify.setSingleShot(true);
    m_notify.setInterval(400);
    connect(&m_notify, &QTimer::timeout, this, &MusicIndex::changed);
    m_save.setSingleShot(true);
    m_save.setInterval(2000);
    connect(&m_save, &QTimer::timeout, this, &MusicIndex::save);
    connect(&prefs(), &Prefs::changed, this, [this](const QString &key) {
        if (key == "library/musicFolders" && m_loaded)
            rescan();
    });
}

QString MusicIndex::fieldName(Field field)
{
    switch (field) {
    case Album: return tr("Albums");
    case Artist: return tr("Artists");
    case Genre: return tr("Genres");
    case AlbumArtist: return tr("Album Artists");
    }
    return {};
}

void MusicIndex::ensureScanned()
{
    if (m_loaded)
        return;
    m_loaded = true;
    load();
    rescan();
}

void MusicIndex::load()
{
    const QJsonObject root = DataFile::readJson(indexFile());
    // Version 1 didn't store years: keep showing it, but read every file again.
    const bool stale = root.value("version").toInt() < 2;
    const QJsonArray arr = root.value("tracks").toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Track t;
        t.path = o.value("p").toString();
        t.title = o.value("t").toString();
        t.artist = o.value("a").toString();
        t.albumArtist = o.value("aa").toString();
        t.album = o.value("al").toString();
        t.genre = o.value("g").toString();
        t.art = o.value("art").toString();
        t.number = o.value("n").toInt();
        t.year = o.value("y").toInt();
        t.duration = qint64(o.value("d").toDouble());
        t.mtime = stale ? -1 : qint64(o.value("m").toDouble());
        if (!t.path.isEmpty())
            m_tracks.insert(t.path, t);
    }
}

void MusicIndex::save() const
{
    QJsonArray arr;
    for (const Track &t : m_tracks) {
        QJsonObject o{{"p", t.path}, {"m", double(t.mtime)}};
        auto put = [&o](const char *k, const QString &v) {
            if (!v.isEmpty())
                o.insert(k, v);
        };
        put("t", t.title);
        put("a", t.artist);
        put("aa", t.albumArtist);
        put("al", t.album);
        put("g", t.genre);
        put("art", t.art);
        if (t.number)
            o.insert("n", t.number);
        if (t.year)
            o.insert("y", t.year);
        if (t.duration)
            o.insert("d", double(t.duration));
        arr.append(o);
    }
    DataFile::write(indexFile(), QJsonDocument(QJsonObject{{"version", 2}, {"tracks", arr}}).toJson(QJsonDocument::Compact));
}

void MusicIndex::rescan()
{
    m_loaded = true;
    if (m_scanning) {
        m_rescanAgain = true;
        return;
    }
    m_scanning = true;
    m_notify.start();
    // Walking a big collection takes a moment; do it off the UI thread.
    const QStringList roots = musicRoots();
    QThread *walker = QThread::create([this, roots] {
        QHash<QString, qint64> files;
        for (const QString &root : roots) {
            QDirIterator it(root, QDir::Files | QDir::Readable, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
            while (it.hasNext()) {
                const QString p = it.next();
                if (Library::isAudioFile(p) && !files.contains(p))
                    files.insert(p, it.fileInfo().lastModified().toSecsSinceEpoch());
            }
        }
        QMetaObject::invokeMethod(this, [this, files] { scanned(files); }, Qt::QueuedConnection);
    });
    connect(walker, &QThread::finished, walker, &QObject::deleteLater);
    walker->start(QThread::LowPriority);
}

void MusicIndex::scanned(const QHash<QString, qint64> &files)
{
    m_scanning = false;
    bool dropped = false;
    for (auto it = m_tracks.begin(); it != m_tracks.end();) {
        if (!files.contains(it.key())) {
            it = m_tracks.erase(it);
            dropped = true;
        } else {
            ++it;
        }
    }
    m_queue.clear();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto known = m_tracks.constFind(it.key());
        if (known == m_tracks.constEnd() || known->mtime != it.value())
            m_queue.enqueue({it.key(), it.value()});
    }
    if (dropped)
        m_save.start();
    m_notify.start();
    if (m_rescanAgain) {
        m_rescanAgain = false;
        rescan();
        return;
    }
    pump();
}

static void onParsed(const libvlc_event_t *event, void *opaque)
{
    if (event->u.media_parsed_changed.new_status == 0)
        return;
    auto *self = static_cast<MusicIndex *>(opaque);
    auto *media = static_cast<libvlc_media_t *>(event->p_obj);
    QMetaObject::invokeMethod(self, "onParsedQueued", Qt::QueuedConnection, Q_ARG(void *, media));
}

void MusicIndex::pump()
{
    if (m_queue.isEmpty())
        return;
    if (!m_vlc) {
        m_vlc = Player::parserInstance(); // shared with Durations
        if (!m_vlc) {
            m_queue.clear();
            return;
        }
    }
    while (m_inFlight < kMaxInFlight && !m_queue.isEmpty()) {
        const auto job = m_queue.dequeue();
        libvlc_media_t *media = libvlc_media_new_path(m_vlc, QFile::encodeName(job.first).constData());
        if (!media)
            continue;
        m_parsing.insert(media, job);
        ++m_inFlight;
        libvlc_event_attach(libvlc_media_event_manager(media), libvlc_MediaParsedChanged, onParsed, this);
        if (libvlc_media_parse_with_options(
                media, libvlc_media_parse_flag_t(libvlc_media_parse_local | libvlc_media_fetch_local), 10000) != 0) {
            libvlc_event_detach(libvlc_media_event_manager(media), libvlc_MediaParsedChanged, onParsed, this);
            m_parsing.remove(media);
            libvlc_media_release(media);
            --m_inFlight;
        }
    }
}

void MusicIndex::onParsedQueued(void *media)
{
    parsed(static_cast<libvlc_media_t *>(media));
}

void MusicIndex::parsed(libvlc_media_t *media)
{
    if (!m_parsing.contains(media))
        return;
    const auto job = m_parsing.take(media);
    --m_inFlight;
    auto meta = [media](libvlc_meta_t m) {
        char *v = libvlc_media_get_meta(media, m);
        const QString s = v ? QString::fromUtf8(v).trimmed() : QString();
        libvlc_free(v);
        return s;
    };
    Track t;
    t.path = job.first;
    t.mtime = job.second;
    t.title = meta(libvlc_meta_Title);
    // libVLC reports the file name as the title when there's no tag.
    if (t.title == QFileInfo(t.path).fileName())
        t.title.clear();
    t.artist = meta(libvlc_meta_Artist);
    t.albumArtist = meta(libvlc_meta_AlbumArtist);
    t.album = meta(libvlc_meta_Album);
    t.genre = meta(libvlc_meta_Genre);
    t.number = meta(libvlc_meta_TrackNumber).section('/', 0, 0).toInt();
    // "2019", "2019-05-01" or "2019-05-01T00:00:00Z": take the year.
    static const QRegularExpression yearRe("(\\d{4})");
    const QRegularExpressionMatch ym = yearRe.match(meta(libvlc_meta_Date));
    if (ym.hasMatch() && ym.captured(1).toInt() > 1000)
        t.year = ym.captured(1).toInt();
    const QString art = meta(libvlc_meta_ArtworkURL);
    if (art.startsWith("file://"))
        t.art = QUrl(art).toLocalFile();
    const libvlc_time_t d = libvlc_media_get_duration(media);
    t.duration = d > 0 ? d : 0;
    libvlc_event_detach(libvlc_media_event_manager(media), libvlc_MediaParsedChanged, onParsed, this);
    libvlc_media_release(media);

    const Track old = m_tracks.value(t.path);
    m_tracks.insert(t.path, t);
    if (old.art != t.art)
        Thumbnailer::instance().forget(t.path);
    m_save.start();
    if (!m_notify.isActive())
        m_notify.start();
    pump();
    if (!isScanning())
        m_notify.start(50);
}

const MusicIndex::Track *MusicIndex::track(const QString &path) const
{
    const auto it = m_tracks.constFind(path);
    return it == m_tracks.constEnd() ? nullptr : &*it;
}

QList<MusicIndex::Track> MusicIndex::tracks() const
{
    QList<Track> out = m_tracks.values();
    std::sort(out.begin(), out.end(), [](const Track &a, const Track &b) {
        const int c = collator().compare(a.displayTitle(), b.displayTitle());
        return c ? c < 0 : before(a.path, b.path);
    });
    return out;
}

static QString displayFor(const MusicIndex::Track &t, MusicIndex::Field field)
{
    switch (field) {
    case MusicIndex::Album: return t.displayAlbum();
    case MusicIndex::Artist: return t.displayArtist();
    case MusicIndex::Genre: return t.displayGenre();
    case MusicIndex::AlbumArtist: return t.displayAlbumArtist();
    }
    return {};
}

QString MusicIndex::groupKey(const Track &t, Field field)
{
    switch (field) {
    case Album: return t.album + '\n' + t.albumArtist;
    case Artist: return t.artist;
    case Genre: return t.genre;
    case AlbumArtist: return t.albumArtist.isEmpty() ? t.artist : t.albumArtist;
    }
    return {};
}

QList<MusicIndex::Group> MusicIndex::groups(Field field, const Within &within) const
{
    QHash<QString, Group> map;
    for (const Track &t : m_tracks) {
        if (within.active && groupKey(t, within.field) != within.key)
            continue;
        const QString key = groupKey(t, field);
        Group &g = map[key];
        if (!g.count) {
            g.key = key;
            g.name = displayFor(t, field);
            g.cover = t.path;
            g.artist = !t.albumArtist.isEmpty() ? t.albumArtist : t.artist;
        } else if (field == Album && t.albumArtist.isEmpty() && t.artist != g.artist) {
            g.artist = tr("Various Artists");
        }
        // Prefer a track with a cover, then the album's first track.
        const Track *cur = track(g.cover);
        if (cur && ((cur->art.isEmpty() && !t.art.isEmpty()) ||
                    (cur->art.isEmpty() == t.art.isEmpty() && t.number && (!cur->number || t.number < cur->number))))
            g.cover = t.path;
        if (t.year && (!g.year || t.year < g.year))
            g.year = t.year;
        ++g.count;
    }
    QList<Group> out = map.values();
    std::sort(out.begin(), out.end(), [](const Group &a, const Group &b) {
        // "Unknown …" (no tag) goes last.
        const bool ua = a.key.section('\n', 0, 0).isEmpty(), ub = b.key.section('\n', 0, 0).isEmpty();
        if (ua != ub)
            return ub;
        const int c = collator().compare(a.name, b.name);
        return c ? c < 0 : before(a.artist, b.artist);
    });
    return out;
}

QHash<QString, QStringList> MusicIndex::samplePaths(Field field, int n) const
{
    QHash<QString, QStringList> out;
    for (const Track &t : m_tracks) {
        QStringList &paths = out[groupKey(t, field)];
        if (paths.size() < n)
            paths << t.path;
    }
    return out;
}

QString MusicIndex::groupName(Field field, const QString &key) const
{
    for (const Track &t : m_tracks)
        if (groupKey(t, field) == key)
            return displayFor(t, field);
    return key.section('\n', 0, 0);
}

QList<MusicIndex::Track> MusicIndex::tracksIn(Field field, const QString &key, const Within &within) const
{
    QList<Track> out;
    for (const Track &t : m_tracks)
        if (groupKey(t, field) == key && (!within.active || groupKey(t, within.field) == within.key))
            out << t;
    // An album's year is its earliest track's.
    QHash<QString, int> years;
    for (const Track &t : out) {
        int &y = years[t.album + '\n' + t.albumArtist];
        if (t.year && (!y || t.year < y))
            y = t.year;
    }
    std::sort(out.begin(), out.end(), [field, &years](const Track &a, const Track &b) {
        if (field == Genre && a.artist != b.artist)
            return before(a.displayArtist(), b.displayArtist());
        if (field != Album && a.album != b.album) {
            const int ya = years.value(a.album + '\n' + a.albumArtist), yb = years.value(b.album + '\n' + b.albumArtist);
            if (ya != yb)
                return (ya ? ya : INT_MAX) < (yb ? yb : INT_MAX);
            return before(a.displayAlbum(), b.displayAlbum());
        }
        if (a.number != b.number)
            return (a.number ? a.number : INT_MAX) < (b.number ? b.number : INT_MAX);
        const int c = collator().compare(a.displayTitle(), b.displayTitle());
        return c ? c < 0 : before(a.path, b.path);
    });
    return out;
}
