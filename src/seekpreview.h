#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QTimer>
#include <QWidget>

struct libvlc_instance_t;

// Thumbnails for the seek bar. A hidden libVLC player decodes small frames of
// the current file at the hovered time: it seeks (to the nearest keyframe),
// takes the first picture and pauses again until the next request.
class SeekPreviewer : public QObject {
    Q_OBJECT
public:
    explicit SeekPreviewer(QObject *parent = nullptr);
    ~SeekPreviewer() override;

    // Only local files: a second network connection or a disc reader would
    // fight with the main player.
    static bool supports(const QString &location);

    // Asks for a frame near timeMs, `width` pixels wide. `aspect` is the
    // video's display size. frameReady() follows, at once if it's cached.
    void request(const QString &location, qint64 timeMs, const QSize &aspect, int width);
    // Stops decoding soon (frames already made stay cached for the file).
    void idle();

    // Plumbing for libVLC's video callbacks.
    struct Session;
    void onFrame(int id, const QImage &image);

signals:
    void frameReady(qint64 timeMs, const QImage &image);

private:
    void startSession(const QString &location, const QSize &size, qint64 key);
    void endSession();
    void issue();
    static qint64 bucket(qint64 ms) { return ms / 1000 * 1000; }

    libvlc_instance_t *m_vlc = nullptr;
    Session *m_session = nullptr;
    int m_nextId = 1;
    bool m_busy = false;
    qint64 m_inflight = -1;
    qint64 m_pending = -1;
    QString m_cacheFor;
    QHash<qint64, QImage> m_cache;
    QTimer m_idle;  // tears the hidden player down a while after the last hover
    QTimer m_stuck; // gives up on a seek that produced no picture
};

// The card above the seek bar: thumbnail (when there is one) and the time.
class SeekPreviewPopup : public QWidget {
    Q_OBJECT
public:
    explicit SeekPreviewPopup(QWidget *parent);
    // Thumbnail size (logical pixels) for a video of this display size.
    static QSize thumbSize(const QSize &aspect) { return aspect.scaled(224, 224, Qt::KeepAspectRatio); }

    // `aspect` empty: audio, show just the time.
    void showAt(const QPoint &anchor, const QString &time, const QSize &aspect);
    void setImage(const QImage &image) { m_image = image; update(); }
    void clearImage() { m_image = {}; update(); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_time;
    QSize m_aspect;
    QImage m_image;
};
