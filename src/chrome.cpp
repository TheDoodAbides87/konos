#include "chrome.h"
#include "player.h"
#include "prefs.h"
#include "seekpreview.h"
#include "theme.h"
#include "waveform.h"
#include "library.h"
#include "widgets.h"

#include <QHBoxLayout>
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
    setTitle(QStringLiteral("VMP"));
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

    h->addWidget(m_prev);
    h->addWidget(m_play);
    h->addWidget(m_next);
    h->addSpacing(14);
    h->addWidget(m_time);
    h->addWidget(m_seek, 1);
    h->addWidget(m_duration);
    h->addSpacing(16);
    h->addWidget(m_volIcon);
    h->addWidget(m_volume);
    h->addSpacing(16);
    h->addWidget(m_audio);
    h->addWidget(m_subs);
    h->addWidget(m_settings);
    h->addSpacing(8);
    h->addWidget(m_queue);

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

QWidget *ControlBar::audioButton() const { return m_audio; }
QWidget *ControlBar::subtitleButton() const { return m_subs; }
QWidget *ControlBar::settingsButton() const { return m_settings; }

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
