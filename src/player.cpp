#include "player.h"
#include "library.h"
#include "disc.h"
#include "prefs.h"
#include "version.h"
#include "vlcvars.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTime>
#include <QUrl>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vlc/vlc.h>
#include <vlc/libvlc_dialog.h>

// ---------------------------------------------------------------------------
// Logging: libVLC messages go to stderr in developer mode and/or to a log file.

namespace {
struct LogSink {
    QMutex mutex;
    QFile file;
    std::atomic<bool> developer{false};
    std::atomic<bool> toFile{false};
    std::atomic<int> minLevel{LIBVLC_ERROR};

    void configure()
    {
        QMutexLocker lock(&mutex);
        developer = prefs().flag("diag/developer");
        toFile = prefs().flag("diag/saveLog");
        static const int levels[] = {LIBVLC_ERROR, LIBVLC_WARNING, LIBVLC_NOTICE, LIBVLC_DEBUG};
        minLevel = levels[qBound(0, prefs().integer("diag/logLevel"), 3)];
        const QString path = prefs().string("diag/logFile");
        if (file.isOpen() && (file.fileName() != path || !toFile))
            file.close();
        if (toFile && !file.isOpen() && !path.isEmpty()) {
            file.setFileName(path);
            (void)file.open(QIODevice::Append | QIODevice::Text);
        }
    }
};
LogSink &logSink()
{
    static LogSink sink;
    return sink;
}
} // namespace

static void vlcLog(void *, int level, const libvlc_log_t *ctx, const char *fmt, va_list args)
{
    LogSink &sink = logSink();
    const bool dev = sink.developer;
    const bool file = sink.toFile && level >= sink.minLevel;
    if (!dev && !file)
        return;
    char buffer[2048];
    vsnprintf(buffer, sizeof buffer, fmt, args);
    const char *module = nullptr;
    const char *file_ = nullptr;
    unsigned line = 0;
    libvlc_log_get_context(ctx, &module, &file_, &line);
    static const char *names[] = {"debug", "?", "info", "warning", "error"};
    const char *levelName = names[qBound(0, level, 4)];
    if (dev)
        fprintf(stderr, "[vlc %s] %s: %s\n", levelName, module ? module : "-", buffer);
    if (file) {
        QMutexLocker lock(&sink.mutex);
        if (sink.file.isOpen()) {
            const QByteArray lineText = QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8() + " [" +
                                        levelName + "] " + (module ? module : "-") + ": " + buffer + "\n";
            sink.file.write(lineText);
            sink.file.flush();
        }
    }
}

// ---------------------------------------------------------------------------

Player::Player(QObject *parent) : QObject(parent)
{
    m_volume = prefs().integer("audio/volume");
    m_muted = prefs().flag("audio/muted");
    m_rate = prefs().get("playback/speed").toFloat();
    m_deinterlace = prefs().string("video/deinterlace");

    logSink().configure();
    connect(&prefs(), &Prefs::changed, this, [](const QString &key) {
        if (key.startsWith("diag/"))
            logSink().configure();
    });

    m_effectsTimer.setSingleShot(true);
    m_effectsTimer.setInterval(40);
    connect(&m_effectsTimer, &QTimer::timeout, this, [this] {
        const auto groups = m_dirtyGroups;
        m_dirtyGroups.clear();
        for (int g : groups)
            applyEffects(Effects::Group(g));
    });
    connect(&fx(), &Effects::changed, this, [this](Effects::Group g, bool live) {
        if (!live)
            return;
        m_dirtyGroups.insert(g);
        m_effectsTimer.start();
    });

    createInstance();
}

Player::~Player()
{
    destroyInstance();
}

QStringList Player::instanceArgs() const
{
    QStringList args = {
        "--no-video-title-show",
        "--no-snapshot-preview",
        "--no-stats",
        "--no-osd",
        "--no-disable-screensaver",
        QString("--network-caching=%1").arg(prefs().integer("network/caching")),
        QString("--file-caching=%1").arg(prefs().integer("performance/fileCaching")),
        QString("--avcodec-skiploopfilter=%1").arg(prefs().integer("performance/skipLoop")),
    };
    if (prefs().flag("performance/fastSeek"))
        args << "--input-fast-seek";
    if (const int fuzzy = qBound(0, prefs().integer("subs/fuzzy"), 4); fuzzy == 0)
        args << "--no-sub-autodetect-file";
    else
        args << "--sub-autodetect-file" << QString("--sub-autodetect-fuzzy=%1").arg(fuzzy);
    if (const QString enc = prefs().string("subs/encoding"); !enc.isEmpty())
        args << "--subsdec-encoding=" + enc;
    if (const QString paths = prefs().string("subs/paths").trimmed(); !paths.isEmpty())
        args << "--sub-autodetect-path=" + paths;
    if (!prefs().flag("video/dropLate"))
        args << "--no-drop-late-frames" << "--no-skip-frames";
    if (!prefs().flag("audio/timeStretch"))
        args << "--no-audio-time-stretch";
    if (const QString rg = prefs().string("audio/replayGain"); rg == "track" || rg == "album")
        args << "--audio-replay-gain-mode=" + rg;
    if (const QString aout = prefs().string("audio/output"); !aout.isEmpty())
        args << "--aout=" + aout;
    if (prefs().flag("network/reconnect"))
        args << "--http-reconnect";
    if (const int threads = prefs().integer("performance/threads"); threads > 0)
        args << QString("--avcodec-threads=%1").arg(threads);
    if (prefs().flag("performance/fastDecode"))
        args << "--avcodec-fast";
    const QString lang = prefs().string("subs/language").trimmed();
    if (!lang.isEmpty())
        args << "--sub-language=" + lang;
    if (!prefs().flag("privacy/metaNetwork"))
        args << "--no-metadata-network-access";
    if (prefs().flag("diag/developer"))
        args << "--verbose=2";
    args << QProcess::splitCommand(prefs().string("advanced/args"));
    return args;
}

void Player::createInstance()
{
    // VLC 3's HTTP access reads the proxy from the environment, like curl.
    static const QByteArray envHttp = qgetenv("http_proxy"), envHttps = qgetenv("https_proxy");
    const QByteArray proxy = prefs().string("network/proxy").trimmed().toUtf8();
    qputenv("http_proxy", proxy.isEmpty() ? envHttp : proxy);
    qputenv("https_proxy", proxy.isEmpty() ? envHttps : proxy);
    const QStringList args = instanceArgs();
    QList<QByteArray> storage;
    std::vector<const char *> argv;
    for (const QString &a : args)
        storage << a.toUtf8();
    for (const QByteArray &a : storage)
        argv.push_back(a.constData());

    m_vlc = libvlc_new(int(argv.size()), argv.data());
    if (!m_vlc) {
        // Retry without user-supplied arguments in case they were invalid.
        qWarning("libVLC refused these options, starting without them: %s", qPrintable(args.join(' ')));
        m_vlc = libvlc_new(0, nullptr);
    }
    if (!m_vlc) {
        const char *err = libvlc_errmsg();
        m_error = err ? QString::fromUtf8(err) : tr("libVLC could not be initialised. Are VLC's plugins installed?");
        return;
    }
    libvlc_set_user_agent(m_vlc, "Konos", "Konos/" KONOS_VERSION);
    libvlc_set_app_id(m_vlc, "konos", KONOS_VERSION, "konos");
    libvlc_log_set(m_vlc, vlcLog, this);

    // Modules report problems such as "needs a library for AACS decoding" as
    // dialogs; surface the errors. Other dialog kinds keep VLC's defaults.
    static const libvlc_dialog_cbs dialogs = {
        [](void *data, const char *title, const char *text) {
            auto *self = static_cast<Player *>(data);
            const QString message = QString::fromUtf8(text ? text : title ? title : "");
            QMetaObject::invokeMethod(self, [self, message] { emit self->errorOccurred(message); },
                                      Qt::QueuedConnection);
        },
        nullptr, // login
        nullptr, // question
        nullptr, // progress
        nullptr, // cancel
        nullptr, // update progress
    };
    libvlc_dialog_set_callbacks(m_vlc, &dialogs, this);

    m_mp = libvlc_media_player_new(m_vlc);
    if (!m_mp) {
        m_error = tr("Could not create the media player.");
        return;
    }
    libvlc_video_set_callbacks(m_mp, vmemLock, vmemUnlock, vmemDisplay, this);
    libvlc_video_set_format_callbacks(m_mp, vmemSetup, vmemCleanup);
    libvlc_video_set_key_input(m_mp, false);
    libvlc_video_set_mouse_input(m_mp, false);
    attachEvents();

    libvlc_audio_set_volume(m_mp, m_volume);
    libvlc_audio_set_mute(m_mp, m_muted);
    libvlc_media_player_set_rate(m_mp, m_rate);
    applyAll();
}

void Player::destroyInstance()
{
    if (m_renderer) { // it belongs to this instance
        libvlc_renderer_item_release(m_renderer);
        m_renderer = nullptr;
        m_rendererName.clear();
    }
    if (m_mp) {
        libvlc_media_player_stop(m_mp);
        libvlc_media_player_release(m_mp);
        m_mp = nullptr;
    }
    if (m_media) {
        libvlc_media_release(m_media);
        m_media = nullptr;
    }
    if (m_vlc) {
        libvlc_log_unset(m_vlc);
        libvlc_release(m_vlc);
        m_vlc = nullptr;
    }
}

void Player::reinitialize()
{
    const QString location = m_location;
    const qint64 at = time();
    const bool paused = m_state == Paused;
    destroyInstance();
    createInstance();
    if (!location.isEmpty())
        open(location, at, paused);
}

void Player::attachEvents()
{
    libvlc_event_manager_t *em = libvlc_media_player_event_manager(m_mp);
    const libvlc_event_type_t events[] = {
        libvlc_MediaPlayerOpening,     libvlc_MediaPlayerBuffering,  libvlc_MediaPlayerPlaying,
        libvlc_MediaPlayerPaused,      libvlc_MediaPlayerStopped,    libvlc_MediaPlayerEndReached,
        libvlc_MediaPlayerEncounteredError, libvlc_MediaPlayerTimeChanged, libvlc_MediaPlayerLengthChanged,
        libvlc_MediaPlayerVout,        libvlc_MediaPlayerESAdded,    libvlc_MediaPlayerESDeleted,
        libvlc_MediaPlayerESSelected,  libvlc_MediaPlayerTitleChanged, libvlc_MediaPlayerChapterChanged,
    };
    for (auto e : events)
        libvlc_event_attach(em, e, handleEvent, this);
}

void Player::attachMediaEvents(libvlc_media_t *media)
{
    libvlc_event_manager_t *em = libvlc_media_event_manager(media);
    libvlc_event_attach(em, libvlc_MediaMetaChanged, handleEvent, this);
    libvlc_event_attach(em, libvlc_MediaParsedChanged, handleEvent, this);
}

// Called on libVLC threads. Never call back into libVLC here; hop to the GUI thread.
void Player::handleEvent(const libvlc_event_t *event, void *opaque)
{
    auto *self = static_cast<Player *>(opaque);
    const int type = event->type;
    switch (type) {
    case libvlc_MediaPlayerTimeChanged: {
        const qint64 t = event->u.media_player_time_changed.new_time;
        QMetaObject::invokeMethod(self, [self, t] {
            self->m_lastTime = t;
            emit self->timeChanged(t);
        }, Qt::QueuedConnection);
        return;
    }
    case libvlc_MediaPlayerLengthChanged: {
        const qint64 l = event->u.media_player_length_changed.new_length;
        QMetaObject::invokeMethod(self, [self, l] {
            self->m_length = l;
            emit self->lengthChanged(l);
        }, Qt::QueuedConnection);
        return;
    }
    case libvlc_MediaPlayerBuffering:
        return; // too chatty; Playing/Paused cover the useful transitions
    case libvlc_MediaPlayerESSelected:
        // Remembered for vmemSetup, which must not call back into the player
        // (libvlc_video_get_track waits on the lock that stop() holds).
        if (event->u.media_player_es_changed.i_type == libvlc_track_video)
            self->m_videoEs.store(event->u.media_player_es_changed.i_id);
        break;
    default:
        break;
    }

    QMetaObject::invokeMethod(self, [self, type] {
        switch (type) {
        case libvlc_MediaPlayerOpening:
            self->setState(Opening);
            break;
        case libvlc_MediaPlayerPlaying:
            self->setState(Playing);
            self->restoreSessionState();
            break;
        case libvlc_MediaPlayerPaused:
            self->setState(Paused);
            break;
        case libvlc_MediaPlayerStopped:
            self->setState(Stopped);
            break;
        case libvlc_MediaPlayerEndReached:
            self->setState(Ended);
            emit self->endReached();
            break;
        case libvlc_MediaPlayerEncounteredError: {
            self->setState(Error);
            const char *err = libvlc_errmsg();
            emit self->errorOccurred(err ? QString::fromUtf8(err)
                                         : tr("Couldn't open “%1”.").arg(self->title()));
            break;
        }
        case libvlc_MediaPlayerVout: {
            // The video output now exists; push effect settings onto it.
            self->applyAll();
            emit self->videoSizeChanged(self->videoSize());
            emit self->tracksChanged();
            break;
        }
        case libvlc_MediaPlayerTitleChanged: {
            // DVD menus are their own "title" flagged as a menu.
            libvlc_title_description_t **titles = nullptr;
            const int n = libvlc_media_player_get_full_title_descriptions(self->m_mp, &titles);
            const int cur = libvlc_media_player_get_title(self->m_mp);
            const bool menu = cur >= 0 && cur < n && (titles[cur]->i_flags & libvlc_title_menu);
            if (n > 0)
                libvlc_title_descriptions_release(titles, unsigned(n));
            if (menu != self->m_inMenu) {
                self->m_inMenu = menu;
                emit self->discMenuChanged(menu);
            }
            emit self->chaptersChanged();
            break;
        }
        case libvlc_MediaPlayerChapterChanged:
            emit self->chaptersChanged();
            break;
        case libvlc_MediaPlayerESAdded:
        case libvlc_MediaPlayerESDeleted:
        case libvlc_MediaPlayerESSelected:
            self->checkOverlayDecoding();
            emit self->tracksChanged();
            break;
        case libvlc_MediaMetaChanged:
        case libvlc_MediaParsedChanged:
            self->readMeta();
            break;
        }
    }, Qt::QueuedConnection);
}

void Player::setState(State state)
{
    if (state == m_state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void Player::readMeta()
{
    if (!m_media)
        return;
    auto meta = [this](libvlc_meta_t m) {
        char *v = libvlc_media_get_meta(m_media, m);
        QString s = v ? QString::fromUtf8(v) : QString();
        libvlc_free(v);
        return s;
    };
    m_title = meta(libvlc_meta_Title);
    m_artist = meta(libvlc_meta_Artist);
    m_album = meta(libvlc_meta_Album);
    const QString art = meta(libvlc_meta_ArtworkURL);
    m_artwork = art.startsWith("file://") ? QUrl(art).toLocalFile() : QString();
    emit metaChanged();
}

QString Player::title() const
{
    // libVLC falls back to the file name for Title; prefer it if it looks real.
    if (Disc::isDiscLocation(m_location)) {
        // libVLC names a disc after its device node ("sr0"); use the volume label.
        const QString node = QUrl(m_location).path().section('/', -1);
        return m_title.isEmpty() || m_title == node || m_title.startsWith(node + "#") ? Disc::displayName(m_location)
                                                                                     : m_title;
    }
    if (!m_title.isEmpty())
        return m_title;
    if (m_location.isEmpty())
        return {};
    const QUrl url(m_location);
    if (url.isValid() && !url.scheme().isEmpty() && url.scheme() != "file")
        return m_location;
    return QFileInfo(url.isLocalFile() ? url.toLocalFile() : m_location).fileName();
}

bool Player::hasVideo() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_width > 0 && m_height > 0;
}

// ---------------------------------------------------------------------------
// Transport

void Player::open(const QString &location, qint64 startMs, bool startPaused)
{
    if (!m_mp)
        return;
    const bool newItem = location != m_location;
    m_videoEs.store(-1);
    if (newItem) {
        m_slaves.clear();
        m_subDelay = 0;
        m_audioDelay = 0;
        m_pendingAudioTrack = m_pendingSubTrack = -2;
        m_forceSoftware = false;
    }
    m_location = location;
    m_title.clear();
    m_artist.clear();
    m_album.clear();
    m_artwork.clear();
    m_length = 0;
    m_lastTime = startMs;

    if (m_media)
        libvlc_media_release(m_media);

    m_inMenu = false;
    QString mrl = location;
    int cdTrack = 0;
    if (location.startsWith("cdda://") && location.contains('#')) {
        cdTrack = location.section('#', -1).toInt();
        mrl = location.section('#', 0, 0);
    }

    const QUrl url(location);
    const bool isUrl = url.isValid() && url.scheme().size() > 1;
    if (Disc::isDiscLocation(location))
        m_media = libvlc_media_new_location(m_vlc, mrl.toUtf8().constData());
    else if (isUrl && url.isLocalFile())
        m_media = libvlc_media_new_path(m_vlc, url.toLocalFile().toUtf8().constData());
    else if (isUrl)
        m_media = libvlc_media_new_location(m_vlc, location.toUtf8().constData());
    else
        m_media = libvlc_media_new_path(m_vlc, location.toUtf8().constData());
    if (!m_media) {
        emit errorOccurred(tr("Could not open “%1”.").arg(location));
        return;
    }
    for (const QString &o : m_mediaOptions.value(location))
        libvlc_media_add_option(m_media, o.toUtf8().constData());

    // With memory output, VLC's automatic choice ("any") ends up refusing VA-API;
    // naming the DRM variant decodes on the GPU and copies frames back. Where
    // VA-API is missing it falls back to software decoding on its own.
    // VLC 3 can't blend subtitles or DVD menu highlights onto GPU frames in this
    // mode, nor run its video filters on them, so discs and media with video
    // effects decode in software, and checkOverlayDecoding() switches over when
    // a subtitle track or an effect turns on.
    m_hwDecode = prefs().flag("video/hw") && !Disc::isDiscLocation(location) && !m_forceSoftware &&
                 videoChain().isEmpty();
    libvlc_media_add_option(m_media, m_hwDecode ? ":avcodec-hw=vaapi_drm" : ":avcodec-hw=none");
    if (cdTrack > 0)
        libvlc_media_add_option(m_media, QString(":cdda-track=%1").arg(cdTrack).toUtf8().constData());
    if (startMs > 0)
        libvlc_media_add_option(m_media,
                                QString(":start-time=%1").arg(startMs / 1000.0, 0, 'f', 3).toUtf8().constData());
    // Adaptive streams (YouTube-style, DASH/HLS): the quality to pick.
    if (const int res = prefs().integer("video/streamResolution"); res > 0 && isUrl && !url.isLocalFile())
        libvlc_media_add_option(m_media, QString(":preferred-resolution=%1").arg(res).toUtf8().constData());
    // Audio-only files: a visualization takes the place of the cover. The
    // audio output belongs to the player (not the media), so these are set
    // on the player, and cleared for everything else.
    m_visualActive = !m_visualizer.isEmpty() && Library::isAudioFile(isUrl ? url.path() : location);
    VlcVars::setString(m_mp, "audio-visual", m_visualActive ? (m_visualizer == "goom" ? "goom" : "visual") : "");
    VlcVars::setString(m_mp, "effect-list", m_visualActive && m_visualizer != "goom" ? m_visualizer : "spectrum");
    for (const QString &slave : m_slaves) // "audio:" marks extra audio tracks
        if (slave.startsWith("audio:"))
            libvlc_media_slaves_add(m_media, libvlc_media_slave_type_audio, 4, slave.mid(6).toUtf8().constData());
        else
            libvlc_media_slaves_add(m_media, libvlc_media_slave_type_subtitle, 4, slave.toUtf8().constData());

    if (m_recording) {
        QDir().mkpath(prefs().string("record/dir"));
        m_recordPath = prefs().string("record/dir") + "/" +
                       QFileInfo(title()).completeBaseName() + " " +
                       QDateTime::currentDateTime().toString("yyyy-MM-dd hh.mm.ss") + ".ts";
        QString sout = QString(":sout=#duplicate{dst=display,dst=std{access=file,mux=ts,dst='%1'}}")
                           .arg(QString(m_recordPath).replace("'", "\\'"));
        libvlc_media_add_option(m_media, sout.toUtf8().constData());
        libvlc_media_add_option(m_media, ":sout-keep");
    }

    attachMediaEvents(m_media);
    libvlc_media_player_set_media(m_mp, m_media);
    applyAll();
    libvlc_media_parse_with_options(m_media,
                                    libvlc_media_parse_flag_t(libvlc_media_parse_local | libvlc_media_fetch_local),
                                    5000);
    m_pendingPause = startPaused;
    if (const QString dev = prefs().string("audio/device"); !dev.isEmpty())
        libvlc_audio_output_device_set(m_mp, nullptr, dev.toUtf8().constData());
    libvlc_media_player_play(m_mp);
    libvlc_media_player_set_rate(m_mp, m_rate);
    emit mediaChanged();
    readMeta();
}

void Player::restoreSessionState()
{
    libvlc_audio_set_volume(m_mp, m_volume);
    libvlc_audio_set_mute(m_mp, m_muted);
    libvlc_media_player_set_rate(m_mp, m_rate);
    if (m_audioDelay)
        libvlc_audio_set_delay(m_mp, m_audioDelay * 1000);
    if (m_subDelay)
        libvlc_video_set_spu_delay(m_mp, m_subDelay * 1000);
    if (m_pendingAudioTrack != -2) {
        libvlc_audio_set_track(m_mp, m_pendingAudioTrack);
        m_pendingAudioTrack = -2;
    }
    if (m_pendingSubTrack != -2) {
        libvlc_video_set_spu(m_mp, m_pendingSubTrack);
        m_pendingSubTrack = -2;
    }
    if (m_pendingPause) {
        m_pendingPause = false;
        libvlc_media_player_set_pause(m_mp, 1);
    }
    applyAll();
}

// GPU frames in memory-output mode can't have subtitles/menu highlights blended
// onto them, and VLC's software video filters can't process them.
bool Player::needsSoftwareDecoding() const
{
    return !videoChain().isEmpty() || (m_mp && libvlc_video_get_spu(m_mp) >= 0);
}

void Player::checkOverlayDecoding()
{
    if (!m_hwDecode || m_forceSoftware || !m_mp || !needsSoftwareDecoding())
        return;
    // Reopen in place with software decoding.
    m_forceSoftware = true;
    QTimer::singleShot(0, this, &Player::restart);
}

void Player::setRenderer(libvlc_renderer_item_t *item)
{
    if (!m_mp || item == m_renderer)
        return;
    if (m_renderer)
        libvlc_renderer_item_release(m_renderer);
    m_renderer = item ? libvlc_renderer_item_hold(item) : nullptr;
    m_rendererName = item ? QString::fromUtf8(libvlc_renderer_item_name(item)) : QString();
    libvlc_media_player_set_renderer(m_mp, m_renderer);
    restart();
    emit rendererChanged(m_rendererName);
}

void Player::restart()
{
    if (m_location.isEmpty() || !m_mp)
        return;
    const qint64 at = time();
    m_pendingAudioTrack = audioTrack();
    m_pendingSubTrack = subtitleTrack();
    const bool paused = m_state == Paused;
    libvlc_media_player_stop(m_mp);
    open(m_location, at, paused);
}

void Player::play()
{
    if (!m_mp)
        return;
    if (m_state == Ended || m_state == Stopped || m_state == Error) {
        if (!m_location.isEmpty())
            open(m_location, 0);
        return;
    }
    libvlc_media_player_set_pause(m_mp, 0);
}

void Player::pause()
{
    if (m_mp && libvlc_media_player_can_pause(m_mp))
        libvlc_media_player_set_pause(m_mp, 1);
}

void Player::togglePause()
{
    if (m_state == Playing || m_state == Opening || m_state == Buffering)
        pause();
    else
        play();
}

void Player::stop()
{
    if (!m_mp)
        return;
    libvlc_media_player_stop(m_mp);
    setState(Stopped);
}

qint64 Player::time() const
{
    if (!m_mp)
        return 0;
    const qint64 t = libvlc_media_player_get_time(m_mp);
    return t >= 0 ? t : m_lastTime;
}

float Player::position() const
{
    return m_mp ? libvlc_media_player_get_position(m_mp) : 0.0f;
}

bool Player::isSeekable() const
{
    return m_mp && libvlc_media_player_is_seekable(m_mp);
}

void Player::seek(qint64 ms)
{
    if (!m_mp)
        return;
    if (m_state == Ended || m_state == Stopped) {
        open(m_location, ms);
        return;
    }
    ms = qBound<qint64>(0, ms, m_length > 0 ? m_length - 250 : ms);
    libvlc_media_player_set_time(m_mp, ms);
    m_lastTime = ms;
    emit timeChanged(ms);
}

void Player::seekRelative(qint64 deltaMs)
{
    seek(time() + deltaMs);
}

void Player::setVolume(int volume)
{
    volume = qBound(0, volume, 125);
    m_volume = volume;
    if (m_mp)
        libvlc_audio_set_volume(m_mp, volume);
    prefs().set("audio/volume", volume);
    emit volumeChanged(volume);
    if (m_muted && volume > 0)
        setMuted(false);
}

void Player::setMuted(bool muted)
{
    m_muted = muted;
    if (m_mp)
        libvlc_audio_set_mute(m_mp, muted);
    prefs().set("audio/muted", muted);
    emit mutedChanged(muted);
}

void Player::setRate(float rate)
{
    m_rate = qBound(0.25f, rate, 4.0f);
    if (m_mp)
        libvlc_media_player_set_rate(m_mp, m_rate);
    emit rateChanged(m_rate);
}

// ---------------------------------------------------------------------------
// Tracks

static QList<Player::Track> toTracks(libvlc_track_description_t *list)
{
    QList<Player::Track> out;
    for (auto *t = list; t; t = t->p_next)
        out.append({t->i_id, QString::fromUtf8(t->psz_name ? t->psz_name : "")});
    if (list)
        libvlc_track_description_list_release(list);
    return out;
}

QList<Player::Track> Player::audioTracks() const
{
    return m_mp ? toTracks(libvlc_audio_get_track_description(m_mp)) : QList<Track>{};
}

QList<Player::Track> Player::subtitleTracks() const
{
    return m_mp ? toTracks(libvlc_video_get_spu_description(m_mp)) : QList<Track>{};
}

QList<Player::SubtitleTrack> Player::subtitleTrackList() const
{
    QList<SubtitleTrack> out;
    for (const Track &t : subtitleTracks())
        if (t.id >= 0)
            out.append({t.id, t.name, false, {}});
    if (!m_mp || out.isEmpty())
        return out;
    // libVLC lists subtitle files as the media's "slaves", and their tracks
    // come after the file's own, in the same order.
    QStringList files;
    if (libvlc_media_t *media = libvlc_media_player_get_media(m_mp)) {
        libvlc_media_slave_t **slaves = nullptr;
        const unsigned n = libvlc_media_slaves_get(media, &slaves);
        for (unsigned i = 0; i < n; ++i)
            if (slaves[i]->i_type == libvlc_media_slave_type_subtitle)
                files << QUrl(QString::fromUtf8(slaves[i]->psz_uri)).toLocalFile();
        if (n)
            libvlc_media_slaves_release(slaves, n);
        libvlc_media_release(media);
    }
    const int k = int(qMin(files.size(), out.size()));
    static const QRegularExpression lang(R"(\[([^\]]+)\]\s*$)");
    for (int i = 0; i < k; ++i) {
        SubtitleTrack &t = out[out.size() - k + i];
        t.external = true;
        t.file = files.at(i);
        const QRegularExpressionMatch m = lang.match(t.name);
        t.name = QFileInfo(files.at(i)).fileName() + (m.hasMatch() ? " [" + m.captured(1) + "]" : QString());
    }
    return out;
}

QList<Player::TrackInfo> Player::trackInfo() const
{
    QList<TrackInfo> out;
    if (!m_mp)
        return out;
    libvlc_media_t *media = libvlc_media_player_get_media(m_mp);
    if (!media)
        return out;
    libvlc_media_track_t **tracks = nullptr;
    const unsigned n = libvlc_media_tracks_get(media, &tracks);
    for (unsigned i = 0; i < n; ++i) {
        const libvlc_media_track_t *t = tracks[i];
        TrackInfo info;
        info.id = t->i_id;
        info.language = QString::fromUtf8(t->psz_language ? t->psz_language : "").trimmed();
        info.description = QString::fromUtf8(t->psz_description ? t->psz_description : "");
        info.bitrate = t->i_bitrate;
        info.fourcc = t->i_codec;
        const char *codec = libvlc_media_get_codec_description(t->i_type, t->i_codec);
        info.codec = QString::fromUtf8(codec ? codec : "");
        if (info.codec.isEmpty()) { // fourcc as text
            char fourcc[5] = {};
            memcpy(fourcc, &t->i_codec, 4);
            info.codec = QString::fromLatin1(fourcc).trimmed().toUpper();
        }
        switch (t->i_type) {
        case libvlc_track_audio:
            info.type = TrackInfo::Audio;
            info.channels = int(t->audio->i_channels);
            info.sampleRate = int(t->audio->i_rate);
            break;
        case libvlc_track_video:
            info.type = TrackInfo::Video;
            info.width = int(t->video->i_width);
            info.height = int(t->video->i_height);
            if (t->video->i_frame_rate_den)
                info.fps = double(t->video->i_frame_rate_num) / t->video->i_frame_rate_den;
            break;
        case libvlc_track_text:
            info.type = TrackInfo::Subtitle;
            break;
        default:
            break;
        }
        out << info;
    }
    if (n)
        libvlc_media_tracks_release(tracks, n);
    libvlc_media_release(media);
    return out;
}

void Player::setSubtitleMargin(int px)
{
    if (!m_mp)
        return;
    VlcVars::setInt(m_mp, "sub-margin", px);
    const auto vouts = VlcVars::findObjects(m_mp, "video output");
    for (void *v : vouts)
        VlcVars::setInt(v, "sub-margin", px);
    VlcVars::release(vouts);
}

QList<Player::AudioDevice> Player::audioDevices() const
{
    QList<AudioDevice> out;
    if (!m_mp)
        return out;
    libvlc_audio_output_device_t *list = libvlc_audio_output_device_enum(m_mp);
    for (auto *d = list; d; d = d->p_next)
        out.append({QString::fromUtf8(d->psz_device ? d->psz_device : ""),
                    QString::fromUtf8(d->psz_description ? d->psz_description : "")});
    if (list)
        libvlc_audio_output_device_list_release(list);
    return out;
}

QString Player::audioDevice() const
{
    if (!m_mp)
        return {};
    char *id = libvlc_audio_output_device_get(m_mp);
    const QString out = QString::fromUtf8(id ? id : "");
    libvlc_free(id);
    return out;
}

void Player::setAudioDevice(const QString &id)
{
    prefs().set("audio/device", id);
    if (m_mp)
        libvlc_audio_output_device_set(m_mp, nullptr, id.toUtf8().constData());
}

QList<Player::Track> Player::videoTracks() const
{
    return m_mp ? toTracks(libvlc_video_get_track_description(m_mp)) : QList<Track>{};
}

int Player::audioTrack() const { return m_mp ? libvlc_audio_get_track(m_mp) : -1; }
int Player::subtitleTrack() const { return m_mp ? libvlc_video_get_spu(m_mp) : -1; }
int Player::videoTrack() const { return m_mp ? libvlc_video_get_track(m_mp) : -1; }

void Player::setAudioTrack(int id)
{
    if (m_mp)
        libvlc_audio_set_track(m_mp, id);
    emit tracksChanged();
}

// ---------------------------------------------------------------------------
// Discs, titles and chapters

bool Player::isDisc() const { return Disc::isDiscLocation(m_location); }

void Player::navigate(Nav nav)
{
    static const unsigned modes[] = {libvlc_navigate_activate, libvlc_navigate_up, libvlc_navigate_down,
                                     libvlc_navigate_left, libvlc_navigate_right};
    if (m_mp)
        libvlc_media_player_navigate(m_mp, modes[int(nav)]);
}

bool Player::discMenu()
{
    if (!m_mp)
        return false;
    libvlc_title_description_t **titles = nullptr;
    const int n = libvlc_media_player_get_full_title_descriptions(m_mp, &titles);
    int menu = -1;
    for (int i = 0; i < n && menu < 0; ++i)
        if (titles[i]->i_flags & libvlc_title_menu)
            menu = i;
    if (n > 0)
        libvlc_title_descriptions_release(titles, unsigned(n));
    if (menu >= 0)
        libvlc_media_player_set_title(m_mp, menu);
    else if (isDisc())
        libvlc_media_player_navigate(m_mp, libvlc_navigate_popup);
    return menu >= 0 || isDisc();
}

void Player::pointerOnVideo(QPointF framePos, bool click)
{
    if (!m_mp || !m_inMenu)
        return;
    unsigned fw, fh, sw, sh;
    {
        QMutexLocker lock(&m_frameMutex);
        fw = m_displayWidth;
        fh = m_displayHeight;
        sw = m_width;
        sh = m_height;
    }
    if (!fw || !fh)
        return;
    // dvdnav expects coordinates in the decoded picture, before aspect correction.
    const int x = int(framePos.x() * sw / fw);
    const int y = int(framePos.y() * sh / fh);
    const QList<void *> vouts = VlcVars::findObjects(m_mp, "video output");
    for (void *vout : vouts) {
        VlcVars::setCoords(vout, "mouse-moved", x, y);
        if (click)
            VlcVars::setCoords(vout, "mouse-clicked", x, y);
    }
    VlcVars::release(vouts);
}

QList<Player::Track> Player::titles() const
{
    QList<Track> out;
    if (!m_mp)
        return out;
    libvlc_title_description_t **titles = nullptr;
    const int n = libvlc_media_player_get_full_title_descriptions(m_mp, &titles);
    for (int i = 0; i < n; ++i) {
        const auto *t = titles[i];
        QString name = QString::fromUtf8(t->psz_name ? t->psz_name : "");
        if (t->i_flags & libvlc_title_menu)
            name = tr("Disc Menu");
        else if (name.isEmpty())
            name = tr("Title %1").arg(i);
        if (t->i_duration > 0)
            name += QString("\t%1").arg(QTime(0, 0).addMSecs(int(t->i_duration)).toString(
                t->i_duration >= 3600000 ? "h:mm:ss" : "m:ss"));
        out.append({i, name});
    }
    if (n > 0)
        libvlc_title_descriptions_release(titles, unsigned(n));
    return out;
}

int Player::currentTitle() const { return m_mp ? libvlc_media_player_get_title(m_mp) : -1; }

void Player::setTitle(int index)
{
    if (m_mp)
        libvlc_media_player_set_title(m_mp, index);
}

QList<Player::Track> Player::chapters() const
{
    QList<Track> out;
    if (!m_mp)
        return out;
    libvlc_chapter_description_t **chapters = nullptr;
    const int n = libvlc_media_player_get_full_chapter_descriptions(m_mp, -1, &chapters);
    for (int i = 0; i < n; ++i) {
        const auto *c = chapters[i];
        QString name = QString::fromUtf8(c->psz_name ? c->psz_name : "");
        if (name.isEmpty())
            name = tr("Chapter %1").arg(i + 1);
        const qint64 at = c->i_time_offset;
        if (!m_inMenu) // a DVD menu's "chapters" are its sub-menus, which have no times
            name += QString("\t%1").arg(QTime(0, 0).addMSecs(int(at)).toString(at >= 3600000 ? "h:mm:ss" : "m:ss"));
        out.append({i, name});
    }
    if (n > 0)
        libvlc_chapter_descriptions_release(chapters, unsigned(n));
    return out;
}

QList<Player::Chapter> Player::chapterList() const
{
    QList<Chapter> out;
    if (!m_mp || m_inMenu)
        return out;
    libvlc_chapter_description_t **chapters = nullptr;
    const int n = libvlc_media_player_get_full_chapter_descriptions(m_mp, -1, &chapters);
    for (int i = 0; i < n; ++i) {
        QString name = QString::fromUtf8(chapters[i]->psz_name ? chapters[i]->psz_name : "").trimmed();
        if (name.isEmpty())
            name = tr("Chapter %1").arg(i + 1);
        out.append({i, name, chapters[i]->i_time_offset});
    }
    if (n > 0)
        libvlc_chapter_descriptions_release(chapters, unsigned(n));
    return out;
}

int Player::currentChapter() const { return m_mp ? libvlc_media_player_get_chapter(m_mp) : -1; }

void Player::setChapter(int index)
{
    if (m_mp)
        libvlc_media_player_set_chapter(m_mp, index);
}

bool Player::nextChapter()
{
    if (!m_mp || libvlc_media_player_get_chapter_count(m_mp) < 2 ||
        libvlc_media_player_get_chapter(m_mp) + 1 >= libvlc_media_player_get_chapter_count(m_mp))
        return false;
    libvlc_media_player_next_chapter(m_mp);
    return true;
}

bool Player::previousChapter()
{
    if (!m_mp || libvlc_media_player_get_chapter_count(m_mp) < 2 || libvlc_media_player_get_chapter(m_mp) <= 0)
        return false;
    libvlc_media_player_previous_chapter(m_mp);
    return true;
}

void Player::setSubtitleTrack(int id)
{
    if (m_mp)
        libvlc_video_set_spu(m_mp, id);
    checkOverlayDecoding();
    emit tracksChanged();
}

void Player::setVideoTrack(int id)
{
    if (m_mp)
        libvlc_video_set_track(m_mp, id);
    emit tracksChanged();
}

bool Player::addSubtitleFile(const QString &path)
{
    if (!m_mp)
        return false;
    const QString uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    if (libvlc_media_player_add_slave(m_mp, libvlc_media_slave_type_subtitle, uri.toUtf8().constData(), true) != 0)
        return false;
    m_slaves << uri;
    return true;
}

bool Player::addAudioFile(const QString &path)
{
    if (!m_mp)
        return false;
    const QString uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    if (libvlc_media_player_add_slave(m_mp, libvlc_media_slave_type_audio, uri.toUtf8().constData(), true) != 0)
        return false;
    m_slaves << "audio:" + uri;
    return true;
}

void Player::setAudioDelay(qint64 ms)
{
    m_audioDelay = ms;
    if (m_mp)
        libvlc_audio_set_delay(m_mp, ms * 1000);
}

void Player::setSubtitleDelay(qint64 ms)
{
    m_subDelay = ms;
    if (m_mp)
        libvlc_video_set_spu_delay(m_mp, ms * 1000);
}

void Player::setSubtitleSpeed(float factor)
{
    m_subSpeed = factor;
    const auto inputs = VlcVars::findObjects(m_mp, "input");
    for (void *in : inputs)
        VlcVars::setIfExists(in, "sub-fps", VlcVars::Type::Float, factor);
    VlcVars::release(inputs);
}

void Player::setSubtitleDurationFactor(float factor)
{
    m_subDuration = factor;
    if (!m_mp)
        return;
    applySubtitleFilter(m_mp);
    const auto vouts = VlcVars::findObjects(m_mp, "video output");
    for (void *v : vouts)
        applySubtitleFilter(v);
    VlcVars::release(vouts);
}

int Player::stereoMode() const
{
    return m_mp ? libvlc_audio_get_channel(m_mp) : 0;
}

void Player::setStereoMode(int mode)
{
    if (m_mp)
        libvlc_audio_set_channel(m_mp, mode);
}

void Player::setDeinterlace(const QString &mode)
{
    m_deinterlace = mode;
    prefs().set("video/deinterlace", mode);
    if (!m_mp)
        return;
    if (mode == "off" || mode.isEmpty())
        libvlc_video_set_deinterlace(m_mp, nullptr);
    else
        libvlc_video_set_deinterlace(m_mp, mode.toUtf8().constData());
}

void Player::setRecording(bool on)
{
    if (on == m_recording)
        return;
    m_recording = on;
    emit recordingChanged(on);
    if (!m_location.isEmpty())
        restart();
}

// ---------------------------------------------------------------------------
// Effects

QString Player::audioChain() const
{
    const EffectsData &e = fx().data();
    QStringList chain;
    if (e.eqEnabled)
        chain << "equalizer";
    if (e.compEnabled)
        chain << "compressor";
    if (e.spatialEnabled)
        chain << (e.spatialMode == 0 ? "headphone" : "spatializer");
    if (e.widenEnabled)
        chain << "stereo_widen";
    if (e.normalize)
        chain << "normvol";
    return chain.join(':');
}

QString Player::videoChain() const
{
    const EffectsData &e = fx().data();
    QStringList chain;
    if (e.adjustActive())
        chain << "adjust";
    if (e.sharpen > 0.0f)
        chain << "sharpen";
    if (e.debandRadius > 0)
        chain << "gradfun";
    if (e.grain > 0.0f)
        chain << "grain";
    if (e.extractEnabled)
        chain << "extract";
    if (e.thresholdEnabled)
        chain << "colorthres";
    if (e.negate)
        chain << "invert";
    if (e.posterize)
        chain << "posterize";
    if (e.gradient)
        chain << "gradient";
    if (e.sepia > 0)
        chain << "sepia";
    return chain.join(':');
}

void Player::applyAudioEffects(void *o, bool setChain)
{
    using namespace VlcVars;
    const EffectsData &e = fx().data();
    QStringList bands;
    for (float b : e.eqBands)
        bands << QString::number(b, 'f', 1);
    setBool(o, "equalizer-vlcfreqs", true);
    setBool(o, "equalizer-2pass", e.eqTwoPass);
    setFloat(o, "equalizer-preamp", e.eqPreamp);
    setString(o, "equalizer-bands", bands.join(' '));

    setFloat(o, "compressor-rms-peak", e.compRmsPeak);
    setFloat(o, "compressor-attack", e.compAttack);
    setFloat(o, "compressor-release", e.compRelease);
    setFloat(o, "compressor-threshold", e.compThreshold);
    setFloat(o, "compressor-ratio", e.compRatio);
    setFloat(o, "compressor-knee", e.compKnee);
    setFloat(o, "compressor-makeup-gain", e.compMakeup);

    setFloat(o, "spatializer-roomsize", e.reverbSize * 0.11f);
    setFloat(o, "spatializer-width", e.reverbWidth / 10.0f);
    setFloat(o, "spatializer-wet", e.reverbWet / 10.0f);
    setFloat(o, "spatializer-dry", e.reverbDry / 10.0f);
    setFloat(o, "spatializer-damp", e.reverbDamp / 10.0f);

    setFloat(o, "stereowiden-delay", e.widenDelay);
    setFloat(o, "stereowiden-feedback", e.widenFeedback);
    setFloat(o, "stereowiden-crossfeed", e.widenCrossfeed);
    setFloat(o, "stereowiden-dry-mix", e.widenDry);

    if (setChain) {
        // Changing audio-filter restarts the filter chain, so only touch it when it differs.
        const QString chain = audioChain();
        if (getString(o, "audio-filter") != chain)
            setString(o, "audio-filter", chain);
    }
}

void Player::applyVideoEffects(void *o, bool setChain)
{
    using namespace VlcVars;
    const EffectsData &e = fx().data();
    setFloat(o, "hue", e.hue);
    setFloat(o, "brightness", e.brightness);
    setFloat(o, "contrast", e.contrast);
    setFloat(o, "saturation", e.saturation);
    setFloat(o, "gamma", e.gamma);
    setFloat(o, "sharpen-sigma", qMax(0.01f, e.sharpen));
    setInt(o, "gradfun-radius", qMax(4, e.debandRadius));
    setFloat(o, "grain-variance", e.grain);
    setInt(o, "extract-component", e.extractColor.rgb() & 0xffffff);
    setInt(o, "colorthres-color", e.thresholdColor.rgb() & 0xffffff);
    setInt(o, "colorthres-saturationthres", e.thresholdSaturation);
    setInt(o, "colorthres-similaritythres", e.thresholdSimilarity);
    setInt(o, "posterize-level", 6);
    static const char *modes[] = {"gradient", "edge", "hough"};
    setString(o, "gradient-mode", modes[qBound(0, e.gradientMode, 2)]);
    setInt(o, "gradient-type", e.gradientColor ? 1 : 0);
    setBool(o, "gradient-cartoon", e.gradientCartoon);
    setInt(o, "sepia-intensity", e.sepia);

    if (setChain) {
        const QString chain = videoChain();
        if (getString(o, "video-filter") != chain)
            setString(o, "video-filter", chain);
    }
}

void Player::applySubtitleStyle(void *o)
{
    using namespace VlcVars;
    const EffectsData &e = fx().data();
    setString(o, "freetype-font", e.subFont);
    setInt(o, "freetype-color", e.subColor.rgb() & 0xffffff);
    setInt(o, "freetype-outline-color", e.subOutline.rgb() & 0xffffff);
    // Relative size (a fraction of the picture height) or an exact pixel size.
    setInt(o, "freetype-rel-fontsize", e.subSizePixels > 0 ? 0 : e.subSize);
    setInt(o, "freetype-fontsize", e.subSizePixels);
    setInt(o, "freetype-outline-thickness", e.subOutlineThickness);
    setInt(o, "freetype-background-opacity", e.subBackgroundOpacity);
    setInt(o, "freetype-background-color", 0);
    setInt(o, "sub-margin", e.subMargin);
    // Off: ignore the fonts and colours in SRT-style text. (Styled SSA/ASS
    // tracks are swapped for a plain copy instead: see MainWindow.)
    setBool(o, "subsdec-formatted", e.subEmbeddedStyle);
}

void Player::applySubtitleFilter(void *o)
{
    using namespace VlcVars;
    if (m_subDuration > 0.0f) {
        setInt(o, "subsdelay-mode", 1);
        setFloat(o, "subsdelay-factor", m_subDuration);
        setString(o, "sub-filter", "subsdelay");
    } else {
        setString(o, "sub-filter", "");
    }
}

void Player::applyEffects(Effects::Group group)
{
    if (!m_mp)
        return;
    switch (group) {
    case Effects::Equalizer:
    case Effects::AudioFilters: {
        applyAudioEffects(m_mp, true);
        const auto aouts = VlcVars::findObjects(m_mp, "audio output");
        for (void *a : aouts)
            applyAudioEffects(a, true);
        VlcVars::release(aouts);
        break;
    }
    case Effects::VideoAdjust:
    case Effects::VideoFilters: {
        applyVideoEffects(m_mp, true);
        const auto vouts = VlcVars::findObjects(m_mp, "video output");
        for (void *v : vouts)
            applyVideoEffects(v, true);
        VlcVars::release(vouts);
        checkOverlayDecoding();
        break;
    }
    case Effects::Crop:
        emit frameReady(); // cropping happens when painting
        break;
    case Effects::SubtitleStyle: {
        applySubtitleStyle(m_mp);
        // The text renderer reads its style when created; reopen to apply.
        if (hasMedia() && m_state != Stopped && m_state != Ended)
            restart();
        break;
    }
    }
}

void Player::applyAll()
{
    if (!m_mp)
        return;
    applyAudioEffects(m_mp, true);
    applyVideoEffects(m_mp, true);
    applySubtitleStyle(m_mp);
    applySubtitleFilter(m_mp);
    const auto aouts = VlcVars::findObjects(m_mp, "audio output");
    for (void *a : aouts) {
        applyAudioEffects(a, true);
        // The output outlives items: switch its visualization per item.
        // Setting "audio-visual" on it rebuilds its filters (and the
        // visualization's picture output).
        const QString module = !m_visualActive ? QString("none") : m_visualizer == "goom" ? QString("goom") : QString("visual");
        if (m_visualActive && m_visualizer != "goom")
            VlcVars::setString(a, "effect-list", m_visualizer);
        if (VlcVars::getString(a, "audio-visual") != module)
            VlcVars::setString(a, "audio-visual", module);
    }
    VlcVars::release(aouts);
    const auto vouts = VlcVars::findObjects(m_mp, "video output");
    for (void *v : vouts)
        applyVideoEffects(v, true);
    VlcVars::release(vouts);
    if (m_deinterlace != "off" && !m_deinterlace.isEmpty())
        libvlc_video_set_deinterlace(m_mp, m_deinterlace.toUtf8().constData());
}

libvlc_instance_t *Player::parserInstance()
{
    static libvlc_instance_t *vlc = [] {
        const char *args[] = {"--quiet", "--no-lua", "--no-video", "--no-audio"};
        return libvlc_new(int(std::size(args)), args);
    }();
    return vlc;
}

QList<QPair<QString, QString>> Player::filterModules(bool audio)
{
    QList<QPair<QString, QString>> out;
    libvlc_instance_t *vlc = libvlc_new(0, nullptr);
    if (!vlc)
        return out;
    libvlc_module_description_t *list = audio ? libvlc_audio_filter_list_get(vlc) : libvlc_video_filter_list_get(vlc);
    for (auto *m = list; m; m = m->p_next)
        out.append({QString::fromUtf8(m->psz_shortname ? m->psz_shortname : m->psz_name),
                    QString::fromUtf8(m->psz_longname ? m->psz_longname : "")});
    if (list)
        libvlc_module_description_list_release(list);
    libvlc_release(vlc);
    return out;
}

void Player::debugDump() const
{
    VlcVars::dumpTree(m_mp);
    const auto aouts = VlcVars::findObjects(m_mp, "audio output");
    for (void *a : aouts)
        qDebug() << "aout audio-filter =" << VlcVars::getString(a, "audio-filter");
    VlcVars::release(aouts);
    const auto vouts = VlcVars::findObjects(m_mp, "video output");
    for (void *v : vouts)
        qDebug() << "vout video-filter =" << VlcVars::getString(v, "video-filter");
    VlcVars::release(vouts);
}

// ---------------------------------------------------------------------------
// Video memory callbacks (vout thread)

unsigned Player::vmemSetup(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches,
                           unsigned *lines)
{
    auto *self = static_cast<Player *>(*opaque);
    // Planar YUV at the decoded size: VLC hands pictures over without any
    // colour conversion or scaling; VideoWidget does both on the GPU.
    memcpy(chroma, "I420", 4);

    // VLC passes the padded buffer size here; use the track's visible size.
    unsigned w = *width, h = *height, dw = 0, dh = 0;
    if (libvlc_media_t *media = libvlc_media_player_get_media(self->m_mp)) {
        libvlc_media_track_t **tracks = nullptr;
        const unsigned n = libvlc_media_tracks_get(media, &tracks);
        const int current = self->m_videoEs.load();
        for (unsigned i = 0; i < n; ++i) {
            const libvlc_media_track_t *t = tracks[i];
            if (t->i_type != libvlc_track_video || !t->video->i_width || !t->video->i_height)
                continue;
            if (current >= 0 && t->i_id != current)
                continue;
            w = t->video->i_width;
            h = t->video->i_height;
            dw = w;
            dh = h;
            if (t->video->i_sar_num && t->video->i_sar_den && t->video->i_sar_num != t->video->i_sar_den)
                dw = unsigned(double(w) * t->video->i_sar_num / t->video->i_sar_den + 0.5);
            if (t->video->i_orientation >= libvlc_video_orient_left_top) { // rotated 90/270
                std::swap(w, h);
                std::swap(dw, dh);
            }
            break;
        }
        if (n)
            libvlc_media_tracks_release(tracks, n);
        libvlc_media_release(media);
    }
    w = (w + 1) & ~1u;
    h = (h + 1) & ~1u;
    if (!dw || !dh) {
        dw = w;
        dh = h;
    }
    const unsigned pitchY = (w + 31) & ~31u;
    const unsigned pitchC = (w / 2 + 31) & ~31u;
    {
        QMutexLocker lock(&self->m_frameMutex);
        self->m_width = w;
        self->m_height = h;
        self->m_displayWidth = dw;
        self->m_displayHeight = dh;
        self->m_pitchY = pitchY;
        self->m_pitchC = pitchC;
        // A little slack below the last line: some converters write aligned blocks.
        const size_t size = size_t(pitchY) * (h + 16) + 2 * size_t(pitchC) * (h / 2 + 16);
        for (auto &b : self->m_buffers)
            b.assign(size, 0);
        self->m_front = 0;
        self->m_backWritten = false;
    }
    pitches[0] = pitchY;
    pitches[1] = pitches[2] = pitchC;
    lines[0] = h;
    lines[1] = lines[2] = h / 2;
    QMetaObject::invokeMethod(self, [self, dw, dh] { emit self->videoSizeChanged(QSize(int(dw), int(dh))); },
                              Qt::QueuedConnection);
    return 3;
}

void Player::vmemCleanup(void *opaque)
{
    auto *self = static_cast<Player *>(opaque);
    {
        QMutexLocker lock(&self->m_frameMutex);
        self->m_width = self->m_height = self->m_displayWidth = self->m_displayHeight = 0;
        self->m_serial = 0;
        for (auto &b : self->m_buffers)
            std::vector<uchar>().swap(b);
    }
    QMetaObject::invokeMethod(self, [self] {
        emit self->videoSizeChanged(QSize());
        emit self->frameReady();
    }, Qt::QueuedConnection);
}

void *Player::vmemLock(void *opaque, void **planes)
{
    auto *self = static_cast<Player *>(opaque);
    QMutexLocker lock(&self->m_frameMutex);
    uchar *base = self->m_buffers[1 - self->m_front].data();
    planes[0] = base;
    planes[1] = base + size_t(self->m_pitchY) * (self->m_height + 16);
    planes[2] = static_cast<uchar *>(planes[1]) + size_t(self->m_pitchC) * (self->m_height / 2 + 16);
    self->m_backWritten = true;
    return nullptr;
}

void Player::vmemUnlock(void *, void *, void *const *) {}

void Player::vmemDisplay(void *opaque, void *)
{
    auto *self = static_cast<Player *>(opaque);
    {
        QMutexLocker lock(&self->m_frameMutex);
        if (self->m_backWritten) {
            self->m_front = 1 - self->m_front;
            self->m_backWritten = false;
            ++self->m_serial;
        }
    }
    // Coalesce: only queue a repaint if the previous one has been handled.
    if (!self->m_framePending.exchange(true)) {
        QMetaObject::invokeMethod(self, [self] {
            self->m_framePending = false;
            emit self->frameReady();
        }, Qt::QueuedConnection);
    }
}

void Player::withFrame(const std::function<void(const Frame &)> &fn) const
{
    QMutexLocker lock(&m_frameMutex);
    if (m_width == 0 || m_height == 0 || m_buffers[m_front].empty() || m_serial == 0)
        return;
    const uchar *base = m_buffers[m_front].data();
    const uchar *u = base + size_t(m_pitchY) * (m_height + 16);
    const uchar *v = u + size_t(m_pitchC) * (m_height / 2 + 16);
    const Frame frame{{base, u, v},
                      {int(m_pitchY), int(m_pitchC), int(m_pitchC)},
                      QSize(int(m_width), int(m_height)),
                      QSize(int(m_displayWidth), int(m_displayHeight)),
                      m_serial,
                      m_height >= 720};
    fn(frame);
}

bool Player::hasFrame() const
{
    QMutexLocker lock(&m_frameMutex);
    return m_width > 0 && m_serial > 0;
}

QSize Player::videoSize() const
{
    QMutexLocker lock(&m_frameMutex);
    return QSize(int(m_displayWidth), int(m_displayHeight));
}

QImage Player::snapshot() const
{
    QImage out;
    withFrame([&](const Frame &f) {
        // Limited-range YUV to RGB (BT.709 for HD, BT.601 otherwise), 16.16 fixed point.
        const int rv = f.bt709 ? 117504 : 104597, gu = f.bt709 ? 13954 : 25675, gv = f.bt709 ? 34903 : 53279,
                  bu = f.bt709 ? 138453 : 132201;
        QImage img(f.size, QImage::Format_RGB32);
        for (int y = 0; y < f.size.height(); ++y) {
            const uchar *py = f.planes[0] + y * f.pitches[0];
            const uchar *pu = f.planes[1] + (y / 2) * f.pitches[1];
            const uchar *pv = f.planes[2] + (y / 2) * f.pitches[2];
            auto *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < f.size.width(); ++x) {
                const int c = (py[x] - 16) * 76309, d = pu[x / 2] - 128, e = pv[x / 2] - 128;
                dst[x] = qRgb(qBound(0, (c + rv * e + 32768) >> 16, 255),
                              qBound(0, (c - gu * d - gv * e + 32768) >> 16, 255),
                              qBound(0, (c + bu * d + 32768) >> 16, 255));
            }
        }
        out = f.displaySize == f.size ? img
                                      : img.scaled(f.displaySize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    });
    return out;
}

void Player::debugTracks() const
{
    if (!m_mp)
        return;
    libvlc_media_t *m = libvlc_media_player_get_media(m_mp);
    if (!m)
        return;
    libvlc_media_track_t **tracks = nullptr;
    const unsigned n = libvlc_media_tracks_get(m, &tracks);
    for (unsigned i = 0; i < n; ++i)
        qInfo() << "media track" << tracks[i]->i_type << tracks[i]->i_id << tracks[i]->psz_language
                << tracks[i]->psz_description;
    libvlc_media_tracks_release(tracks, n);
    libvlc_media_slave_t **slaves = nullptr;
    const unsigned ns = libvlc_media_slaves_get(m, &slaves);
    for (unsigned i = 0; i < ns; ++i)
        qInfo() << "slave" << slaves[i]->psz_uri << slaves[i]->i_type;
    libvlc_media_slaves_release(slaves, ns);
    libvlc_media_release(m);
}
