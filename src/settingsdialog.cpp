#include "settingsdialog.h"
#include "effects.h"
#include "filedialogs.h"
#include "fxpanels.h"
#include "library.h"
#include "opensubtitles.h"
#include "player.h"
#include "prefs.h"
#include "theme.h"
#include "updatecheck.h"
#include "version.h"
#include "videowidget.h"
#include "widgets.h"

#include <QButtonGroup>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <vlc/vlc.h>

namespace {

// Toggle bound to a boolean preference.
ToggleSwitch *prefToggle(const QString &key, QObject *ctx)
{
    auto *t = new ToggleSwitch;
    t->setChecked(prefs().flag(key));
    QObject::connect(t, &ToggleSwitch::toggled, ctx, [key](bool on) { prefs().set(key, on); });
    QObject::connect(&prefs(), &Prefs::changed, t, [t, key](const QString &k) {
        if (k == key) {
            const QSignalBlocker b(t);
            t->setChecked(prefs().flag(key));
            t->update();
        }
    });
    return t;
}

// Toggle bound to a boolean effects field.
ToggleSwitch *fxToggle(bool EffectsData::*field, Effects::Group group, QObject *ctx)
{
    auto *t = new ToggleSwitch;
    t->setChecked(fx().data().*field);
    QObject::connect(t, &ToggleSwitch::toggled, ctx, [field, group](bool on) {
        fx().d().*field = on;
        fx().commit(group);
    });
    QObject::connect(&fx(), &Effects::changed, t, [t, field] {
        const QSignalBlocker b(t);
        t->setChecked(fx().data().*field);
        t->update();
    });
    return t;
}

// Dropdown bound to a preference; items carry their stored value as data.
Dropdown *prefDropdown(const QString &key, const QList<QPair<QString, QVariant>> &items, QObject *ctx)
{
    auto *d = new Dropdown;
    d->setFixedWidth(330);
    for (const auto &[label, value] : items)
        d->addItem(label, value);
    const int idx = d->findData(prefs().get(key));
    d->setCurrentIndex(idx < 0 ? 0 : idx);
    QObject::connect(d, &QComboBox::currentIndexChanged, ctx, [d, key](int i) { prefs().set(key, d->itemData(i)); });
    return d;
}

QWidget *pathRow(const QString &key, bool directory, QWidget *parent)
{
    auto *w = new QWidget(parent);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(12);
    auto *edit = new QLineEdit(prefs().string(key));
    edit->setFixedSize(260, 40);
    auto *browse = new QPushButton(QObject::tr("Browse"));
    browse->setCursor(Qt::PointingHandCursor);
    browse->setFixedSize(120, 40);
    h->addWidget(edit);
    h->addWidget(browse);
    QObject::connect(edit, &QLineEdit::editingFinished, w, [edit, key] { prefs().set(key, edit->text()); });
    QObject::connect(browse, &QPushButton::clicked, w, [edit, key, directory, w] {
        const QString path = directory
                                 ? FileDialogs::existingDirectory(w->window(), QObject::tr("Choose folder"), edit->text())
                                 : FileDialogs::saveFile(w->window(), QObject::tr("Choose file"), edit->text());
        if (!path.isEmpty()) {
            edit->setText(path);
            prefs().set(key, path);
        }
    });
    return w;
}

QPushButton *wideButton(const QString &text)
{
    auto *b = new QPushButton(text);
    b->setProperty("role", "wide");
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(46);
    return b;
}

QLabel *note(const QString &text)
{
    auto *l = new QLabel(text);
    l->setProperty("role", "muted");
    l->setWordWrap(true);
    return l;
}

// Editable list of folders stored in a preference.
QWidget *folderList(const QString &key, QWidget *parent)
{
    auto *w = new QWidget(parent);
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    auto *list = new QListWidget;
    list->setFixedHeight(130);
    list->setStyleSheet(QString("QListWidget { background: %1; border-radius: 8px; padding: 6px; }")
                            .arg(theme().pal().input.name()));
    list->addItems(prefs().list(key));
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(QObject::tr("Add folder…"));
    auto *remove = new QPushButton(QObject::tr("Remove"));
    add->setCursor(Qt::PointingHandCursor);
    remove->setCursor(Qt::PointingHandCursor);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    v->addWidget(list);
    v->addLayout(buttons);
    auto store = [list, key] {
        QStringList items;
        for (int i = 0; i < list->count(); ++i)
            items << list->item(i)->text();
        prefs().set(key, items);
    };
    QObject::connect(add, &QPushButton::clicked, w, [w, list, store] {
        const QString dir = FileDialogs::existingDirectory(w->window(), QObject::tr("Add library folder"));
        if (!dir.isEmpty() && list->findItems(dir, Qt::MatchExactly).isEmpty()) {
            list->addItem(dir);
            store();
        }
    });
    QObject::connect(remove, &QPushButton::clicked, w, [list, store] {
        delete list->currentItem();
        store();
    });
    QObject::connect(&prefs(), &Prefs::changed, list, [list, key](const QString &k) {
        if (k != key)
            return;
        list->clear();
        list->addItems(prefs().list(key));
    });
    return w;
}

} // namespace

SettingsOverlay::SettingsOverlay(Player *player, QWidget *parent) : QWidget(parent), m_player(player)
{
    hide();
    setFocusPolicy(Qt::StrongFocus);
    m_panel = new QWidget(this);
    m_panel->setObjectName("SettingsPanel");
    m_panel->setAttribute(Qt::WA_StyledBackground);
    auto *h = new QHBoxLayout(m_panel);
    h->setContentsMargins(18, 18, 18, 18);
    h->setSpacing(24);

    // Left column: search + navigation.
    auto *left = new QVBoxLayout;
    left->setSpacing(18);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search"));
    m_search->setProperty("role", "search");
    m_search->setFixedHeight(40);
    m_search->setClearButtonEnabled(true);
    auto *icon = new QLabel(m_search);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    icon->setGeometry(12, 11, 18, 18);
    auto refreshIcon = [icon] { icon->setPixmap(theme().pixmap("search", theme().pal().textDim, 18, 2.0)); };
    refreshIcon();
    connect(&theme(), &Theme::changed, icon, refreshIcon);
    left->addWidget(m_search);

    auto *nav = new QWidget;
    nav->setObjectName("SettingsNav");
    nav->setAttribute(Qt::WA_StyledBackground);
    nav->setFixedWidth(230);
    auto *navLay = new QVBoxLayout(nav);
    navLay->setContentsMargins(8, 14, 8, 14);
    navLay->setSpacing(2);
    struct NavDef {
        QString group;
        QString icon;
        QString label;
        QString title;
    };
    const QList<NavDef> defs = {
        {tr("App"), "tune", tr("General"), tr("Global Settings")},
        {{}, "pointer", tr("Interface"), tr("Interface")},
        {{}, "shield", tr("Privacy"), tr("Privacy")},
        {{}, "keyboard", tr("Controls"), tr("Controls")},
        {tr("Media"), "play", tr("Playback"), tr("Playback")},
        {{}, "video", tr("Video"), tr("Video Defaults")},
        {{}, "audio", tr("Audio"), tr("Audio Defaults")},
        {{}, "subtitles", tr("Subtitles"), tr("Subtitle Defaults")},
        {{}, "folder", tr("Library"), tr("Library")},
        {tr("System"), "network", tr("Network"), tr("Network")},
        {{}, "gauge", tr("Performance"), tr("Performance")},
        {{}, "record", tr("Recording"), tr("Recording & Snapshots")},
        {{}, "puzzle", tr("Extensions"), tr("Extensions")},
    };
    for (int i = 0; i < defs.size(); ++i) {
        if (!defs[i].group.isEmpty()) {
            auto *g = new QLabel(defs[i].group);
            g->setProperty("role", "section");
            g->setContentsMargins(12, i == 0 ? 2 : 14, 0, 6);
            g->setStyleSheet("font-size: 14px;");
            navLay->addWidget(g);
            m_groupLabels << g;
        }
        auto *b = new IconButton(defs[i].icon);
        b->setLabel(defs[i].label);
        b->setIconPixelSize(22);
        b->setFixedHeight(42);
        navLay->addWidget(b);
        m_nav << b;
        m_titles << defs[i].title;
        connect(b, &IconButton::clicked, this, [this, i] { setPage(i); });
    }
    navLay->addStretch();
    left->addWidget(nav, 1);
    h->addLayout(left);

    // Right column: title + pages.
    auto *right = new QVBoxLayout;
    right->setSpacing(10);
    auto *header = new QHBoxLayout;
    m_title = new QLabel;
    m_title->setStyleSheet("font-size: 19px;");
    auto *close = new IconButton("close");
    close->setIconPixelSize(22);
    close->setToolTip(tr("Close (Esc)"));
    connect(close, &IconButton::clicked, this, &SettingsOverlay::close);
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(close);
    right->addLayout(header);
    right->addWidget(hline());
    m_stack = new QStackedWidget;
    right->addWidget(m_stack, 1);
    h->addLayout(right, 1);

    const QList<QWidget *(SettingsOverlay::*)()> builders = {
        &SettingsOverlay::pageGeneral,  &SettingsOverlay::pageInterface,  &SettingsOverlay::pagePrivacy,
        &SettingsOverlay::pageControls, &SettingsOverlay::pagePlayback,   &SettingsOverlay::pageVideo,
        &SettingsOverlay::pageAudio,    &SettingsOverlay::pageSubtitles,  &SettingsOverlay::pageLibrary,
        &SettingsOverlay::pageNetwork,  &SettingsOverlay::pagePerformance, &SettingsOverlay::pageRecording,
        &SettingsOverlay::pageExtensions,
    };
    for (int i = 0; i < builders.size(); ++i) {
        QWidget *content = (this->*builders[i])();
        // Collect row labels for search.
        QStringList words = {defs[i].label, defs[i].title};
        for (QWidget *row : content->findChildren<QWidget *>("SettingRow"))
            words << row->property("searchText").toString();
        for (QLabel *l : content->findChildren<QLabel *>())
            words << l->text();
        m_keywords << words;
        m_stack->addWidget(wrap(content));
    }
    connect(m_search, &QLineEdit::textChanged, this, &SettingsOverlay::filter);
    if (parent)
        parent->installEventFilter(this);
    setPage(General);
}

QWidget *SettingsOverlay::wrap(QWidget *content)
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *holder = new QWidget;
    auto *v = new QVBoxLayout(holder);
    v->setContentsMargins(4, 16, 18, 24);
    v->addWidget(content);
    v->addStretch();
    scroll->setWidget(holder);
    return scroll;
}

void SettingsOverlay::open(Page page)
{
    setPage(page);
    layoutPanel();
    show();
    raise();
    setFocus();
}

void SettingsOverlay::close()
{
    hide();
    if (parentWidget())
        parentWidget()->setFocus();
}

void SettingsOverlay::setPage(int page)
{
    if (page == Extensions && !m_extensionsLoaded) {
        m_extensionsLoaded = true;
        // Filled lazily: enumerating modules spins up a temporary libVLC instance.
        auto *content = m_stack->widget(Extensions)->findChild<QWidget *>("ExtensionsContent");
        if (content) {
            auto *lay = static_cast<QVBoxLayout *>(content->layout());
            for (bool audio : {true, false}) {
                lay->addWidget(sectionHeading(audio ? tr("Audio filters") : tr("Video filters")));
                auto *list = new QListWidget;
                list->setFixedHeight(260);
                list->setStyleSheet(QString("QListWidget { background: %1; border-radius: 8px; padding: 6px; }")
                                        .arg(theme().pal().input.name()));
                for (const auto &[name, desc] : Player::filterModules(audio))
                    list->addItem(desc.isEmpty() ? name : QString("%1  —  %2").arg(name, desc));
                lay->addWidget(list);
            }
        }
    }
    m_stack->setCurrentIndex(page);
    m_title->setText(m_titles.value(page));
    for (int i = 0; i < m_nav.size(); ++i)
        m_nav[i]->setSelectedLook(i == page);
}

void SettingsOverlay::filter(const QString &text)
{
    const QString t = text.trimmed();
    int first = -1;
    for (int i = 0; i < m_nav.size(); ++i) {
        bool match = t.isEmpty();
        for (const QString &w : m_keywords[i])
            if (!match && w.contains(t, Qt::CaseInsensitive))
                match = true;
        m_nav[i]->setVisible(match);
        if (match && first < 0)
            first = i;
    }
    for (QLabel *g : m_groupLabels)
        g->setVisible(t.isEmpty());
    if (first >= 0 && !m_nav[m_stack->currentIndex()]->isVisible())
        setPage(first);
}

void SettingsOverlay::layoutPanel()
{
    if (!parentWidget())
        return;
    setGeometry(parentWidget()->rect());
    const int w = qMin(1180, width() - 60);
    const int h = qMin(900, height() - 60);
    m_panel->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);
}

bool SettingsOverlay::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == parentWidget() && event->type() == QEvent::Resize && isVisible())
        layoutPanel();
    return QWidget::eventFilter(obj, event);
}

void SettingsOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 150));
}

void SettingsOverlay::mousePressEvent(QMouseEvent *e)
{
    if (!m_panel->geometry().contains(e->position().toPoint()))
        close();
}

void SettingsOverlay::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------------------
// Pages

static QVBoxLayout *pageLayout(QWidget *&page)
{
    page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    return v;
}

QWidget *SettingsOverlay::pageGeneral()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Appearance")));
    auto *lang = new Dropdown;
    lang->setFixedWidth(330);
    lang->addItem(tr("English"));
    v->addWidget(settingRow(tr("Interface language"), lang));
    auto *themeSeg = new SegmentedControl(Theme::modeNames());
    themeSeg->setFixedWidth(330);
    themeSeg->setCurrentIndex(theme().mode());
    connect(themeSeg, &SegmentedControl::currentChanged, this, [](int i) { Theme::pick(Theme::Mode(i)); });
    connect(&theme(), &Theme::changed, themeSeg, [themeSeg] { themeSeg->setCurrentIndex(theme().mode()); });
    v->addWidget(settingRow(tr("Theme"), themeSeg));
    auto *swatches = new AccentSwatches;
    connect(swatches, &AccentSwatches::picked, this, &Theme::pickAccent);
    v->addWidget(settingRow(tr("Accent color"), swatches));

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Startup")));
    v->addWidget(settingRow(tr("Show confirmation and error dialogs"), prefToggle("startup/confirmDialogs", this),
                            tr("When off, playback errors appear as a brief notification instead of a dialog")));
    v->addWidget(settingRow(tr("Resume last session on launch"), prefToggle("startup/restoreSession", this),
                            tr("Reopen the queue you were playing when the app was closed")));

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Updates")));
    v->addWidget(settingRow(tr("Check for updates"), prefToggle("updates/check", this),
                            tr("Once a day, ask GitHub whether a newer VMP is out. Nothing else is sent.")));
    auto *checkNow = new QPushButton(tr("Check Now"));
    checkNow->setCursor(Qt::PointingHandCursor);
    auto *updateStatus = note(UpdateCheck::updatedByPacman()
                                  ? tr("You have %1. Updates install with your system: sudo pacman -Syu")
                                        .arg(UpdateCheck::currentVersion())
                                  : tr("You have %1.").arg(UpdateCheck::currentVersion()));
    updateStatus->setTextFormat(Qt::RichText);
    updateStatus->setOpenExternalLinks(true);
    v->addWidget(checkNow, 0, Qt::AlignLeft);
    v->addWidget(updateStatus);
    connect(checkNow, &QPushButton::clicked, this, [checkNow, updateStatus] {
        checkNow->setEnabled(false);
        updateStatus->setText(tr("Checking…"));
        UpdateCheck::instance().checkNow();
    });
    connect(&UpdateCheck::instance(), &UpdateCheck::finished, updateStatus,
            [checkNow, updateStatus](bool newer, const QString &version, const QString &page, const QString &error, bool) {
                checkNow->setEnabled(true);
                if (!error.isEmpty())
                    updateStatus->setText(error.toHtmlEscaped());
                else if (!newer)
                    updateStatus->setText(tr("VMP %1 is the latest version.").arg(UpdateCheck::currentVersion()));
                else
                    updateStatus->setText(
                        (UpdateCheck::updatedByPacman() ? tr("VMP %1 is out. Update with: sudo pacman -Syu")
                                                        : tr("VMP %1 is out."))
                            .arg(version.toHtmlEscaped()) +
                        QString(" · <a href=\"%1\">%2</a>").arg(page.toHtmlEscaped(), tr("What's new")));
            });

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Diagnostics")));
    v->addWidget(settingRow(tr("Developer mode"), prefToggle("diag/developer", this),
                            tr("Print libVLC's messages to the terminal")));
    v->addWidget(settingRow(tr("Save a log"), prefToggle("diag/saveLog", this),
                            tr("Write libVLC's messages to a log file")));
    v->addWidget(settingRow(tr("Log file"), pathRow("diag/logFile", false, page), tr("Where the log is written")));
    v->addWidget(settingRow(tr("Log details"),
                            prefDropdown("diag/logLevel",
                                         {{tr("Errors only"), 0}, {tr("Warnings"), 1}, {tr("Information"), 2},
                                          {tr("Everything (debug)"), 3}},
                                         this),
                            tr("How much detail goes into the log")));

    v->addSpacing(18);
    auto *advanced = wideButton(tr("Advanced Options  ›"));
    v->addWidget(advanced);
    auto *advBox = new QWidget;
    auto *advLay = new QVBoxLayout(advBox);
    advLay->setContentsMargins(0, 6, 0, 0);
    auto *args = new QLineEdit(prefs().string("advanced/args"));
    args->setPlaceholderText("--no-audio-time-stretch --aout=pulse");
    args->setFixedHeight(40);
    auto *apply = new QPushButton(tr("Apply and restart engine"));
    apply->setCursor(Qt::PointingHandCursor);
    advLay->addWidget(settingRow(tr("Extra libVLC options"), nullptr,
                                 tr("Passed straight to libVLC. Invalid options are ignored.")));
    advLay->addWidget(args);
    advLay->addWidget(apply, 0, Qt::AlignLeft);
    advLay->addWidget(note(tr("These are the same options the vlc command line accepts (see vlc --longhelp).")));
    advBox->hide();
    v->addWidget(advBox);
    connect(advanced, &QPushButton::clicked, this, [advanced, advBox] {
        advBox->setVisible(!advBox->isVisible());
        advanced->setText(advBox->isVisible() ? tr("Advanced Options  ⌄") : tr("Advanced Options  ›"));
    });
    connect(apply, &QPushButton::clicked, this, [args] { prefs().set("advanced/args", args->text().trimmed()); });
    return page;
}

QWidget *SettingsOverlay::pageInterface()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Theme")));
    auto *cards = new QHBoxLayout;
    cards->setSpacing(18);
    auto *group = new QButtonGroup(page);
    const QStringList names = Theme::modeNames();
    for (int i = 0; i < names.size(); ++i) {
        auto *card = new ThemeCard(names[i], i);
        card->setChecked(theme().mode() == i);
        group->addButton(card, i);
        cards->addWidget(card);
    }
    cards->addStretch();
    v->addLayout(cards);
    connect(group, &QButtonGroup::idClicked, this, [](int i) { Theme::pick(Theme::Mode(i)); });
    connect(&theme(), &Theme::changed, group, [group] {
        if (auto *b = group->button(theme().mode()))
            b->setChecked(true);
    });
    v->addWidget(settingRow(tr("Accent color"), [this] {
        auto *s = new AccentSwatches;
        connect(s, &AccentSwatches::picked, this, &Theme::pickAccent);
        return s;
    }()));

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Layout")));
    v->addWidget(settingRow(tr("Open the library on startup"), prefToggle("ui/startInLibrary", this),
                            tr("Otherwise the player view is shown")));
    v->addWidget(settingRow(tr("Show remaining time"), prefToggle("ui/remainingTime", this)));
    v->addWidget(settingRow(tr("Click on video to play / pause"), prefToggle("ui/clickPause", this)));
    v->addWidget(settingRow(tr("Pause when minimized"), prefToggle("ui/pauseOnMinimize", this)));
    v->addWidget(settingRow(tr("Hide controls in fullscreen after"),
                            prefDropdown("ui/autoHideMs",
                                         {{tr("1.5 seconds"), 1500}, {tr("2.5 seconds"), 2500},
                                          {tr("4 seconds"), 4000}, {tr("Never"), 0}},
                                         this)));

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Thumbnail size")));
    auto *radios = new QHBoxLayout;
    radios->setSpacing(24);
    auto *rg = new QButtonGroup(page);
    const QStringList sizes = {tr("Small"), tr("Medium"), tr("Large")};
    for (int i = 0; i < 3; ++i) {
        auto *r = new RadioDot(sizes[i]);
        r->setChecked(prefs().integer("ui/thumbSize") == i);
        rg->addButton(r, i);
        radios->addWidget(r);
    }
    radios->addStretch();
    v->addLayout(radios);
    connect(rg, &QButtonGroup::idClicked, this, [](int i) { prefs().set("ui/thumbSize", i); });
    return page;
}

QWidget *SettingsOverlay::pagePrivacy()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("History")));
    v->addWidget(settingRow(tr("Remember recently played media"), prefToggle("privacy/history", this)));
    v->addWidget(settingRow(tr("Remember playback positions"), prefToggle("privacy/positions", this),
                            tr("Used to resume playback and draw progress bars on thumbnails")));
    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Network")));
    v->addWidget(settingRow(tr("Fetch artwork and metadata online"), prefToggle("privacy/metaNetwork", this),
                            tr("Lets libVLC look up album art on the internet")));
    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Clear data")));
    auto *row = new QHBoxLayout;
    auto *recent = new QPushButton(tr("Clear recent history"));
    auto *positions = new QPushButton(tr("Clear playback positions"));
    auto *thumbs = new QPushButton(tr("Clear thumbnail cache"));
    for (auto *b : {recent, positions, thumbs}) {
        b->setCursor(Qt::PointingHandCursor);
        row->addWidget(b);
    }
    row->addStretch();
    v->addLayout(row);
    connect(recent, &QPushButton::clicked, this, [] { library().clearRecent(); });
    connect(positions, &QPushButton::clicked, this, [] { library().clearPositions(); });
    connect(thumbs, &QPushButton::clicked, this, [] { Thumbnailer::instance().clearCache(); });
    return page;
}

QWidget *SettingsOverlay::pageControls()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Behaviour")));
    v->addWidget(settingRow(tr("Seek step"),
                            prefDropdown("controls/seekStep",
                                         {{tr("5 seconds"), 5}, {tr("10 seconds"), 10}, {tr("15 seconds"), 15},
                                          {tr("30 seconds"), 30}, {tr("1 minute"), 60}},
                                         this),
                            tr("How far the arrow keys jump")));
    v->addWidget(settingRow(tr("Volume step"),
                            prefDropdown("controls/volumeStep", {{"2 %", 2}, {"5 %", 5}, {"10 %", 10}}, this)));
    v->addWidget(settingRow(tr("Mouse wheel over video"),
                            prefDropdown("controls/wheel", {{tr("Volume"), 0}, {tr("Seek"), 1}, {tr("Nothing"), 2}}, this)));
    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Keyboard shortcuts")));
    const QList<QPair<QString, QString>> keys = {
        {tr("Play / Pause"), "Space"},          {tr("Stop"), "S"},
        {tr("Previous / Next item"), "P / N"},  {tr("Seek back / forward"), "← / →"},
        {tr("Long jump"), "Ctrl + ← / →"},      {tr("Volume"), "↑ / ↓"},
        {tr("Mute"), "M"},                      {tr("Fullscreen"), "F / F11"},
        {tr("Exit fullscreen"), "Esc"},         {tr("Hide UI"), "Ctrl + \\"},
        {tr("Toggle sidebar"), "Ctrl + B"},     {tr("Library / Player"), "Ctrl + L"},
        {tr("Record"), "R"},                    {tr("Snapshot"), "Shift + S"},
        {tr("Faster / Slower"), "] / ["},       {tr("Normal speed"), "="},
        {tr("Open file"), "Ctrl + O"},          {tr("Open network stream"), "Ctrl + N"},
        {tr("Settings"), "Ctrl + P"},           {tr("Quit"), "Ctrl + Q"},
    };
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(30);
    grid->setVerticalSpacing(10);
    for (int i = 0; i < keys.size(); ++i) {
        auto *name = new QLabel(keys[i].first);
        name->setProperty("role", "dim");
        auto *key = new QLabel(keys[i].second);
        key->setStyleSheet(QString("background: %1; border-radius: 4px; padding: 3px 8px; font-size: 12px;")
                               .arg(theme().pal().surface.name()));
        grid->addWidget(name, i / 2, (i % 2) * 2);
        grid->addWidget(key, i / 2, (i % 2) * 2 + 1, Qt::AlignLeft);
    }
    v->addLayout(grid);
    return page;
}

QWidget *SettingsOverlay::pagePlayback()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Resume playback")));
    auto *radios = new QHBoxLayout;
    radios->setSpacing(24);
    auto *group = new QButtonGroup(page);
    const QStringList opts = {tr("Always"), tr("Ask"), tr("Never")};
    for (int i = 0; i < 3; ++i) {
        auto *r = new RadioDot(opts[i]);
        r->setChecked(prefs().integer("playback/resume") == i);
        group->addButton(r, i);
        radios->addWidget(r);
    }
    radios->addStretch();
    v->addLayout(radios);
    connect(group, &QButtonGroup::idClicked, this, [](int i) { prefs().set("playback/resume", i); });
    v->addWidget(note(tr("Continue where you left off when reopening a file you didn't finish.")));

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Queue")));
    v->addWidget(settingRow(tr("Play next item automatically"), prefToggle("playback/autoNext", this)));
    v->addWidget(settingRow(tr("Shuffle"), prefToggle("playback/shuffle", this)));
    auto *repeat = new SegmentedControl({tr("Off"), tr("One"), tr("All")});
    repeat->setFixedWidth(330);
    repeat->setCurrentIndex(prefs().integer("playback/repeat"));
    connect(repeat, &SegmentedControl::currentChanged, this, [](int i) { prefs().set("playback/repeat", i); });
    connect(&prefs(), &Prefs::changed, repeat, [repeat](const QString &k) {
        if (k == "playback/repeat")
            repeat->setCurrentIndex(prefs().integer(k));
    });
    v->addWidget(settingRow(tr("Repeat"), repeat));
    v->addWidget(settingRow(tr("Default speed"),
                            prefDropdown("playback/speed",
                                         {{"0.5×", 0.5}, {"0.75×", 0.75}, {"1×", 1.0}, {"1.25×", 1.25},
                                          {"1.5×", 1.5}, {"2×", 2.0}},
                                         this),
                            tr("Speed used when the app starts")));
    return page;
}

QWidget *SettingsOverlay::pageVideo()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Decoding")));
    v->addWidget(settingRow(tr("Hardware-accelerated decoding"), prefToggle("video/hw", this),
                            tr("Uses VA-API/VDPAU when available. Applies from the next item.")));
    auto *deint = prefDropdown("video/deinterlace",
                               {{tr("Off"), "off"}, {tr("Blend"), "blend"}, {tr("Bob"), "bob"},
                                {tr("Linear"), "linear"}, {tr("Yadif"), "yadif"}, {tr("Yadif (2x)"), "yadif2x"},
                                {tr("Phosphor"), "phosphor"}, {tr("Film NTSC (IVTC)"), "ivtc"}},
                               this);
    connect(deint, &QComboBox::currentIndexChanged, this,
            [this, deint](int i) { m_player->setDeinterlace(deint->itemData(i).toString()); });
    v->addWidget(settingRow(tr("Deinterlace"), deint));
    QList<QPair<QString, QVariant>> aspects;
    for (const QString &a : VideoWidget::aspectRatios())
        aspects.append({a, a});
    v->addWidget(settingRow(tr("Default aspect ratio"), prefDropdown("video/aspect", aspects, this)));

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Effects")));
    v->addWidget(FxPanels::imageAdjust(page));
    v->addWidget(FxPanels::crop(page));
    v->addWidget(FxPanels::colorEffects(page));
    return page;
}

QWidget *SettingsOverlay::pageAudio()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Effects")));
    v->addWidget(settingRow(tr("Equalizer"), fxToggle(&EffectsData::eqEnabled, Effects::Equalizer, this),
                            tr("10-band graphic equalizer")));
    auto *preset = new Dropdown;
    preset->setFixedWidth(330);
    preset->addItem(tr("None"));
    preset->addItems(Effects::eqPresetNames());
    preset->setCurrentIndex(fx().data().eqPreset + 1);
    connect(preset, &QComboBox::activated, this, [](int i) {
        if (i == 0) {
            fx().d().eqPreset = -1;
            fx().commit(Effects::Equalizer);
            return;
        }
        fx().d().eqEnabled = true;
        fx().applyEqPreset(i - 1);
    });
    connect(&fx(), &Effects::changed, preset, [preset] {
        const QSignalBlocker b(preset);
        preset->setCurrentIndex(fx().data().eqPreset + 1);
    });
    v->addWidget(settingRow(tr("Equalizer preset"), preset, tr("Start from one of VLC's built-in curves")));
    v->addWidget(FxPanels::equalizerVertical(page));
    v->addWidget(settingRow(tr("Two-pass"), fxToggle(&EffectsData::eqTwoPass, Effects::Equalizer, this),
                            tr("Filter the audio twice for a stronger effect")));
    v->addWidget(settingRow(tr("Spatial audio"), fxToggle(&EffectsData::spatialEnabled, Effects::AudioFilters, this),
                            tr("Headphone virtualization or room reverb")));
    auto *mode = new Dropdown;
    mode->setFixedWidth(330);
    mode->addItems({tr("Headphone virtualization"), tr("Room reverb")});
    mode->setCurrentIndex(fx().data().spatialMode);
    connect(mode, &QComboBox::currentIndexChanged, this, [](int i) {
        fx().d().spatialMode = i;
        fx().commit(Effects::AudioFilters);
    });
    connect(&fx(), &Effects::changed, mode, [mode] {
        const QSignalBlocker b(mode);
        mode->setCurrentIndex(fx().data().spatialMode);
    });
    v->addWidget(settingRow(tr("Spatial audio mode"), mode, tr("Room reverb uses the settings below")));
    v->addWidget(FxPanels::reverb(page));
    v->addWidget(settingRow(tr("Stereo widening"), fxToggle(&EffectsData::widenEnabled, Effects::AudioFilters, this),
                            tr("Makes the stereo image wider")));
    v->addWidget(FxPanels::stereoWiden(page));
    v->addWidget(settingRow(tr("Dynamic range compressor"), fxToggle(&EffectsData::compEnabled, Effects::AudioFilters, this),
                            tr("Evens out loud and quiet passages")));
    v->addWidget(FxPanels::compressor(page));
    v->addWidget(settingRow(tr("Volume normalization"), fxToggle(&EffectsData::normalize, Effects::AudioFilters, this),
                            tr("Keeps loudness consistent between items")));
    return page;
}

QWidget *SettingsOverlay::pageSubtitles()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Loading")));
    v->addWidget(settingRow(tr("Load matching subtitle files automatically"), prefToggle("subs/autoload", this),
                            tr("Looks for .srt/.ass files next to the video")));
    auto *lang = new QLineEdit(prefs().string("subs/language"));
    lang->setPlaceholderText("en,eng");
    lang->setFixedSize(330, 40);
    connect(lang, &QLineEdit::editingFinished, this, [lang] { prefs().set("subs/language", lang->text().trimmed()); });
    v->addWidget(settingRow(tr("Preferred subtitle language"), lang, tr("Comma-separated language codes")));

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("On-Screen Messages")));
    v->addWidget(settingRow(tr("Show the key that was pressed"), prefToggle("osd/showKeys", this),
                            tr("Messages from keyboard shortcuts also name the key, like “Pressed C”")));

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("OpenSubtitles")));
    v->addWidget(note(tr("Downloads go through the OpenSubtitles.com API (the same subtitles as opensubtitles.org). "
                         "It needs a free API key: sign up at opensubtitles.com, then create one under "
                         "Profile → API consumers.")));
    auto *key = new QLineEdit(prefs().string("opensubtitles/apiKey"));
    key->setPlaceholderText(OpenSubtitles::instance().hasKey() ? tr("Using the key built into this app")
                                                               : tr("Paste your API key"));
    key->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    key->setFixedSize(330, 40);
    connect(key, &QLineEdit::editingFinished, this, [key] { prefs().set("opensubtitles/apiKey", key->text().trimmed()); });
    v->addWidget(settingRow(tr("API key"), key));

    auto *account = new QWidget;
    auto *accLay = new QHBoxLayout(account);
    accLay->setContentsMargins(0, 0, 0, 0);
    accLay->setSpacing(10);
    auto *user = new QLineEdit(OpenSubtitles::instance().user());
    user->setPlaceholderText(tr("Username"));
    user->setFixedSize(160, 40);
    auto *password = new QLineEdit;
    password->setPlaceholderText(tr("Password"));
    password->setEchoMode(QLineEdit::Password);
    password->setFixedSize(160, 40);
    auto *signIn = new QPushButton;
    signIn->setCursor(Qt::PointingHandCursor);
    signIn->setFixedHeight(40);
    accLay->addWidget(user);
    accLay->addWidget(password);
    accLay->addWidget(signIn);
    auto *accountRow = settingRow(tr("Account"), account,
                                  tr("Optional. Signed-in accounts get more downloads a day. Only the session "
                                     "token is kept, not the password."));
    v->addWidget(accountRow);
    auto *accountState = note(QString());
    v->addWidget(accountState);
    auto refreshAccount = [user, password, signIn, accountState] {
        const bool in = OpenSubtitles::instance().signedIn();
        user->setEnabled(!in);
        password->setVisible(!in);
        signIn->setText(in ? tr("Sign Out") : tr("Sign In"));
        accountState->setText(in ? tr("Signed in as %1.").arg(OpenSubtitles::instance().user()) : QString());
        accountState->setVisible(in);
    };
    refreshAccount();
    connect(&OpenSubtitles::instance(), &OpenSubtitles::accountChanged, accountState, refreshAccount);
    connect(signIn, &QPushButton::clicked, this, [key, user, password, signIn, accountState] {
        prefs().set("opensubtitles/apiKey", key->text().trimmed());
        if (OpenSubtitles::instance().signedIn()) {
            OpenSubtitles::instance().signOut();
            return;
        }
        if (user->text().trimmed().isEmpty() || password->text().isEmpty())
            return;
        signIn->setEnabled(false);
        OpenSubtitles::instance().signIn(user->text().trimmed(), password->text(),
                                         [signIn, password, accountState](const QString &error) {
                                             signIn->setEnabled(true);
                                             password->clear();
                                             if (!error.isEmpty()) {
                                                 accountState->setText(error);
                                                 accountState->setVisible(true);
                                             }
                                         });
    });

    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Appearance")));
    auto *font = new Dropdown;
    font->setFixedWidth(330);
    font->addItems(QFontDatabase::families());
    font->setCurrentText(fx().data().subFont);
    connect(font, &QComboBox::currentTextChanged, this, [](const QString &f) {
        fx().d().subFont = f;
        fx().commit(Effects::SubtitleStyle);
    });
    v->addWidget(settingRow(tr("Font"), font));
    auto *size = new Dropdown;
    size->setFixedWidth(330);
    const QList<QPair<QString, int>> sizes = {{tr("Smaller"), 20}, {tr("Small"), 18}, {tr("Normal"), 16},
                                              {tr("Large"), 12},   {tr("Larger"), 6}};
    for (const auto &[label, value] : sizes)
        size->addItem(label, value);
    size->setCurrentIndex(qMax(0, size->findData(fx().data().subSize)));
    connect(size, &QComboBox::currentIndexChanged, this, [size](int i) {
        fx().d().subSize = size->itemData(i).toInt();
        fx().commit(Effects::SubtitleStyle);
    });
    v->addWidget(settingRow(tr("Text size"), size));
    auto *color = new ColorField(fx().data().subColor);
    connect(color, &ColorField::colorChanged, this, [](const QColor &c) {
        fx().d().subColor = c;
        fx().commit(Effects::SubtitleStyle);
    });
    v->addWidget(settingRow(tr("Text color"), color));
    auto *outline = new ColorField(fx().data().subOutline);
    connect(outline, &ColorField::colorChanged, this, [](const QColor &c) {
        fx().d().subOutline = c;
        fx().commit(Effects::SubtitleStyle);
    });
    v->addWidget(settingRow(tr("Outline color"), outline));
    v->addWidget(settingRow(tr("Use the subtitles' own styling"),
                            fxToggle(&EffectsData::subEmbeddedStyle, Effects::SubtitleStyle, this)));
    v->addWidget(note(tr("Exact sizes, outline, background and position are in the Subtitles menu while "
                         "something plays.")));
    return page;
}

QWidget *SettingsOverlay::pageLibrary()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Video folders")));
    v->addWidget(folderList("library/videoFolders", page));
    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Music folders")));
    v->addWidget(folderList("library/musicFolders", page));
    v->addSpacing(8);
    v->addWidget(note(tr("Folders are browsed live; nothing is imported or copied. Thumbnails are generated with "
                         "ffmpegthumbnailer or ffmpeg when installed.")));
    return page;
}

QWidget *SettingsOverlay::pageNetwork()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Open a stream")));
    auto *row = new QHBoxLayout;
    auto *url = new QLineEdit;
    url->setPlaceholderText("https://, rtsp://, udp://, smb:// …");
    url->setFixedHeight(40);
    auto *open = new QPushButton(tr("Open"));
    open->setProperty("role", "accent");
    open->setCursor(Qt::PointingHandCursor);
    open->setFixedHeight(40);
    row->addWidget(url, 1);
    row->addWidget(open);
    v->addLayout(row);
    auto go = [this, url] {
        if (url->text().trimmed().isEmpty())
            return;
        emit openUrlRequested(url->text().trimmed());
        url->clear();
        close();
    };
    connect(open, &QPushButton::clicked, this, go);
    connect(url, &QLineEdit::returnPressed, this, go);
    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Buffering")));
    auto *caching = new ValueSlider(tr("Network caching"), 0, 10000, prefs().integer("network/caching"),
                                    [](double ms) { return QString("%1 ms").arg(int(ms)); });
    caching->slider()->setStep(50);
    connect(caching->slider(), &SliderBar::sliderReleased, this,
            [caching] { prefs().set("network/caching", int(caching->value())); });
    v->addWidget(caching);
    v->addWidget(note(tr("More caching makes unstable streams smoother at the cost of latency. "
                         "Changing it restarts the playback engine.")));
    return page;
}

QWidget *SettingsOverlay::pagePerformance()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Decoding")));
    v->addWidget(settingRow(tr("Hardware-accelerated decoding"), prefToggle("video/hw", this)));
    v->addWidget(settingRow(tr("Skip H.264 loop filter"),
                            prefDropdown("performance/skipLoop",
                                         {{tr("None (best quality)"), 0}, {tr("Non-reference frames"), 1},
                                          {tr("Bidirectional frames"), 2}, {tr("Non-key frames"), 3},
                                          {tr("All frames (fastest)"), 4}},
                                         this),
                            tr("Trades picture quality for speed on slow machines")));
    v->addWidget(settingRow(tr("Fast seek"), prefToggle("performance/fastSeek", this),
                            tr("Jump to the nearest keyframe instead of the exact time")));
    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Caching")));
    auto *caching = new ValueSlider(tr("File caching"), 0, 5000, prefs().integer("performance/fileCaching"),
                                    [](double ms) { return QString("%1 ms").arg(int(ms)); });
    caching->slider()->setStep(50);
    connect(caching->slider(), &SliderBar::sliderReleased, this,
            [caching] { prefs().set("performance/fileCaching", int(caching->value())); });
    v->addWidget(caching);
    v->addWidget(note(tr("Changes on this page restart the playback engine; the current item resumes where it was.")));
    return page;
}

QWidget *SettingsOverlay::pageRecording()
{
    QWidget *page;
    auto *v = pageLayout(page);
    v->addWidget(sectionHeading(tr("Recording")));
    v->addWidget(settingRow(tr("Recordings folder"), pathRow("record/dir", true, page)));
    auto *rec = new QPushButton;
    rec->setCursor(Qt::PointingHandCursor);
    auto updateRec = [this, rec] {
        rec->setText(m_player->isRecording() ? tr("Stop recording") : tr("Start recording"));
        rec->setIcon(theme().icon("record", m_player->isRecording() ? QColor(0xe5, 0x48, 0x4d) : theme().accent()));
    };
    updateRec();
    connect(m_player, &Player::recordingChanged, rec, updateRec);
    connect(rec, &QPushButton::clicked, this, &SettingsOverlay::recordToggled);
    v->addWidget(rec, 0, Qt::AlignLeft);
    v->addWidget(note(tr("Recording saves what's playing to an MPEG-TS file (R toggles it). "
                         "The item briefly reopens when recording starts or stops.")));
    v->addSpacing(16);
    v->addWidget(sectionHeading(tr("Snapshots")));
    v->addWidget(settingRow(tr("Snapshots folder"), pathRow("snapshot/dir", true, page)));
    v->addWidget(settingRow(tr("Format"), prefDropdown("snapshot/format", {{"PNG", "png"}, {"JPEG", "jpg"}}, this)));
    return page;
}

QWidget *SettingsOverlay::pageExtensions()
{
    QWidget *page;
    auto *v = pageLayout(page);
    page->setObjectName("ExtensionsContent");
    v->addWidget(sectionHeading(tr("About")));
    v->addWidget(note(tr("Vibes Media Player %1").arg(QStringLiteral(VMP_VERSION_LABEL VMP_EDITION))));
    v->addWidget(note(tr("libVLC %1 · %2").arg(QString::fromUtf8(libvlc_get_version()),
                                                QString::fromUtf8(libvlc_get_compiler()))));
    v->addWidget(note(tr("The filter modules below come from VLC's plugin folder and power the audio and "
                         "video effects.")));
    return page;
}
