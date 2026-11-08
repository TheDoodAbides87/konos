#include "mediadialogs.h"
#include "filedialogs.h"
#include "library.h"
#include "player.h"
#include "prefs.h"
#include "theme.h"
#include "version.h"
#include "vlcvars.h"
#include "widgets.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QTreeWidget>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPainter>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <vlc/vlc.h>

using Dialog::button;
using Dialog::caption;
using FileDialogs::mediaFilter;

namespace {


Dropdown *dropdown(int width = 330)
{
    auto *d = new Dropdown;
    d->setFixedWidth(width);
    return d;
}

QWidget *box(QVBoxLayout *&lay)
{
    auto *w = new QWidget;
    lay = new QVBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);
    return w;
}

QProgressBar *progressBar()
{
    auto *bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setTextVisible(false);
    bar->setFixedHeight(6);
    bar->setStyleSheet(QString("QProgressBar { background: %1; border: none; border-radius: 3px; }"
                               "QProgressBar::chunk { background: %2; border-radius: 3px; }")
                           .arg(theme().pal().input.name(), theme().accent().name()));
    return bar;
}


// Quoted for a sout chain.
QString soutQuote(const QString &s)
{
    return "'" + QString(s).replace('\\', "\\\\").replace('\'', "\\'") + "'";
}
} // namespace

// ---------------------------------------------------------------------------
// SoutJob

SoutJob::SoutJob(QObject *parent) : QObject(parent)
{
    m_poll.setInterval(250);
    connect(&m_poll, &QTimer::timeout, this, &SoutJob::poll);
}

SoutJob::~SoutJob()
{
    release();
    if (m_vlc)
        libvlc_release(m_vlc);
}

bool SoutJob::start(const QString &location, const QString &sout, const QStringList &options)
{
    release();
    if (!m_vlc) {
        const char *args[] = {"--no-video-title-show", "--no-stats", "--no-osd", "--quiet",
                              "--no-sub-autodetect-file", "--no-metadata-network-access"};
        m_vlc = libvlc_new(int(std::size(args)), args);
        if (!m_vlc)
            return false;
        libvlc_set_user_agent(m_vlc, "Konos", "Konos/" KONOS_VERSION);
    }
    const QUrl url(location);
    const bool isUrl = url.isValid() && url.scheme().size() > 1;
    if ((!isUrl || url.isLocalFile()) && !QFileInfo::exists(isUrl ? url.toLocalFile() : location))
        return false; // VLC would "finish" a missing file without complaint
    libvlc_media_t *media =
        isUrl && !url.isLocalFile() ? libvlc_media_new_location(m_vlc, location.toUtf8().constData())
                                    : libvlc_media_new_path(m_vlc, (isUrl ? url.toLocalFile() : location).toUtf8().constData());
    if (!media)
        return false;
    libvlc_media_add_option(media, (":sout=" + sout).toUtf8().constData());
    libvlc_media_add_option(media, ":sout-keep");
    for (const QString &o : options)
        libvlc_media_add_option(media, o.toUtf8().constData());
    m_mp = libvlc_media_player_new_from_media(media);
    libvlc_media_release(media);
    if (!m_mp)
        return false;
    m_location = location;
    m_started = false;
    if (libvlc_media_player_play(m_mp) != 0) {
        release();
        return false;
    }
    m_poll.start();
    return true;
}

void SoutJob::stop()
{
    release();
}

void SoutJob::release()
{
    m_poll.stop();
    if (m_mp) {
        libvlc_media_player_stop(m_mp);
        libvlc_media_player_release(m_mp);
        m_mp = nullptr;
    }
}

void SoutJob::poll()
{
    if (!m_mp)
        return;
    const libvlc_state_t s = libvlc_media_player_get_state(m_mp);
    if (s == libvlc_Playing)
        m_started = true;
    if (s == libvlc_Ended || s == libvlc_Error) {
        const bool ok = s == libvlc_Ended; // a short item can end before it was ever seen playing
        release();
        emit finished(ok, ok ? QString() : tr("VLC couldn't read or encode this item."));
        return;
    }
    const float pos = libvlc_media_player_get_position(m_mp);
    if (pos >= 0)
        emit progress(pos);
}

// ---------------------------------------------------------------------------
// CaptureDeviceOverlay

QList<CaptureDeviceOverlay::Device> CaptureDeviceOverlay::cameras()
{
    QList<Device> out;
    const QDir sys("/sys/class/video4linux");
    for (const QString &node : sys.entryList({"video*"}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
        // Each camera has several nodes; index 0 is the one with the picture.
        QFile index(sys.filePath(node + "/index"));
        if (index.open(QIODevice::ReadOnly) && index.readAll().trimmed() != "0")
            continue;
        QFile name(sys.filePath(node + "/name"));
        QString label = name.open(QIODevice::ReadOnly) ? QString::fromUtf8(name.readAll()).trimmed() : node;
        out.append({"/dev/" + node, label.isEmpty() ? node : label});
    }
    return out;
}

QList<CaptureDeviceOverlay::Device> CaptureDeviceOverlay::soundInputs()
{
    QList<Device> out;
    QProcess p;
    p.start("pactl", {"-f", "json", "list", "sources"});
    if (p.waitForFinished(2000) && p.exitCode() == 0) {
        for (const QJsonValue &v : QJsonDocument::fromJson(p.readAllStandardOutput()).array()) {
            const QJsonObject o = v.toObject();
            const QString name = o.value("name").toString();
            if (name.isEmpty())
                continue;
            QString label = o.value("description").toString();
            if (name.endsWith(".monitor"))
                label = tr("What's playing on %1").arg(label.remove("Monitor of ").trimmed());
            out.append({name, label.isEmpty() ? name : label});
        }
    }
    return out;
}

bool CaptureDeviceOverlay::screenCaptureWorks()
{
    return qEnvironmentVariable("XDG_SESSION_TYPE") != "wayland" && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
}

CaptureDeviceOverlay::CaptureDeviceOverlay(QWidget *window, PlayFn play)
    : PanelOverlay(tr("Open Capture Device"), {620, 380}, window), m_playFn(std::move(play))
{
    m_kind = new SegmentedControl({tr("Camera"), tr("Screen"), tr("Sound Input")});
    m_kind->setFixedWidth(420);
    body()->addWidget(m_kind, 0, Qt::AlignLeft);

    QVBoxLayout *lay;
    m_cameraBox = box(lay);
    m_camera = dropdown();
    for (const Device &d : cameras())
        m_camera->addItem(d.name, d.id);
    lay->addWidget(settingRow(tr("Camera"), m_camera));
    m_cameraSound = dropdown();
    m_cameraSound->addItem(tr("No sound"), QString());
    for (const Device &d : soundInputs())
        m_cameraSound->addItem(d.name, d.id);
    lay->addWidget(settingRow(tr("Sound from"), m_cameraSound));
    body()->addWidget(m_cameraBox);

    m_screenBox = box(lay);
    m_fps = dropdown();
    for (int fps : {10, 15, 30, 60})
        m_fps->addItem(tr("%1 frames a second").arg(fps), fps);
    m_fps->setCurrentIndex(2);
    lay->addWidget(settingRow(tr("Frame rate"), m_fps));
    body()->addWidget(m_screenBox);

    m_soundBox = box(lay);
    m_sound = dropdown();
    m_sound->addItem(tr("Default input"), QString());
    for (const Device &d : soundInputs())
        m_sound->addItem(d.name, d.id);
    lay->addWidget(settingRow(tr("Input"), m_sound));
    body()->addWidget(m_soundBox);

    m_status = caption(QString());
    body()->addWidget(m_status);
    body()->addStretch();
    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *cancel = button(tr("Cancel"));
    m_play = button(tr("Play"), true);
    foot->addWidget(cancel);
    foot->addWidget(m_play);
    body()->addLayout(foot);

    connect(m_kind, &SegmentedControl::currentChanged, this, &CaptureDeviceOverlay::updateKind);
    connect(cancel, &QPushButton::clicked, this, &PanelOverlay::close);
    connect(m_play, &QPushButton::clicked, this, &CaptureDeviceOverlay::go);
    m_kind->setCurrentIndex(m_camera->count() == 0 && !screenCaptureWorks() ? 2 : 0);
    updateKind();
}

void CaptureDeviceOverlay::updateKind()
{
    const int k = m_kind->currentIndex();
    m_cameraBox->setVisible(k == 0);
    m_screenBox->setVisible(k == 1);
    m_soundBox->setVisible(k == 2);
    bool ok = true;
    QString status;
    if (k == 0 && m_camera->count() == 0) {
        ok = false;
        status = tr("No camera found. Plug one in, then open this again.");
    } else if (k == 1 && !screenCaptureWorks()) {
        ok = false;
        status = tr("VLC 3 can only capture the screen in an X11 session. Under Wayland, use your desktop's "
                    "screen recorder instead.");
    } else if (k == 1) {
        status = tr("Plays the whole screen. Press R while it plays to record it.");
    } else if (k == 2) {
        status = tr("Plays the sound coming in. Press R while it plays to record it.");
    }
    m_status->setText(status);
    m_play->setEnabled(ok);
}

void CaptureDeviceOverlay::go()
{
    QString mrl;
    QStringList options;
    switch (m_kind->currentIndex()) {
    case 0:
        mrl = "v4l2://" + m_camera->currentData().toString();
        if (const QString sound = m_cameraSound->currentData().toString(); !sound.isEmpty())
            options << ":input-slave=pulse://" + sound;
        break;
    case 1:
        mrl = "screen://";
        options << QString(":screen-fps=%1").arg(m_fps->currentData().toInt());
        break;
    default:
        mrl = "pulse://" + m_sound->currentData().toString();
        break;
    }
    m_playFn(mrl, options);
    close();
}

// ---------------------------------------------------------------------------
// ConvertOverlay

QList<ConvertOverlay::Profile> ConvertOverlay::profiles()
{
    const QString file = ":std{access=file,mux=%3,dst=%2}";
    return {
        {tr("Video — H.264 + AAC (MP4)"), "mp4",
         "#transcode{vcodec=h264,venc=x264{preset=fast,crf=21},acodec=mp4a,ab=192,channels=2,samplerate=48000,"
         "scodec=none%1}" + file.arg("%2", "mp4"), true},
        {tr("Video — H.265 + AAC (MP4, smaller, slower)"), "mp4",
         "#transcode{vcodec=hevc,acodec=mp4a,ab=192,channels=2,samplerate=48000,scodec=none%1}" + file.arg("%2", "mp4"),
         true},
        {tr("Video — Theora + Vorbis (OGG)"), "ogv",
         "#transcode{vcodec=theo,vb=2500,acodec=vorb,ab=192,channels=2,samplerate=44100,scodec=none%1}" +
             file.arg("%2", "ogg"), true},
        {tr("Audio — MP3"), "mp3", "#transcode{acodec=mp3,ab=192,channels=2,samplerate=44100%1}" + file.arg("%2", "raw"),
         false},
        {tr("Audio — FLAC (lossless)"), "flac", "#transcode{acodec=flac%1}" + file.arg("%2", "raw"), false},
        {tr("Audio — Ogg Vorbis"), "ogg", "#transcode{acodec=vorb,ab=192,channels=2,samplerate=44100%1}" + file.arg("%2", "ogg"),
         false},
        {tr("Audio — WAV (uncompressed)"), "wav", "#transcode{acodec=s16l,channels=2,samplerate=44100%1}" + file.arg("%2", "wav"),
         false},
    };
}

QString ConvertOverlay::soutFor(const Profile &p, const QString &dest, int maxHeight)
{
    const QString extra = p.video && maxHeight > 0 ? QString(",maxheight=%1").arg(maxHeight) : QString();
    return QString(p.chain).arg(extra, soutQuote(dest));
}

ConvertOverlay::ConvertOverlay(const QStringList &files, QWidget *window)
    : PanelOverlay(tr("Convert / Save"), {720, 720}, window), m_job(new SoutJob(this))
{
    auto *head = new QHBoxLayout;
    head->addWidget(caption(tr("Files to convert")), 1);
    auto *add = button(tr("Add Files…"));
    auto *remove = button(tr("Remove"));
    head->addWidget(add);
    head->addWidget(remove);
    body()->addLayout(head);
    m_list = new QListWidget;
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    body()->addWidget(m_list, 1);

    m_profile = dropdown();
    for (const Profile &p : profiles())
        m_profile->addItem(p.name);
    body()->addWidget(settingRow(tr("Convert to"), m_profile));
    m_size = dropdown();
    m_size->addItem(tr("Keep the original size"), 0);
    for (int h : {2160, 1080, 720, 480})
        m_size->addItem(tr("At most %1p").arg(h), h);
    body()->addWidget(settingRow(tr("Picture size"), m_size));

    auto *dest = new QWidget;
    auto *dl = new QHBoxLayout(dest);
    dl->setContentsMargins(0, 0, 0, 0);
    m_folder = new QLineEdit;
    m_folder->setPlaceholderText(tr("Next to each original"));
    m_folder->setFixedSize(220, 36);
    auto *browse = button(tr("Browse"));
    dl->addWidget(m_folder);
    dl->addWidget(browse);
    body()->addWidget(settingRow(tr("Save in"), dest));

    m_bar = progressBar();
    m_bar->hide();
    body()->addWidget(m_bar);
    m_status = caption(tr("The originals stay as they are; each converted copy gets the new format's extension."));
    body()->addWidget(m_status);

    auto *foot = new QHBoxLayout;
    foot->addStretch();
    m_cancel = button(tr("Close"));
    m_start = button(tr("Start"), true);
    foot->addWidget(m_cancel);
    foot->addWidget(m_start);
    body()->addLayout(foot);

    connect(add, &QPushButton::clicked, this, [this] {
        addFiles(FileDialogs::openFiles(this, tr("Files to convert"),
                                        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation), mediaFilter()));
    });
    connect(remove, &QPushButton::clicked, this, [this] { qDeleteAll(m_list->selectedItems()); });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = FileDialogs::existingDirectory(this, tr("Save converted files in"), m_folder->text());
        if (!dir.isEmpty())
            m_folder->setText(dir);
    });
    connect(m_profile, &QComboBox::currentIndexChanged, this,
            [this](int i) { m_size->setEnabled(profiles().value(i).video); });
    connect(m_start, &QPushButton::clicked, this, &ConvertOverlay::start);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_job->isRunning()) {
            m_job->stop();
            QFile::remove(m_dest); // half a file is no use
            finish(tr("Cancelled. %1 of %2 converted.").arg(m_done).arg(m_list->count()));
        } else {
            close();
        }
    });
    connect(m_job, &SoutJob::progress, this, [this](double pos) {
        m_bar->setValue(int((m_at + pos) / qMax(1, m_list->count()) * 1000));
    });
    connect(m_job, &SoutJob::finished, this, [this](bool ok, const QString &error) {
        if (ok) {
            ++m_done;
            m_list->item(m_at)->setText(QFileInfo(m_dest).fileName() + "  ✓");
        } else {
            ++m_failed;
            QFile::remove(m_dest);
            m_list->item(m_at)->setText(m_list->item(m_at)->data(Qt::UserRole).toString() + "  — " + error);
        }
        next();
    });
    addFiles(files);
}

void ConvertOverlay::addFiles(const QStringList &files)
{
    for (const QString &f : files) {
        if (f.isEmpty())
            continue;
        auto *item = new QListWidgetItem(Library::displayName(f), m_list);
        item->setData(Qt::UserRole, f);
        item->setToolTip(f);
    }
}

QString ConvertOverlay::destinationFor(const QString &file, const Profile &p) const
{
    const QUrl url(file);
    const QString local = url.isLocalFile() ? url.toLocalFile() : file;
    const bool isLocal = QFileInfo(local).isAbsolute() && QFileInfo::exists(local);
    QString dir = m_folder->text().trimmed();
    if (dir.isEmpty())
        dir = isLocal ? QFileInfo(local).absolutePath()
                      : QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    QString base = isLocal ? QFileInfo(local).completeBaseName() : Library::displayName(file);
    base.replace('/', '-');
    QString dest = QDir(dir).filePath(base + "." + p.ext);
    for (int n = 2; QFileInfo::exists(dest) || dest == local; ++n)
        dest = QDir(dir).filePath(QString("%1 (%2).%3").arg(base).arg(n).arg(p.ext));
    return dest;
}

void ConvertOverlay::start()
{
    if (m_list->count() == 0) {
        m_status->setText(tr("Add the files to convert first."));
        return;
    }
    for (int i = 0; i < m_list->count(); ++i)
        m_list->item(i)->setText(Library::displayName(m_list->item(i)->data(Qt::UserRole).toString()));
    m_at = -1;
    m_done = m_failed = 0;
    m_start->setEnabled(false);
    m_cancel->setText(tr("Cancel"));
    m_bar->setValue(0);
    m_bar->show();
    next();
}

void ConvertOverlay::next()
{
    ++m_at;
    if (m_at >= m_list->count()) {
        finish(m_failed ? tr("Done: %1 converted, %2 failed.").arg(m_done).arg(m_failed)
                        : (m_done == 1 ? tr("Done: 1 file converted.") : tr("Done: %1 files converted.").arg(m_done)));
        return;
    }
    const Profile p = profiles().value(m_profile->currentIndex());
    const QString file = m_list->item(m_at)->data(Qt::UserRole).toString();
    m_dest = destinationFor(file, p);
    QDir().mkpath(QFileInfo(m_dest).path());
    m_status->setText(tr("Converting %1 of %2: %3").arg(m_at + 1).arg(m_list->count()).arg(Library::displayName(file)));
    m_list->setCurrentRow(m_at);
    QStringList options;
    if (!p.video)
        options << ":no-sout-video" << ":no-sout-spu";
    if (!m_job->start(file, soutFor(p, m_dest, m_size->currentData().toInt()), options)) {
        ++m_failed;
        m_list->item(m_at)->setText(Library::displayName(file) + "  — " + tr("couldn't be opened"));
        QMetaObject::invokeMethod(this, &ConvertOverlay::next, Qt::QueuedConnection);
    }
}

void ConvertOverlay::finish(const QString &message)
{
    m_status->setText(message);
    m_start->setEnabled(true);
    m_cancel->setText(tr("Close"));
    m_bar->setValue(1000);
}

// ---------------------------------------------------------------------------
// StreamOverlay

SoutJob *StreamOverlay::httpJob()
{
    static SoutJob *job = new SoutJob(qApp);
    return job;
}

QStringList StreamOverlay::addresses(int port)
{
    QStringList out;
    for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
        if (!(i.flags() & QNetworkInterface::IsUp) || (i.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &e : i.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && !e.ip().isLoopback())
                out << QString("http://%1:%2/stream").arg(e.ip().toString()).arg(port);
    }
    return out;
}

StreamOverlay::StreamOverlay(Player *player, const QString &current, QWidget *window, bool cast)
    : PanelOverlay(tr("Stream"), {680, 560}, window), m_player(player), m_source(current)
{
    auto *src = new QWidget;
    auto *sl = new QHBoxLayout(src);
    sl->setContentsMargins(0, 0, 0, 0);
    m_sourceLabel = new QLabel;
    m_sourceLabel->setFixedWidth(260);
    auto *choose = button(tr("Choose…"));
    sl->addWidget(m_sourceLabel);
    sl->addWidget(choose);
    body()->addWidget(settingRow(tr("What to stream"), src));

    m_method = new SegmentedControl({tr("This Network"), tr("Chromecast")});
    m_method->setFixedWidth(330);
    body()->addWidget(settingRow(tr("Send to"), m_method));

    QVBoxLayout *lay;
    m_httpBox = box(lay);
    m_port = new QLineEdit("8080");
    m_port->setFixedSize(120, 36);
    m_port->setInputMask("00000");
    lay->addWidget(settingRow(tr("Port"), m_port));
    m_quality = dropdown();
    m_quality->addItem(tr("H.264 + AAC (plays anywhere)"), true);
    m_quality->addItem(tr("Original (no re-encoding)"), false);
    lay->addWidget(settingRow(tr("Format"), m_quality));
    lay->addWidget(caption(tr("Other devices on your network open the address below, in VLC or a browser. "
                              "A firewall may need the port opened.")));
    body()->addWidget(m_httpBox);

    m_castBox = box(lay);
    m_devices = new QListWidget;
    m_devices->setMinimumHeight(140);
    m_devices->setStyleSheet(QString("QListWidget { background: %1; border-radius: 8px; padding: 6px; }")
                                 .arg(theme().pal().input.name()));
    lay->addWidget(m_devices, 1);
    lay->addWidget(caption(tr("What's playing moves to the device; the controls here keep working. "
                              "Chromecasts are found on your network automatically.")));
    body()->addWidget(m_castBox, 1);

    m_status = caption(QString());
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body()->addWidget(m_status);
    body()->addStretch();
    auto *foot = new QHBoxLayout;
    m_copy = button(tr("Copy Address"));
    auto *copy = m_copy;
    foot->addWidget(copy);
    foot->addStretch();
    m_stop = button(tr("Stop Streaming"));
    m_go = button(tr("Start"), true);
    auto *close = button(tr("Close"));
    foot->addWidget(m_stop);
    foot->addWidget(close);
    foot->addWidget(m_go);
    body()->addLayout(foot);

    connect(choose, &QPushButton::clicked, this, [this] {
        const QString f = FileDialogs::openFile(this, tr("File to stream"),
                                                QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
                                                mediaFilter());
        if (!f.isEmpty()) {
            m_source = f;
            m_method->setCurrentIndex(0); // casting is for what's playing
            updateState();
        }
    });
    connect(m_method, &SegmentedControl::currentChanged, this, [this] {
        if (m_method->currentIndex() == 1)
            discover();
        updateState();
    });
    connect(copy, &QPushButton::clicked, this, [this] {
        const QStringList a = addresses(m_port->text().toInt());
        if (!a.isEmpty())
            QApplication::clipboard()->setText(a.first());
    });
    connect(close, &QPushButton::clicked, this, &PanelOverlay::close);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (m_method->currentIndex() == 1)
            m_player->setRenderer(nullptr);
        else
            httpJob()->stop();
        updateState();
    });
    connect(m_go, &QPushButton::clicked, this, [this] {
        if (m_method->currentIndex() == 0) {
            startHttp();
            return;
        }
        const int row = m_devices->currentRow();
        if (row < 0 || row >= m_items.size()) {
            m_status->setText(tr("Pick a device first."));
            return;
        }
        m_player->setRenderer(m_items[row]);
        updateState();
    });
    connect(httpJob(), &SoutJob::finished, this, [this](bool ok, const QString &error) {
        updateState();
        m_status->setText(ok ? tr("The stream reached the end.") : error);
    });
    connect(m_devices, &QListWidget::itemDoubleClicked, m_go, &QPushButton::click);
    m_method->setCurrentIndex(cast || !m_player->renderer().isEmpty() ? 1 : 0);
    if (m_method->currentIndex() == 1)
        discover();
    if (httpJob()->isRunning())
        m_source = httpJob()->location();
    updateState();
}

StreamOverlay::~StreamOverlay()
{
    if (m_discoverer) {
        libvlc_renderer_discoverer_stop(m_discoverer);
        libvlc_renderer_discoverer_release(m_discoverer);
    }
    for (libvlc_renderer_item_t *i : m_items)
        libvlc_renderer_item_release(i);
}

void StreamOverlay::updateState()
{
    const bool cast = m_method->currentIndex() == 1;
    m_httpBox->setVisible(!cast);
    m_castBox->setVisible(cast);
    m_sourceLabel->setText(m_sourceLabel->fontMetrics().elidedText(
        m_source.isEmpty() ? tr("Nothing — choose a file") : Library::displayName(m_source), Qt::ElideMiddle, 250));
    const bool streaming = httpJob()->isRunning();
    if (!cast) {
        m_go->setText(streaming ? tr("Restart") : tr("Start"));
        m_go->setEnabled(!m_source.isEmpty());
        m_stop->setVisible(streaming);
        m_copy->setVisible(streaming);
        if (streaming) {
            const QStringList a = addresses(m_port->text().toInt());
            m_status->setText(a.isEmpty() ? tr("Streaming, but this computer has no network address.")
                                          : tr("Streaming %1 at:\n%2").arg(Library::displayName(httpJob()->location()),
                                                                           a.join('\n')));
        } else {
            m_status->clear();
        }
        return;
    }
    const QString device = m_player->renderer();
    m_copy->hide();
    m_go->setText(tr("Cast"));
    m_go->setEnabled(m_player->hasMedia());
    m_stop->setVisible(!device.isEmpty());
    if (!device.isEmpty())
        m_status->setText(tr("Casting to %1.").arg(device));
    else if (!m_player->hasMedia())
        m_status->setText(tr("Play something first, then cast it."));
    else if (m_items.isEmpty())
        m_status->setText(tr("Looking for Chromecasts on your network…"));
    else
        m_status->clear();
}

void StreamOverlay::startHttp()
{
    if (m_source.isEmpty())
        return;
    const int port = qBound(1, m_port->text().toInt(), 65535);
    const QString out = QString(":http{mux=ts,dst=:%1/stream}").arg(port);
    const QString sout = m_quality->currentData().toBool()
                             ? // Headers at every keyframe, so a device can join mid-stream.
                             "#transcode{vcodec=h264,venc=x264{preset=veryfast,tune=zerolatency,keyint=50,"
                               "options={repeat-headers=1}},vb=4000,"
                               "acodec=mp4a,ab=192,channels=2,samplerate=48000,scodec=none}" + out
                             : "#" + out.mid(1);
    if (!httpJob()->start(m_source, sout)) {
        m_status->setText(tr("Couldn't start the stream."));
        return;
    }
    updateState();
}

void StreamOverlay::discover()
{
    if (m_discoverer || !m_player->instance())
        return;
    // Discovery reports from VLC's own thread; hand each item over to ours.
    m_discoverer = libvlc_renderer_discoverer_new(m_player->instance(), "microdns_renderer");
    if (!m_discoverer) {
        m_status->setText(tr("Finding Chromecasts needs VLC's mDNS plugin (vlc-plugin-mdns)."));
        return;
    }
    libvlc_event_manager_t *em = libvlc_renderer_discoverer_event_manager(m_discoverer);
    auto added = [](const libvlc_event_t *e, void *self) {
        libvlc_renderer_item_t *item = libvlc_renderer_item_hold(e->u.renderer_discoverer_item_added.item);
        QMetaObject::invokeMethod(static_cast<StreamOverlay *>(self), [self, item] {
            auto *me = static_cast<StreamOverlay *>(self);
            me->m_items << item;
            me->m_devices->addItem(QString::fromUtf8(libvlc_renderer_item_name(item)));
            if (me->m_devices->currentRow() < 0)
                me->m_devices->setCurrentRow(0);
            me->updateState();
        });
    };
    libvlc_event_attach(em, libvlc_RendererDiscovererItemAdded, added, this);
    libvlc_renderer_discoverer_start(m_discoverer);
}

// ---------------------------------------------------------------------------
// AboutOverlay

namespace {
class LogoWidget : public QWidget {
public:
    LogoWidget() { setFixedSize(88, 88); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        Theme::paintLogo(&p, QRectF(rect()), theme().accent());
    }
};
} // namespace

AboutOverlay::AboutOverlay(QWidget *window) : PanelOverlay(tr("About Konos"), {560, 470}, window)
{
    auto *head = new QHBoxLayout;
    head->setSpacing(18);
    head->addWidget(new LogoWidget);
    auto *name = new QLabel(QString("<div style='font-size:22px'>Konos Media Player</div>"
                                    "<div>%1</div>")
                                .arg(tr("Version %1").arg(QStringLiteral(KONOS_VERSION_LABEL KONOS_EDITION))));
    head->addWidget(name, 1);
    body()->addLayout(head);

    auto *text = new QLabel(
        tr("<p><b>Konos is an overhaul of VLC.</b> Playback, codecs, subtitles, discs and effects come from "
           "<a href='https://www.videolan.org/vlc/'>libVLC</a> %1, the engine inside VLC, made by VideoLAN and "
           "the VLC contributors. Konos gives it a new interface.</p>"
           "<p>The design is by <a href='https://www.youtube.com/@juxtopposed'>Juxtopposed</a>, from the video "
           "<a href='https://youtu.be/ERFeOgqk324'>I Redesigned VLC After 25 Years</a>.</p>"
           "<p>Konos is in early development: it changes often and things may break.</p>"
           "<p>Konos's own code is MIT-licensed. libVLC is LGPL 2.1 or later; Qt 6 is LGPL 3. Konos is independent "
           "and not affiliated with VideoLAN or Juxtopposed. “VLC” is a trademark of VideoLAN.</p>"
           "<p><a href='https://github.com/" KONOS_RELEASES_REPO "/releases'>Releases and what's new</a></p>")
            .arg(QString::fromUtf8(libvlc_get_version()).section(' ', 0, 0)));
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setOpenExternalLinks(true);
    text->setStyleSheet(QString("a { color: %1; }").arg(theme().accent().name()));
    body()->addWidget(text);
    body()->addStretch();
    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *ok = button(tr("Close"), true);
    foot->addWidget(ok);
    body()->addLayout(foot);
    connect(ok, &QPushButton::clicked, this, &PanelOverlay::close);
}

// ---------------------------------------------------------------------------
// PluginsOverlay

PluginsOverlay::PluginsOverlay(QWidget *window) : PanelOverlay(tr("Plugins"), {900, 760}, window)
{
    auto *intro = caption(tr("The VLC plugins Konos plays and converts with, as installed on this computer. VLC "
                             "picks the highest-scoring plugin that can handle a job. Konos can't install VLC "
                             "add-ons, skins or Lua extensions: libVLC doesn't support them."));
    body()->addWidget(intro);
    auto *bar = new QHBoxLayout;
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search plugins…"));
    m_search->setProperty("role", "search");
    m_search->setFixedHeight(38);
    m_search->setClearButtonEnabled(true);
    auto *icon = new QLabel(m_search);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    icon->setGeometry(12, 10, 18, 18);
    icon->setPixmap(theme().pixmap("search", theme().pal().textDim, 18, 2.0));
    m_capability = dropdown(230);
    bar->addWidget(m_search, 1);
    bar->addWidget(m_capability);
    body()->addLayout(bar);

    m_table = new QTreeWidget;
    m_table->setColumnCount(3);
    m_table->setHeaderLabels({tr("Name"), tr("Capability"), tr("Score")});
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setSortingEnabled(true);
    m_table->header()->setStretchLastSection(false);
    m_table->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_table->setColumnWidth(1, 220);
    m_table->setColumnWidth(2, 80);
    QStringList caps;
    for (const VlcVars::Module &m : VlcVars::modules()) {
        auto *item = new QTreeWidgetItem(m_table);
        item->setText(0, m.longName.isEmpty() ? m.name : m.longName);
        item->setText(1, m.capability);
        item->setData(2, Qt::DisplayRole, m.score); // sorts as a number
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(0, QString("%1 (%2)").arg(m.longName, m.object));
        if (!caps.contains(m.capability))
            caps << m.capability;
    }
    caps.sort();
    m_capability->addItem(tr("All capabilities"), QString());
    for (const QString &c : caps)
        m_capability->addItem(c, c);
    m_table->sortByColumn(1, Qt::AscendingOrder);
    body()->addWidget(m_table, 1);

    auto *foot = new QHBoxLayout;
    m_count = caption(QString());
    foot->addWidget(m_count, 1);
    auto *close = button(tr("Close"), true);
    foot->addWidget(close);
    body()->addLayout(foot);
    connect(close, &QPushButton::clicked, this, &PanelOverlay::close);
    connect(m_search, &QLineEdit::textChanged, this, &PluginsOverlay::filter);
    connect(m_capability, &QComboBox::currentIndexChanged, this, &PluginsOverlay::filter);
    filter();
    m_search->setFocus();
}

void PluginsOverlay::filter()
{
    const QString t = m_search->text().trimmed();
    const QString cap = m_capability->currentData().toString();
    int shown = 0;
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_table->topLevelItem(i);
        const bool match = (cap.isEmpty() || item->text(1) == cap) &&
                           (t.isEmpty() || item->text(0).contains(t, Qt::CaseInsensitive) ||
                            item->text(1).contains(t, Qt::CaseInsensitive) ||
                            item->toolTip(0).contains(t, Qt::CaseInsensitive));
        item->setHidden(!match);
        shown += match;
    }
    m_count->setText(shown == m_table->topLevelItemCount() ? tr("%1 plugins").arg(shown)
                                                           : tr("%1 of %2 plugins").arg(shown).arg(m_table->topLevelItemCount()));
}

