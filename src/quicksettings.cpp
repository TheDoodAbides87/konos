#include "quicksettings.h"
#include "showpresets.h"
#include "effects.h"
#include "fxpanels.h"
#include "player.h"
#include "prefs.h"
#include "theme.h"
#include "videowidget.h"
#include "widgets.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <vlc/vlc.h>

namespace {

QScrollArea *scrollPage(QWidget *content)
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

QLineEdit *valueEdit()
{
    auto *e = new QLineEdit;
    e->setProperty("role", "value");
    e->setFixedSize(150, 38);
    return e;
}

// "-10 s        0 s        +10 s" scale under a slider.
QWidget *scaleLabels(const QStringList &labels)
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < labels.size(); ++i) {
        auto *l = new QLabel(labels[i]);
        l->setProperty("role", "muted");
        h->addWidget(l);
        if (i < labels.size() - 1)
            h->addStretch();
    }
    return w;
}

QWidget *section(QVBoxLayout *&body)
{
    auto *w = new QWidget;
    body = new QVBoxLayout(w);
    body->setContentsMargins(26, 22, 26, 22);
    body->setSpacing(10);
    return w;
}

// "External Tracks                [+]"
QWidget *headingWith(const QString &text, QWidget *button)
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(sectionHeading(text));
    h->addStretch();
    h->addWidget(button);
    return w;
}

// A preview of an equalizer preset: its gain curve across the bands.
QIcon eqCurveIcon(const QVector<float> &bands, bool filled)
{
    QIcon icon;
    for (qreal dpr : {1.0, 2.0}) {
        QPixmap pm(QSize(40, 20) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor accent = theme().accent();
        const QRectF r(1, 2, 38, 16);
        p.setPen(QPen(QColor(128, 128, 128, 90), 1, Qt::DotLine));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        QPainterPath curve;
        for (int i = 0; i < bands.size(); ++i) {
            const QPointF pt(r.left() + i * r.width() / (bands.size() - 1),
                             r.center().y() - qBound(-20.0f, bands[i], 20.0f) / 20.0f * (r.height() / 2));
            if (i == 0) {
                curve.moveTo(pt);
            } else { // smooth: horizontal tangents between points
                const QPointF prev = curve.currentPosition();
                const qreal mid = (prev.x() + pt.x()) / 2;
                curve.cubicTo(QPointF(mid, prev.y()), QPointF(mid, pt.y()), pt);
            }
        }
        if (filled) {
            QPainterPath area = curve;
            area.lineTo(r.right(), r.bottom());
            area.lineTo(r.left(), r.bottom());
            area.closeSubpath();
            QColor fill = accent;
            fill.setAlpha(50);
            p.fillPath(area, fill);
        }
        p.setPen(QPen(filled ? accent : QColor(140, 140, 140), 1.6));
        p.drawPath(curve);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QWidget *presetPicker()
{
    auto *p = new PresetPicker;
    p->setFixedWidth(200);
    return p;
}

// A switch bound to a yes/no preference, kept in step with it.
ToggleSwitch *prefSwitch(const QString &key)
{
    auto *t = new ToggleSwitch;
    t->setChecked(prefs().flag(key));
    QObject::connect(t, &ToggleSwitch::toggled, t, [key](bool on) { prefs().set(key, on); });
    QObject::connect(&prefs(), &Prefs::changed, t, [t, key](const QString &k) {
        if (k == key) {
            const QSignalBlocker b(t);
            t->setChecked(prefs().flag(key));
            t->update();
        }
    });
    return t;
}

void addSectionTo(QVBoxLayout *page, QWidget *section, bool separator = true)
{
    if (separator && page->count() > 0)
        page->addWidget(hline());
    page->addWidget(section);
}

// Effects presets for the footer dropdown.
struct Preset {
    QString name;
    std::function<void(EffectsData &)> apply;
};

QList<Preset> presets()
{
    return {
        {QObject::tr("Default Preset"), [](EffectsData &d) { d = EffectsData(); }},
        {QObject::tr("Cinema"),
         [](EffectsData &d) {
             d = EffectsData();
             d.contrast = 1.08f;
             d.saturation = 1.12f;
             d.gamma = 0.95f;
             d.spatialEnabled = true;
             d.spatialMode = 1;
             d.reverbSize = 6;
             d.reverbWet = 2;
             d.reverbDry = 8;
         }},
        {QObject::tr("Night Mode"),
         [](EffectsData &d) {
             d = EffectsData();
             d.compEnabled = true;
             d.compThreshold = -20;
             d.compRatio = 6;
             d.compMakeup = 10;
             d.brightness = 0.92f;
         }},
        {QObject::tr("Vivid"),
         [](EffectsData &d) {
             d = EffectsData();
             d.saturation = 1.4f;
             d.contrast = 1.1f;
             d.sharpen = 0.25f;
         }},
        {QObject::tr("Headphones"),
         [](EffectsData &d) {
             d = EffectsData();
             d.spatialEnabled = true;
             d.spatialMode = 0;
         }},
    };
}

} // namespace

QuickSettings::QuickSettings(Player *player, VideoWidget *video, QWidget *parent)
    : QWidget(parent), m_player(player), m_video(video)
{
    // An in-window panel rather than a popup window: Wayland only allows popups
    // in response to input and constrains where they can go.
    hide();
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 14, 14, 14);
    m_panel = new QWidget(this);
    m_panel->setObjectName("QuickSettings");
    m_panel->setAttribute(Qt::WA_StyledBackground);
    outer->addWidget(m_panel);

    auto *v = new QVBoxLayout(m_panel);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *tabRow = new QWidget(m_panel);
    tabRow->setObjectName("QuickTabs");
    tabRow->setFixedHeight(54);
    auto *tabs = new QHBoxLayout(tabRow);
    tabs->setContentsMargins(10, 0, 10, 0);
    tabs->setSpacing(0);
    // Settings has Playback and Video tabs; Audio and Subtitles are menus of
    // their own (from their control bar buttons) with just a title.
    const QList<QPair<QString, QString>> defs = {{"playback", tr("Playback")}, {"video", tr("Video")}};
    for (int i = 0; i < defs.size(); ++i) {
        auto *t = new TabButton(defs[i].first, defs[i].second, tabRow);
        tabs->addWidget(t, 1);
        m_tabs << t;
        connect(t, &QAbstractButton::clicked, this, [this, i] { setTab(i); });
    }
    m_titleIcon = new QLabel(tabRow);
    m_titleIcon->setFixedSize(20, 20);
    m_titleText = new QLabel(tabRow);
    m_titleText->setStyleSheet("font-size: 15px; font-weight: 600;");
    tabs->addSpacing(10);
    tabs->addWidget(m_titleIcon);
    tabs->addSpacing(10);
    tabs->addWidget(m_titleText, 1);
    v->addWidget(tabRow);

    m_stack = new QStackedWidget(m_panel);
    m_stack->addWidget(scrollPage(buildPlayback()));
    m_stack->addWidget(scrollPage(buildVideo()));
    m_stack->addWidget(scrollPage(buildAudio()));
    m_stack->addWidget(scrollPage(buildSubtitles()));
    v->addWidget(m_stack, 1);
    v->addWidget(buildFooter());

    auto *shadow = new QGraphicsDropShadowEffect(m_panel);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, 160));
    m_panel->setGraphicsEffect(shadow);

    auto restyle = [tabRow, this] {
        tabRow->setStyleSheet(QString("QWidget#QuickTabs { background: %1; border-top-left-radius: 12px; "
                                      "border-top-right-radius: 12px; }")
                                  .arg(theme().isDark() ? theme().tone(QColor(0x14, 0x14, 0x14)).name() : theme().pal().chrome.darker(104).name()));
        update();
    };
    restyle();
    connect(&theme(), &Theme::changed, this, restyle);
    connect(&theme(), &Theme::changed, this, [this] { setTab(m_stack->currentIndex()); }); // title icon colour

    connect(player, &Player::tracksChanged, this, &QuickSettings::refreshTracks);
    connect(player, &Player::mediaChanged, this, &QuickSettings::refreshValues);
    connect(player, &Player::rateChanged, this, &QuickSettings::refreshValues);
    connect(&fx(), &Effects::changed, this, [this] {
        if (!m_refreshing && m_eqPreset) {
            const QSignalBlocker b(m_eqPreset);
            m_eqPreset->setCurrentIndex(fx().data().eqPreset + 1);
        }
    });
    setTab(PlaybackTab);
}

void QuickSettings::paintEvent(QPaintEvent *)
{
    // Rounded panel background is drawn by the stylesheet on m_panel.
}

void QuickSettings::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refreshTracks();
    refreshDevices();
    refreshValues();
}

void QuickSettings::setTab(int tab)
{
    const bool menu = tab == AudioTab || tab == SubtitlesTab; // a menu of its own, not a settings tab
    for (int i = 0; i < m_tabs.size(); ++i) {
        m_tabs[i]->setChecked(i == tab);
        m_tabs[i]->setVisible(!menu);
    }
    m_titleIcon->setVisible(menu);
    m_titleText->setVisible(menu);
    if (menu) {
        m_titleIcon->setPixmap(theme().pixmap(tab == AudioTab ? "audio" : "subtitles", theme().accent(), 20, devicePixelRatioF()));
        m_titleText->setText(tab == AudioTab ? tr("Audio") : tr("Subtitles"));
    }
    m_preset->setVisible(!menu); // effects presets touch audio and video alike: Settings only
    m_advanced->setText(tab == AudioTab ? tr("Open Audio Settings")
                        : tab == SubtitlesTab ? tr("Open Subtitle Settings") : tr("Open Advanced Settings"));
    m_stack->setCurrentIndex(tab);
}

void QuickSettings::openAt(QWidget *anchor, Tab tab)
{
    // Clicking the button that opened the panel closes it again.
    if (m_closedAt.isValid() && m_closedAt.elapsed() < 250 && m_stack->currentIndex() == tab) {
        m_closedAt.invalidate();
        return;
    }
    setTab(tab);
    QWidget *host = parentWidget();
    int h = 790;
    if (tab == AudioTab || tab == SubtitlesTab) {
        // Just as tall as the menu's contents (plus title, footer and margins).
        auto *scroll = qobject_cast<QScrollArea *>(m_stack->currentWidget());
        layout()->activate();
        h = 54 + (scroll && scroll->widget() ? scroll->widget()->sizeHint().height() : 500) +
            m_footer->sizeHint().height() + 28 + 4;
    }
    h = qMin(h, host->height() - 70);
    resize(468, qMax(300, h));
    const QPoint anchorTopRight = anchor->mapTo(host, QPoint(anchor->width(), 0));
    int x = anchorTopRight.x() - width() + 70;
    x = qBound(0, x, host->width() - width());
    const int y = qMax(0, anchorTopRight.y() - height() + 4);
    move(x, y);
    show();
    raise();
    qApp->installEventFilter(this);
}

bool QuickSettings::contains(QObject *obj) const
{
    for (auto *w = qobject_cast<QWidget *>(obj); w; w = w->parentWidget())
        if (w == this)
            return true;
    return false;
}

bool QuickSettings::eventFilter(QObject *obj, QEvent *event)
{
    if (isVisible() && event->type() == QEvent::MouseButtonPress && obj->isWidgetType() && !contains(obj)) {
        hide();
    }
    if (isVisible() && event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        hide();
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

void QuickSettings::hideEvent(QHideEvent *e)
{
    qApp->removeEventFilter(this);
    m_closedAt.start();
    QWidget::hideEvent(e);
}

void QuickSettings::setSleepMinutes(int minutes)
{
    if (!m_sleep)
        return;
    const QSignalBlocker b(m_sleep);
    const int idx = m_sleep->findData(minutes);
    m_sleep->setCurrentIndex(idx < 0 ? 0 : idx);
}

// ---------------------------------------------------------------------------

QWidget *QuickSettings::buildPlayback()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    QVBoxLayout *b;
    QWidget *speed = section(b);
    b->addWidget(sectionHeading(tr("Speed")));
    m_speedBox = valueEdit();
    b->addWidget(settingRow(tr("Playback Speed"), m_speedBox));
    m_speed = new SliderBar;
    m_speed->setRange(0.25, 4.0);
    m_speed->setStep(0.05);
    m_speed->setMarker(1.0);
    b->addWidget(m_speed);
    b->addWidget(scaleLabels({"0.25×", "1×", "2×", "3×", "4×"}));
    auto *presetsRow = new QHBoxLayout;
    for (double r : {0.5, 0.75, 1.0, 1.25, 1.5, 2.0}) {
        auto *btn = new QPushButton(QString("%1×").arg(r));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet("padding: 6px 4px; font-size: 12px;");
        connect(btn, &QPushButton::clicked, this, [this, r] { m_player->setRate(float(r)); });
        presetsRow->addWidget(btn);
    }
    b->addLayout(presetsRow);
    connect(m_speed, &SliderBar::valueChanged, this, [this](double r) { m_player->setRate(float(r)); });
    connect(m_speedBox, &QLineEdit::editingFinished, this, [this] {
        bool ok = false;
        const double r = m_speedBox->text().remove(QRegularExpression("[^0-9.,]")).replace(',', '.').toDouble(&ok);
        if (ok)
            m_player->setRate(float(r));
        refreshValues();
    });
    addSectionTo(v, speed);

    QWidget *order = section(b);
    b->addWidget(sectionHeading(tr("Order")));
    m_repeat = new SegmentedControl({tr("Off"), tr("One"), tr("All")});
    m_repeat->setFixedWidth(240);
    m_repeat->setCurrentIndex(prefs().integer("playback/repeat"));
    b->addWidget(settingRow(tr("Repeat"), m_repeat));
    m_shuffle = new ToggleSwitch;
    m_shuffle->setChecked(prefs().flag("playback/shuffle"));
    b->addWidget(settingRow(tr("Shuffle"), m_shuffle));
    auto *autoNext = new ToggleSwitch;
    autoNext->setChecked(prefs().flag("playback/autoNext"));
    b->addWidget(settingRow(tr("Play Next Automatically"), autoNext));
    connect(m_repeat, &SegmentedControl::currentChanged, this, [](int i) { prefs().set("playback/repeat", i); });
    connect(m_shuffle, &ToggleSwitch::toggled, this, [](bool on) { prefs().set("playback/shuffle", on); });
    connect(autoNext, &ToggleSwitch::toggled, this, [](bool on) { prefs().set("playback/autoNext", on); });
    connect(&prefs(), &Prefs::changed, this, [this, autoNext](const QString &key) {
        if (key == "playback/repeat")
            m_repeat->setCurrentIndex(prefs().integer(key));
        if (key == "playback/shuffle") {
            const QSignalBlocker b(m_shuffle);
            m_shuffle->setChecked(prefs().flag(key));
        }
        if (key == "playback/autoNext") {
            const QSignalBlocker b(autoNext);
            autoNext->setChecked(prefs().flag(key));
        }
    });
    addSectionTo(v, order);

    QWidget *timer = section(b);
    b->addWidget(sectionHeading(tr("Sleep Timer")));
    m_sleep = new Dropdown;
    m_sleep->setFixedWidth(200);
    m_sleep->addItem(tr("Off"), 0);
    for (int m : {15, 30, 45, 60, 90, 120})
        m_sleep->addItem(tr("%1 minutes").arg(m), m);
    m_sleep->addItem(tr("End of current item"), -1);
    b->addWidget(settingRow(tr("Pause After"), m_sleep));
    connect(m_sleep, &QComboBox::currentIndexChanged, this,
            [this](int i) { emit sleepTimerRequested(m_sleep->itemData(i).toInt()); });
    addSectionTo(v, timer);

    QWidget *show = section(b);
    b->addWidget(sectionHeading(tr("Show")));
    b->addWidget(settingRow(tr("Subtitle Timeline"), prefSwitch("playback/subTimeline"),
                            tr("When subtitle lines appear, above the controls; drag it to sync")));
    b->addWidget(settingRow(tr("Waveform on Seek Bar"), prefSwitch("playback/waveform"),
                            tr("The sound's loudness along the seek bar (music always has it)")));
    b->addWidget(settingRow(tr("Metadata"), prefSwitch("playback/metadata"),
                            tr("Title and video and audio details over the video")));
    auto *vis = new Dropdown;
    vis->setFixedWidth(200);
    for (const auto &[label, mode] : QList<QPair<QString, QString>>{{tr("Off (Cover Art)"), ""},
                                                                    {tr("Spectrum"), "spectrum"},
                                                                    {tr("Oscilloscope"), "scope"},
                                                                    {tr("Spectrometer"), "spectrometer"},
                                                                    {tr("VU Meter"), "vuMeter"},
                                                                    {tr("Goom"), "goom"}})
        vis->addItem(label, mode);
    vis->setCurrentIndex(qMax(0, vis->findData(prefs().string("audio/visualizer"))));
    b->addWidget(settingRow(tr("Audio Visualizer"), vis, tr("For music, from the next song")));
    connect(vis, &QComboBox::currentIndexChanged, this, [vis](int i) { prefs().set("audio/visualizer", vis->itemData(i).toString()); });
    addSectionTo(v, show);
    v->addStretch();
    return page;
}

QWidget *QuickSettings::buildVideo()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    QVBoxLayout *b;
    QWidget *defaults = section(b);
    b->addWidget(sectionHeading(tr("Display")));
    m_videoTrack = new Dropdown;
    m_videoTrack->setFixedWidth(200);
    b->addWidget(settingRow(tr("Video Track"), m_videoTrack));
    m_aspect = new Dropdown;
    m_aspect->setFixedWidth(200);
    m_aspect->addItems(VideoWidget::aspectRatios());
    b->addWidget(settingRow(tr("Aspect Ratio"), m_aspect));
    m_deinterlace = new Dropdown;
    m_deinterlace->setFixedWidth(200);
    const QList<QPair<QString, QString>> modes = {{tr("Off"), "off"},         {tr("Blend"), "blend"},
                                                  {tr("Bob"), "bob"},         {tr("Linear"), "linear"},
                                                  {tr("Yadif"), "yadif"},     {tr("Yadif (2x)"), "yadif2x"},
                                                  {tr("Phosphor"), "phosphor"}, {tr("Film NTSC (IVTC)"), "ivtc"}};
    for (const auto &[label, mode] : modes)
        m_deinterlace->addItem(label, mode);
    b->addWidget(settingRow(tr("Deinterlace"), m_deinterlace));
    addSectionTo(v, defaults);

    // Picture transforms (applied while drawing, so instantly).
    QWidget *transform = section(b);
    b->addWidget(sectionHeading(tr("Transform")));
    m_rotate = new SegmentedControl({"0°", "90°", "180°", "270°"});
    m_rotate->setFixedWidth(240);
    b->addWidget(settingRow(tr("Rotate"), m_rotate, tr("Clockwise")));
    m_flipH = new ToggleSwitch;
    b->addWidget(settingRow(tr("Flip Horizontally"), m_flipH));
    m_flipV = new ToggleSwitch;
    b->addWidget(settingRow(tr("Flip Vertically"), m_flipV));
    m_zoom = new Dropdown;
    m_zoom->setFixedWidth(200);
    for (const auto &[label, z] : VideoWidget::zoomLevels())
        m_zoom->addItem(label, z);
    b->addWidget(settingRow(tr("Zoom"), m_zoom));
    connect(m_rotate, &SegmentedControl::currentChanged, this, [this](int i) { m_video->setRotation(i); });
    connect(m_flipH, &ToggleSwitch::toggled, this, [this](bool on) { m_video->setFlip(on, m_video->flipV()); });
    connect(m_flipV, &ToggleSwitch::toggled, this, [this](bool on) { m_video->setFlip(m_video->flipH(), on); });
    connect(m_zoom, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_refreshing && i >= 0)
            m_video->setZoom(m_zoom->itemData(i).toDouble());
    });
    addSectionTo(v, transform);

    QWidget *quality = section(b);
    b->addWidget(sectionHeading(tr("Quality")));
    auto *res = new Dropdown;
    res->setFixedWidth(200);
    res->addItem(tr("Best Available"), -1);
    for (int h : {2160, 1440, 1080, 720, 480, 360, 240})
        res->addItem(QString("%1p").arg(h), h);
    res->setCurrentIndex(qMax(0, res->findData(prefs().integer("video/streamResolution"))));
    b->addWidget(settingRow(tr("Stream Resolution"), res, tr("For online streams that come in several qualities (from the next one opened)")));
    connect(res, &QComboBox::currentIndexChanged, this, [res](int i) { prefs().set("video/streamResolution", res->itemData(i).toInt()); });
    m_hdr = new ToggleSwitch;
    b->addWidget(settingRow(tr("HDR to SDR"), m_hdr, tr("Turn on if an HDR video looks washed out and grey")));
    connect(m_hdr, &ToggleSwitch::toggled, this, [this](bool on) { m_video->setHdrToneMapping(on); });
    addSectionTo(v, quality);

    QWidget *snapSection = section(b);
    auto *snap = new QPushButton(theme().icon("camera", theme().accent()), tr("Take Snapshot"));
    snap->setCursor(Qt::PointingHandCursor);
    b->addWidget(snap);
    connect(snap, &QPushButton::clicked, this, &QuickSettings::snapshotRequested);
    connect(m_videoTrack, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_refreshing && i >= 0)
            m_player->setVideoTrack(m_videoTrack->itemData(i).toInt());
    });
    connect(m_aspect, &QComboBox::currentTextChanged, this, [this](const QString &t) {
        if (!m_refreshing) {
            m_video->setAspectRatio(t);
            prefs().set("video/aspect", t);
        }
    });
    connect(m_deinterlace, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_refreshing)
            m_player->setDeinterlace(m_deinterlace->itemData(i).toString());
    });
    addSectionTo(v, snapSection);

    QWidget *fxs = section(b);
    b->setSpacing(14);
    b->addWidget(sectionHeading(tr("Effects")));
    b->addWidget(FxPanels::imageAdjust(fxs));
    b->addWidget(FxPanels::crop(fxs));
    b->addWidget(FxPanels::colorEffects(fxs));
    auto *reset = new QPushButton(tr("Reset Video Effects"));
    reset->setProperty("role", "link");
    reset->setCursor(Qt::PointingHandCursor);
    connect(reset, &QPushButton::clicked, this, [] {
        fx().resetGroup(Effects::VideoAdjust);
        fx().resetGroup(Effects::VideoFilters);
        fx().resetGroup(Effects::Crop);
    });
    b->addWidget(reset, 0, Qt::AlignLeft);
    addSectionTo(v, fxs);
    v->addStretch();
    return page;
}

QWidget *QuickSettings::buildAudio()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    QVBoxLayout *b;
    QWidget *preset = section(b);
    b->addWidget(settingRow(tr("Show Preset"), presetPicker(),
                            tr("Remembers the audio delay and default track for this show, with the subtitle settings")));
    addSectionTo(v, preset);

    QWidget *tracks = section(b);
    b->addWidget(sectionHeading(tr("Tracks")));
    m_audioList = new ChoiceList;
    m_audioList->setEmptyText(tr("No audio"));
    b->addWidget(m_audioList);
    connect(m_audioList, &ChoiceList::chosen, this, [this](const QVariant &id) { m_player->setAudioTrack(id.toInt()); });
    addSectionTo(v, tracks);

    QWidget *output = section(b);
    auto *refresh = new IconButton("refresh");
    refresh->setToolTip(tr("Look for devices again"));
    b->addWidget(headingWith(tr("Output Device"), refresh));
    m_deviceList = new ChoiceList;
    m_deviceList->setEmptyText(tr("Starts listing once something plays"));
    b->addWidget(m_deviceList);
    connect(refresh, &IconButton::clicked, this, &QuickSettings::refreshDevices);
    connect(m_deviceList, &ChoiceList::chosen, this, [this](const QVariant &id) {
        m_player->setAudioDevice(id.toString());
        QTimer::singleShot(300, this, &QuickSettings::refreshDevices); // show what it switched to
    });
    addSectionTo(v, output);

    QWidget *defaults = section(b);
    b->addWidget(sectionHeading(tr("Defaults")));
    m_stereo = new Dropdown;
    m_stereo->setFixedWidth(200);
    m_stereo->addItem(tr("Stereo"), libvlc_AudioChannel_Stereo);
    m_stereo->addItem(tr("Reverse Stereo"), libvlc_AudioChannel_RStereo);
    m_stereo->addItem(tr("Left"), libvlc_AudioChannel_Left);
    m_stereo->addItem(tr("Right"), libvlc_AudioChannel_Right);
    m_stereo->addItem(tr("Dolby Surround"), libvlc_AudioChannel_Dolbys);
    b->addWidget(settingRow(tr("Stereo Mode"), m_stereo));
    auto *audioDefault = new DefaultTrackPicker(false, m_player);
    audioDefault->setFixedWidth(200);
    b->addWidget(settingRow(tr("Pick Automatically"), audioDefault,
                            tr("The audio track or language to pick for every episode")));
    connect(m_stereo, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_refreshing && i >= 0)
            m_player->setStereoMode(m_stereo->itemData(i).toInt());
    });
    addSectionTo(v, defaults);

    QWidget *sync = section(b);
    b->addWidget(sectionHeading(tr("Synchronization")));
    m_audioDelayBox = valueEdit();
    b->addWidget(settingRow(tr("Audio Track Delay"), m_audioDelayBox));
    m_audioDelay = new SliderBar;
    m_audioDelay->setRange(-10000, 10000);
    m_audioDelay->setStep(50);
    m_audioDelay->setMarker(0);
    b->addWidget(m_audioDelay);
    b->addWidget(scaleLabels({"-10 s", "0 s", "+10 s"}));
    connect(m_audioDelay, &SliderBar::valueChanged, this, [this](double ms) {
        m_player->setAudioDelay(qint64(ms));
        m_audioDelayBox->setText(QString("%1 s").arg(ms / 1000.0, 0, 'f', 3));
    });
    connect(m_audioDelayBox, &QLineEdit::editingFinished, this, [this] {
        bool ok = false;
        const double s = m_audioDelayBox->text().remove(QRegularExpression("[^0-9.,\\-]")).replace(',', '.').toDouble(&ok);
        if (ok)
            m_player->setAudioDelay(qint64(s * 1000));
        refreshValues();
    });
    addSectionTo(v, sync);

    QWidget *effects = section(b);
    b->setSpacing(12);
    b->addWidget(sectionHeading(tr("Effects")));
    auto addToggle = [&](const QString &label, bool EffectsData::*field, const QString &info) {
        auto *t = new ToggleSwitch;
        t->setChecked(fx().data().*field);
        connect(t, &ToggleSwitch::toggled, this, [field](bool on) {
            fx().d().*field = on;
            fx().commit(Effects::AudioFilters);
        });
        connect(&fx(), &Effects::changed, t, [t, field] {
            const QSignalBlocker block(t);
            t->setChecked(fx().data().*field);
            t->update();
        });
        b->addWidget(settingRow(label, t, info));
    };
    {
        auto *eqToggle = new ToggleSwitch;
        eqToggle->setChecked(fx().data().eqEnabled);
        connect(eqToggle, &ToggleSwitch::toggled, this, [](bool on) {
            fx().d().eqEnabled = on;
            fx().commit(Effects::Equalizer);
        });
        connect(&fx(), &Effects::changed, eqToggle, [eqToggle] {
            const QSignalBlocker block(eqToggle);
            eqToggle->setChecked(fx().data().eqEnabled);
            eqToggle->update();
        });
        b->addWidget(settingRow(tr("Equalizer"), eqToggle, tr("10-band graphic equalizer")));
        m_eqPreset = new Dropdown;
        m_eqPreset->setFixedWidth(200);
        m_eqPreset->addItem(eqCurveIcon(QVector<float>(10, 0.0f), false), tr("Custom"));
        const QStringList eqNames = Effects::eqPresetNames();
        for (int i = 0; i < eqNames.size(); ++i)
            m_eqPreset->addItem(eqCurveIcon(Effects::eqPresetBands(i), true), eqNames[i]);
        m_eqPreset->setIconSize(QSize(40, 20));
        m_eqPreset->view()->setIconSize(QSize(40, 20));
        m_eqPreset->setCurrentIndex(fx().data().eqPreset + 1);
        connect(m_eqPreset, &QComboBox::currentIndexChanged, this, [](int i) {
            if (i > 0) {
                fx().d().eqEnabled = true;
                fx().applyEqPreset(i - 1);
            }
        });
        b->addWidget(settingRow(tr("Equalizer Preset"), m_eqPreset));
        b->addWidget(FxPanels::equalizerHorizontal(effects));
    }
    b->addWidget(hline());
    addToggle(tr("Compressor"), &EffectsData::compEnabled, tr("Evens out loud and quiet passages"));
    b->addWidget(FxPanels::compressor(effects));
    b->addWidget(hline());
    addToggle(tr("Spatial Audio"), &EffectsData::spatialEnabled, tr("Headphone virtualization or room reverb"));
    auto *mode = new Dropdown;
    mode->setFixedWidth(200);
    mode->addItems({tr("Headphone virtualization"), tr("Room reverb")});
    mode->setCurrentIndex(fx().data().spatialMode);
    connect(mode, &QComboBox::currentIndexChanged, this, [](int i) {
        fx().d().spatialMode = i;
        fx().commit(Effects::AudioFilters);
    });
    b->addWidget(settingRow(tr("Spatial Audio Mode"), mode));
    b->addWidget(FxPanels::reverb(effects));
    b->addWidget(hline());
    addToggle(tr("Stereo Widening"), &EffectsData::widenEnabled, tr("Makes stereo sound wider"));
    b->addWidget(FxPanels::stereoWiden(effects));
    b->addWidget(hline());
    addToggle(tr("Volume Normalization"), &EffectsData::normalize, tr("Keeps loudness consistent between items"));
    addSectionTo(v, effects);
    v->addStretch();
    return page;
}

QWidget *QuickSettings::buildSubtitles()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    QVBoxLayout *b;
    QWidget *preset = section(b);
    b->addWidget(settingRow(tr("Show Preset"), presetPicker(),
                            tr("Remembers the subtitle look and timing and default tracks for this show")));
    addSectionTo(v, preset);

    QWidget *embedded = section(b);
    b->addWidget(sectionHeading(tr("Embedded Tracks")));
    m_subEmbedded = new ChoiceList;
    b->addWidget(m_subEmbedded);
    addSectionTo(v, embedded);

    QWidget *external = section(b);
    auto *add = new IconButton("plus");
    add->setToolTip(tr("Add a subtitle file…"));
    b->addWidget(headingWith(tr("External Tracks"), add));
    m_subExternal = new ChoiceList;
    m_subExternal->setEmptyText(tr("No subtitle files"));
    b->addWidget(m_subExternal);
    auto *online = new QPushButton(tr("Find Online…"));
    online->setCursor(Qt::PointingHandCursor);
    online->setFixedWidth(200);
    b->addWidget(settingRow(tr("Download Subtitles"), online));
    connect(add, &IconButton::clicked, this, [this] {
        hide();
        emit loadSubtitleRequested();
    });
    connect(online, &QPushButton::clicked, this, [this] {
        hide();
        emit findSubtitlesRequested();
    });
    for (ChoiceList *list : {m_subEmbedded, m_subExternal})
        connect(list, &ChoiceList::chosen, this, [this](const QVariant &id) { m_player->setSubtitleTrack(id.toInt()); });
    addSectionTo(v, external);

    QWidget *subDefaults = section(b);
    b->addWidget(sectionHeading(tr("Defaults")));
    auto *subDefault = new DefaultTrackPicker(true, m_player);
    subDefault->setFixedWidth(200);
    b->addWidget(settingRow(tr("Turn On Automatically"), subDefault,
                            tr("The subtitle track or language to pick for every episode")));
    addSectionTo(v, subDefaults);

    QWidget *sync = section(b);
    b->addWidget(sectionHeading(tr("Synchronization")));
    b->addWidget(settingRow(tr("Subtitle Timeline"), prefSwitch("playback/subTimeline"),
                            tr("See when lines appear and drag them into place")));
    m_subDelayBox = valueEdit();
    b->addWidget(settingRow(tr("Subtitle Track Delay"), m_subDelayBox));
    m_subDelay = new SliderBar;
    m_subDelay->setRange(-10000, 10000);
    m_subDelay->setStep(50);
    m_subDelay->setMarker(0);
    b->addWidget(m_subDelay);
    b->addWidget(scaleLabels({"-10 s", "0 s", "+10 s"}));
    b->addSpacing(8);
    m_subSpeedBox = valueEdit();
    b->addWidget(settingRow(tr("Subtitle Speed"), m_subSpeedBox));
    m_subSpeed = new SliderBar;
    m_subSpeed->setRange(0.5, 4.0);
    m_subSpeed->setStep(0.01);
    b->addWidget(m_subSpeed);
    b->addSpacing(8);
    m_subDurationBox = valueEdit();
    b->addWidget(settingRow(tr("Subtitle Duration Factor"), m_subDurationBox));
    m_subDuration = new SliderBar;
    m_subDuration->setRange(0, 20);
    m_subDuration->setStep(0.1);
    b->addWidget(m_subDuration);

    connect(m_subDelay, &SliderBar::valueChanged, this, [this](double ms) {
        m_player->setSubtitleDelay(qint64(ms));
        m_subDelayBox->setText(QString("%1 s").arg(ms / 1000.0, 0, 'f', 3));
    });
    connect(m_subSpeed, &SliderBar::valueChanged, this, [this](double f) {
        m_player->setSubtitleSpeed(float(f));
        m_subSpeedBox->setText(QString("%1 fps").arg(f, 0, 'f', 3));
    });
    connect(m_subDuration, &SliderBar::valueChanged, this, [this](double f) {
        m_player->setSubtitleDurationFactor(float(f));
        m_subDurationBox->setText(QString::number(f, 'f', 3));
    });
    auto parse = [](QLineEdit *e) {
        bool ok = false;
        const double v = e->text().remove(QRegularExpression("[^0-9.,\\-]")).replace(',', '.').toDouble(&ok);
        return ok ? v : qQNaN();
    };
    connect(m_subDelayBox, &QLineEdit::editingFinished, this, [this, parse] {
        const double s = parse(m_subDelayBox);
        if (!qIsNaN(s))
            m_player->setSubtitleDelay(qint64(s * 1000));
        refreshValues();
    });
    connect(m_subSpeedBox, &QLineEdit::editingFinished, this, [this, parse] {
        const double s = parse(m_subSpeedBox);
        if (!qIsNaN(s))
            m_player->setSubtitleSpeed(float(qBound(0.5, s, 4.0)));
        refreshValues();
    });
    connect(m_subDurationBox, &QLineEdit::editingFinished, this, [this, parse] {
        const double s = parse(m_subDurationBox);
        if (!qIsNaN(s))
            m_player->setSubtitleDurationFactor(float(qBound(0.0, s, 20.0)));
        refreshValues();
    });
    addSectionTo(v, sync);

    QWidget *look = section(b);
    b->addWidget(sectionHeading(tr("Appearance")));
    // Each control writes its field and saves; the look itself needs the item
    // reopened (VLC reads it once), except the position, which applies live.
    auto styleChanged = [] { fx().commit(Effects::SubtitleStyle); };
    auto *font = new Dropdown;
    font->setFixedWidth(200);
    font->addItems(QFontDatabase::families());
    font->setCurrentText(fx().data().subFont);
    b->addWidget(settingRow(tr("Font"), font));
    connect(font, &QComboBox::currentTextChanged, this, [styleChanged](const QString &f) {
        fx().d().subFont = f;
        styleChanged();
    });

    // Size: relative to the picture (VLC's named sizes) or exact pixels.
    auto *sizeMode = new SegmentedControl({tr("Relative"), tr("Exact")});
    sizeMode->setFixedWidth(200);
    b->addWidget(settingRow(tr("Text Size"), sizeMode, tr("Relative follows the video's height; Exact is in pixels")));
    auto *relSize = new Dropdown;
    relSize->setFixedWidth(200);
    const QList<QPair<QString, int>> sizes = {{tr("Smaller"), 20}, {tr("Small"), 18}, {tr("Normal"), 16},
                                              {tr("Large"), 12},   {tr("Larger"), 6}};
    for (const auto &[label, value] : sizes)
        relSize->addItem(label, value);
    auto *relRow = settingRow(tr("Relative Size"), relSize);
    b->addWidget(relRow);
    auto *pxBox = valueEdit();
    auto *pxRow = settingRow(tr("Size in Pixels"), pxBox);
    b->addWidget(pxRow);
    auto *pxSlider = new SliderBar;
    pxSlider->setRange(10, 120);
    pxSlider->setStep(1);
    b->addWidget(pxSlider);
    auto showSize = [=] {
        const EffectsData &e = fx().data();
        const bool exact = e.subSizePixels > 0;
        const QSignalBlocker b0(sizeMode);
        sizeMode->setCurrentIndex(exact ? 1 : 0);
        relRow->setVisible(!exact);
        pxRow->setVisible(exact);
        pxSlider->setVisible(exact);
        const QSignalBlocker b1(relSize);
        relSize->setCurrentIndex(qMax(0, relSize->findData(e.subSize)));
        pxSlider->setValue(exact ? e.subSizePixels : 40);
        pxBox->setText(QString("%1 px").arg(exact ? e.subSizePixels : 40));
    };
    showSize();
    connect(sizeMode, &SegmentedControl::currentChanged, this, [=](int i) {
        const int px = i == 1 ? qMax(10, int(pxSlider->value())) : 0;
        if (px == fx().data().subSizePixels)
            return;
        fx().d().subSizePixels = px;
        styleChanged();
        showSize();
    });
    connect(relSize, &QComboBox::currentIndexChanged, this, [=](int i) {
        fx().d().subSize = relSize->itemData(i).toInt();
        styleChanged();
    });
    // The slider moves freely and applies when let go (each change reopens the item).
    connect(pxSlider, &SliderBar::valueChanged, this, [pxBox](double v) { pxBox->setText(QString("%1 px").arg(int(v))); });
    connect(pxSlider, &SliderBar::sliderReleased, this, [=] {
        fx().d().subSizePixels = int(pxSlider->value());
        styleChanged();
    });
    connect(pxBox, &QLineEdit::editingFinished, this, [=] {
        bool ok = false;
        const int v = pxBox->text().remove(QRegularExpression("[^0-9]")).toInt(&ok);
        if (ok && v >= 6 && v != fx().data().subSizePixels) {
            fx().d().subSizePixels = qMin(v, 300);
            styleChanged();
        }
        showSize();
    });

    auto *textColor = new ColorField(fx().data().subColor);
    b->addWidget(settingRow(tr("Text Color"), textColor));
    auto *outline = new ColorField(fx().data().subOutline);
    b->addWidget(settingRow(tr("Outline Color"), outline));
    auto *thickness = new Dropdown;
    thickness->setFixedWidth(200);
    for (const auto &[label, value] : QList<QPair<QString, int>>{{tr("None"), 0}, {tr("Thin"), 2}, {tr("Normal"), 4}, {tr("Thick"), 6}})
        thickness->addItem(label, value);
    thickness->setCurrentIndex(qMax(0, thickness->findData(fx().data().subOutlineThickness)));
    b->addWidget(settingRow(tr("Outline Thickness"), thickness));
    connect(thickness, &QComboBox::currentIndexChanged, this, [=](int i) {
        fx().d().subOutlineThickness = thickness->itemData(i).toInt();
        styleChanged();
    });

    auto *bgBox = valueEdit();
    b->addWidget(settingRow(tr("Background Opacity"), bgBox, tr("A box behind the text")));
    auto *bgSlider = new SliderBar;
    bgSlider->setRange(0, 100);
    bgSlider->setStep(5);
    b->addWidget(bgSlider);
    auto showBg = [=] {
        const int pct = int(std::lround(fx().data().subBackgroundOpacity * 100 / 255.0));
        bgSlider->setValue(pct);
        bgBox->setText(QString("%1%").arg(pct));
    };
    showBg();
    connect(bgSlider, &SliderBar::valueChanged, this, [bgBox](double v) { bgBox->setText(QString("%1%").arg(int(v))); });
    connect(bgSlider, &SliderBar::sliderReleased, this, [=] {
        fx().d().subBackgroundOpacity = int(std::lround(bgSlider->value() * 255 / 100));
        styleChanged();
    });
    connect(bgBox, &QLineEdit::editingFinished, this, [=] {
        bool ok = false;
        const int v = bgBox->text().remove(QRegularExpression("[^0-9]")).toInt(&ok);
        if (ok) {
            fx().d().subBackgroundOpacity = int(std::lround(qBound(0, v, 100) * 255 / 100.0));
            styleChanged();
        }
        showBg();
    });

    auto *marginBox = valueEdit();
    b->addWidget(settingRow(tr("Position"), marginBox, tr("Raise the subtitles from the bottom of the picture")));
    auto *margin = new SliderBar;
    margin->setRange(0, 300);
    margin->setStep(2);
    margin->setMarker(0);
    b->addWidget(margin);
    b->addWidget(scaleLabels({tr("Bottom"), "150 px", "300 px"}));
    auto showMargin = [=] {
        margin->setValue(fx().data().subMargin);
        marginBox->setText(QString("%1 px").arg(fx().data().subMargin));
    };
    showMargin();
    connect(margin, &SliderBar::valueChanged, this, [this, marginBox](double v) {
        fx().d().subMargin = int(v);
        m_player->setSubtitleMargin(int(v)); // live
        marginBox->setText(QString("%1 px").arg(int(v)));
    });
    connect(margin, &SliderBar::sliderReleased, this, [] { fx().commit(Effects::SubtitleStyle, false); });
    connect(marginBox, &QLineEdit::editingFinished, this, [=] {
        bool ok = false;
        const int v = marginBox->text().remove(QRegularExpression("[^0-9]")).toInt(&ok);
        if (ok) {
            fx().d().subMargin = qBound(0, v, 1000);
            m_player->setSubtitleMargin(fx().data().subMargin);
            fx().commit(Effects::SubtitleStyle, false);
        }
        showMargin();
    });

    auto *ownStyle = new ToggleSwitch;
    ownStyle->setChecked(fx().data().subEmbeddedStyle);
    b->addWidget(settingRow(tr("Use the Subtitles' Own Styling"), ownStyle,
                            tr("Off: styled subtitles (like anime .ass files) use the look above instead")));
    connect(ownStyle, &ToggleSwitch::toggled, this, [=](bool on) {
        fx().d().subEmbeddedStyle = on;
        styleChanged();
    });
    connect(textColor, &ColorField::colorChanged, this, [=](const QColor &c) {
        fx().d().subColor = c;
        styleChanged();
    });
    connect(outline, &ColorField::colorChanged, this, [=](const QColor &c) {
        fx().d().subOutline = c;
        styleChanged();
    });
    // Presets (and the settings window) change these too.
    connect(&fx(), &Effects::changed, look, [=](Effects::Group g) {
        if (g != Effects::SubtitleStyle)
            return;
        const EffectsData &e = fx().data();
        const QSignalBlocker b1(font), b2(thickness), b3(ownStyle), b4(textColor), b5(outline);
        font->setCurrentText(e.subFont);
        thickness->setCurrentIndex(qMax(0, thickness->findData(e.subOutlineThickness)));
        ownStyle->setChecked(e.subEmbeddedStyle);
        textColor->setColor(e.subColor);
        outline->setColor(e.subOutline);
        showSize();
        showBg();
        if (!margin->isDragging())
            showMargin();
    });
    auto *note = new QLabel(tr("Changes to the look reopen the current item at the same position."));
    note->setProperty("role", "muted");
    note->setWordWrap(true);
    b->addWidget(note);
    addSectionTo(v, look);
    v->addStretch();
    return page;
}

QWidget *QuickSettings::buildFooter()
{
    auto *footer = m_footer = new QWidget;
    footer->setObjectName("QuickFooter");
    auto restyle = [footer] {
        footer->setStyleSheet(QString("QWidget#QuickFooter { background: %1; border-bottom-left-radius: 12px; "
                                      "border-bottom-right-radius: 12px; }")
                                  .arg(theme().isDark() ? theme().tone(QColor(0x14, 0x14, 0x14)).name() : theme().pal().chrome.darker(104).name()));
    };
    restyle();
    connect(&theme(), &Theme::changed, footer, restyle);
    auto *v = new QVBoxLayout(footer);
    v->setContentsMargins(20, 16, 20, 10);
    v->setSpacing(6);
    m_preset = new Dropdown;
    m_preset->setFixedHeight(44);
    for (const Preset &p : presets())
        m_preset->addItem(p.name);
    m_preset->setToolTip(tr("Apply an effects preset"));
    connect(m_preset, &QComboBox::activated, this, [](int i) {
        const auto list = presets();
        if (i < 0 || i >= list.size())
            return;
        list[i].apply(fx().d());
        for (auto g : {Effects::Equalizer, Effects::AudioFilters, Effects::VideoAdjust, Effects::VideoFilters,
                       Effects::Crop})
            fx().commit(g);
    });
    v->addWidget(m_preset);
    auto *advanced = m_advanced = new QPushButton(theme().icon("settings", theme().pal().textDim), tr("Open Advanced Settings"));
    advanced->setProperty("role", "link");
    advanced->setCursor(Qt::PointingHandCursor);
    advanced->setStyleSheet("font-size: 15px;");
    connect(&theme(), &Theme::changed, advanced,
            [advanced] { advanced->setIcon(theme().icon("settings", theme().pal().textDim)); });
    connect(advanced, &QPushButton::clicked, this, [this] {
        hide();
        emit advancedSettingsRequested(m_stack->currentIndex());
    });
    v->addWidget(advanced);
    return footer;
}

void QuickSettings::refreshTracks()
{
    m_refreshing = true;
    auto fill = [](Dropdown *d, const QList<Player::Track> &tracks, int current, const QString &none) {
        d->clear();
        if (tracks.isEmpty())
            d->addItem(none, -1);
        for (const auto &t : tracks)
            d->addItem(t.name, t.id);
        const int idx = d->findData(current);
        d->setCurrentIndex(idx < 0 ? 0 : idx);
        d->setEnabled(!tracks.isEmpty());
    };
    fill(m_videoTrack, m_player->videoTracks(), m_player->videoTrack(), tr("No video"));
    QList<QPair<QString, QVariant>> audio;
    for (const auto &t : m_player->audioTracks())
        audio.append({t.id < 0 ? tr("None") : t.name, t.id});
    m_audioList->setItems(audio, m_player->audioTrack());
    // Subtitles: "None" sits with the file's own tracks; files are listed apart.
    const int sub = m_player->subtitleTrack();
    QList<QPair<QString, QVariant>> inside{{tr("None"), -1}}, files;
    for (const auto &t : m_player->subtitleTrackList())
        (t.external ? files : inside).append({t.name, t.id});
    m_subEmbedded->setItems(inside, sub);
    m_subExternal->setItems(files, sub);
    m_refreshing = false;
}

void QuickSettings::refreshDevices()
{
    const QString current = m_player->audioDevice();
    QList<QPair<QString, QVariant>> items;
    for (const auto &d : m_player->audioDevices())
        items.append({d.name.isEmpty() ? d.id : d.name, d.id});
    m_deviceList->setItems(items, current);
}

void QuickSettings::refreshValues()
{
    m_refreshing = true;
    m_speed->setValue(m_player->rate());
    m_speedBox->setText(QString("%1×").arg(m_player->rate(), 0, 'f', 2));
    m_aspect->setCurrentText(m_video->aspectRatio());
    m_deinterlace->setCurrentIndex(qMax(0, m_deinterlace->findData(m_player->deinterlace())));
    m_stereo->setCurrentIndex(qMax(0, m_stereo->findData(m_player->stereoMode())));
    m_audioDelay->setValue(double(m_player->audioDelay()));
    m_audioDelayBox->setText(QString("%1 s").arg(m_player->audioDelay() / 1000.0, 0, 'f', 3));
    m_subDelay->setValue(double(m_player->subtitleDelay()));
    m_subDelayBox->setText(QString("%1 s").arg(m_player->subtitleDelay() / 1000.0, 0, 'f', 3));
    m_subSpeed->setValue(m_player->subtitleSpeed());
    m_subSpeedBox->setText(QString("%1 fps").arg(m_player->subtitleSpeed(), 0, 'f', 3));
    m_subDuration->setValue(m_player->subtitleDurationFactor());
    m_subDurationBox->setText(QString::number(m_player->subtitleDurationFactor(), 'f', 3));
    {
        const QSignalBlocker b1(m_rotate), b2(m_flipH), b3(m_flipV), b4(m_hdr);
        m_rotate->setCurrentIndex(m_video->rotation());
        m_flipH->setChecked(m_video->flipH());
        m_flipV->setChecked(m_video->flipV());
        m_hdr->setChecked(m_video->hdrToneMapping());
        double best = 0;
        int at = 0;
        for (int i = 0; i < m_zoom->count(); ++i)
            if (qAbs(m_zoom->itemData(i).toDouble() - m_video->zoom()) < qAbs(best - m_video->zoom()) || i == 0) {
                best = m_zoom->itemData(i).toDouble();
                at = i;
            }
        m_zoom->setCurrentIndex(at);
    }
    m_refreshing = false;
}
