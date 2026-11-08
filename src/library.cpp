#include "datafile.h"
#include "library.h"
#include "disc.h"
#include "musicindex.h"
#include "prefs.h"

#include <QCollator>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImageReader>
#include <QProcess>
#include <QThreadPool>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

static const QSet<QString> kVideoExt = {"mp4", "mkv", "avi", "mov", "webm", "m4v", "wmv", "flv", "mpg",
                                       "mpeg", "ts", "m2ts", "mts", "3gp", "ogv", "vob", "divx", "asf",
                                       "rm", "rmvb", "f4v", "mxf", "y4m"};
static const QSet<QString> kAudioExt = {"mp3", "flac", "ogg", "oga", "opus", "m4a", "aac", "wav", "wma",
                                       "alac", "aiff", "aif", "ape", "mka", "wv", "mpc", "ac3", "dts",
                                       "mid", "midi", "spx", "tta", "m4b"};
static const QSet<QString> kPlaylistExt = {"m3u", "m3u8", "pls", "xspf"};
static const QSet<QString> kSubtitleExt = {"srt", "ass", "ssa", "sub", "vtt", "idx", "smi", "txt"};

static QString ext(const QString &path)
{
    const int dot = path.lastIndexOf('.');
    return dot < 0 ? QString() : path.mid(dot + 1).toLower();
}

bool Library::isVideoFile(const QString &path) { return kVideoExt.contains(ext(path)); }
bool Library::isAudioFile(const QString &path) { return kAudioExt.contains(ext(path)); }
bool Library::isPlaylistFile(const QString &path) { return kPlaylistExt.contains(ext(path)); }
bool Library::isSubtitleFile(const QString &path) { return kSubtitleExt.contains(ext(path)); }

bool Library::isUrl(const QString &location)
{
    const int colon = location.indexOf("://");
    return colon > 1 && !location.startsWith("file://");
}

QString Library::displayName(const QString &location)
{
    if (Disc::isDiscLocation(location))
        return Disc::displayName(location);
    if (isUrl(location)) {
        const QUrl url(location);
        const QString file = url.fileName();
        return file.isEmpty() ? url.host() + url.path() : file;
    }
    const QString path = location.startsWith("file://") ? QUrl(location).toLocalFile() : location;
    return QFileInfo(path).fileName();
}

EntryKind Library::kindFor(const QString &location)
{
    if (isUrl(location))
        return isAudioFile(QUrl(location).path()) ? EntryKind::Audio : EntryKind::Stream;
    if (isAudioFile(location))
        return EntryKind::Audio;
    return EntryKind::Video;
}

Library &Library::instance()
{
    static Library lib;
    return lib;
}

Library::Library()
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Library::save);
    load();
    // Once Konos is up: forget positions of files that were deleted.
    QTimer::singleShot(10000, this, &Library::pruneMissing);
}

void Library::pruneMissing()
{
    int removed = 0;
    for (auto it = m_positions.begin(); it != m_positions.end();) {
        const QString &loc = it.key();
        // Only local files whose folder is still there: a file on a drive
        // that isn't plugged in right now keeps its position.
        const QFileInfo fi(loc);
        if (!isUrl(loc) && fi.isAbsolute() && !Disc::isDiscLocation(loc) && QFileInfo(fi.absolutePath()).isDir() &&
            !fi.exists()) {
            it = m_positions.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed)
        scheduleSave();
}

static QString dataFile()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/library.json";
}

void Library::load()
{
    const QJsonObject root = DataFile::readJson(dataFile());
    for (const auto &v : root.value("recent").toArray())
        m_recent << v.toString();
    const QJsonObject pos = root.value("positions").toObject();
    for (auto it = pos.begin(); it != pos.end(); ++it) {
        const QJsonArray a = it.value().toArray();
        m_positions.insert(it.key(), {qint64(a.at(0).toDouble()), qint64(a.at(1).toDouble())});
    }
    for (const auto &v : root.value("playlists").toArray()) {
        const QJsonObject o = v.toObject();
        const QString name = o.value("name").toString();
        QStringList items;
        for (const auto &i : o.value("items").toArray())
            items << i.toString();
        m_playlists.insert(name, items);
        m_playlistOrder << name;
    }
    for (const auto &v : root.value("streams").toArray())
        m_streams << v.toString();
    const QJsonObject names = root.value("names").toObject();
    for (auto it = names.begin(); it != names.end(); ++it) {
        const QJsonArray a = it.value().toArray();
        m_names.insert(it.key(), {a.at(0).toString(), a.at(1).toString()});
    }
    for (const auto &v : root.value("channelLists").toArray())
        m_channelLists << v.toString();
    for (const auto &v : root.value("podcasts").toArray())
        m_podcasts << v.toString();
    for (const auto &v : root.value("pins").toArray()) {
        const QJsonObject o = v.toObject();
        m_pins << Pin{o.value("kind").toString(), o.value("target").toString(), o.value("name").toString(),
                      o.value("section").toInt()};
    }
    const QJsonObject meta = root.value("meta").toObject();
    for (auto it = meta.begin(); it != meta.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        m_meta.insert(it.key(), {o.value("title").toString(), o.value("artist").toString(), o.value("art").toString()});
    }
    const QJsonObject visits = root.value("folderVisits").toObject();
    for (auto it = visits.begin(); it != visits.end(); ++it)
        m_folderVisits.insert(it.key(), it.value().toInt());
}

void Library::scheduleSave()
{
    m_saveTimer.start();
}

void Library::save() const
{
    QJsonObject root;
    root.insert("recent", QJsonArray::fromStringList(m_recent));
    QJsonObject pos;
    for (auto it = m_positions.begin(); it != m_positions.end(); ++it)
        pos.insert(it.key(), QJsonArray{double(it.value().first), double(it.value().second)});
    root.insert("positions", pos);
    QJsonArray playlists;
    for (const QString &name : m_playlistOrder)
        playlists.append(QJsonObject{{"name", name}, {"items", QJsonArray::fromStringList(m_playlists.value(name))}});
    root.insert("playlists", playlists);
    root.insert("streams", QJsonArray::fromStringList(m_streams));
    root.insert("channelLists", QJsonArray::fromStringList(m_channelLists));
    QJsonObject names;
    for (auto it = m_names.begin(); it != m_names.end(); ++it) // only for what's still saved
        if (m_streams.contains(it.key()) || m_channelLists.contains(it.key()) || m_podcasts.contains(it.key()))
            names.insert(it.key(), QJsonArray{it->first, it->second});
    root.insert("names", names);
    root.insert("podcasts", QJsonArray::fromStringList(m_podcasts));
    QJsonArray pins;
    for (const Pin &p : m_pins)
        pins.append(QJsonObject{{"kind", p.kind}, {"target", p.target}, {"name", p.name}, {"section", p.section}});
    root.insert("pins", pins);
    QJsonObject meta;
    for (auto it = m_meta.begin(); it != m_meta.end(); ++it) {
        QJsonObject o;
        if (!it->title.isEmpty())
            o.insert("title", it->title);
        if (!it->artist.isEmpty())
            o.insert("artist", it->artist);
        if (!it->art.isEmpty())
            o.insert("art", it->art);
        meta.insert(it.key(), o);
    }
    root.insert("meta", meta);
    QJsonObject visits;
    for (auto it = m_folderVisits.begin(); it != m_folderVisits.end(); ++it)
        visits.insert(it.key(), it.value());
    root.insert("folderVisits", visits);
    DataFile::write(dataFile(), QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QList<LibraryEntry> Library::listFolder(const QString &dir, int filter) const
{
    QList<LibraryEntry> out;
    QDir d(dir);
    const QFileInfoList infos =
        d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable |
                            (prefs().flag("library/showHidden") ? QDir::Hidden : QDir::Filters()),
                        QDir::DirsFirst);
    for (const QFileInfo &fi : infos) {
        LibraryEntry e;
        e.path = fi.absoluteFilePath();
        e.name = fi.fileName();
        e.modified = fi.lastModified();
        if (fi.isDir()) {
            e.kind = EntryKind::Folder;
        } else {
            const QString p = fi.fileName();
            if (isVideoFile(p) && filter != 1)
                e.kind = EntryKind::Video;
            else if (isAudioFile(p) && filter != 0)
                e.kind = EntryKind::Audio;
            else
                continue;
            e.size = fi.size();
        }
        out << e;
    }
    return out;
}

QStringList Library::mediaInFolder(const QString &dir, int filter) const
{
    QStringList files;
    for (const LibraryEntry &e : listFolder(dir, filter))
        if (e.kind != EntryKind::Folder)
            files << e.path;
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(files.begin(), files.end(), [&](const QString &a, const QString &b) {
        return collator.compare(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
    });
    return files;
}

static QStringList parsePlaylistFile(const QString &path)
{
    QStringList out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    const QString base = QFileInfo(path).absolutePath();
    QTextStream in(&f);
    const QString e = ext(path);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (e == "pls") {
            if (!line.startsWith("File", Qt::CaseInsensitive))
                continue;
            line = line.section('=', 1);
        } else if (e == "xspf") {
            const int a = line.indexOf("<location>");
            const int b = line.indexOf("</location>");
            if (a < 0 || b < 0)
                continue;
            line = line.mid(a + 10, b - a - 10);
            line = QUrl::fromPercentEncoding(line.toUtf8());
        } else if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        if (line.startsWith("file://"))
            line = QUrl(line).toLocalFile();
        else if (!Library::isUrl(line) && QFileInfo(line).isRelative())
            line = QDir(base).absoluteFilePath(line);
        out << line;
    }
    return out;
}

QStringList Library::expand(const QStringList &locations) const
{
    QStringList out;
    for (const QString &loc : locations) {
        const QString path = loc.startsWith("file://") ? QUrl(loc).toLocalFile() : loc;
        const QFileInfo fi(path);
        if (!isUrl(path) && fi.isDir())
            out << mediaInFolder(path);
        else if (!isUrl(path) && isPlaylistFile(path))
            out << parsePlaylistFile(path);
        else
            out << path;
    }
    return out;
}

void Library::addRecent(const QString &location)
{
    if (!prefs().flag("privacy/history"))
        return;
    m_recent.removeAll(location);
    m_recent.prepend(location);
    while (m_recent.size() > 60)
        m_recent.removeLast();
    if (!isUrl(location) && QFileInfo(location).isFile())
        visitFolder(QFileInfo(location).absolutePath());
    scheduleSave();
    emit recentChanged();
}

void Library::setMeta(const QString &location, const Meta &meta)
{
    if (!prefs().flag("privacy/history") || location.isEmpty())
        return;
    const Meta old = m_meta.value(location);
    if (old.title == meta.title && old.artist == meta.artist && old.art == meta.art)
        return;
    m_meta.insert(location, meta);
    if (old.art != meta.art)
        Thumbnailer::instance().forget(location);
    // Only what's still in the history is worth keeping.
    if (m_meta.size() > 200)
        for (auto it = m_meta.begin(); it != m_meta.end();)
            it = m_recent.contains(it.key()) ? std::next(it) : m_meta.erase(it);
    scheduleSave();
    emit recentChanged();
}

void Library::visitFolder(const QString &dir)
{
    if (!prefs().flag("privacy/history") || dir.isEmpty())
        return;
    ++m_folderVisits[QDir::cleanPath(dir)];
    scheduleSave();
}

QStringList Library::frequentFolders(int count) const
{
    QList<QPair<int, QString>> ranked;
    for (auto it = m_folderVisits.begin(); it != m_folderVisits.end(); ++it)
        if (QFileInfo(it.key()).isDir())
            ranked.append({it.value(), it.key()});
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    QStringList out;
    for (int i = 0; i < qMin(count, int(ranked.size())); ++i)
        out << ranked[i].second;
    return out;
}

void Library::removeRecent(const QString &location)
{
    m_recent.removeAll(location);
    scheduleSave();
    emit recentChanged();
}

void Library::clearRecent()
{
    m_recent.clear();
    m_meta.clear();
    m_folderVisits.clear();
    scheduleSave();
    emit recentChanged();
}

qint64 Library::resumeTime(const QString &location) const
{
    return m_positions.value(location).first;
}

double Library::progress(const QString &location) const
{
    const auto p = m_positions.value(location);
    return p.second > 0 ? qBound(0.0, double(p.first) / double(p.second), 1.0) : 0.0;
}

void Library::setPosition(const QString &location, qint64 timeMs, qint64 lengthMs)
{
    if (!prefs().flag("privacy/positions") || lengthMs <= 0)
        return;
    // Paused, or the same spot as last time: nothing to save or redraw.
    if (const auto it = m_positions.constFind(location);
        it != m_positions.cend() && qAbs(it->first - timeMs) < 1000 && it->second == lengthMs)
        return;
    m_positions.insert(location, {timeMs, lengthMs});
    scheduleSave();
    emit positionsChanged();
}

void Library::setWatched(const QString &location, bool watched)
{
    const qint64 len = qMax<qint64>(1, m_positions.value(location).second);
    m_positions.insert(location, {watched ? len : 0, len});
    scheduleSave();
    emit positionsChanged();
}

void Library::clearPositions()
{
    m_positions.clear();
    scheduleSave();
    emit positionsChanged();
}

QStringList Library::playlistNames() const
{
    return m_playlistOrder;
}

QString Library::createPlaylist(const QString &name)
{
    QString unique = name.trimmed().isEmpty() ? tr("New playlist") : name.trimmed();
    const QString base = unique;
    for (int i = 2; m_playlists.contains(unique); ++i)
        unique = QString("%1 %2").arg(base).arg(i);
    m_playlists.insert(unique, {});
    m_playlistOrder << unique;
    scheduleSave();
    emit playlistsChanged();
    return unique;
}

void Library::renamePlaylist(const QString &from, const QString &to)
{
    const QString name = to.trimmed();
    if (!m_playlists.contains(from) || name.isEmpty() || m_playlists.contains(name))
        return;
    m_playlists.insert(name, m_playlists.take(from));
    m_playlistOrder[m_playlistOrder.indexOf(from)] = name;
    for (Pin &p : m_pins) // pinned playlists follow the new name
        if (p.kind == "playlist" && p.target == from) {
            p.target = name;
            p.name = name;
            emit pinsChanged();
        }
    scheduleSave();
    emit playlistsChanged();
}

void Library::deletePlaylist(const QString &name)
{
    m_playlists.remove(name);
    m_playlistOrder.removeAll(name);
    unpin("playlist", name);
    scheduleSave();
    emit playlistsChanged();
}

void Library::addToPlaylist(const QString &name, const QStringList &items)
{
    if (!m_playlists.contains(name))
        return;
    m_playlists[name] << items;
    scheduleSave();
    emit playlistsChanged();
}

void Library::removeFromPlaylist(const QString &name, int index)
{
    if (!m_playlists.contains(name) || index < 0 || index >= m_playlists[name].size())
        return;
    m_playlists[name].removeAt(index);
    scheduleSave();
    emit playlistsChanged();
}

void Library::addStream(const QString &url, const QString &name)
{
    m_streams.removeAll(url);
    m_streams.prepend(url);
    if (!name.isEmpty())
        m_names[url].first = name;
    scheduleSave();
    emit streamsChanged();
}

void Library::addChannelList(const QString &url, const QString &name)
{
    m_channelLists.removeAll(url);
    m_channelLists.prepend(url);
    if (!name.isEmpty())
        m_names[url].first = name;
    scheduleSave();
    emit streamsChanged();
}

void Library::removeChannelList(const QString &url)
{
    m_channelLists.removeAll(url);
    scheduleSave();
    emit streamsChanged();
}

void Library::addPodcast(const QString &feedUrl, const QString &name)
{
    if (!m_podcasts.contains(feedUrl))
        m_podcasts.append(feedUrl);
    if (!name.isEmpty())
        m_names[feedUrl].first = name;
    scheduleSave();
    emit streamsChanged();
}

void Library::removePodcast(const QString &feedUrl)
{
    m_podcasts.removeAll(feedUrl);
    scheduleSave();
    emit streamsChanged();
}

void Library::setStreamInfo(const QString &url, const QString &name, const QString &image)
{
    const QPair<QString, QString> info{name, image.isEmpty() ? m_names.value(url).second : image};
    if (m_names.value(url) == info)
        return;
    m_names.insert(url, info);
    scheduleSave();
}

bool Library::isPinned(const QString &kind, const QString &target) const
{
    return m_pins.contains(Pin{kind, target, {}, 0});
}

void Library::pin(const Pin &pin)
{
    if (m_pins.contains(pin))
        return;
    m_pins.append(pin);
    scheduleSave();
    emit pinsChanged();
}

void Library::unpin(const QString &kind, const QString &target)
{
    if (m_pins.removeAll(Pin{kind, target, {}, 0}) == 0)
        return;
    scheduleSave();
    emit pinsChanged();
}

void Library::movePin(int from, int to)
{
    if (from < 0 || from >= m_pins.size() || to < 0 || to >= m_pins.size() || from == to)
        return;
    m_pins.move(from, to);
    scheduleSave();
    emit pinsChanged();
}

void Library::removeStream(const QString &url)
{
    m_streams.removeAll(url);
    scheduleSave();
    emit streamsChanged();
}

// ---------------------------------------------------------------------------
// Thumbnailer

Thumbnailer &Thumbnailer::instance()
{
    static Thumbnailer t;
    return t;
}

Thumbnailer::Thumbnailer()
{
    m_memory.setMaxCost(96 * 1024); // KB: a few hundred thumbnails
    if (!QStandardPaths::findExecutable("ffmpegthumbnailer").isEmpty())
        m_tool = "ffmpegthumbnailer";
    else if (!QStandardPaths::findExecutable("ffmpeg").isEmpty())
        m_tool = "ffmpeg";
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/thumbs");
}

QString Thumbnailer::cacheFile(const QString &path) const
{
    const QFileInfo fi(path);
    const QByteArray key = path.toUtf8() + QByteArray::number(fi.lastModified().toSecsSinceEpoch());
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/thumbs/" +
           QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex() + ".jpg";
}

static QString folderCover(const QString &dir)
{
    static const QStringList names = {"cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg",
                                      "front.png", "Cover.jpg", "Folder.jpg", "AlbumArt.jpg", "albumart.jpg"};
    for (const QString &n : names) {
        const QString p = dir + "/" + n;
        if (QFile::exists(p))
            return p;
    }
    return {};
}

void Thumbnailer::forget(const QString &path)
{
    m_memory.remove(path);
    m_failed.remove(path);
    // Loads already under way may bring the old picture: drop them all (they
    // start again when next asked for).
    ++m_epoch;
    m_loading.clear();
}

namespace {
enum LoadResult { Loaded, NoSource, Failed };

// A picture at most 480 pixels across: album art is often several
// megapixels, and the JPEG decoder can downscale while it reads.
QImage readScaled(const QString &file)
{
    QImageReader reader(file);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.isValid() && (full.width() > 480 || full.height() > 480))
        reader.setScaledSize(full.scaled(480, 480, Qt::KeepAspectRatio));
    return reader.read();
}
} // namespace

QPixmap Thumbnailer::get(const QString &path)
{
    if (QPixmap *pm = m_memory.object(path))
        return *pm;
    if (m_failed.contains(path) || m_pending.contains(path) || m_loading.contains(path))
        return {};
    // What's known in memory is looked up here; the disk is the worker's job.
    QString metaArt, trackArt;
    if (Library::isAudioFile(path)) {
        metaArt = library().meta(path).art;
        if (const MusicIndex::Track *t = musicIndex().track(path))
            trackArt = t->art;
    }
    m_loading.insert(path);
    const quint64 epoch = m_epoch;
    QThreadPool::globalInstance()->start([this, path, metaArt, trackArt, epoch] {
        const QFileInfo fi(path);
        QString source;
        if (fi.isDir()) {
            // Music folders: album art if there is some (also one level down, for artist folders).
            source = folderCover(path);
            const QFileInfoList subs = source.isEmpty() ? QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)
                                                        : QFileInfoList();
            for (int i = 0; i < qMin<qsizetype>(subs.size(), 4) && source.isEmpty(); ++i)
                source = folderCover(subs[i].absoluteFilePath());
        } else if (QStringList{"jpg", "jpeg", "png", "webp"}.contains(fi.suffix().toLower())) {
            source = path; // a picture already (e.g. an artist image)
        } else if (Library::isAudioFile(path)) {
            source = !metaArt.isEmpty() && QFile::exists(metaArt)   ? metaArt
                     : !trackArt.isEmpty() && QFile::exists(trackArt) ? trackArt
                                                                      : folderCover(fi.absolutePath());
        }
        QImage image;
        int result;
        if (!source.isEmpty()) {
            image = readScaled(source);
            result = image.isNull() ? Failed : Loaded;
        } else if (fi.isDir()) {
            result = Failed;
        } else if (const QString cached = cacheFile(path); QFile::exists(cached)) {
            image = QImage(cached); // an empty marker means it couldn't be made
            result = image.isNull() ? Failed : Loaded;
        } else {
            result = NoSource; // make one with ffmpeg
        }
        QMetaObject::invokeMethod(this, [this, path, image, result, epoch] {
            if (epoch == m_epoch) // not forgotten meanwhile
                loaded(path, image, result);
        }, Qt::QueuedConnection);
    });
    return {};
}

void Thumbnailer::loaded(const QString &path, const QImage &image, int result)
{
    m_loading.remove(path);
    if (result == Loaded) {
        insert(path, QPixmap::fromImage(image));
        emit ready(path);
    } else if (result == NoSource && !m_tool.isEmpty() && !Library::isUrl(path)) {
        m_pending.insert(path);
        m_queue.enqueue(path);
        pump();
    } else {
        m_failed.insert(path);
    }
}

void Thumbnailer::insert(const QString &path, const QPixmap &pm)
{
    // Cost in KB, so the cache holds a size rather than a count.
    m_memory.insert(path, new QPixmap(pm), qMax<qsizetype>(1, qsizetype(pm.width()) * pm.height() * 4 / 1024));
}

QPixmap Thumbnailer::scaled(const QPixmap &pm, const QSize &size, qreal dpr, Qt::AspectRatioMode mode)
{
    if (pm.isNull() || size.isEmpty())
        return pm;
    static QCache<QString, QPixmap> cache(32 * 1024); // KB
    const QSize px = (QSizeF(size) * dpr).toSize();
    const QString key = QStringLiteral("%1|%2x%3|%4").arg(pm.cacheKey()).arg(px.width()).arg(px.height()).arg(int(mode));
    if (QPixmap *hit = cache.object(key))
        return *hit;
    QPixmap out = pm.scaled(px, mode, Qt::SmoothTransformation);
    if (mode == Qt::KeepAspectRatioByExpanding) // crop to the middle
        out = out.copy((out.width() - px.width()) / 2, (out.height() - px.height()) / 2, px.width(), px.height());
    out.setDevicePixelRatio(dpr);
    cache.insert(key, new QPixmap(out), qMax(1, out.width() * out.height() * 4 / 1024));
    return out;
}

void Thumbnailer::pump()
{
    while (m_running < 3 && !m_queue.isEmpty()) {
        const QString path = m_queue.dequeue();
        const QString out = cacheFile(path);
        auto *proc = new QProcess(this);
        proc->setProcessChannelMode(QProcess::MergedChannels);
        QStringList args;
        if (m_tool == "ffmpegthumbnailer")
            args = {"-i", path, "-o", out, "-s", "480", "-t", "12%", "-q", "8", "-c", "jpeg"};
        else
            args = {"-nostdin", "-loglevel", "error", "-y", "-ss", "20", "-i", path, "-frames:v", "1",
                    "-vf", "scale=480:-2", out};
        ++m_running;
        connect(proc, &QProcess::finished, this, [this, proc, path, out](int code, QProcess::ExitStatus status) {
            --m_running;
            proc->deleteLater();
            finished(path, out, code == 0 && status == QProcess::NormalExit && QFile::exists(out));
            pump();
        });
        proc->start(m_tool, args);
    }
}

void Thumbnailer::finished(const QString &path, const QString &out, bool ok)
{
    m_pending.remove(path);
    if (!ok) {
        m_failed.insert(path);
        // Leave an empty marker so we don't retry broken files every launch.
        QFile marker(out);
        if (marker.open(QIODevice::WriteOnly))
            marker.close();
        return;
    }
    m_loading.insert(path);
    const quint64 epoch = m_epoch;
    QThreadPool::globalInstance()->start([this, path, out, epoch] {
        const QImage image(out);
        QMetaObject::invokeMethod(this, [this, path, image, epoch] {
            if (epoch == m_epoch)
                loaded(path, image, image.isNull() ? Failed : Loaded);
        }, Qt::QueuedConnection);
    });
}

void Thumbnailer::clearCache()
{
    m_memory.clear();
    m_failed.clear();
    ++m_epoch;
    m_loading.clear();
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/thumbs");
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
}
