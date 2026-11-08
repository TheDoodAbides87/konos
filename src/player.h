#pragma once

#include "effects.h"

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <functional>
#include <vector>

struct libvlc_instance_t;
struct libvlc_media_player_t;
struct libvlc_media_t;
struct libvlc_event_t;
struct libvlc_renderer_item_t;

// Wraps a libVLC instance + media player. Video is rendered into memory
// (vmem callbacks) and painted by VideoWidget, which keeps the UI fully custom
// and works the same on X11 and Wayland.
class Player : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Opening, Buffering, Playing, Paused, Stopped, Ended, Error };
    Q_ENUM(State)

    struct Track {
        int id;
        QString name;
    };

    explicit Player(QObject *parent = nullptr);
    ~Player() override;

    bool isValid() const { return m_mp != nullptr; }
    QString lastError() const { return m_error; }

    // Opens a local path or URL. startMs > 0 starts from that position.
    void open(const QString &location, qint64 startMs = 0, bool startPaused = false);
    void play();
    void pause();
    void togglePause();
    void stop();

    State state() const { return m_state; }
    bool isPlaying() const { return m_state == Playing || m_state == Buffering || m_state == Opening; }
    bool hasMedia() const { return !m_location.isEmpty(); }
    QString location() const { return m_location; }
    QString title() const;
    QString artist() const { return m_artist; }
    QString album() const { return m_album; }
    QString artworkPath() const { return m_artwork; }
    bool hasVideo() const;

    qint64 time() const;
    qint64 length() const { return m_length; }
    float position() const;
    bool isSeekable() const;
    void seek(qint64 ms);
    void seekRelative(qint64 deltaMs);

    int volume() const { return m_volume; }
    void setVolume(int volume);
    bool isMuted() const { return m_muted; }
    void setMuted(bool muted);

    float rate() const { return m_rate; }
    void setRate(float rate);

    QList<Track> audioTracks() const;
    QList<Track> subtitleTracks() const;
    // Subtitle tracks split by where they come from: inside the file, or a
    // separate subtitle file (found next to it or added).
    struct SubtitleTrack {
        int id;
        QString name;
        bool external;
        QString file; // external: the subtitle file's local path
    };
    QList<SubtitleTrack> subtitleTrackList() const;
    // Where sound goes (speakers, headphones…). Empty id = the system default.
    struct AudioDevice {
        QString id, name;
    };
    QList<AudioDevice> audioDevices() const;
    QString audioDevice() const;
    void setAudioDevice(const QString &id);
    QList<Track> videoTracks() const;
    // What libVLC knows about each track of the current media: language and
    // codec, plus picture or sound details.
    struct TrackInfo {
        enum Type { Audio, Video, Subtitle, Other } type = Other;
        int id = -1;
        QString language; // ISO 639 code as tagged in the file, may be empty
        QString codec;    // e.g. "H264", "AAC"
        quint32 fourcc = 0;
        QString description;
        int width = 0, height = 0;
        double fps = 0;
        int channels = 0, sampleRate = 0;
        quint32 bitrate = 0; // bits per second, 0 if unknown
    };
    QList<TrackInfo> trackInfo() const;
    int audioTrack() const;
    int subtitleTrack() const;
    int videoTrack() const;
    void setAudioTrack(int id);
    void setSubtitleTrack(int id);
    void setVideoTrack(int id);
    bool addSubtitleFile(const QString &path);
    bool addAudioFile(const QString &path); // an extra audio track (a dub), selected

    qint64 audioDelay() const { return m_audioDelay; }
    void setAudioDelay(qint64 ms);
    qint64 subtitleDelay() const { return m_subDelay; }
    void setSubtitleDelay(qint64 ms);
    float subtitleSpeed() const { return m_subSpeed; }
    void setSubtitleSpeed(float factor);
    float subtitleDurationFactor() const { return m_subDuration; }
    void setSubtitleDurationFactor(float factor);

    // Subtitle position: pixels up from the bottom of the picture (applies live).
    void setSubtitleMargin(int px);
    // Audio-only media: a picture to show instead of the cover ("spectrum",
    // "scope", "spectrometer", "vuMeter" or "goom"); empty for none. Applies
    // from the next item opened.
    void setVisualizer(const QString &mode) { m_visualizer = mode; }
    QString visualizer() const { return m_visualizer; }

    int stereoMode() const;
    void setStereoMode(int mode);

    void setDeinterlace(const QString &mode);
    QString deinterlace() const { return m_deinterlace; }

    bool isRecording() const { return m_recording; }
    void setRecording(bool on);
    QString recordingPath() const { return m_recordPath; }

    // Reopens the current media at the current position, applying settings that
    // libVLC only reads when outputs are created.
    void restart();
    // Recreates the libVLC instance (for instance-wide options such as caching).
    void reinitialize();

    // Casting: play on a renderer (a Chromecast) found by a discoverer on
    // instance(); nullptr plays here again. The item restarts where it was.
    libvlc_instance_t *instance() const { return m_vlc; }
    void setRenderer(libvlc_renderer_item_t *item);
    QString renderer() const { return m_rendererName; } // empty when playing here
    // Extra VLC options for one location (capture devices), used whenever it opens.
    void setMediaOptions(const QString &location, const QStringList &options) { m_mediaOptions[location] = options; }

    // The latest decoded picture: I420 planes at the decoded size. The display
    // size has the sample aspect ratio applied; the GPU scales between them.
    struct Frame {
        const uchar *planes[3];
        int pitches[3];
        QSize size;
        QSize displaySize;
        quint64 serial; // changes with every new picture
        bool bt709;
    };
    // Calls fn with the most recent frame while it is locked (no-op if none).
    void withFrame(const std::function<void(const Frame &)> &fn) const;
    bool hasFrame() const;
    QSize videoSize() const; // display size
    QImage snapshot() const; // RGB copy of the current frame at display size

    // Discs (DVD menus) and titles/chapters, which any media may have.
    bool isDisc() const;
    bool inDiscMenu() const { return m_inMenu; }
    enum class Nav { Activate, Up, Down, Left, Right };
    void navigate(Nav nav);
    bool discMenu();                                   // jumps to the DVD/Blu-ray menu
    void pointerOnVideo(QPointF framePos, bool click); // DVD menu buttons under the mouse
    QList<Track> titles() const;
    int currentTitle() const;
    void setTitle(int index);
    QList<Track> chapters() const;
    struct Chapter {
        int id;
        QString name;
        qint64 start; // ms
    };
    QList<Chapter> chapterList() const; // without times in the names; empty in a disc menu
    int currentChapter() const;
    void setChapter(int index);
    bool nextChapter();
    bool previousChapter();

    static QList<QPair<QString, QString>> filterModules(bool audio);
    // A second engine, quiet and with no audio or video output, for reading
    // files' tags and lengths in the background (shared, made on first use).
    static libvlc_instance_t *parserInstance();

    // Prints libVLC's object tree and active filter chains (developer aid).
    void debugDump() const;
    void debugTracks() const;

signals:
    void frameReady();
    void rendererChanged(const QString &name);
    void stateChanged(Player::State state);
    void timeChanged(qint64 ms);
    void lengthChanged(qint64 ms);
    void endReached();
    void mediaChanged();
    void metaChanged();
    void tracksChanged();
    void errorOccurred(const QString &message);
    void videoSizeChanged(QSize size);
    void volumeChanged(int volume);
    void mutedChanged(bool muted);
    void rateChanged(float rate);
    void recordingChanged(bool recording);
    void discMenuChanged(bool inMenu);
    void chaptersChanged();

private:
    void createInstance();
    void destroyInstance();
    QStringList instanceArgs() const;
    void attachEvents();
    void attachMediaEvents(libvlc_media_t *media);
    void setState(State state);
    void readMeta();
    void applyEffects(Effects::Group group);
    void applyAudioEffects(void *target, bool setChain);
    void applyVideoEffects(void *target, bool setChain);
    void applySubtitleStyle(void *target);
    void applySubtitleFilter(void *target);
    void applyAll();
    void restoreSessionState();
    QString audioChain() const;
    bool needsSoftwareDecoding() const;
    void checkOverlayDecoding();
    QString videoChain() const;

    static void handleEvent(const libvlc_event_t *event, void *opaque);
    static unsigned vmemSetup(void **opaque, char *chroma, unsigned *width, unsigned *height,
                              unsigned *pitches, unsigned *lines);
    static void vmemCleanup(void *opaque);
    static void *vmemLock(void *opaque, void **planes);
    static void vmemUnlock(void *opaque, void *picture, void *const *planes);
    static void vmemDisplay(void *opaque, void *picture);

    libvlc_instance_t *m_vlc = nullptr;
    libvlc_media_player_t *m_mp = nullptr;
    libvlc_media_t *m_media = nullptr;
    QString m_error;

    State m_state = Idle;
    QString m_location;
    QString m_title, m_artist, m_album, m_artwork;
    qint64 m_length = 0;
    qint64 m_lastTime = 0;
    int m_volume = 80;
    bool m_muted = false;
    float m_rate = 1.0f;
    qint64 m_audioDelay = 0;
    qint64 m_subDelay = 0;
    float m_subSpeed = 1.0f;
    float m_subDuration = 0.0f;
    QString m_deinterlace = "off";
    QString m_visualizer;
    bool m_visualActive = false; // the current item shows m_visualizer
    QStringList m_slaves;
    libvlc_renderer_item_t *m_renderer = nullptr;
    QString m_rendererName;
    QHash<QString, QStringList> m_mediaOptions;
    bool m_recording = false;
    bool m_hwDecode = false;       // the current media decodes on the GPU
    bool m_forceSoftware = false;  // this item needs overlays, which can't be drawn on GPU frames
    QString m_recordPath;

    // Restored after restart()
    int m_pendingAudioTrack = -2;
    int m_pendingSubTrack = -2;
    bool m_pendingPause = false;
    qint64 m_pausedStart = 0; // opened paused part-way in: where (VLC only goes there on playing)

    // Frame buffers (written by the vout thread, read by the GUI thread)
    mutable QMutex m_frameMutex;
    std::vector<uchar> m_buffers[2];
    int m_front = 0;
    bool m_backWritten = false;
    unsigned m_width = 0, m_height = 0;               // decoded (buffer) size
    unsigned m_displayWidth = 0, m_displayHeight = 0; // with sample aspect ratio applied
    unsigned m_pitchY = 0, m_pitchC = 0;
    quint64 m_serial = 0;
    bool m_inMenu = false;
    std::atomic<bool> m_framePending{false};
    std::atomic<int> m_videoEs{-1}; // selected video track, set from libVLC events

    QTimer m_effectsTimer;
    QSet<int> m_dirtyGroups;
};
