#include "datafile.h"
#include "player.h"
#include "durations.h"
#include "library.h"
#include "musicindex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <vlc/vlc.h>

namespace {
QString storeFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/durations.json";
}

void onParsed(const libvlc_event_t *event, void *opaque)
{
    QMetaObject::invokeMethod(static_cast<Durations *>(opaque), "onParsedQueued", Qt::QueuedConnection,
                              Q_ARG(void *, event->p_obj));
}
} // namespace

Durations &Durations::instance()
{
    static Durations *d = new Durations; // never destroyed, like the music index
    return *d;
}

Durations::Durations()
{
    const QJsonObject o = DataFile::readJson(storeFile());
    for (auto it = o.begin(); it != o.end(); ++it)
        m_known.insert(it.key(), qint64(it.value().toDouble()));
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(2000);
    connect(&m_saveTimer, &QTimer::timeout, this, &Durations::save);
}

QString Durations::key(const QString &location) const
{
    const QFileInfo fi(location);
    return location + '\n' + QString::number(fi.lastModified().toSecsSinceEpoch());
}

qint64 Durations::duration(const QString &location)
{
    if (const MusicIndex::Track *t = musicIndex().track(location); t && t->duration > 0)
        return t->duration;
    if (Library::isUrl(location) || !QFileInfo(location).isFile())
        return 0; // streams have no fixed length
    const QString k = key(location);
    if (const auto it = m_known.constFind(k); it != m_known.constEnd())
        return *it;
    if (!m_queued.contains(location)) {
        m_queued.insert(location);
        m_queue.enqueue(location);
        pump();
    }
    return -1;
}

void Durations::pump()
{
    while (m_parsing.size() < 2 && !m_queue.isEmpty()) {
        if (!m_vlc) {
            m_vlc = Player::parserInstance(); // shared with the music index
            if (!m_vlc) {
                m_queue.clear();
                return;
            }
        }
        const QString path = m_queue.dequeue();
        libvlc_media_t *media = libvlc_media_new_path(m_vlc, QFile::encodeName(path).constData());
        if (!media)
            continue;
        m_parsing.insert(media, path);
        libvlc_event_attach(libvlc_media_event_manager(media), libvlc_MediaParsedChanged, onParsed, this);
        if (libvlc_media_parse_with_options(media, libvlc_media_parse_local, 8000) != 0) {
            libvlc_event_detach(libvlc_media_event_manager(media), libvlc_MediaParsedChanged, onParsed, this);
            m_parsing.remove(media);
            libvlc_media_release(media);
        }
    }
}

void Durations::onParsedQueued(void *p)
{
    auto *media = static_cast<libvlc_media_t *>(p);
    const auto it = m_parsing.find(media);
    if (it == m_parsing.end())
        return; // already handled
    const QString path = it.value();
    m_parsing.erase(it);
    libvlc_event_detach(libvlc_media_event_manager(media), libvlc_MediaParsedChanged, onParsed, this);
    const qint64 ms = libvlc_media_get_duration(media);
    libvlc_media_release(media);
    m_known.insert(key(path), qMax<qint64>(0, ms));
    m_queued.remove(path);
    m_saveTimer.start();
    emit ready(path);
    pump();
}

void Durations::save()
{
    QJsonObject o;
    for (auto it = m_known.constBegin(); it != m_known.constEnd(); ++it)
        if (QFileInfo::exists(it.key().section('\n', 0, 0)))
            o.insert(it.key(), double(it.value()));
    DataFile::write(storeFile(), QJsonDocument(o).toJson(QJsonDocument::Compact));
}
