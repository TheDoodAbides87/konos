#include "waveform.h"
#include "library.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <vlc/vlc.h>

struct Waveform::Job {
    QString path;
    std::vector<float> buckets; // loudest sample per 10 ms
    unsigned inBucket = 0;
    float current = 0;
};

namespace {
QSet<QString> &failed()
{
    static QSet<QString> set;
    return set;
}

// smem: VLC asks for a buffer, fills it, then hands it back.
void prerender(void *, uint8_t **buffer, size_t size) { *buffer = static_cast<uint8_t *>(malloc(size)); }

void postrender(void *data, uint8_t *buffer, int channels, int rate, unsigned samples, unsigned bits, size_t,
                int64_t)
{
    auto *job = static_cast<Waveform::Job *>(data);
    if (bits == 16 && channels > 0 && rate > 0) {
        const auto *s = reinterpret_cast<const int16_t *>(buffer);
        const unsigned per = unsigned(qMax(1, rate / 100));
        for (unsigned i = 0; i < samples; ++i) {
            const float v = std::abs(s[i * unsigned(channels)]) / 32768.0f;
            job->current = qMax(job->current, v);
            if (++job->inBucket == per) {
                job->buckets.push_back(job->current);
                job->current = 0;
                job->inBucket = 0;
            }
        }
    }
    free(buffer);
}

void onEnded(const libvlc_event_t *, void *opaque)
{
    QMetaObject::invokeMethod(static_cast<Waveform *>(opaque), "jobEnded", Qt::QueuedConnection);
}
} // namespace

Waveform &Waveform::instance()
{
    static Waveform *w = new Waveform; // never destroyed, like the music index
    return *w;
}

bool Waveform::supports(const QString &location)
{
    return !location.isEmpty() && !Library::isUrl(location) &&
           (Library::isAudioFile(location) || Library::isVideoFile(location)) && QFileInfo(location).isFile();
}

Waveform::Waveform()
{
    m_memory.setMaxCost(64);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] { finish(false); });
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/waveforms");
}

QString Waveform::cacheFile(const QString &path, const char *suffix) const
{
    const QFileInfo fi(path);
    const QByteArray key = path.toUtf8() + QByteArray::number(fi.lastModified().toSecsSinceEpoch()) +
                           QByteArray::number(fi.size());
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/waveforms/" +
           QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex() + suffix;
}

QVector<float> Waveform::peaks(const QString &path)
{
    if (QVector<float> *p = m_memory.object(path))
        return *p;
    if (!supports(path) || failed().contains(path))
        return {};
    QFile f(cacheFile(path));
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray data = f.readAll();
        if (data.size() == kBins) {
            QVector<float> out(kBins);
            for (int i = 0; i < kBins; ++i)
                out[i] = quint8(data[i]) / 255.0f;
            m_memory.insert(path, new QVector<float>(out));
            return out;
        }
    }
    if (!m_queue.contains(path) && !(m_job && m_job->path == path)) {
        m_queue.enqueue(path);
        if (!m_job)
            start();
    }
    return {};
}

QVector<float> Waveform::detail(const QString &path)
{
    if (path == m_detailPath)
        return m_detail;
    QFile f(cacheFile(path, ".hi"));
    if (!f.open(QIODevice::ReadOnly)) {
        // Outlines cached before detail existed: decode again to get it.
        if (m_memory.contains(path) || QFile::exists(cacheFile(path))) {
            m_memory.remove(path);
            QFile::remove(cacheFile(path));
            peaks(path);
        }
        return {};
    }
    const QByteArray data = f.readAll();
    m_detailPath = path;
    m_detail.resize(data.size());
    for (int i = 0; i < data.size(); ++i)
        m_detail[i] = quint8(data[i]) / 255.0f;
    return m_detail;
}

void Waveform::start()
{
    if (m_job || m_queue.isEmpty())
        return;
    if (!m_vlc) {
        const char *args[] = {"--quiet", "--no-lua", "--no-video", "--no-sub-autodetect-file", "--no-stats"};
        m_vlc = libvlc_new(int(std::size(args)), args);
        if (!m_vlc) {
            m_queue.clear();
            return;
        }
    }
    m_job = new Job;
    m_job->path = m_queue.dequeue();
    libvlc_media_t *media = libvlc_media_new_path(m_vlc, QFile::encodeName(m_job->path).constData());
    if (!media) {
        finish(false);
        return;
    }
    // Decode to 8 kHz mono and hand the samples over without waiting for the
    // clock ("no-time-sync"), so a whole song takes a fraction of a second.
    const QString sout = QString(":sout=#transcode{acodec=s16l,channels=1,samplerate=8000}"
                                 ":smem{audio-prerender-callback=%1,audio-postrender-callback=%2,audio-data=%3,"
                                 "no-time-sync}")
                             .arg(qulonglong(reinterpret_cast<quintptr>(&prerender)))
                             .arg(qulonglong(reinterpret_cast<quintptr>(&postrender)))
                             .arg(qulonglong(reinterpret_cast<quintptr>(m_job)));
    libvlc_media_add_option(media, sout.toUtf8().constData());
    libvlc_media_add_option(media, ":no-sout-video");
    libvlc_media_add_option(media, ":no-sout-spu");
    m_mp = libvlc_media_player_new_from_media(media);
    libvlc_media_release(media);
    if (!m_mp) {
        finish(false);
        return;
    }
    libvlc_event_manager_t *em = libvlc_media_player_event_manager(m_mp);
    libvlc_event_attach(em, libvlc_MediaPlayerEndReached, onEnded, this);
    libvlc_event_attach(em, libvlc_MediaPlayerEncounteredError, onEnded, this);
    // Videos take longer: the whole file is read for its sound.
    m_timeout.start(Library::isVideoFile(m_job->path) ? 600000 : 60000);
    libvlc_media_player_play(m_mp);
}

void Waveform::jobEnded()
{
    if (m_job)
        finish(true);
}

void Waveform::finish(bool ok)
{
    m_timeout.stop();
    if (m_mp) {
        libvlc_event_manager_t *em = libvlc_media_player_event_manager(m_mp);
        libvlc_event_detach(em, libvlc_MediaPlayerEndReached, onEnded, this);
        libvlc_event_detach(em, libvlc_MediaPlayerEncounteredError, onEnded, this);
        libvlc_media_player_stop(m_mp); // joins the decoder, so the job is ours again
        libvlc_media_player_release(m_mp);
        m_mp = nullptr;
    }
    Job *job = m_job;
    m_job = nullptr;
    if (!job)
        return;
    if (job->inBucket)
        job->buckets.push_back(job->current);

    const auto &b = job->buckets;
    if (!ok || b.size() < 10) {
        failed().insert(job->path);
    } else {
        QVector<float> out(kBins, 0.0f);
        for (int i = 0; i < kBins; ++i) {
            const size_t from = size_t(double(i) * b.size() / kBins);
            const size_t to = qMax(from + 1, size_t(double(i + 1) * b.size() / kBins));
            for (size_t j = from; j < to && j < b.size(); ++j)
                out[i] = qMax(out[i], b[j]);
        }
        // Scale to the loud parts (ignoring a few spikes) and lift quiet ones a
        // little so soft passages still show.
        QVector<float> sorted = out;
        std::sort(sorted.begin(), sorted.end());
        const float top = qMax(0.02f, sorted[int(kBins * 0.98)]);
        QByteArray data(kBins, 0);
        for (int i = 0; i < kBins; ++i) {
            out[i] = std::pow(qMin(1.0f, out[i] / top), 0.75f);
            data[i] = char(quint8(std::lround(out[i] * 255)));
        }
        QFile f(cacheFile(job->path));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(data);
        // Detailed outline: 10 ms buckets folded to kDetailRate per second, same scaling.
        const size_t per = size_t(100 / kDetailRate);
        QByteArray hi(int((b.size() + per - 1) / per), 0);
        for (int i = 0; i < hi.size(); ++i) {
            float v = 0;
            for (size_t j = size_t(i) * per; j < size_t(i + 1) * per && j < b.size(); ++j)
                v = qMax(v, b[j]);
            hi[i] = char(quint8(std::lround(std::pow(qMin(1.0f, v / top), 0.75f) * 255)));
        }
        QFile hf(cacheFile(job->path, ".hi"));
        if (hf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            hf.write(hi);
        if (m_detailPath == job->path)
            m_detailPath.clear();
        m_memory.insert(job->path, new QVector<float>(out));
        emit ready(job->path);
    }
    delete job;
    start();
}
