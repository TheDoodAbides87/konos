#pragma once

#include "musiceditors.h" // PanelOverlay

#include <QTimer>
#include <functional>

struct libvlc_instance_t;
struct libvlc_media_player_t;
struct libvlc_renderer_discoverer_t;
struct libvlc_renderer_item_t;
class Dropdown;
class Player;
class QProgressBar;
class SegmentedControl;

// Runs one item through a VLC stream-output chain ("sout") in an engine of
// its own, so converting or streaming doesn't touch what's playing.
class SoutJob : public QObject {
    Q_OBJECT
public:
    explicit SoutJob(QObject *parent = nullptr);
    ~SoutJob() override;
    bool start(const QString &location, const QString &sout, const QStringList &options = {});
    void stop();
    bool isRunning() const { return m_mp != nullptr; }
    QString location() const { return m_location; }

signals:
    void progress(double position); // 0–1
    void finished(bool ok, const QString &error);

private:
    void poll();
    void release();
    libvlc_instance_t *m_vlc = nullptr;
    libvlc_media_player_t *m_mp = nullptr;
    QString m_location;
    QTimer m_poll;
    bool m_started = false;
};

// Main menu → "Open Capture Device": a camera, the screen or a sound input.
class CaptureDeviceOverlay : public PanelOverlay {
    Q_OBJECT
public:
    // play(mrl, options): open it like any other item.
    using PlayFn = std::function<void(const QString &mrl, const QStringList &options)>;
    CaptureDeviceOverlay(QWidget *window, PlayFn play);

    struct Device {
        QString id, name;
    };
    static QList<Device> cameras();       // /dev/videoN that capture video
    static QList<Device> soundInputs();   // PulseAudio / PipeWire sources
    static bool screenCaptureWorks();     // VLC 3 only captures X11 screens

private:
    void updateKind();
    void go();

    SegmentedControl *m_kind;
    QWidget *m_cameraBox, *m_screenBox, *m_soundBox;
    Dropdown *m_camera, *m_cameraSound, *m_fps, *m_sound;
    QLabel *m_status;
    QPushButton *m_play;
    PlayFn m_playFn;
};

// Main menu → "Convert / Save": re-encode files into another format.
class ConvertOverlay : public PanelOverlay {
    Q_OBJECT
public:
    ConvertOverlay(const QStringList &files, QWidget *window);

    struct Profile {
        QString name, ext;
        QString chain; // %1: extra transcode options, %2: destination
        bool video;
    };
    static QList<Profile> profiles();
    static QString soutFor(const Profile &p, const QString &dest, int maxHeight);

private:
    void addFiles(const QStringList &files);
    void start();
    void next();
    void finish(const QString &message);
    QString destinationFor(const QString &file, const Profile &p) const;

    QListWidget *m_list;
    Dropdown *m_profile, *m_size;
    QLineEdit *m_folder;
    QProgressBar *m_bar;
    QLabel *m_status;
    QPushButton *m_start, *m_cancel;
    SoutJob *m_job;
    int m_at = -1, m_done = 0, m_failed = 0;
    QString m_dest;
};

// Main menu → "Stream": send what's playing (or a file) to another device,
// over HTTP on this network, or to a Chromecast.
class StreamOverlay : public PanelOverlay {
    Q_OBJECT
public:
    StreamOverlay(Player *player, const QString &current, QWidget *window, bool cast = false);
    ~StreamOverlay() override;
    static SoutJob *httpJob(); // keeps running after the dialog closes
    static QStringList addresses(int port);

private:
    void updateState();
    void startHttp();
    void discover();

    Player *m_player;
    QString m_source;
    QLabel *m_sourceLabel, *m_status;
    SegmentedControl *m_method;
    QWidget *m_httpBox, *m_castBox;
    QLineEdit *m_port;
    Dropdown *m_quality;
    QListWidget *m_devices;
    QPushButton *m_go, *m_stop, *m_copy;
    libvlc_renderer_discoverer_t *m_discoverer = nullptr;
    QList<libvlc_renderer_item_t *> m_items;
};

// Main menu → "About VMP".
class AboutOverlay : public PanelOverlay {
    Q_OBJECT
public:
    explicit AboutOverlay(QWidget *window);
};
