#include "chrome.h"
#include "player.h"
#include "prefs.h"
#include "seekpreview.h"
#include "theme.h"
#include "waveform.h"
#include "library.h"
#include "widgets.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QUrl>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWindow>

namespace {
class LogoWidget : public QWidget {
public:
    explicit LogoWidget(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(24, 24);
        connect(&theme(), &Theme::changed, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        Theme::paintLogo(&p, QRectF(2, 1, 20, 21), theme().accent());
    }
};
} // namespace

// ---------------------------------------------------------------------------
// TitleBar

TitleBar::TitleBar(QWidget *parent) : QWidget(parent)
{
    setObjectName("Chrome");
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(42);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(14, 0, 8, 0);
    h->setSpacing(6);

    m_logo = new LogoWidget(this);
    m_menu = new IconButton("more", this);
    m_menu->setToolTip(tr("Menu"));
    m_sidebar = new IconButton("sidebar", this);
    m_sidebar->setToolTip(tr("Toggle sidebar (Ctrl+B)"));
    m_separator = new QWidget(this);
    m_separator->setFixedSize(1, 18);
    m_separator->setStyleSheet(QString("background: %1;").arg(theme().pal().border.name()));
    connect(&theme(), &Theme::changed, m_separator,
            [this] { m_separator->setStyleSheet(QString("background: %1;").arg(theme().pal().border.name())); });
    m_location = new IconButton("folder", this);
    m_location->setLabelPixelSize(14);
    m_location->setToolTip(tr("Open library"));

    m_title = new QLabel(this);
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setProperty("role", "dim");
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_rec = new QLabel(tr("● REC"), this);
    m_rec->setStyleSheet("color: #e5484d; font-weight: 600; font-size: 12px;");
    m_rec->hide();

    m_min = new IconButton("minimize", this);
    m_pip = new IconButton("pip", this);
    m_pip->setToolTip(tr("Picture in picture"));
    m_max = new IconButton("maximize", this);
    m_full = new IconButton("fullscreen", this);
    m_close = new IconButton("close", this);
    m_min->setToolTip(tr("Minimize"));
    m_max->setToolTip(tr("Maximize"));
    m_full->setToolTip(tr("Fullscreen (F)"));
    m_close->setToolTip(tr("Close"));

    h->addWidget(m_logo);
    h->addSpacing(8);
    h->addWidget(m_menu);
    h->addWidget(m_sidebar);
    h->addSpacing(4);
    h->addWidget(m_separator);
    h->addSpacing(4);
    h->addWidget(m_location);
    h->addStretch();
    h->addWidget(m_rec);
    h->addSpacing(10);
    h->addWidget(m_min);
    h->addWidget(m_pip);
    h->addWidget(m_max);
    h->addWidget(m_full);
    h->addWidget(m_close);

    m_logo->installEventFilter(this);
    m_logo->setCursor(Qt::PointingHandCursor);
    connect(m_menu, &IconButton::clicked, this,
            [this] { emit menuRequested(m_menu->mapToGlobal(QPoint(0, m_menu->height() + 4))); });
    connect(m_sidebar, &IconButton::clicked, this, &TitleBar::sidebarToggled);
    connect(m_location, &IconButton::clicked, this, &TitleBar::locationClicked);
    connect(m_min, &IconButton::clicked, this, &TitleBar::minimizeRequested);
    connect(m_pip, &IconButton::clicked, this, &TitleBar::pipRequested);
    connect(m_max, &IconButton::clicked, this, &TitleBar::maximizeRequested);
    connect(m_full, &IconButton::clicked, this, &TitleBar::fullscreenRequested);
    connect(m_close, &IconButton::clicked, this, &TitleBar::closeRequested);
    setTitle(QStringLiteral("Konos Media Player"));
}

bool TitleBar::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_logo && event->type() == QEvent::MouseButtonRelease) {
        emit menuRequested(m_logo->mapToGlobal(QPoint(0, m_logo->height() + 8)));
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

void TitleBar::setMode(Mode mode)
{
    m_mode = mode;
    const bool pip = mode == PipMode;
    const bool player = mode == PlayerMode;
    const bool minimal = mode == MinimalMode;
    m_logo->setVisible(!pip);
    m_menu->setVisible(!minimal && !pip);
    m_sidebar->setVisible((player || minimal) && !m_overlay);
    m_separator->setVisible(player);
    m_location->setVisible(player);
    m_title->setVisible(!minimal);
    // Picture in picture: just the title, a way back and close.
    m_pip->setVisible(mode != LibraryMode);
    m_pip->setActive(pip);
    m_pip->setToolTip(pip ? tr("Back to the full window") : tr("Picture in picture"));
    for (QWidget *w : {static_cast<QWidget *>(m_min), static_cast<QWidget *>(m_max), static_cast<QWidget *>(m_full)})
        w->setVisible(!pip);
    m_max->setVisible(!pip && !m_overlay);
    resizeEvent(nullptr);
}

void TitleBar::setOverlay(bool overlay)
{
    // Floating over fullscreen video: no sidebar toggle or maximize, and the
    // fullscreen button leaves fullscreen.
    m_overlay = overlay;
    m_full->setIconName(overlay ? "fullscreen-exit" : "fullscreen");
    m_full->setToolTip(overlay ? tr("Exit fullscreen (F)") : tr("Fullscreen (F)"));
    setMode(m_mode);
}

void TitleBar::setTitle(const QString &title)
{
    m_title->setText(title);
    m_title->setToolTip(title);
    resizeEvent(nullptr);
}

void TitleBar::setLocation(Section section)
{
    m_location->setLabel(sectionName(section));
    m_location->setIconName(section == Section::Queue ? "queue" : "folder");
}

void TitleBar::setSidebarActive(bool active)
{
    m_sidebar->setActive(false);
    m_sidebar->setIconName(active ? "sidebar" : "sidebar");
    m_sidebar->setToolTip(active ? tr("Hide sidebar (Ctrl+B)") : tr("Show sidebar (Ctrl+B)"));
}

void TitleBar::setMaximized(bool maximized)
{
    m_max->setIconName(maximized ? "restore" : "maximize");
    m_max->setToolTip(maximized ? tr("Restore") : tr("Maximize"));
}

void TitleBar::setRecording(bool recording)
{
    m_rec->setVisible(recording);
}

void TitleBar::resizeEvent(QResizeEvent *)
{
    // Centre the title over the whole bar, but keep clear of the side controls.
    const int sideWidth = m_mode == PipMode ? 16 + 2 * 36 : qMin(300, width() / 4);
    const int w = qMax(0, width() - 2 * sideWidth);
    m_title->setGeometry(sideWidth, 0, w, height());
    m_title->setText(QFontMetrics(m_title->font()).elidedText(m_title->toolTip(), Qt::ElideMiddle, w));
}

void TitleBar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && window()->windowHandle())
        window()->windowHandle()->startSystemMove();
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        emit maximizeRequested();
}

// ---------------------------------------------------------------------------
// NavBar

NavBar::NavBar(bool expanded, QWidget *parent) : QWidget(parent), m_expanded(expanded)
{
    setObjectName(expanded ? "NavPanel" : "Chrome");
    setAttribute(Qt::WA_StyledBackground);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(expanded ? 10 : 6, expanded ? 16 : 10, expanded ? 10 : 6, 10);
    v->setSpacing(expanded ? 4 : 6);
    const QList<Section> sections = {Section::Home, Section::Videos, Section::Music, Section::Network,
                                     Section::Playlists, Section::Recent};
    for (Section s : sections) {
        auto *b = new IconButton(sectionIcon(s), this);
        if (expanded) {
            b->setLabel(sectionName(s));
            b->setFixedHeight(42);
        } else {
            b->setFixedSize(40, 40);
            b->setToolTip(sectionName(s));
        }
        v->addWidget(b);
        m_items.append({s, b});
        connect(b, &IconButton::clicked, this, [this, s] { emit sectionClicked(s); });
    }
    // Pinned folders, playlists, albums, lists… (right-click → Pin to Sidebar).
    m_pinRule = hline(this);
    v->addSpacing(4);
    v->addWidget(m_pinRule);
    v->addSpacing(4);
    m_pins = new QVBoxLayout;
    m_pins->setContentsMargins(0, 0, 0, 0);
    m_pins->setSpacing(expanded ? 2 : 6);
    v->addLayout(m_pins);
    rebuildPins();
    connect(&library(), &Library::pinsChanged, this, &NavBar::rebuildPins);
    auto *add = new IconButton("plus", this);
    if (expanded) {
        add->setLabel(tr("Add item"));
        add->setFixedHeight(42);
    } else {
        add->setFixedSize(40, 40);
        add->setToolTip(tr("Add item"));
    }
    connect(add, &IconButton::clicked, this, [this, add] { emit addClicked(add->mapToGlobal(QPoint(add->width() + 6, 0))); });
    v->addWidget(add);
    v->addStretch();
    if (expanded)
        setFixedWidth(212);
    else
        setFixedWidth(52);
}

void NavBar::rebuildPins()
{
    while (QLayoutItem *item = m_pins->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QList<Library::Pin> pins = library().pins();
    m_pinRule->setVisible(!pins.isEmpty());
    auto iconFor = [](const Library::Pin &p) -> QString {
        if (p.kind == "folder")
            return "folder";
        if (p.kind == "playlist")
            return "playlist";
        if (p.kind == "channels")
            return "stream";
        if (p.kind == "podcast")
            return "podcast";
        if (p.kind == "network")
            return p.target == "radio" ? "radio" : "podcast";
        if (p.kind == "music") {
            const QStringList parts = p.target.split('\n');
            if (!parts.value(2).isEmpty() || parts.value(0) == "albums")
                return "album";
            return parts.value(0) == "artists" ? "artist" : parts.value(0) == "genres" ? "genre" : "song";
        }
        return Library::isAudioFile(p.target) ? "note" : Library::isUrl(p.target) ? "stream" : "film";
    };
    for (int i = 0; i < pins.size(); ++i) {
        const Library::Pin pin = pins.at(i);
        auto *b = new IconButton(iconFor(pin), this);
        if (m_expanded) {
            b->setLabel(QFontMetrics(font()).elidedText(pin.name, Qt::ElideRight, 150));
            b->setFixedHeight(36);
        } else {
            b->setFixedSize(40, 40);
        }
        b->setToolTip(pin.name);
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &IconButton::clicked, this, [this, pin] { emit pinClicked(pin); });
        connect(b, &QWidget::customContextMenuRequested, this, [this, b, pin, i, count = pins.size()](const QPoint &pos) {
            QMenu menu(this);
            const QColor a = theme().accent();
            menu.addAction(theme().icon("pin", a), tr("Unpin from Sidebar"), this, [pin] { library().unpin(pin.kind, pin.target); });
            if (i > 0)
                menu.addAction(theme().icon("chevron-up", a), tr("Move Up"), this, [i] { library().movePin(i, i - 1); });
            if (i < count - 1)
                menu.addAction(theme().icon("chevron-down", a), tr("Move Down"), this, [i] { library().movePin(i, i + 1); });
            menu.exec(b->mapToGlobal(pos));
        });
        m_pins->addWidget(b);
    }
}

void NavBar::setCurrent(Section section, bool highlight)
{
    for (auto &[s, b] : m_items) {
        const bool on = highlight && s == section;
        if (m_expanded)
            b->setSelectedLook(on);
        else
            b->setActive(on);
    }
}

// ---------------------------------------------------------------------------
// ControlBar

// ---------------------------------------------------------------------------
// Control items

const QList<ControlItem> &controlItems()
{
    static const QList<ControlItem> items = {
        {"prev", QObject::tr("Previous"), "prev-fill"},
        {"play", QObject::tr("Play / Pause"), "play-fill"},
        {"next", QObject::tr("Next"), "next-fill"},
        {"volume", QObject::tr("Volume"), "volume"},
        {"audio", QObject::tr("Audio"), "audio"},
        {"subtitles", QObject::tr("Subtitles"), "subtitles"},
        {"settings", QObject::tr("Playback Settings"), "tune"},
        {"queue", QObject::tr("Right Sidebar"), "queue"},
        {"spacer", QObject::tr("Spacer"), "spacer"},
        {"divider", QObject::tr("Divider"), "divider"},
        {"speed", QObject::tr("Playback Speed"), "speed"},
        {"fwd10", QObject::tr("Forward 10s"), "fwd-10"},
        {"back10", QObject::tr("Backward 10s"), "back-10"},
        {"fwd15", QObject::tr("Forward 15s"), "fwd-15"},
        {"back15", QObject::tr("Backward 15s"), "back-15"},
        {"stop", QObject::tr("Stop"), "stop"},
        {"repeat", QObject::tr("Repeat"), "repeat"},
        {"shuffle", QObject::tr("Shuffle"), "shuffle"},
        {"record", QObject::tr("Record"), "record"},
        {"snapshot", QObject::tr("Snapshot"), "camera"},
        {"pip", QObject::tr("Picture in Picture"), "pip"},
        {"fullscreen", QObject::tr("Fullscreen"), "fullscreen"},
        {"aspect", QObject::tr("Aspect Ratio"), "aspect"},
        {"home", QObject::tr("Go Home"), "home"},
        {"thumbnail", QObject::tr("Thumbnail"), "film"},
        {"filename", QObject::tr("File Name"), "file", true},
        {"artist", QObject::tr("Artist Name"), "artist", true},
        {"foldername", QObject::tr("Folder Name"), "folder", true},
        {"info", QObject::tr("Properties"), "info"},
        {"replay", QObject::tr("Replay"), "refresh"},
        {"chapters", QObject::tr("Chapters"), "chapters"},
        {"favorite", QObject::tr("Favorite"), "heart"},
        {"bookmark", QObject::tr("Bookmark"), "bookmark"},
        {"bookmarks", QObject::tr("Bookmarks"), "bookmarks"},
        {"playlists", QObject::tr("Playlists"), "playlist"},
        {"appsettings", QObject::tr("Konos Settings"), "settings"},
        {"quit", QObject::tr("Quit Konos"), "quit"},
    };
    return items;
}

const ControlItem *controlItem(const QString &id)
{
    for (const ControlItem &i : controlItems())
        if (i.id == id)
            return &i;
    return nullptr;
}

QStringList defaultControls(bool left)
{
    return left ? QStringList{"prev", "play", "next"}
                : QStringList{"volume", "spacer", "audio", "subtitles", "settings", "queue"};
}

QStringList savedControls(bool left)
{
    const QString key = left ? "ui/controlsLeft" : "ui/controlsRight";
    const QString saved = prefs().string(key);
    if (saved == "default")
        return defaultControls(left);
    QStringList out;
    for (const QString &id : saved.split(',', Qt::SkipEmptyParts))
        if (controlItem(id))
            out << id;
    return out;
}

ControlBar::ControlBar(Player *player, QWidget *parent) : QWidget(parent), m_player(player)
{
    setObjectName("Chrome");
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(58);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(14, 0, 14, 0);
    h->setSpacing(6);

    m_prev = new IconButton("prev-fill", this);
    m_prev->setIconPixelSize(16);
    m_prev->setToolTip(tr("Previous (P)"));
    m_play = new PlayButton(this);
    m_play->setToolTip(tr("Play / Pause (Space)"));
    m_next = new IconButton("next-fill", this);
    m_next->setIconPixelSize(16);
    m_next->setToolTip(tr("Next (N)"));
    m_time = new QLabel("00:00", this);
    m_time->setProperty("role", "time");
    m_time->setMinimumWidth(66);
    m_time->setAlignment(Qt::AlignCenter);
    m_seek = new SliderBar(Qt::Horizontal, this);
    m_seek->setThickness(26);
    m_seek->setHandleOvershoot(0);
    m_seek->setRadius(4);
    m_seek->setRange(0, 1);
    m_seek->setHoverTracking(true);
    m_previewer = new SeekPreviewer(this);
    connect(m_seek, &SliderBar::hovered, this, &ControlBar::showSeekPreview);
    connect(m_seek, &SliderBar::hoverEnded, this, &ControlBar::hideSeekPreview);
    connect(m_previewer, &SeekPreviewer::frameReady, this, [this](qint64, const QImage &image) {
        // Show whatever arrives while hovering; the latest request follows.
        if (m_preview && m_preview->isVisible())
            m_preview->setImage(image);
    });
    m_duration = new QLabel("00:00", this);
    m_duration->setProperty("role", "time");
    m_duration->setMinimumWidth(66);
    m_duration->setAlignment(Qt::AlignCenter);
    m_duration->setCursor(Qt::PointingHandCursor);
    m_duration->setToolTip(tr("Click to toggle remaining time"));
    m_duration->installEventFilter(this);
    m_volIcon = new IconButton("volume", this);
    m_volIcon->setToolTip(tr("Mute (M)"));
    m_volume = new VolumeBar(this);
    m_audio = new IconButton("audio", this);
    m_audio->setToolTip(tr("Audio"));
    m_subs = new IconButton("subtitles", this);
    m_subs->setToolTip(tr("Subtitles"));
    m_settings = new IconButton("settings", this);
    m_settings->setToolTip(tr("Playback settings"));
    m_queue = new IconButton("queue", this);
    m_queue->setToolTip(tr("Play queue"));

    m_layout = h;
    m_volumeBox = new QWidget(this);
    auto *vb = new QHBoxLayout(m_volumeBox);
    vb->setContentsMargins(0, 0, 0, 0);
    vb->setSpacing(6);
    m_volIcon->setParent(m_volumeBox);
    m_volume->setParent(m_volumeBox);
    vb->addWidget(m_volIcon);
    vb->addWidget(m_volume);
    m_items = {{"prev", m_prev}, {"play", m_play}, {"next", m_next}, {"volume", m_volumeBox},
               {"audio", m_audio}, {"subtitles", m_subs}, {"settings", m_settings}, {"queue", m_queue}};
    rebuild();
    connect(&prefs(), &Prefs::changed, this, [this](const QString &key) {
        if (key == "ui/controlsLeft" || key == "ui/controlsRight")
            rebuild();
        if (key == "playback/repeat" || key == "playback/shuffle") {
            if (auto *b = qobject_cast<IconButton *>(m_items.value("repeat"))) {
                const int r = prefs().integer("playback/repeat");
                b->setIconName(r == 1 ? "repeat-one" : "repeat");
                b->setActive(r != 0);
            }
            if (auto *b = qobject_cast<IconButton *>(m_items.value("shuffle")))
                b->setActive(prefs().flag("playback/shuffle"));
        }
    });
    connect(player, &Player::recordingChanged, this, [this](bool on) {
        if (auto *b = qobject_cast<IconButton *>(m_items.value("record")))
            b->setActive(on);
    });
    connect(player, &Player::metaChanged, this, &ControlBar::updateLabels);
    connect(player, &Player::mediaChanged, this, &ControlBar::updateLabels);
    connect(&Thumbnailer::instance(), &Thumbnailer::ready, this, [this](const QString &path) {
        if (path == m_player->location())
            updateLabels();
    });

    m_volume->setValue(player->volume());
    m_volume->setMuted(player->isMuted());
    m_volIcon->setIconName(player->isMuted() ? "volume-mute" : "volume");

    // Seek while dragging, throttled so libVLC isn't flooded.
    m_seekThrottle.setInterval(90);
    m_seekThrottle.setSingleShot(true);
    connect(&m_seekThrottle, &QTimer::timeout, this, [this] {
        if (m_pendingSeek >= 0)
            m_player->seek(m_pendingSeek);
        m_pendingSeek = -1;
    });
    connect(m_seek, &SliderBar::valueChanged, this, [this](double v) {
        m_pendingSeek = qint64(v);
        updateTime(qint64(v));
        if (!m_seekThrottle.isActive())
            m_seekThrottle.start();
    });
    connect(m_seek, &SliderBar::sliderReleased, this, [this] {
        m_seekThrottle.stop();
        if (m_pendingSeek >= 0)
            m_player->seek(m_pendingSeek);
        m_pendingSeek = -1;
    });

    connect(m_prev, &IconButton::clicked, this, &ControlBar::previousClicked);
    connect(m_next, &IconButton::clicked, this, &ControlBar::nextClicked);
    connect(m_play, &PlayButton::clicked, player, &Player::togglePause);
    connect(m_volIcon, &IconButton::clicked, this, [this] { m_player->setMuted(!m_player->isMuted()); });
    connect(m_volume, &VolumeBar::valueChanged, player, &Player::setVolume);
    connect(m_audio, &IconButton::clicked, this, &ControlBar::audioClicked);
    connect(m_subs, &IconButton::clicked, this, &ControlBar::subtitlesClicked);
    connect(m_settings, &IconButton::clicked, this, &ControlBar::settingsClicked);
    connect(m_queue, &IconButton::clicked, this, &ControlBar::queueClicked);

    connect(player, &Player::timeChanged, this, [this](qint64 t) {
        if (!m_seek->isDragging() && m_pendingSeek < 0) {
            m_seek->setValue(double(t));
            updateTime(t);
        }
    });
    connect(player, &Player::lengthChanged, this, [this](qint64 l) {
        m_seek->setRange(0, qMax<qint64>(1, l));
        updateTime(m_player->time());
    });
    connect(player, &Player::stateChanged, this, &ControlBar::updateState);
    // Music always shows its waveform on the seek bar; videos when asked to.
    auto wantWave = [this] {
        const QString loc = m_player->location();
        return Library::isAudioFile(loc) || prefs().flag("playback/waveform");
    };
    connect(&Waveform::instance(), &Waveform::ready, this, [this, wantWave](const QString &path) {
        if (path == m_player->location() && wantWave())
            m_seek->setWaveform(Waveform::instance().peaks(path));
    });
    connect(&prefs(), &Prefs::changed, this, [this, wantWave](const QString &key) {
        if (key == "playback/waveform")
            m_seek->setWaveform(wantWave() ? Waveform::instance().peaks(m_player->location()) : QVector<float>());
    });
    connect(player, &Player::mediaChanged, this, [this, wantWave] {
        m_seek->setRange(0, 1);
        m_seek->setValue(0);
        // The seek bar shows the waveform (once it's decoded).
        m_seek->setWaveform(wantWave() ? Waveform::instance().peaks(m_player->location()) : QVector<float>());
        updateTime(0);
        updateState();
    });
    connect(player, &Player::volumeChanged, this, [this](int v) {
        m_volume->setValue(v);
        m_volIcon->setIconName(m_player->isMuted() ? "volume-mute" : (v < 50 ? "volume-low" : "volume"));
    });
    connect(player, &Player::mutedChanged, this, [this](bool muted) {
        m_volume->setMuted(muted);
        m_volIcon->setIconName(muted ? "volume-mute" : (m_player->volume() < 50 ? "volume-low" : "volume"));
    });
    connect(&prefs(), &Prefs::changed, this, [this](const QString &key) {
        if (key == "ui/remainingTime")
            updateTime(m_player->time());
    });
    updateState();
}

// A popup's anchor: the button when it's in the bar, else the bar's right end.
QWidget *ControlBar::anchor(QWidget *preferred) const
{
    if (preferred->isVisibleTo(this))
        return preferred;
    for (const char *id : {"settings", "subtitles", "audio", "queue"})
        if (QWidget *w = m_items.value(id); w && w->isVisibleTo(this))
            return w;
    return const_cast<ControlBar *>(this);
}

QWidget *ControlBar::audioButton() const { return anchor(m_audio); }
QWidget *ControlBar::subtitleButton() const { return anchor(m_subs); }
QWidget *ControlBar::settingsButton() const { return anchor(m_settings); }

QWidget *ControlBar::itemWidget(const QString &id) const
{
    QWidget *w = m_items.value(id);
    return w && m_shown.contains(id) ? w : nullptr;
}

void ControlBar::setFavorite(bool on)
{
    if (auto *b = qobject_cast<IconButton *>(m_items.value("favorite"))) {
        b->setIconName(on ? "heart-fill" : "heart");
        b->setActive(on);
        b->setToolTip(on ? tr("Unpin from the sidebar") : tr("Pin to the sidebar"));
    }
}

QWidget *ControlBar::item(const QString &id)
{
    if (QWidget *w = m_items.value(id))
        return w;
    const ControlItem *def = controlItem(id);
    if (!def)
        return nullptr;
    QWidget *w = nullptr;
    if (def->text) {
        auto *l = new QLabel(this);
        l->setProperty("role", "dim");
        l->setMaximumWidth(240);
        if (id == "foldername") {
            l->setCursor(Qt::PointingHandCursor);
            l->installEventFilter(this);
        }
        w = l;
    } else if (id == "thumbnail") {
        auto *l = new QLabel(this);
        l->setFixedSize(54, 32);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(theme().pal().input.name()));
        w = l;
    } else {
        auto *b = new IconButton(def->icon, this);
        b->setToolTip(def->label);
        connect(b, &IconButton::clicked, this, [this, id] { emit actionRequested(id); });
        if (id == "repeat") {
            const int r = prefs().integer("playback/repeat");
            b->setIconName(r == 1 ? "repeat-one" : "repeat");
            b->setActive(r != 0);
        } else if (id == "shuffle") {
            b->setActive(prefs().flag("playback/shuffle"));
        } else if (id == "record") {
            b->setActive(m_player->isRecording());
        }
        w = b;
    }
    w->setObjectName("control-" + id);
    m_items.insert(id, w);
    return w;
}

void ControlBar::rebuild()
{
    while (QLayoutItem *it = m_layout->takeAt(0)) {
        if (it->widget())
            it->widget()->hide();
        delete it;
    }
    qDeleteAll(m_transient);
    m_transient.clear();
    m_shown.clear();
    auto place = [this](const QString &id) {
        if (id == "spacer") {
            m_layout->addSpacing(16);
            return;
        }
        if (id == "divider") {
            auto *d = new QWidget(this);
            d->setFixedSize(1, 22);
            d->setStyleSheet(QString("background: %1;").arg(theme().pal().border.name()));
            m_layout->addWidget(d);
            m_transient << d;
            return;
        }
        if (QWidget *w = item(id); w && !m_shown.contains(id)) {
            m_layout->addWidget(w);
            w->show();
            m_shown << id;
        }
    };
    for (const QString &id : savedControls(true))
        place(id);
    m_layout->addSpacing(14);
    for (QWidget *w : {static_cast<QWidget *>(m_time), static_cast<QWidget *>(m_seek), static_cast<QWidget *>(m_duration)}) {
        m_layout->addWidget(w, w == m_seek ? 1 : 0);
        w->show();
    }
    m_layout->addSpacing(16);
    for (const QString &id : savedControls(false))
        place(id);
    updateLabels();
    fitToWidth();
}

void ControlBar::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    fitToWidth();
}

void ControlBar::fitToWidth()
{
    // Everything chosen first, then off again from the least important end
    // until the seek bar keeps a usable width. Play always stays.
    QList<QWidget *> items;
    for (const QString &id : m_shown)
        items << m_items.value(id);
    items << m_transient;
    for (QWidget *w : items)
        w->setVisible(true);
    auto rank = [this](QWidget *w) {
        const QString id = m_items.key(w);
        if (id.isEmpty())
            return 2; // a divider
        if (const ControlItem *it = controlItem(id); it && it->text)
            return 0;
        static const QStringList core = {"settings", "subtitles", "audio", "queue", "next", "prev", "volume"};
        if (id == "thumbnail")
            return 1;
        if (!core.contains(id))
            return 3;
        return 4 + int(core.indexOf(id));
    };
    QList<QWidget *> order;
    for (int i = items.size() - 1; i >= 0; --i) // within a rank, the rightmost goes first
        if (m_items.key(items[i]) != "play")
            order << items[i];
    std::stable_sort(order.begin(), order.end(), [&](QWidget *a, QWidget *b) { return rank(a) < rank(b); });
    const int spacing = m_layout->spacing();
    auto needed = [&] {
        int w = m_layout->contentsMargins().left() + m_layout->contentsMargins().right() + 30 + 140; // fixed gaps, seek
        w += m_time->sizeHint().width() + m_duration->sizeHint().width() + 2 * spacing;
        for (QWidget *it : items)
            if (!it->isHidden())
                w += it->sizeHint().width() + spacing;
        w += 16 * int(savedControls(true).count("spacer") + savedControls(false).count("spacer"));
        return w;
    };
    for (QWidget *w : order) {
        if (needed() <= width())
            break;
        w->setVisible(false);
    }
}

void ControlBar::updateLabels()
{
    const QString loc = m_player->location();
    const QFileInfo fi(Library::isUrl(loc) ? QUrl(loc).path() : loc);
    auto setText = [this](const QString &id, const QString &text) {
        if (auto *l = qobject_cast<QLabel *>(m_items.value(id))) {
            l->setText(l->fontMetrics().elidedText(text, Qt::ElideRight, l->maximumWidth()));
            l->setToolTip(text);
        }
    };
    setText("filename", loc.isEmpty() ? QString() : (m_player->title().isEmpty() ? fi.fileName() : m_player->title()));
    setText("artist", m_player->artist());
    setText("foldername", loc.isEmpty() || Library::isUrl(loc) ? QString() : fi.dir().dirName());
    if (auto *l = qobject_cast<QLabel *>(m_items.value("thumbnail"))) {
        QPixmap pm;
        if (!m_player->artworkPath().isEmpty())
            pm.load(m_player->artworkPath());
        if (pm.isNull() && !loc.isEmpty() && !Library::isUrl(loc))
            pm = Thumbnailer::instance().get(loc);
        if (!pm.isNull()) { // fill the box, cropped to the middle
            pm = pm.scaled(l->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            pm = pm.copy((pm.width() - l->width()) / 2, (pm.height() - l->height()) / 2, l->width(), l->height());
        }
        l->setPixmap(pm.isNull() ? theme().pixmap("film", theme().pal().textDim, 18) : pm);
    }
}

void ControlBar::setQueueActive(bool active)
{
    m_queue->setActive(active);
}

void ControlBar::setOverlay(bool overlay)
{
    m_overlay = overlay;
    update();
}

void ControlBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QColor bg = theme().pal().chrome;
    if (m_overlay)
        bg.setAlpha(235);
    p.fillRect(rect(), bg);
}

void ControlBar::showSeekPreview(double ms, const QPoint &pos)
{
    if (!m_player->hasMedia() || m_player->length() <= 0) {
        hideSeekPreview();
        return;
    }
    QWidget *win = window();
    if (!m_preview || m_preview->parentWidget() != win)
        m_preview = new SeekPreviewPopup(win);
    const bool video = m_player->hasVideo() && SeekPreviewer::supports(m_player->location());
    const QSize aspect = video ? m_player->videoSize() : QSize();
    const QPoint anchor = m_seek->mapTo(win, QPoint(pos.x(), 0));
    if (!m_preview->isVisible())
        m_preview->clearImage();
    m_preview->showAt(anchor, formatTime(qint64(ms)), aspect);
    if (!aspect.isEmpty()) {
        const int width = qRound(SeekPreviewPopup::thumbSize(aspect).width() * devicePixelRatioF());
        // The very end has no picture to show; stay a moment before it.
        const qint64 at = qMin(qint64(ms), m_player->length() - 500);
        m_previewer->request(m_player->location(), at, aspect, width);
    }
}

void ControlBar::hideEvent(QHideEvent *e)
{
    // Auto-hidden controls in fullscreen never see the pointer leave.
    hideSeekPreview();
    QWidget::hideEvent(e);
}

void ControlBar::hideSeekPreview()
{
    if (m_preview)
        m_preview->hide();
    m_previewer->idle();
}

void ControlBar::updateTime(qint64 ms)
{
    const qint64 len = m_player->length();
    const bool hours = len >= 3600 * 1000;
    m_time->setText(formatTime(ms, hours));
    if (len <= 0) {
        m_duration->setText(m_player->hasMedia() && m_player->isPlaying() ? tr("Live") : formatTime(0));
        return;
    }
    if (prefs().flag("ui/remainingTime"))
        m_duration->setText("-" + formatTime(len - ms, hours));
    else
        m_duration->setText(formatTime(len, hours));
}

void ControlBar::updateState()
{
    m_play->setPlaying(m_player->isPlaying());
    const bool media = m_player->hasMedia();
    m_seek->setEnabled(media);
}

bool ControlBar::eventFilter(QObject *obj, QEvent *event)
{
    // Clicking the duration toggles between total and remaining time.
    if (obj == m_duration && event->type() == QEvent::MouseButtonRelease) {
        prefs().set("ui/remainingTime", !prefs().flag("ui/remainingTime"));
        return true;
    }
    if (obj == m_items.value("foldername") && event->type() == QEvent::MouseButtonRelease) {
        emit actionRequested("foldername");
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

// ---------------------------------------------------------------------------
// Toast

Toast::Toast(QWidget *parent) : QWidget(parent)
{
    setObjectName("Toast");
    setAttribute(Qt::WA_StyledBackground);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(16, 10, 10, 10);
    h->setSpacing(12);
    m_text = new QLabel(this);
    m_text->setStyleSheet("font-size: 14px;");
    m_key = new QLabel(this);
    m_key->setProperty("role", "dim");
    m_key->setStyleSheet("font-size: 13px;");
    m_key->hide();
    m_action = new QPushButton(this);
    m_action->setCursor(Qt::PointingHandCursor);
    auto *texts = new QVBoxLayout;
    texts->setSpacing(2);
    texts->addWidget(m_text);
    texts->addWidget(m_key);
    h->addLayout(texts);
    h->addWidget(m_action);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &QWidget::hide);
    connect(m_action, &QPushButton::clicked, this, [this] {
        hide();
        if (m_onAction)
            m_onAction();
    });
    parent->installEventFilter(this);
    hide();
}

void Toast::show(const QString &text, const QString &action, std::function<void()> onAction, int ms)
{
    showAt(Top, text, action, std::move(onAction), ms);
}

void Toast::showAt(Place place, const QString &text, const QString &action, std::function<void()> onAction, int ms)
{
    m_place = place;
    m_text->setText(text);
    // "Pressed C" under messages that come from a keyboard shortcut.
    m_key->setVisible(!m_keyHint.isEmpty());
    m_key->setText(m_keyHint.isEmpty() ? QString() : tr("Pressed %1").arg(m_keyHint));
    m_action->setVisible(!action.isEmpty());
    m_action->setText(action);
    m_action->setStyleSheet(QString("QPushButton { background: transparent; color: %1; font-weight: 600; padding: 4px 8px; }"
                                    "QPushButton:hover { background: rgba(255,255,255,20); }")
                                .arg(theme().accent().name()));
    m_onAction = std::move(onAction);
    adjustSize();
    reposition();
    raise();
    QWidget::show();
    m_timer.start(ms);
}

void Toast::reposition()
{
    if (!parentWidget())
        return;
    const QSize s = sizeHint();
    resize(s);
    const int y = m_place == Top ? 24 : parentWidget()->height() - s.height() - m_bottomMargin;
    move((parentWidget()->width() - s.width()) / 2, qMax(0, y));
}

bool Toast::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == parentWidget() && event->type() == QEvent::Resize)
        reposition();
    return QWidget::eventFilter(obj, event);
}
