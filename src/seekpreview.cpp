#include "seekpreview.h"
#include "theme.h"

#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QThreadPool>
#include <QUrl>
#include <atomic>
#include <vlc/vlc.h>

// One hidden player for one file. Lives until stopped in the background;
// libVLC's video callbacks get a pointer to it.
struct SeekPreviewer::Session {
    SeekPreviewer *owner = nullptr;
    int id = 0;
    QString location;
    QSize size;
    QByteArray buffer;
    libvlc_media_player_t *mp = nullptr;
    std::atomic<bool> wanted{false};
};

static void *previewLock(void *opaque, void **planes)
{
    auto *s = static_cast<SeekPreviewer::Session *>(opaque);
    planes[0] = s->buffer.data();
    return nullptr;
}

static void previewDisplay(void *opaque, void *)
{
    // Called on libVLC's video thread; only the first picture after a request
    // is wanted. The next lock() comes from this same thread, so the buffer is
    // stable while it's copied.
    auto *s = static_cast<SeekPreviewer::Session *>(opaque);
    if (!s->wanted.exchange(false))
        return;
    const QImage image = QImage(reinterpret_cast<const uchar *>(s->buffer.constData()), s->size.width(),
                                s->size.height(), s->size.width() * 4, QImage::Format_RGB32)
                             .copy();
    QMetaObject::invokeMethod(s->owner, [owner = s->owner, id = s->id, image] { owner->onFrame(id, image); },
                              Qt::QueuedConnection);
}

SeekPreviewer::SeekPreviewer(QObject *parent) : QObject(parent)
{
    m_idle.setSingleShot(true);
    m_idle.setInterval(15000);
    connect(&m_idle, &QTimer::timeout, this, &SeekPreviewer::endSession);
    m_stuck.setSingleShot(true);
    connect(&m_stuck, &QTimer::timeout, this, [this] {
        // No picture: past the last frame (the player has ended) or a broken
        // file. Start over on the next request.
        const QString location = m_session ? m_session->location : QString();
        const QSize size = m_session ? m_session->size : QSize();
        const qint64 pending = m_pending;
        endSession();
        if (pending >= 0 && !location.isEmpty())
            startSession(location, size, pending);
    });
}

SeekPreviewer::~SeekPreviewer()
{
    endSession();
    QThreadPool::globalInstance()->waitForDone();
    if (m_vlc)
        libvlc_release(m_vlc);
}

bool SeekPreviewer::supports(const QString &location)
{
    const QUrl url(location);
    if (url.isValid() && url.scheme().size() > 1)
        return url.isLocalFile();
    return QFileInfo(location).isFile();
}

void SeekPreviewer::request(const QString &location, qint64 timeMs, const QSize &aspect, int width)
{
    if (!supports(location) || aspect.isEmpty())
        return;
    m_idle.stop();
    const qint64 key = bucket(qMax<qint64>(0, timeMs));
    if (m_cacheFor != location) {
        m_cache.clear();
        m_cacheFor = location;
    }
    if (const auto it = m_cache.constFind(key); it != m_cache.cend()) {
        emit frameReady(key, *it);
        return;
    }
    // Even dimensions keep libVLC's scaler happy.
    const QSize size(width & ~1, qMax(2, int(qRound(double(width) * aspect.height() / aspect.width())) & ~1));
    if (!m_session || m_session->location != location || m_session->size != size) {
        startSession(location, size, key);
        return;
    }
    m_pending = key;
    if (!m_busy)
        issue();
}

void SeekPreviewer::idle()
{
    m_pending = -1;
    m_idle.start();
}

void SeekPreviewer::startSession(const QString &location, const QSize &size, qint64 key)
{
    endSession();
    if (!m_vlc) {
        const char *args[] = {"--quiet", "--no-audio", "--no-spu", "--no-osd", "--no-video-title-show",
                              "--no-sub-autodetect-file", "--no-stats", "--no-lua"};
        m_vlc = libvlc_new(int(std::size(args)), args);
        if (!m_vlc)
            return;
    }
    const QUrl url(location);
    const QString path = url.isLocalFile() ? url.toLocalFile() : location;
    libvlc_media_t *media = libvlc_media_new_path(m_vlc, path.toUtf8().constData());
    if (!media)
        return;
    // Software decoding: frames are tiny and copied out anyway. Seeks are exact
    // (keyframe seeking can be seconds off): ~70 ms at 1080p, ~300 ms at 4K,
    // and requests made meanwhile collapse into the latest one.
    for (const char *opt : {":no-audio", ":no-spu", ":avcodec-hw=none"})
        libvlc_media_add_option(media, opt);
    libvlc_media_add_option(media, QStringLiteral(":start-time=%1").arg(key / 1000.0).toUtf8().constData());

    auto *s = new Session;
    s->owner = this;
    s->id = m_nextId++;
    s->location = location;
    s->size = size;
    s->buffer.resize(size.width() * size.height() * 4 + 64);
    s->mp = libvlc_media_player_new_from_media(media);
    libvlc_media_release(media);
    if (!s->mp) {
        delete s;
        return;
    }
    libvlc_video_set_callbacks(s->mp, previewLock, nullptr, previewDisplay, s);
    libvlc_video_set_format(s->mp, "RV32", unsigned(size.width()), unsigned(size.height()), unsigned(size.width() * 4));
    m_session = s;
    m_inflight = key;
    m_pending = -1;
    m_busy = true;
    s->wanted = true;
    libvlc_media_player_play(s->mp);
    m_stuck.start(5000); // opening takes longer than a seek
}

void SeekPreviewer::endSession()
{
    m_idle.stop();
    m_stuck.stop();
    m_busy = false;
    m_pending = -1;
    Session *s = std::exchange(m_session, nullptr);
    if (!s)
        return;
    // Stopping blocks until libVLC's threads finish; after that no callback can
    // reach the session, so it's freed there too.
    QThreadPool::globalInstance()->start([s] {
        libvlc_media_player_stop(s->mp);
        libvlc_media_player_release(s->mp);
        delete s;
    });
}

void SeekPreviewer::issue()
{
    if (!m_session || m_pending < 0)
        return;
    m_inflight = std::exchange(m_pending, -1);
    m_busy = true;
    m_session->wanted = true;
    libvlc_media_player_set_time(m_session->mp, m_inflight);
    m_stuck.start(2500);
}

void SeekPreviewer::onFrame(int id, const QImage &image)
{
    if (!m_session || id != m_session->id)
        return;
    m_stuck.stop();
    m_busy = false;
    if (m_cache.size() > 600)
        m_cache.clear();
    m_cache.insert(m_inflight, image);
    emit frameReady(m_inflight, image);
    if (m_pending >= 0 && m_pending != m_inflight) {
        issue();
    } else {
        m_pending = -1;
        libvlc_media_player_set_pause(m_session->mp, 1);
    }
}

// ---------------------------------------------------------------------------

SeekPreviewPopup::SeekPreviewPopup(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
}

void SeekPreviewPopup::showAt(const QPoint &anchor, const QString &time, const QSize &aspect)
{
    m_time = time;
    m_aspect = aspect;
    const int pad = 8, timeH = 30;
    QSize s;
    if (aspect.isEmpty()) {
        s = QSize(QFontMetrics(font()).horizontalAdvance(time) + 28, 32);
    } else {
        const QSize t = thumbSize(aspect);
        s = QSize(qMax(t.width(), 110) + 2 * pad, t.height() + pad + timeH);
    }
    const QWidget *p = parentWidget();
    const int x = qBound(8, anchor.x() - s.width() / 2, qMax(8, p->width() - s.width() - 8));
    setGeometry(x, anchor.y() - s.height() - 10, s.width(), s.height());
    if (!isVisible())
        show();
    raise();
    update();
}

void SeekPreviewPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette &c = theme().pal();
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(c.border, 1));
    p.setBrush(theme().tone(QColor(0x1c, 0x1c, 0x1c)));
    p.drawRoundedRect(card, 12, 12);

    QFont f = font();
    f.setPixelSize(13);
    f.setFeature(QFont::Tag("tnum"), 1);
    p.setFont(f);
    p.setPen(theme().tone(QColor(0xe6, 0xe6, 0xe6)));
    if (m_aspect.isEmpty()) {
        p.drawText(rect(), Qt::AlignCenter, m_time);
        return;
    }
    const int pad = 8;
    const QSize t = thumbSize(m_aspect);
    const QRectF thumb((width() - t.width()) / 2.0, pad, t.width(), t.height());
    QPainterPath clip;
    clip.addRoundedRect(thumb, 7, 7);
    p.fillPath(clip, c.videoBg);
    if (!m_image.isNull()) {
        p.save();
        p.setClipPath(clip);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(thumb, m_image);
        p.restore();
    }
    p.drawText(QRectF(0, thumb.bottom(), width(), 30), Qt::AlignCenter, m_time);
}
