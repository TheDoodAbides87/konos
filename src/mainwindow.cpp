#include "mainwindow.h"
#include "mediadialogs.h"
#include "shortcuts.h"
#include "datafile.h"
#include "customizeplayer.h"
#include "onboarding.h"
#include "profiles.h"
#include "mediainfocard.h"
#include "showpresets.h"
#include "subtitletimeline.h"
#include "subtitlecues.h"
#include "musiceditors.h"
#include "musicindex.h"
#include "playerpanel.h"
#include "librarydialogs.h"
#include "miniplayer.h"
#include "netfeeds.h"
#include "durations.h"
#include <QAbstractItemView>
#include <QListView>
#include <QListWidget>
#include "chrome.h"
#include "effects.h"
#include "filedialogs.h"
#include "library.h"
#include "player.h"
#include "prefs.h"
#include "quicksettings.h"
#include "settingsdialog.h"
#include "opensubtitles.h"
#include "subtitleoverlay.h"
#include "theme.h"
#include "updatecheck.h"
#include "version.h"
#include "videowidget.h"
#include "widgets.h"

#include <QAction>
#include <QActionGroup>
#include <QClipboard>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QTextStream>
#include <QUrl>
#include <QApplication>
#include <QCloseEvent>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QPushButton>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QMenu>
#include <QPainter>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QShortcutEvent>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWindow>
#include <random>
#include <vlc/vlc.h>

static constexpr int kResizeBorder = 6;


MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Konos Media Player"));
    setWindowFlag(Qt::FramelessWindowHint);
    setAcceptDrops(true);
    setMinimumSize(760, 480);

    m_player = new Player(this);
    buildUi();
    createActions();
    connectPlayer();

    m_positionTimer.setInterval(5000);
    connect(&m_positionTimer, &QTimer::timeout, this, &MainWindow::savePosition);
    m_positionTimer.start();

    m_hideTimer.setSingleShot(true);
    connect(&m_hideTimer, &QTimer::timeout, this, [this] {
        if (!(m_fullscreen || m_pip) || m_quick->isVisible() || QApplication::activePopupWidget() ||
            m_controls->underMouse() || m_title->underMouse() || m_timeline->underMouse())
            return;
        m_controls->setVisible(false);
        m_title->hide();
        m_video->setCursor(Qt::BlankCursor);
    });

    // Fullscreen / picture in picture: the bars hide soon after the pointer
    // leaves them (see overlayHover).
    m_awayTimer.setSingleShot(true);
    m_awayTimer.setInterval(350);
    connect(&m_awayTimer, &QTimer::timeout, this, [this] {
        if (!(m_fullscreen || m_pip) || m_quick->isVisible() || QApplication::activePopupWidget())
            return;
        if (!m_nearTop && !m_title->underMouse())
            m_title->hide();
        if (m_fullscreen && !m_nearBottom && !m_controls->underMouse() && !m_timeline->underMouse())
            m_controls->hide();
    });

    m_sleepTimer.setSingleShot(true);
    connect(&m_sleepTimer, &QTimer::timeout, this, [this] {
        m_player->pause();
        m_sleepMinutes = 0;
        m_quick->setSleepMinutes(0);
        m_toast->show(tr("Sleep timer: playback paused"));
    });

    // Engine-wide options need a fresh libVLC instance.
    m_reinitTimer.setSingleShot(true);
    m_reinitTimer.setInterval(500);
    connect(&m_reinitTimer, &QTimer::timeout, this, [this] { m_player->reinitialize(); });
    connect(&prefs(), &Prefs::changed, this, [this](const QString &key) {
        static const QStringList engineKeys = {
            "network/caching", "performance/fileCaching", "performance/skipLoop", "performance/fastSeek",
            "subs/fuzzy", "subs/language", "subs/encoding", "subs/paths", "privacy/metaNetwork", "advanced/args",
            "video/dropLate", "audio/timeStretch", "audio/replayGain", "audio/output", "network/proxy",
            "network/reconnect", "performance/threads", "performance/fastDecode"};
        if (engineKeys.contains(key))
            m_reinitTimer.start();
        if (key == "ui/libraryQueue" && m_libraryQueue != prefs().flag(key)) {
            m_libraryQueue = prefs().flag(key);
            updateLayout();
        }
        if (key == "ui/miniPlayer")
            updateLayout();
        if (key == "video/hw" && m_player->hasMedia())
            m_player->restart();
        if (key == "video/aspect")
            m_video->setAspectRatio(prefs().string(key));
        if (key == "audio/visualizer")
            m_player->setVisualizer(prefs().string(key));
        if (key == "playback/subTimeline" || key == "playback/metadata") {
            if (key == "playback/metadata" && prefs().flag(key))
                m_infoTimer.start(4000);
            placeVideoOverlays();
        }
    });
    m_player->setVisualizer(prefs().string("audio/visualizer"));
    ShowPresets::instance().setPlayer(m_player);

    m_infoTimer.setSingleShot(true);
    connect(&m_infoTimer, &QTimer::timeout, this, &MainWindow::placeVideoOverlays);
    connect(m_video, &VideoWidget::mouseMoved, this, [this] {
        if (prefs().flag("playback/metadata")) {
            m_infoTimer.start(qMax(1500, prefs().integer("ui/autoHideMs")));
            placeVideoOverlays();
        }
    });
    // Default subtitle/audio tracks: once per item, when its tracks are known.
    m_defaultsTimer.setSingleShot(true);
    m_defaultsTimer.setInterval(600);
    connect(&m_defaultsTimer, &QTimer::timeout, this, [this] {
        if (m_defaultsFor != m_player->location()) {
            m_defaultsFor = m_player->location();
            ShowPresets::instance().applyDefaultTracks();
        }
        usePlainSubtitles();
    });
    connect(m_player, &Player::tracksChanged, this, [this] {
        if (m_player->hasMedia())
            m_defaultsTimer.start();
    });
    connect(&SubtitleCues::instance(), &SubtitleCues::ready, this, [this] { m_defaultsTimer.start(); });
    connect(m_player, &Player::mediaChanged, this, [this] {
        if (prefs().flag("playback/metadata"))
            m_infoTimer.start(4000);
        placeVideoOverlays();
    });

    connect(&theme(), &Theme::changed, this, &MainWindow::applyTheme);
    qApp->installEventFilter(this);

    QSettings s;
    if (!restoreGeometry(s.value("window/geometry").toByteArray()))
        resize(1440, 860);
    m_sidebar = prefs().flag("ui/sidePanel");
    m_panelOpen = prefs().flag("ui/rightPanel");
    m_panel->setTab(PlayerPanel::Tab(qBound(0, prefs().integer("ui/rightTab"), 2)));
    m_video->setAspectRatio(prefs().string("video/aspect"));

    if (!m_player->isValid())
        QMetaObject::invokeMethod(this, [this] { showError(m_player->lastError()); }, Qt::QueuedConnection);

    if (prefs().flag("ui/startInLibrary"))
        showLibrary(Section::Home);
    else
        showPlayer();
    if (prefs().flag("startup/restoreSession"))
        QMetaObject::invokeMethod(this, &MainWindow::restoreSession, Qt::QueuedConnection);
    // A data file that couldn't be read was kept aside (not overwritten): say so.
    if (const QStringList aside = DataFile::setAside(); !aside.isEmpty())
        QTimer::singleShot(1500, this, [this, aside] {
            showError(tr("Some of Konos's saved data couldn't be read and was set aside, so it wasn't lost:\n%1\n"
                         "Konos started fresh for that part.").arg(aside.join('\n')));
        });
    // The first time Konos runs: welcome, theme, keys. Not in scripted test runs.
    if (Onboarding::wanted() && qEnvironmentVariableIsEmpty("KONOS_SCRIPT"))
        QMetaObject::invokeMethod(this, [this] { (new Onboarding(this))->open(); }, Qt::QueuedConnection);
    else if (!QSettings().contains("onboarding/done"))
        QSettings().setValue("onboarding/done", true); // people updating from 0.6 have set things up already
#ifdef KONOS_DEVHOOKS
    runDevScript();
#endif
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
}

// ---------------------------------------------------------------------------
// UI construction

void MainWindow::buildUi()
{
    auto *root = new QWidget(this);
    root->setObjectName("Root");
    root->setAttribute(Qt::WA_StyledBackground);
    setCentralWidget(root);
    m_rootLayout = new QVBoxLayout(root);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    m_title = new TitleBar(root);
    m_rootLayout->addWidget(m_title);

    auto *body = new QWidget(root);
    auto *bodyLay = new QHBoxLayout(body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(0);

    m_rail = new NavBar(false, body);
    m_navHolder = new QWidget(body);
    m_navHolder->setObjectName("Root");
    auto *navLay = new QVBoxLayout(m_navHolder);
    navLay->setContentsMargins(8, 8, 0, 8);
    m_nav = new NavBar(true, m_navHolder);
    navLay->addWidget(m_nav);

    m_side = new LibraryView(LibraryView::Compact, body);
    m_side->setFixedWidth(392);
    m_library = new LibraryView(LibraryView::Full, body);

    m_videoArea = new QWidget(body);
    m_videoArea->setObjectName("VideoArea");
    m_videoArea->setAttribute(Qt::WA_StyledBackground);
    auto *videoLay = new QVBoxLayout(m_videoArea);
    videoLay->setContentsMargins(0, 0, 0, 0);
    m_video = new VideoWidget(m_player, m_videoArea);
    videoLay->addWidget(m_video);
    m_toast = new Toast(m_videoArea);
    // Update notices go over the whole window (the library or the player).
    m_updateToast = new Toast(this);
    m_updateToast->setBottomMargin(96);
    connect(&UpdateCheck::instance(), &UpdateCheck::finished, this,
            [this](bool newer, const QString &version, const QString &page, const QString &, bool manual) {
                if (!newer || manual)
                    return;
                m_updateToast->raise();
                if (UpdateCheck::canSelfUpdate() && !UpdateCheck::instance().appImageUrl().isEmpty()) {
                    m_updateToast->showAt(Toast::Bottom, tr("Konos %1 is out.").arg(version), tr("Update Now"), [this] {
                        m_updateToast->showAt(Toast::Bottom, tr("Downloading the update…"), {}, {}, 600000);
                        UpdateCheck::instance().installUpdate();
                    }, 20000);
                    return;
                }
                m_updateToast->showAt(Toast::Bottom,
                                      UpdateCheck::updatedByPacman()
                                          ? tr("Konos %1 is out. Update with: sudo pacman -Syu").arg(version)
                                          : tr("Konos %1 is out.").arg(version),
                                      tr("What's New"), [page] { QDesktopServices::openUrl(QUrl(page)); }, 15000);
            });
    connect(&UpdateCheck::instance(), &UpdateCheck::progress, this, [this](int percent) {
        m_updateToast->showAt(Toast::Bottom, tr("Downloading the update… %1%").arg(percent), {}, {}, 600000);
    });
    connect(&UpdateCheck::instance(), &UpdateCheck::installed, this, [this](bool ok, const QString &error) {
        m_updateToast->raise();
        if (ok)
            m_updateToast->showAt(Toast::Bottom, tr("Update installed. It starts the next time you open Konos."), {}, {}, 10000);
        else
            m_updateToast->showAt(Toast::Bottom, error, {}, {}, 10000);
    });
    if (qEnvironmentVariableIsEmpty("KONOS_SCRIPT")) // not during scripted test runs
        QTimer::singleShot(5000, this, [] { UpdateCheck::instance().checkAutomatically(); });
    m_timeline = new SubtitleTimeline(m_player, m_videoArea);
    m_timeline->hide();
    m_infoCard = new MediaInfoCard(m_player, m_videoArea);
    m_infoCard->hide();
    m_videoArea->installEventFilter(this);

    m_content = new QStackedWidget(body);
    m_content->setObjectName("Content");
    m_content->addWidget(m_library);
    m_content->addWidget(m_videoArea);

    m_panel = new PlayerPanel(m_player, body);
    m_panel->setFixedWidth(392);

    m_mini = new MiniPlayer(m_player, m_library);
    connect(m_mini, &MiniPlayer::expandRequested, this, &MainWindow::showPlayer);
    connect(m_mini, &MiniPlayer::closeRequested, this, [this] {
        m_miniDismissed = true;
        updateMiniPlayer();
    });
    m_libraryQueue = prefs().flag("ui/libraryQueue");

    bodyLay->addWidget(m_rail);
    bodyLay->addWidget(m_navHolder);
    bodyLay->addWidget(m_side);
    bodyLay->addWidget(m_content, 1);
    bodyLay->addWidget(m_panel);
    m_rootLayout->addWidget(body, 1);

    m_controls = new ControlBar(m_player, root);
    m_rootLayout->addWidget(m_controls);

    m_quick = new QuickSettings(m_player, m_video, root);
    m_settings = new SettingsOverlay(m_player, root);
    m_subtitles = new SubtitleOverlay(m_player, root);
    m_subtitles->setQueueProvider([this] { return m_queue; });

    // --- wiring ---
    connect(m_title, &TitleBar::menuRequested, this, [this](const QPoint &pos) {
        QMenu *menu = buildMainMenu();
        menu->exec(pos);
        menu->deleteLater();
    });
    connect(m_title, &TitleBar::sidebarToggled, this, &MainWindow::toggleSidebar);
    connect(m_title, &TitleBar::locationClicked, this, [this] { showLibrary(m_side->section()); });
    connect(m_title, &TitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(m_title, &TitleBar::maximizeRequested, this, [this] { isMaximized() ? showNormal() : showMaximized(); });
    connect(m_title, &TitleBar::fullscreenRequested, this, [this] { setFullscreen(!m_fullscreen); });
    connect(m_title, &TitleBar::pipRequested, this, [this] { setPip(!m_pip); });
    connect(m_title, &TitleBar::closeRequested, this, &QWidget::close);

    connect(m_nav, &NavBar::sectionClicked, this, &MainWindow::showLibrary);
    connect(m_nav, &NavBar::pinClicked, this, [this](const Library::Pin &pin) {
        m_library->openPin(pin);
        if (pin.kind != "file")
            setView(View::Library);
    });
    connect(m_rail, &NavBar::pinClicked, this, [this](const Library::Pin &pin) {
        if (pin.kind == "file") {
            m_side->openPin(pin);
            return;
        }
        openSidePanel(Section::Home); // opens the panel; the pin then picks what it shows
        m_side->openPin(pin);
    });
    connect(m_rail, &NavBar::sectionClicked, this, &MainWindow::openSidePanel);
    auto addMenu = [this](const QPoint &pos) {
        QMenu menu(this);
        const QColor a = theme().accent();
        menu.addAction(theme().icon("open-media", a), tr("Open file…"), this, &MainWindow::openFiles);
        menu.addAction(theme().icon("folder", a), tr("Open folder…"), this, &MainWindow::openFolder);
        menu.addAction(theme().icon("globe", a), tr("Open network stream…"), this, &MainWindow::openNetwork);
        addDiscEntries(&menu);
        menu.addSeparator();
        menu.addAction(theme().icon("plus", a), tr("Add library folder…"), this, [this] {
            const QString dir = FileDialogs::existingDirectory(this, tr("Add library folder"));
            if (dir.isEmpty())
                return;
            const bool music = QMessageBox::question(this, tr("Add library folder"),
                                                     tr("Is “%1” a music folder?").arg(QFileInfo(dir).fileName()),
                                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
            const QString key = music ? "library/musicFolders" : "library/videoFolders";
            QStringList folders = prefs().list(key);
            if (!folders.contains(dir))
                prefs().set(key, folders << dir);
        });
        menu.exec(pos);
    };
    connect(m_nav, &NavBar::addClicked, this, addMenu);
    connect(m_rail, &NavBar::addClicked, this, addMenu);

    for (LibraryView *lv : {m_library, m_side, m_panel->queue()}) {
        connect(lv, &LibraryView::playRequested, this, &MainWindow::playItems);
        connect(lv, &LibraryView::enqueueRequested, this, &MainWindow::enqueue);
        connect(lv, &LibraryView::enqueueAndPlayRequested, this, [this](const QStringList &items) {
            const QStringList expanded = library().expand(items);
            if (expanded.isEmpty())
                return;
            const int at = int(m_queue.size());
            m_queue << expanded;
            m_shuffleHistory.clear();
            playIndex(at);
            showPlayer();
            updateQueueViews();
        });
        connect(lv, &LibraryView::playNextRequested, this, [this, lv](const QStringList &items) {
            emit lv->queueInsertRequested(items, m_index + 1); // at the end when nothing follows
        });
        connect(lv, &LibraryView::queueJumpRequested, this, &MainWindow::playIndex);
        connect(lv, &LibraryView::queueRemoveRequested, this, [this](int i) {
            if (i < 0 || i >= m_queue.size())
                return;
            m_queue.removeAt(i);
            if (i < m_index)
                --m_index;
            else if (i == m_index) {
                if (m_queue.isEmpty()) {
                    m_index = -1;
                    m_player->stop();
                } else {
                    playIndex(qMin(i, int(m_queue.size()) - 1));
                    return;
                }
            }
            updateQueueViews();
        });
        connect(lv, &LibraryView::queueClearRequested, this, [this] {
            savePosition();
            m_queue.clear();
            m_index = -1;
            m_player->stop();
            updateQueueViews();
        });
        connect(lv, &LibraryView::openNetworkRequested, this, &MainWindow::openNetwork);
        connect(lv, &LibraryView::infoRequested, this,
                [this](const QString &location) { (new MediaInfoOverlay(location, this))->open(); });
        connect(lv, &LibraryView::addFilesRequested, this, [this] {
            const QStringList files = FileDialogs::openFiles(
                this, tr("Add to queue"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
                FileDialogs::mediaFilter(true));
            if (!files.isEmpty())
                enqueue(files);
        });
        connect(lv, &LibraryView::queueMoveRequested, this, [this](QList<int> rows, int at) {
            // Rebuild the order: the moved rows go, as a block, where position `at` was.
            const int n = int(m_queue.size());
            rows.erase(std::remove_if(rows.begin(), rows.end(), [n](int r) { return r < 0 || r >= n; }), rows.end());
            std::sort(rows.begin(), rows.end());
            rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
            if (rows.isEmpty())
                return;
            QList<int> order;
            for (int i = 0; i <= n; ++i) {
                if (i == qBound(0, at, n))
                    order << rows;
                if (i < n && !rows.contains(i))
                    order << i;
            }
            QStringList moved;
            for (int i : order)
                moved << m_queue.at(i);
            if (moved == m_queue)
                return;
            m_queue = moved;
            m_index = int(order.indexOf(m_index));
            m_shuffleHistory.clear();
            updateQueueViews();
        });
        connect(lv, &LibraryView::queueRemoveRowsRequested, this, [this](QList<int> rows) {
            std::sort(rows.begin(), rows.end(), std::greater<int>());
            rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
            bool removedCurrent = false;
            for (int i : rows) {
                if (i < 0 || i >= m_queue.size())
                    continue;
                m_queue.removeAt(i);
                if (i < m_index)
                    --m_index;
                else if (i == m_index)
                    removedCurrent = true;
            }
            m_shuffleHistory.clear();
            if (removedCurrent) {
                // Carry on with whatever now sits where the playing item was.
                const int next = qMin(m_index, int(m_queue.size()) - 1);
                if (next < 0) {
                    m_index = -1;
                    m_player->stop();
                } else {
                    playIndex(next);
                    updateQueueViews();
                    return;
                }
            }
            updateQueueViews();
        });
        connect(lv, &LibraryView::queueInsertRequested, this, [this](const QStringList &items, int at) {
            if (m_queue.isEmpty() || at < 0 || at >= m_queue.size()) {
                enqueue(items);
                return;
            }
            const QStringList expanded = library().expand(items);
            if (expanded.isEmpty())
                return;
            for (int i = 0; i < expanded.size(); ++i)
                m_queue.insert(at + i, expanded.at(i));
            if (at <= m_index)
                m_index += int(expanded.size());
            m_shuffleHistory.clear();
            m_toast->show(expanded.size() == 1 ? tr("Added 1 item to the queue")
                                               : tr("Added %1 items to the queue").arg(expanded.size()), {}, {}, 1800);
            updateQueueViews();
        });
        connect(lv, &LibraryView::addFolderRequested, this, [this, lv] {
            const QString dir = FileDialogs::existingDirectory(this, tr("Add library folder"));
            if (dir.isEmpty())
                return;
            const QString key = lv->section() == Section::Music ? "library/musicFolders" : "library/videoFolders";
            QStringList folders = prefs().list(key);
            if (!folders.contains(dir))
                prefs().set(key, folders << dir);
        });
    }
    connect(m_side, &LibraryView::popOutRequested, this, [this] {
        showLibrary(m_side->section());
    });
    connect(m_side, &LibraryView::sectionShown, this, [this](Section s) {
        m_rail->setCurrent(s, m_sidePanel);
        m_title->setLocation(s);
    });
    connect(m_library, &LibraryView::sectionShown, this, [this](Section s) { m_nav->setCurrent(s); });

    connect(m_controls, &ControlBar::previousClicked, this, &MainWindow::previous);
    connect(m_controls, &ControlBar::nextClicked, this, [this] { next(true); });
    connect(m_controls, &ControlBar::audioClicked, this,
            [this] { m_quick->openAt(m_controls->audioButton(), QuickSettings::AudioTab); });
    connect(m_controls, &ControlBar::subtitlesClicked, this,
            [this] { m_quick->openAt(m_controls->subtitleButton(), QuickSettings::SubtitlesTab); });
    connect(m_controls, &ControlBar::settingsClicked, this,
            [this] { m_quick->openAt(m_controls->settingsButton(), QuickSettings::PlaybackTab); });
    connect(m_controls, &ControlBar::actionRequested, this, &MainWindow::controlAction);
    auto updateFavorite = [this] {
        m_controls->setFavorite(!m_current.isEmpty() && library().isPinned("file", m_current));
    };
    connect(m_player, &Player::mediaChanged, this, updateFavorite);
    connect(&library(), &Library::pinsChanged, this, updateFavorite);
    connect(m_controls, &ControlBar::queueClicked, this, [this] {
        if (m_view != View::Player) { // the queue beside the library
            m_libraryQueue = !(m_libraryQueue && m_panel->tab() == PlayerPanel::QueueTab);
            m_panel->setTab(PlayerPanel::QueueTab);
            prefs().set("ui/libraryQueue", m_libraryQueue);
            updateLayout();
            return;
        }
        openPlayerPanel(PlayerPanel::QueueTab);
    });
    connect(m_panel, &PlayerPanel::tabChanged, this, [this](PlayerPanel::Tab tab) {
        prefs().set("ui/rightTab", int(tab));
        updateLayout();
    });

    connect(m_quick, &QuickSettings::advancedSettingsRequested, this, [this](int tab) {
        static const SettingsOverlay::Page pages[] = {SettingsOverlay::Playback, SettingsOverlay::Video,
                                                      SettingsOverlay::Audio, SettingsOverlay::Subtitles};
        m_settings->open(pages[qBound(0, tab, 3)]);
    });
    connect(m_quick, &QuickSettings::snapshotRequested, this, &MainWindow::takeSnapshot);
    connect(m_quick, &QuickSettings::sleepTimerRequested, this, &MainWindow::setSleepTimer);
    connect(m_quick, &QuickSettings::loadSubtitleRequested, this, &MainWindow::openSubtitle);
    connect(m_quick, &QuickSettings::findSubtitlesRequested, this, [this] { m_subtitles->open(); });
    connect(m_subtitles, &SubtitleOverlay::settingsRequested, this,
            [this] { m_settings->open(SettingsOverlay::Subtitles); });
    connect(m_subtitles, &SubtitleOverlay::message, this, [this](const QString &text) { m_toast->show(text, {}, {}, 1600); });
    connect(m_settings, &SettingsOverlay::openUrlRequested, this, [this](const QString &url) {
        library().addStream(url);
        playItems({url}, 0);
    });
    connect(m_settings, &SettingsOverlay::recordToggled, this, &MainWindow::toggleRecording);
    connect(m_settings, &SettingsOverlay::pluginsRequested, this, [this] {
        m_settings->close();
        (new PluginsOverlay(this))->open();
    });
    connect(m_settings, &SettingsOverlay::shortcutsRequested, this, [this] {
        m_settings->close();
        showShortcuts();
    });

    connect(m_video, &VideoWidget::doubleClicked, this, [this] {
        if (m_view != View::Player)
            return;
        switch (prefs().integer("controls/doubleClick")) {
        case 0:
            m_pip ? setPip(false) : setFullscreen(!m_fullscreen);
            break;
        case 1:
            m_player->togglePause();
            break;
        default:
            break;
        }
    });
    connect(m_video, &VideoWidget::clicked, this, [this](const QPoint &pos) {
        if (m_view != View::Player) { // the mini player
            showPlayer();
            return;
        }
        if (m_player->inDiscMenu()) {
            if (const auto p = m_video->framePoint(pos))
                m_player->pointerOnVideo(*p, true);
            return;
        }
        if (prefs().flag("ui/clickPause"))
            m_player->togglePause();
    });
    connect(m_video, &VideoWidget::contextMenuRequested, this, [this](const QPoint &pos) {
        QMenu *menu = buildContextMenu();
        menu->exec(pos);
        menu->deleteLater();
    });
    connect(m_video, &VideoWidget::mouseMoved, this, [this](const QPoint &pos) {
        if (m_view != View::Player)
            return;
        overlayHover(m_video->mapTo(m_videoArea, pos));
        if (m_player->inDiscMenu())
            if (const auto p = m_video->framePoint(pos))
                m_player->pointerOnVideo(*p, false);
    });
    connect(m_video, &VideoWidget::wheelScrolled, this, [this](int delta) {
        const int mode = prefs().integer("controls/wheel");
        const int dir = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
        if (mode == 0)
            m_player->setVolume(m_player->volume() + dir * prefs().integer("controls/volumeStep"));
        else if (mode == 1)
            m_player->seekRelative(dir * prefs().integer("controls/seekStep") * 1000);
    });
    connect(m_video, &VideoWidget::wheelScrolledSideways, this, [this](int delta) {
        const int mode = prefs().integer("controls/hwheel");
        const int dir = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
        if (mode == 0)
            m_player->seekRelative(dir * prefs().integer("controls/seekStep") * 1000);
        else if (mode == 1)
            m_player->setVolume(m_player->volume() + dir * prefs().integer("controls/volumeStep"));
    });
}

void MainWindow::createActions()
{
    // Every shortcut has an id in Shortcuts, which supplies the keys (the
    // user's choice, or the default) and keeps them up to date.
    auto make = [this](const QString &icon, const QString &text, const QString &id, auto slot) {
        auto *a = new QAction(theme().icon(icon, theme().accent()), text, this);
        a->setProperty("iconName", icon);
        Shortcuts::instance().bind(a, id);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };
    m_actPlay = make("play", tr("Play"), "play", [this] { m_queue.isEmpty() && !m_player->hasMedia() ? openFiles() : m_player->togglePause(); });
    m_actStop = make("stop", tr("Stop"), "stop", [this] {
        savePosition();
        m_player->stop();
    });
    m_actPrev = make("prev", tr("Previous Item"), "prev", [this] { previous(); });
    m_actNext = make("next", tr("Next Item"), "next", [this] { next(true); });
    m_actRecord = make("record", tr("Record"), "record", [this] { toggleRecording(); });
    m_actHideUi = make("hide-ui", tr("Hide UI"), "hideUi", [this] { setHideUi(!m_hideUi); });
    m_actQuit = make("quit", tr("Quit Konos"), "quit", [this] { close(); });
    m_actFullscreen = make("fullscreen", tr("Fullscreen"), "fullscreen", [this] { setFullscreen(!m_fullscreen); });
    m_actSnapshot = make("camera", tr("Take Snapshot"), "snapshot", [this] { takeSnapshot(); });
    m_actMute = make("mute", tr("Mute"), "mute", [this] {
        m_player->setMuted(!m_player->isMuted());
        m_toast->show(m_player->isMuted() ? tr("Muted") : tr("Sound on"), {}, {}, 1200);
    });
    m_actOpen = make("open-media", tr("Open File…"), "open", [this] { openFile(); });
    m_actOpenMany = make("files", tr("Open Multiple Files…"), "openMany", [this] { openFiles(); });
    m_actOpenDir = make("folder", tr("Open Directory…"), "openDir", [this] { openFolder(); });
    m_actOpenNet = make("stream", tr("Open Network Stream…"), "network", [this] { openNetwork(); });
    m_actDisc = make("disc", tr("Open Disc"), "disc", [this] { openDisc(); });
    m_actCapture = make("capture", tr("Open Capture Device…"), "capture", [this] { openCapture(); });
    m_actClipboard = make("link", tr("Open Location from Clipboard"), "clipboard", [this] { openClipboard(); });
    m_actSavePlaylist = make("save-down", tr("Save Playlist to File…"), "savePlaylist", [this] { savePlaylistFile(); });
    m_actConvert = make("convert", tr("Convert / Save…"), "convert", [this] { openConvert({}); });
    m_actStream = make("upload", tr("Stream…"), "stream", [this] { openStream(); });
    m_actShortcuts = make("keyboard", tr("Keyboard Shortcuts"), "shortcuts", [this] { showShortcuts(); });
    m_actAbout = make("info", tr("About Konos"), "about", [this] { (new AboutOverlay(this))->open(); });
    m_actSettings = make("settings", tr("Settings"), "settings", [this] { m_settings->open(); });
    m_actSidebar = make("sidebar", tr("Toggle Sidebar"), "sidebar", [this] {
        if (m_view == View::Player)
            toggleSidebar();
    });
    m_actLibrary = make("videos", tr("Library / Player"), "library", [this] {
        if (m_view == View::Library)
            showPlayer();
        else
            showLibrary(m_library->section());
    });

    // Shortcuts without a menu entry.
    auto key = [this](const QString &id, auto slot) {
        auto *a = new QAction(this);
        Shortcuts::instance().bind(a, id);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
    };
    key("bookmark", [this] {
        if (!m_player->hasMedia())
            return;
        m_panel->addBookmark();
        m_toast->show(tr("Bookmark added"), {}, {}, 1500);
    });
    key("escape", [this] {
        if (m_quick->isVisible())
            m_quick->hide();
        else if (m_fullscreen)
            setFullscreen(false);
        else if (m_pip)
            setPip(false);
        else if (m_hideUi)
            setHideUi(false);
    });
    // In a DVD menu the arrow keys and Enter move between the disc's buttons.
    // Seeking says how far it went (and, via the key hint, which key did it).
    auto seekBy = [this](int seconds) {
        if (!m_player->hasMedia())
            return;
        m_player->seekRelative(qint64(seconds) * 1000);
        const QString amount = qAbs(seconds) >= 60 && qAbs(seconds) % 60 == 0 ? tr("%1 min").arg(qAbs(seconds) / 60)
                                                                               : tr("%1 s").arg(qAbs(seconds));
        m_toast->show(seconds > 0 ? tr("Forward %1").arg(amount) : tr("Back %1").arg(amount), {}, {}, 900);
    };
    key("seekFwd", [this, seekBy] {
        m_player->inDiscMenu() ? m_player->navigate(Player::Nav::Right) : seekBy(prefs().integer("controls/seekStep"));
    });
    key("seekBack", [this, seekBy] {
        m_player->inDiscMenu() ? m_player->navigate(Player::Nav::Left) : seekBy(-prefs().integer("controls/seekStep"));
    });
    key("jumpFwd", [seekBy] { seekBy(60); });
    key("jumpBack", [seekBy] { seekBy(-60); });
    // VLC's keys for cycling the aspect ratio, subtitle track and audio track.
    key("aspect", [this] { cycleAspect(); });
    auto cycleTrack = [this](bool subtitles) {
        const QList<Player::Track> tracks = subtitles ? m_player->subtitleTracks() : m_player->audioTracks();
        if (tracks.size() < 2) {
            m_toast->show(subtitles ? tr("No subtitles") : tr("No other audio tracks"), {}, {}, 1400);
            return;
        }
        const int current = subtitles ? m_player->subtitleTrack() : m_player->audioTrack();
        int at = 0;
        for (int i = 0; i < tracks.size(); ++i)
            if (tracks[i].id == current)
                at = i;
        const Player::Track next = tracks[(at + 1) % tracks.size()];
        subtitles ? m_player->setSubtitleTrack(next.id) : m_player->setAudioTrack(next.id);
        const QString name = next.id < 0 ? tr("Off") : next.name;
        m_toast->show(subtitles ? tr("Subtitles: %1").arg(name) : tr("Audio: %1").arg(name), {}, {}, 1600);
    };
    key("subTrack", [cycleTrack] { cycleTrack(true); });
    key("audioTrack", [cycleTrack] { cycleTrack(false); });
    key("volUp", [this] {
        m_player->inDiscMenu() ? m_player->navigate(Player::Nav::Up)
                               : m_player->setVolume(m_player->volume() + prefs().integer("controls/volumeStep"));
    });
    key("volDown", [this] {
        m_player->inDiscMenu() ? m_player->navigate(Player::Nav::Down)
                               : m_player->setVolume(m_player->volume() - prefs().integer("controls/volumeStep"));
    });
    // Only enabled while a disc menu is showing, so Enter still reaches text fields otherwise.
    m_actMenuEnter = new QAction(this);
    m_actMenuEnter->setShortcuts({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    m_actMenuEnter->setShortcutContext(Qt::WindowShortcut);
    m_actMenuEnter->setEnabled(false);
    connect(m_actMenuEnter, &QAction::triggered, this, [this] { m_player->navigate(Player::Nav::Activate); });
    addAction(m_actMenuEnter);
    key("faster", [this] { m_player->setRate(m_player->rate() + 0.1f); m_toast->show(tr("Speed %1×").arg(m_player->rate(), 0, 'f', 2), {}, {}, 1200); });
    key("slower", [this] { m_player->setRate(m_player->rate() - 0.1f); m_toast->show(tr("Speed %1×").arg(m_player->rate(), 0, 'f', 2), {}, {}, 1200); });
    key("normalSpeed", [this] { m_player->setRate(1.0f); m_toast->show(tr("Normal speed"), {}, {}, 1200); });
}

void MainWindow::applyTheme()
{
    qApp->setPalette(theme().palette());
    qApp->setStyleSheet(theme().styleSheet());
    for (QAction *a : actions()) {
        const QString icon = a->property("iconName").toString();
        if (!icon.isEmpty())
            a->setIcon(theme().icon(icon, theme().accent()));
    }
}

void MainWindow::connectPlayer()
{
    connect(m_player, &Player::stateChanged, this, [this](Player::State s) {
        m_actPlay->setText(m_player->isPlaying() ? tr("Pause") : tr("Play"));
        m_actPlay->setProperty("iconName", m_player->isPlaying() ? "pause" : "play");
        m_actPlay->setIcon(theme().icon(m_player->isPlaying() ? "pause" : "play", theme().accent()));
        if (s == Player::Paused || s == Player::Stopped)
            savePosition();
        updateScreensaverInhibit();
        updateLayout();
    });
    connect(m_player, &Player::endReached, this, &MainWindow::onEndReached);
    connect(m_player, &Player::mediaChanged, this, [this] {
        m_miniDismissed = false; // the mini player comes back for the next item
        updateMiniPlayer();
    });
    connect(m_player, &Player::discMenuChanged, m_actMenuEnter, &QAction::setEnabled);
    connect(m_player, &Player::errorOccurred, this, [this](const QString &msg) {
        showError(msg);
        if (m_index >= 0 && m_index + 1 < m_queue.size() && prefs().flag("playback/autoNext"))
            QTimer::singleShot(400, this, [this] { next(false); });
    });
    connect(m_player, &Player::metaChanged, this, &MainWindow::updateTitle);
    connect(m_player, &Player::metaChanged, this, [this] {
        // Remembered for the home page's "Listen Again" (title, artist, cover).
        if (!m_current.isEmpty() && !Disc::isDiscLocation(m_current) && m_player->location() == m_current)
            library().setMeta(m_current, {m_player->title(), m_player->artist(), m_player->artworkPath()});
    });
    connect(m_player, &Player::mediaChanged, this, &MainWindow::updateTitle);
    connect(m_player, &Player::videoSizeChanged, this, [this] { updateScreensaverInhibit(); });
    connect(m_player, &Player::recordingChanged, this, [this](bool on) {
        m_title->setRecording(on);
        m_actRecord->setText(on ? tr("Stop Recording") : tr("Record"));
        if (on)
            m_toast->show(tr("Recording started"));
        else if (!m_player->recordingPath().isEmpty())
            m_toast->show(tr("Recording saved to %1").arg(QFileInfo(m_player->recordingPath()).fileName()), tr("Show"),
                          [this] {
                              QDesktopServices::openUrl(
                                  QUrl::fromLocalFile(QFileInfo(m_player->recordingPath()).absolutePath()));
                          });
    });
    connect(m_player, &Player::volumeChanged, this, [this](int v) {
        if (m_view == View::Player && !m_controls->isVisible())
            m_toast->show(tr("Volume %1%").arg(v), {}, {}, 1000);
    });
}

// ---------------------------------------------------------------------------
// Views & layout

void MainWindow::showLibrary(Section section)
{
    m_library->showSection(section);
    setView(View::Library);
}

void MainWindow::showPlayer()
{
    setView(View::Player);
}

void MainWindow::setView(View view)
{
    m_view = view;
    if (m_fullscreen && view == View::Library)
        setFullscreen(false);
    updateLayout();
    updateTitle();
}

void MainWindow::updateLayout()
{
    const bool player = m_view == View::Player;
    const bool overlay = m_fullscreen || m_pip; // bars float over the video and show on hover
    const bool chromeHidden = overlay || m_hideUi;
    if (!overlay)
        m_title->setVisible(!m_hideUi);
    m_title->setMode(m_pip    ? TitleBar::PipMode
                     : player ? (m_sidebar || m_fullscreen ? TitleBar::PlayerMode : TitleBar::MinimalMode)
                              : TitleBar::LibraryMode);
    m_title->setSidebarActive(m_sidebar);
    m_title->setLocation(m_side->section());
    m_navHolder->setVisible(!player && !chromeHidden);
    m_rail->setVisible(player && m_sidebar && !chromeHidden);
    m_side->setVisible(player && m_sidebar && m_sidePanel && !chromeHidden);
    m_rail->setCurrent(m_side->section(), m_sidePanel);
    m_content->setCurrentWidget(player ? m_videoArea : static_cast<QWidget *>(m_library));
    m_panel->setVisible((player ? m_panelOpen : m_libraryQueue) && !chromeHidden);
    m_controls->setQueueActive((player ? m_panelOpen : m_libraryQueue) && m_panel->tab() == PlayerPanel::QueueTab);

    if (m_fullscreen) {
        placeOverlayControls();
    } else if (m_pip) {
        m_controls->hide();
        placeOverlayControls();
    } else {
        const bool showControls = !m_hideUi && (player || m_player->hasMedia());
        m_controls->setVisible(showControls);
    }
    prefs().set("ui/sidePanel", m_sidebar);
    prefs().set("ui/rightPanel", m_panelOpen);
    updateMiniPlayer();
    placeVideoOverlays();
}

// While browsing the library with something playing, the video moves into
// the mini player; back in the player it returns.
void MainWindow::updateMiniPlayer()
{
    if (!m_mini)
        return;
    const bool show = m_view == View::Library && m_player->hasMedia() && prefs().flag("ui/miniPlayer") &&
                      !m_miniDismissed && !m_hideUi;
    auto *videoLay = static_cast<QVBoxLayout *>(m_videoArea->layout());
    if (show && m_video->parentWidget() != m_mini->videoHost()->parentWidget()) {
        videoLay->removeWidget(m_video);
        m_mini->videoHost()->addWidget(m_video);
        m_video->show();
    } else if (!show && m_video->parentWidget() != m_videoArea) {
        m_mini->videoHost()->removeWidget(m_video);
        videoLay->insertWidget(0, m_video);
        m_video->show();
    }
    m_mini->setVisible(show);
    if (show)
        m_mini->place();
}

// "Use the subtitles' own styling" off: VLC 3 can only show SSA/ASS tracks
// with their styling (libass), so a styled track is swapped for a plain SRT
// copy made from its text, which then takes the look from the settings.
void MainWindow::usePlainSubtitles()
{
    if (fx().data().subEmbeddedStyle || !m_player->hasMedia())
        return;
    const int current = m_player->subtitleTrack();
    if (current < 0)
        return;
    bool styled = false;
    for (const Player::TrackInfo &t : m_player->trackInfo())
        if (t.id == current) {
            char cc[5] = {};
            memcpy(cc, &t.fourcc, 4);
            styled = qstrcmp(cc, "ssa ") == 0;
        }
    if (!styled)
        return;
    const QString done = m_player->location() + "#" + QString::number(current);
    if (m_plainDone.contains(done))
        return;
    QString key;
    int embeddedIndex = 0;
    for (const Player::SubtitleTrack &t : m_player->subtitleTrackList()) {
        if (t.id == current) {
            key = t.external ? t.file : SubtitleCues::embeddedKey(m_player->location(), embeddedIndex);
            break;
        }
        if (!t.external)
            ++embeddedIndex;
    }
    if (key.isEmpty())
        return;
    const auto state = SubtitleCues::instance().state(key);
    if (state == SubtitleCues::Loading)
        return; // ready() brings us back
    m_plainDone.insert(done);
    const QString plain = SubtitleCues::instance().plainFile(key);
    if (!plain.isEmpty())
        m_player->addSubtitleFile(plain); // and selects it
}

void MainWindow::placeVideoOverlays()
{
    const bool playing = m_view == View::Player && m_player->hasMedia() && !m_pip;
    const bool controlsShown = !m_fullscreen || m_controls->isVisible();
    const int W = m_videoArea->width(), H = m_videoArea->height();

    const bool timeline = playing && controlsShown && prefs().flag("playback/subTimeline");
    if (timeline) {
        const int w = qMin(W - 32, 1200);
        const int bottom = m_fullscreen ? H - m_controls->height() - 10 : H - 12;
        m_timeline->setGeometry((W - w) / 2, bottom - m_timeline->height(), w, m_timeline->height());
        m_timeline->raise();
    }
    m_timeline->setVisible(timeline);
    m_toast->setBottomMargin((m_fullscreen ? m_controls->height() + 20 : 28) + (timeline ? m_timeline->height() + 10 : 0));

    const bool card = playing && prefs().flag("playback/metadata") &&
                      (m_infoTimer.isActive() || m_player->state() == Player::Paused);
    if (card) {
        if (!m_infoCard->isVisible())
            m_infoCard->refresh();
        const bool titleOver = m_title->parentWidget() == m_videoArea && m_title->isVisible();
        m_infoCard->move(16, titleOver ? m_title->height() + 12 : 16);
        m_infoCard->raise();
    }
    m_infoCard->setVisible(card);
}

void MainWindow::toggleSidebar()
{
    m_sidebar = !m_sidebar;
    if (m_sidebar)
        m_sidePanel = true;
    updateLayout();
}

void MainWindow::openPlayerPanel(int tab)
{
    const auto t = PlayerPanel::Tab(tab);
    if (m_view == View::Player && m_panelOpen && m_panel->tab() == t && m_panel->isVisible()) {
        m_panelOpen = false;
        updateLayout();
        return;
    }
    m_panelOpen = true;
    m_panel->setTab(t);
    if (m_view != View::Player)
        setView(View::Player);
    else
        updateLayout();
}

void MainWindow::openSidePanel(Section section)
{
    if (section == Section::Queue) { // the queue lives in the panel on the right
        openPlayerPanel(PlayerPanel::QueueTab);
        return;
    }
    if (m_side->section() == section && m_sidePanel && m_side->isVisible()) {
        m_sidePanel = false; // clicking the current section collapses the panel
    } else {
        m_sidePanel = true;
        m_side->showSection(section);
    }
    m_sidebar = true;
    if (m_view != View::Player)
        setView(View::Player);
    else
        updateLayout();
}

void MainWindow::setFullscreen(bool on)
{
    if (on == m_fullscreen)
        return;
    m_fullscreen = on;
    if (on) {
        m_wasMaximized = isMaximized();
        if (m_view != View::Player)
            m_view = View::Player;
        if (m_pip)
            setPip(false);
        // Title and controls float over the video in fullscreen.
        m_rootLayout->removeWidget(m_controls);
        m_controls->setParent(m_videoArea);
        m_controls->setOverlay(true);
        setTitleOverlay(true);
        showFullScreen();
    } else {
        setTitleOverlay(false);
        m_controls->setParent(centralWidget());
        m_controls->setOverlay(false);
        m_rootLayout->addWidget(m_controls);
        m_controls->show();
        m_video->unsetCursor();
        if (m_wasMaximized)
            showMaximized();
        else
            showNormal();
    }
    m_actFullscreen->setText(on ? tr("Exit Fullscreen") : tr("Fullscreen"));
    updateLayout();
    if (on)
        showControlsTemporarily();
}

void MainWindow::placeOverlayControls()
{
    if (!(m_fullscreen || m_pip))
        return;
    if (m_title->parentWidget() == m_videoArea) {
        m_title->setGeometry(0, 0, m_videoArea->width(), m_title->height());
        m_title->raise();
    }
    if (!m_fullscreen)
        return;
    m_controls->setGeometry(0, m_videoArea->height() - m_controls->height(), m_videoArea->width(), m_controls->height());
    m_controls->raise();
}

// Shows both bars for a moment (entering fullscreen, keyboard use).
void MainWindow::showControlsTemporarily()
{
    if (!(m_fullscreen || m_pip))
        return;
    m_video->unsetCursor();
    placeOverlayControls();
    m_title->show();
    if (m_fullscreen)
        m_controls->show();
    const int ms = prefs().integer("ui/autoHideMs");
    if (ms > 0)
        m_hideTimer.start(ms);
}

// The video's idea: the top and bottom bars appear when the pointer comes
// near them and go away again when it moves off.
void MainWindow::overlayHover(const QPoint &pos)
{
    if (!(m_fullscreen || m_pip))
        return;
    m_video->unsetCursor();
    placeOverlayControls();
    const bool nearTop = pos.y() < m_title->height() + (m_pip ? 40 : 90);
    const bool nearBottom = m_fullscreen && pos.y() > m_videoArea->height() - m_controls->height() - 110;
    m_nearTop = nearTop;
    m_nearBottom = nearBottom;
    if (nearTop)
        m_title->show();
    if (nearBottom)
        m_controls->show();
    // A bar the pointer has left goes shortly after (not restarted by every
    // move, so it goes even while the pointer keeps moving).
    const bool leaving = (m_title->isVisible() && !nearTop) || (m_fullscreen && m_controls->isVisible() && !nearBottom);
    if (!leaving)
        m_awayTimer.stop();
    else if (!m_awayTimer.isActive())
        m_awayTimer.start();
    const int ms = prefs().integer("ui/autoHideMs");
    if (ms > 0)
        m_hideTimer.start(ms);
}

void MainWindow::setTitleOverlay(bool on)
{
    if (on == (m_title->parentWidget() == m_videoArea))
        return;
    if (on) {
        m_rootLayout->removeWidget(m_title);
        m_title->setParent(m_videoArea);
        m_title->hide();
    } else {
        m_title->setParent(centralWidget());
        m_rootLayout->insertWidget(0, m_title);
        m_title->show();
    }
    m_title->setOverlay(on && m_fullscreen);
}

// Picture in picture: a small window with just the video. On Hyprland it is
// also floated, pinned to every workspace and put in the bottom-right corner.
void MainWindow::setPip(bool on)
{
    if (on == m_pip)
        return;
    if (on) {
        if (m_fullscreen)
            setFullscreen(false);
        if (m_view != View::Player)
            setView(View::Player);
        m_pipRestore = saveGeometry();
        m_pipWasMaximized = isMaximized();
        m_pip = true;
        setTitleOverlay(true);
        if (isMaximized())
            showNormal();
        setMinimumSize(240, 135);
        const QSize vs = m_player->videoSize();
        const double aspect = vs.isEmpty() ? 16.0 / 9.0 : double(vs.width()) / vs.height();
        const QSize size(480, int(480 / qBound(0.5, aspect, 3.0)));
        updateLayout();
        resize(size);
        hyprlandPip(true, size);
        m_toast->show(tr("Double-click or press Esc to go back"), {}, {}, 2200);
    } else {
        m_pip = false;
        hyprlandPip(false, {});
        setTitleOverlay(false);
        updateLayout();
        setMinimumSize(760, 480);
        restoreGeometry(m_pipRestore);
        if (m_pipWasMaximized)
            showMaximized();
        m_video->unsetCursor();
    }
    updateTitle();
}

void MainWindow::hyprlandPip(bool on, QSize size)
{
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return;
    auto hyprctl = [](const QStringList &args) {
        QProcess p;
        p.start("hyprctl", args);
        p.waitForFinished(1500);
        return p.readAllStandardOutput();
    };
    // Find this window by our process id.
    QJsonObject me;
    for (const QJsonValue &v : QJsonDocument::fromJson(hyprctl({"-j", "clients"})).array())
        if (v.toObject().value("pid").toInteger() == QCoreApplication::applicationPid())
            me = v.toObject();
    const QString address = me.value("address").toString();
    if (address.isEmpty())
        return;
    const QString win = "address:" + address;
    // Hyprland 0.55+ takes Lua dispatchers; older versions the classic ones.
    QList<QPair<QString, QString>> cmds; // (Lua, classic)
    auto lua = [&win](const QString &fn, const QString &args) {
        return QString("hl.dsp.window.%1({ %2window = \"%3\" })").arg(fn, args.isEmpty() ? QString() : args + ", ", win);
    };
    if (on) {
        m_pipWasFloating = me.value("floating").toBool();
        // Bottom-right of its monitor, clear of bars (reserved space).
        QJsonObject mon;
        for (const QJsonValue &v : QJsonDocument::fromJson(hyprctl({"-j", "monitors"})).array())
            if (v.toObject().value("id").toInt() == me.value("monitor").toInt())
                mon = v.toObject();
        const double scale = qMax(0.1, mon.value("scale").toDouble(1.0));
        const QJsonArray reserved = mon.value("reserved").toArray();
        const int right = mon.value("x").toInt() + int(mon.value("width").toInt() / scale) - reserved.at(2).toInt();
        const int bottom = mon.value("y").toInt() + int(mon.value("height").toInt() / scale) - reserved.at(3).toInt();
        const int x = right - size.width() - 24, y = bottom - size.height() - 24;
        if (!m_pipWasFloating)
            cmds.append({lua("float", "action = \"enable\""), "setfloating " + win});
        cmds.append({lua("resize", QString("x = %1, y = %2").arg(size.width()).arg(size.height())),
                     QString("resizewindowpixel exact %1 %2,%3").arg(size.width()).arg(size.height()).arg(win)});
        cmds.append({lua("move", QString("x = %1, y = %2").arg(x).arg(y)),
                     QString("movewindowpixel exact %1 %2,%3").arg(x).arg(y).arg(win)});
        if (!me.value("pinned").toBool())
            cmds.append({lua("pin", "action = \"enable\""), "pin " + win});
    } else {
        if (me.value("pinned").toBool())
            cmds.append({lua("pin", "action = \"disable\""), "pin " + win});
        if (!m_pipWasFloating)
            cmds.append({lua("float", "action = \"disable\""), "settiled " + win});
    }
    static int useLua = -1; // unknown until the first reply
    for (const auto &[luaCmd, classic] : cmds) {
        if (useLua != 0) {
            const QByteArray reply = hyprctl({"dispatch", luaCmd}).trimmed();
            if (useLua == -1)
                useLua = reply.startsWith("ok") ? 1 : 0;
            if (useLua == 1)
                continue;
        }
        hyprctl(QStringList{"dispatch"} + classic.split(' '));
    }
}

void MainWindow::setHideUi(bool on)
{
    m_hideUi = on;
    if (on)
        m_toast->show(tr("Press Ctrl+\\ or Esc to show the interface"), {}, {}, 2500);
    updateLayout();
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    placeOverlayControls();
    m_title->setMaximized(isMaximized());
}

void MainWindow::updateTitle()
{
    QString t = QStringLiteral("Konos Media Player");
    if (m_player->hasMedia() && m_view == View::Player) {
        t = m_player->title();
        if (!m_player->artist().isEmpty())
            t = m_player->artist() + " — " + t;
    }
    m_title->setTitle(t);
    setWindowTitle(m_player->hasMedia() ? m_player->title() + " — Konos Media Player" : QStringLiteral("Konos Media Player"));
}

// ---------------------------------------------------------------------------
// Queue

void MainWindow::playItems(const QStringList &items, int index)
{
    const QStringList expanded = library().expand(items);
    if (expanded.isEmpty())
        return;
    // Map the index through expansion (only shifts when folders/playlists were expanded).
    int mapped = 0;
    if (index > 0 && index < items.size())
        mapped = int(expanded.indexOf(items.at(index)));
    savePosition();
    m_queue = expanded;
    m_shuffleHistory.clear();
    playIndex(qMax(0, mapped));
    showPlayer();
}

void MainWindow::enqueue(const QStringList &items)
{
    const QStringList expanded = library().expand(items);
    if (expanded.isEmpty())
        return;
    const bool start = m_queue.isEmpty() || !m_player->hasMedia();
    m_queue << expanded;
    if (start) { // nothing was playing: this starts it, so show it (as Replace Queue does)
        playIndex(int(m_queue.size() - expanded.size()));
        showPlayer();
    } else
        m_toast->show(expanded.size() == 1 ? tr("Added 1 item to the queue") : tr("Added %1 items to the queue").arg(expanded.size()), {}, {}, 1800);
    updateQueueViews();
}

void MainWindow::playIndex(int index)
{
    if (index < 0 || index >= m_queue.size())
        return;
    savePosition();
    m_index = index;
    m_current = m_queue.at(index);
    qint64 start = 0;
    const qint64 resume = library().resumeTime(m_current);
    const double progress = library().progress(m_current);
    const int mode = prefs().integer("playback/resume");
    const bool canResume = resume > 5000 && progress < 0.95 && prefs().flag("privacy/positions") &&
                           !Disc::isDiscLocation(m_current);
    const qint64 rewound = qMax<qint64>(0, resume - prefs().integer("playback/resumeRewind") * 1000);
    if (canResume && mode == 0)
        start = rewound;
    ShowPresets::instance().beforeOpen(m_current); // the show's subtitle look applies as it opens
    m_defaultsFor.clear();
    m_player->open(m_current, start);
    ShowPresets::instance().afterOpen(); // …and its delays once it's open
    // Files attached to this video in its folder's track settings.
    if (const QString sub = ShowPresets::instance().attachedFile(m_current, true); !sub.isEmpty())
        m_player->addSubtitleFile(sub);
    if (const QString audio = ShowPresets::instance().attachedFile(m_current, false); !audio.isEmpty())
        m_player->addAudioFile(audio);
    if (!Disc::isDiscLocation(m_current))
        library().addRecent(m_current);
    if (canResume && mode == 0) {
        m_toast->showAt(Toast::Bottom, tr("Resumed at %1").arg(formatTime(resume)), tr("Start over"),
                        [this] { m_player->seek(0); }, 5000);
    } else if (canResume && mode == 1) {
        m_toast->showAt(Toast::Bottom, tr("Continue from %1?").arg(formatTime(resume)), tr("Resume"),
                        [this, rewound] { m_player->seek(rewound); }, 8000);
    }
    if (m_sleepAtEnd)
        m_toast->show(tr("Sleep timer: will pause at the end of this item"), {}, {}, 2500);
    updateQueueViews();
}

void MainWindow::next(bool userInitiated)
{
    // Like VLC: on a disc, next/previous step through chapters first.
    if (userInitiated && m_player->isDisc() && !m_player->inDiscMenu() && m_player->nextChapter())
        return;
    if (m_queue.isEmpty())
        return;
    const int repeat = prefs().integer("playback/repeat");
    if (prefs().flag("playback/shuffle") && m_queue.size() > 1) {
        static std::mt19937 rng(std::random_device{}());
        int n;
        do {
            n = std::uniform_int_distribution<int>(0, int(m_queue.size()) - 1)(rng);
        } while (n == m_index);
        m_shuffleHistory << m_index;
        playIndex(n);
        return;
    }
    if (m_index + 1 < m_queue.size()) {
        playIndex(m_index + 1);
    } else if (repeat == 2 || userInitiated) {
        if (repeat == 2 || m_queue.size() > 1)
            playIndex(0);
    } else {
        m_player->stop();
    }
}

void MainWindow::previous()
{
    if (m_player->isDisc() && !m_player->inDiscMenu() && m_player->previousChapter())
        return;
    if (m_player->time() > 3000) {
        m_player->seek(0);
        return;
    }
    if (prefs().flag("playback/shuffle") && !m_shuffleHistory.isEmpty()) {
        playIndex(m_shuffleHistory.takeLast());
        return;
    }
    if (m_index > 0)
        playIndex(m_index - 1);
    else
        m_player->seek(0);
}

void MainWindow::onEndReached()
{
    if (!m_current.isEmpty() && !Disc::isDiscLocation(m_current) && m_player->length() > 0)
        library().setPosition(m_current, m_player->length(), m_player->length());
    if (m_sleepAtEnd) {
        m_sleepAtEnd = false;
        m_sleepMinutes = 0;
        m_quick->setSleepMinutes(0);
        m_toast->show(tr("Sleep timer: playback stopped"));
        return;
    }
    const int repeat = prefs().integer("playback/repeat");
    QTimer::singleShot(0, this, [this, repeat] {
        if (repeat == 1)
            playIndex(m_index);
        else if (prefs().flag("playback/autoNext"))
            next(false);
    });
}

void MainWindow::savePosition()
{
    // A disc location names the drive, not the disc in it, so positions would be wrong.
    if (m_current.isEmpty() || Disc::isDiscLocation(m_current) || !m_player->hasMedia() ||
        m_player->location() != m_current)
        return;
    const qint64 len = m_player->length();
    const qint64 t = m_player->time();
    // Short items (songs, say) can be left out: they start from the beginning.
    if (len > 0 && len < qint64(prefs().integer("playback/minResume")) * 60000)
        return;
    if (len > 0 && t > 0 && m_player->state() != Player::Ended)
        library().setPosition(m_current, t, len);
}

void MainWindow::updateQueueViews()
{
    m_side->setQueue(m_queue, m_index);
    m_panel->queue()->setQueue(m_queue, m_index);
    m_panel->queue()->setNowPlaying(m_current);
    m_library->setQueue(m_queue, m_index);
    m_side->setNowPlaying(m_current);
    m_library->setNowPlaying(m_current);
}

// ---------------------------------------------------------------------------
// Opening media


void MainWindow::openFiles()
{
    const QStringList files = FileDialogs::openFiles(
        this, tr("Open media"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation), FileDialogs::mediaFilter(true));
    if (!files.isEmpty())
        playItems(files, 0);
}

void MainWindow::openFolder()
{
    const QString dir = FileDialogs::existingDirectory(this, tr("Open folder"),
                                                          QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    if (!dir.isEmpty())
        playItems({dir}, 0);
}

void MainWindow::openNetwork()
{
    auto *dialog = new NetworkStreamOverlay(
        this,
        [this](const QStringList &urls, bool play) { play ? playItems(urls, 0) : enqueue(urls); },
        [this](const QString &kind, const QString &url) {
            showLibrary(Section::Network);
            m_library->showNetwork((kind == "channels" ? "m3u:" : "rss:") + url);
        });
    dialog->open();
}

void MainWindow::customizePlayer()
{
    (new CustomizePlayerOverlay(this))->open();
}

void MainWindow::cycleAspect()
{
    const QStringList ratios = VideoWidget::aspectRatios();
    const QString next = ratios.value((ratios.indexOf(prefs().string("video/aspect")) + 1) % ratios.size());
    prefs().set("video/aspect", next);
    m_toast->show(tr("Aspect ratio: %1").arg(next == "Auto" ? tr("Auto") : next), {}, {}, 1400);
}

// The playback bar's optional items (Menu → Customize Player).
void MainWindow::controlAction(const QString &id)
{
    const bool media = m_player->hasMedia();
    auto seek = [this, media](int seconds) {
        if (media)
            m_player->seekRelative(qint64(seconds) * 1000);
    };
    if (id == "stop")
        m_actStop->trigger();
    else if (id == "fwd10")
        seek(10);
    else if (id == "back10")
        seek(-10);
    else if (id == "fwd15")
        seek(15);
    else if (id == "back15")
        seek(-15);
    else if (id == "speed") {
        QMenu menu(this);
        for (double r : {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0}) {
            QAction *a = menu.addAction(QString("%1×").arg(r), this, [this, r] { m_player->setRate(float(r)); });
            a->setCheckable(true);
            a->setChecked(qAbs(m_player->rate() - r) < 0.01);
        }
        QWidget *at = m_controls->itemWidget("speed");
        menu.exec(at ? at->mapToGlobal(QPoint(0, -menu.sizeHint().height())) : QCursor::pos());
    } else if (id == "repeat") {
        static const int next[] = {2, 0, 1}; // off → all → one → off
        prefs().set("playback/repeat", next[qBound(0, prefs().integer("playback/repeat"), 2)]);
    } else if (id == "shuffle")
        prefs().set("playback/shuffle", !prefs().flag("playback/shuffle"));
    else if (id == "record")
        toggleRecording();
    else if (id == "snapshot")
        takeSnapshot();
    else if (id == "pip")
        setPip(!m_pip);
    else if (id == "fullscreen")
        setFullscreen(!m_fullscreen);
    else if (id == "aspect")
        cycleAspect();
    else if (id == "home")
        showLibrary(Section::Home);
    else if (id == "info" && !m_current.isEmpty())
        (new MediaInfoOverlay(m_current, this))->open();
    else if (id == "replay" && media)
        m_player->seek(0);
    else if (id == "chapters")
        openPlayerPanel(PlayerPanel::ChaptersTab);
    else if (id == "bookmarks")
        openPlayerPanel(PlayerPanel::BookmarksTab);
    else if (id == "bookmark" && media) {
        m_panel->addBookmark();
        m_toast->show(tr("Bookmark added"), {}, {}, 1500);
    } else if (id == "playlists")
        openSidePanel(Section::Playlists);
    else if (id == "favorite" && !m_current.isEmpty()) {
        // "Favorite" pins the item to the sidebar (and unpins it).
        if (library().isPinned("file", m_current)) {
            library().unpin("file", m_current);
            m_toast->show(tr("Unpinned from the sidebar"), {}, {}, 1500);
        } else {
            library().pin({"file", m_current, Library::displayName(m_current),
                           int(Library::isAudioFile(m_current) ? Section::Music : Section::Videos)});
            m_toast->show(tr("Pinned to the sidebar"), {}, {}, 1500);
        }
    } else if (id == "foldername" && !m_current.isEmpty() && !Library::isUrl(m_current)) {
        const bool music = Library::isAudioFile(m_current);
        m_library->showFolder(music ? Section::Music : Section::Videos, QFileInfo(m_current).absolutePath());
        setView(View::Library);
    } else if (id == "appsettings")
        m_settings->open();
    else if (id == "quit")
        close();
}

void MainWindow::openFile()
{
    const QString file = FileDialogs::openFile(this, tr("Open file"),
                                               QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
                                               FileDialogs::mediaFilter(true));
    if (!file.isEmpty())
        playItems({file}, 0);
}

void MainWindow::openCapture()
{
    auto *dialog = new CaptureDeviceOverlay(this, [this](const QString &mrl, const QStringList &options) {
        m_player->setMediaOptions(mrl, options);
        playItems({mrl}, 0);
        showPlayer();
    });
    dialog->open();
}

// "Open Location from Clipboard": copied files play at once; addresses go
// into the network stream dialog, to check before opening.
void MainWindow::openClipboard()
{
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    QStringList files, other;
    if (mime && mime->hasUrls()) {
        for (const QUrl &u : mime->urls())
            (u.isLocalFile() ? files : other) << (u.isLocalFile() ? u.toLocalFile() : u.toString());
    } else if (mime && mime->hasText()) {
        for (QString line : mime->text().split('\n', Qt::SkipEmptyParts)) {
            line = line.trimmed();
            if (line.isEmpty())
                continue;
            const QUrl u(line);
            if (u.isLocalFile() && QFileInfo::exists(u.toLocalFile()))
                files << u.toLocalFile();
            else if (line.startsWith('/') && QFileInfo::exists(line))
                files << line;
            else
                other << line;
        }
    }
    if (!files.isEmpty() && other.isEmpty()) {
        playItems(files, 0);
        return;
    }
    if (files.isEmpty() && other.isEmpty()) {
        m_toast->show(tr("There's no address or file on the clipboard."), {}, {}, 2500);
        return;
    }
    openNetwork();
    if (auto *dialog = findChild<NetworkStreamOverlay *>())
        dialog->setUrls(files + other);
}

bool MainWindow::writePlaylist(const QString &path)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QTextStream out(&f);
    auto uri = [](const QString &item) {
        return Library::isUrl(item) ? item : QUrl::fromLocalFile(item).toString(QUrl::FullyEncoded);
    };
    if (path.endsWith(".xspf", Qt::CaseInsensitive)) {
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            << "<playlist xmlns=\"http://xspf.org/ns/0/\" version=\"1\">\n  <trackList>\n";
        for (const QString &item : m_queue) {
            out << "    <track><location>" << uri(item).toHtmlEscaped() << "</location><title>"
                << Library::displayName(item).toHtmlEscaped() << "</title>";
            if (const qint64 ms = Durations::instance().duration(item); ms > 0)
                out << "<duration>" << ms << "</duration>";
            out << "</track>\n";
        }
        out << "  </trackList>\n</playlist>\n";
    } else {
        out << "#EXTM3U\n";
        for (const QString &item : m_queue) {
            const qint64 ms = Durations::instance().duration(item);
            out << "#EXTINF:" << (ms > 0 ? ms / 1000 : -1) << "," << Library::displayName(item) << "\n"
                << item << "\n";
        }
    }
    return f.commit();
}

void MainWindow::savePlaylistFile()
{
    if (m_queue.isEmpty()) {
        m_toast->show(tr("The queue is empty: there's nothing to save."), {}, {}, 2500);
        return;
    }
    const QString name = m_current.isEmpty() ? tr("Playlist") : QFileInfo(Library::displayName(m_current)).completeBaseName();
    QString path = FileDialogs::saveFile(this, tr("Save playlist to file"),
                                         QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/" + name + ".m3u8",
                                         tr("M3U playlist (*.m3u8 *.m3u);;XSPF playlist (*.xspf)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += ".m3u8";
    if (writePlaylist(path))
        m_toast->show((m_queue.size() == 1 ? tr("Saved 1 item to %1").arg(QFileInfo(path).fileName())
                              : tr("Saved %1 items to %2").arg(m_queue.size()).arg(QFileInfo(path).fileName())));
    else
        showError(tr("Couldn't save the playlist to %1.").arg(path));
}

void MainWindow::openConvert(const QStringList &files)
{
    QStringList items = files;
    if (items.isEmpty() && !m_current.isEmpty() && !Disc::isDiscLocation(m_current))
        items << m_current;
    (new ConvertOverlay(items, this))->open();
}

void MainWindow::openStream(bool cast)
{
    (new StreamOverlay(m_player, Disc::isDiscLocation(m_current) ? QString() : m_current, this, cast))->open();
}

void MainWindow::showShortcuts()
{
    (new KeyboardShortcutsOverlay(this))->open();
}

void MainWindow::openDisc()
{
    const QList<Disc::Drive> drives = Disc::drives();
    if (drives.isEmpty()) {
        showError(tr("No disc drive found. Is the drive plugged in?"));
        return;
    }
    // Prefer the first drive that has something in it.
    for (const Disc::Drive &d : drives) {
        if (d.type != Disc::Type::Empty) {
            playDisc(d);
            return;
        }
    }
    showError(tr("There's no disc in %1.").arg(drives.first().model.isEmpty() ? drives.first().device
                                                                               : drives.first().model));
}

void MainWindow::playDisc(const Disc::Drive &drive)
{
    if (drive.type == Disc::Type::Empty) {
        showError(tr("There's no disc in %1.").arg(drive.model.isEmpty() ? drive.device : drive.model));
        return;
    }
    const QStringList items = drive.items();
    if (items.isEmpty()) {
        // A data disc that isn't mounted yet.
        showError(tr("“%1” isn't a video or audio disc. Open it in your file manager to mount it, "
                     "then use Open Folder.").arg(drive.name()));
        return;
    }
    m_toast->show(tr("Opening %1…").arg(drive.name()), {}, {}, 2500);
    playItems(items, 0);
}

void MainWindow::addDiscEntries(QMenu *menu)
{
    const QList<Disc::Drive> drives = Disc::drives();
    if (drives.size() <= 1) {
        menu->addAction(m_actDisc);
        return;
    }
    QMenu *sub = menu->addMenu(theme().icon("disc", theme().accent()), tr("Play Disc"));
    for (const Disc::Drive &d : drives) {
        const QString text = d.type == Disc::Type::Empty
                                 ? tr("%1 (empty)").arg(d.model.isEmpty() ? d.device : d.model)
                                 : d.name();
        sub->addAction(text, this, [this, d] { playDisc(d); });
    }
}

void MainWindow::openSubtitle()
{
    if (!m_player->hasMedia())
        return;
    const QString dir = Library::isUrl(m_current) ? QDir::homePath() : QFileInfo(m_current).absolutePath();
    const QString file = FileDialogs::openFile(this, tr("Add subtitle file"), dir,
                                                      tr("Subtitles (*.srt *.ass *.ssa *.sub *.vtt *.idx *.smi *.txt);;All files (*)"));
    if (file.isEmpty())
        return;
    if (m_player->addSubtitleFile(file))
        m_toast->show(tr("Subtitle added: %1").arg(QFileInfo(file).fileName()));
    else
        showError(tr("Couldn't load “%1”.").arg(QFileInfo(file).fileName()));
}

void MainWindow::openLocations(const QStringList &locations, bool append)
{
    QStringList items;
    for (const QString &l : locations) {
        if (l.isEmpty())
            continue;
        const QUrl url(l);
        if (url.isLocalFile())
            items << url.toLocalFile();
        else if (Library::isUrl(l))
            items << l;
        else
            items << QFileInfo(l).absoluteFilePath();
    }
    if (items.isEmpty())
        return;
    if (append)
        enqueue(items);
    else
        playItems(items, 0);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasText())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    QStringList items;
    for (const QUrl &u : e->mimeData()->urls())
        items << (u.isLocalFile() ? u.toLocalFile() : u.toString());
    if (items.isEmpty() && e->mimeData()->hasText())
        items << e->mimeData()->text().trimmed();
    if (items.size() == 1 && Library::isSubtitleFile(items.first()) && m_player->hasMedia()) {
        if (m_player->addSubtitleFile(items.first()))
            m_toast->show(tr("Subtitle added: %1").arg(QFileInfo(items.first()).fileName()));
        return;
    }
    openLocations(items, e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
    e->acceptProposedAction();
}

// ---------------------------------------------------------------------------
// Misc features

void MainWindow::takeSnapshot()
{
    QImage img = m_video->snapshot();
    if (const int w = prefs().integer("snapshot/width"); w > 0 && !img.isNull() && img.width() != w)
        img = img.scaledToWidth(w, Qt::SmoothTransformation);
    if (img.isNull()) {
        m_toast->show(tr("Nothing to snapshot"));
        return;
    }
    const QString dir = prefs().string("snapshot/dir");
    QDir().mkpath(dir);
    const QString base = m_player->hasMedia() ? QFileInfo(m_player->title()).completeBaseName() : "snapshot";
    const QString when = prefs().integer("snapshot/naming") == 1
                             ? QDateTime::currentDateTime().toString("yyyy-MM-dd hh.mm.ss")
                             : formatTime(m_player->time(), true).replace(':', '.');
    const QString path = QString("%1/%2 %3.%4").arg(dir, base, when, prefs().string("snapshot/format"));
    if (img.save(path))
        m_toast->show(tr("Snapshot saved: %1").arg(QFileInfo(path).fileName()));
    else
        showError(tr("Couldn't save the snapshot to %1").arg(dir));
}

void MainWindow::toggleRecording()
{
    if (!m_player->hasMedia()) {
        m_toast->show(tr("Open something to record first"));
        return;
    }
    m_player->setRecording(!m_player->isRecording());
}

void MainWindow::setSleepTimer(int minutes)
{
    m_sleepTimer.stop();
    m_sleepAtEnd = minutes < 0;
    m_sleepMinutes = minutes;
    if (minutes > 0) {
        m_sleepTimer.start(minutes * 60 * 1000);
        m_toast->show(minutes == 1 ? tr("Sleep timer: pausing in 1 minute") : tr("Sleep timer: pausing in %1 minutes").arg(minutes));
    } else if (minutes < 0) {
        m_toast->show(tr("Sleep timer: will pause at the end of this item"));
    } else {
        m_toast->show(tr("Sleep timer off"), {}, {}, 1500);
    }
}

void MainWindow::showError(const QString &message)
{
    if (prefs().flag("startup/confirmDialogs"))
        QMessageBox::warning(this, QStringLiteral("Konos"), message);
    else
        m_toast->show(message, {}, {}, 5000);
}

void MainWindow::updateScreensaverInhibit()
{
    const bool want = m_player->isPlaying() && m_player->hasVideo();
    QDBusInterface iface("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver");
    if (!iface.isValid())
        return;
    if (want && !m_inhibitCookie) {
        const QDBusReply<uint> reply = iface.call("Inhibit", QStringLiteral("Konos"), tr("Playing video"));
        if (reply.isValid())
            m_inhibitCookie = reply.value();
    } else if (!want && m_inhibitCookie) {
        iface.call("UnInhibit", m_inhibitCookie);
        m_inhibitCookie = 0;
    }
}

// ---------------------------------------------------------------------------
// Right-click / main menu (from the "right-click menu" design)

// The ways to open something, in the order of the redesign's main menu.
void MainWindow::addOpenEntries(QMenu *menu)
{
    menu->addAction(m_actOpen);
    menu->addAction(m_actOpenMany);
    menu->addAction(m_actOpenDir);
    addDiscEntries(menu);
    menu->addAction(m_actOpenNet);
    menu->addAction(m_actCapture);
    menu->addAction(m_actClipboard);
    QMenu *rec = menu->addMenu(theme().icon("recent", theme().accent()), tr("Open Recent"));
    const QStringList recent = library().recent().mid(0, 15);
    for (const QString &r : recent)
        rec->addAction(Library::displayName(r), this, [this, r] { playItems({r}, 0); });
    if (recent.isEmpty())
        rec->addAction(tr("Nothing yet"))->setEnabled(false);
    else {
        rec->addSeparator();
        rec->addAction(tr("Clear Recent"), this, [] { library().clearRecent(); });
    }
}

// The title bar's menu (from the redesign video, 12:56): opening, saving and
// the app's own windows.
QMenu *MainWindow::buildMainMenu()
{
    const QColor a = theme().accent();
    auto *menu = new QMenu(this);
    addOpenEntries(menu);
    menu->addSeparator();
    m_actSavePlaylist->setEnabled(!m_queue.isEmpty());
    menu->addAction(m_actSavePlaylist);
    menu->addAction(m_actConvert);
    menu->addAction(m_actStream);
    // Profiles (19:24): the one in use is ticked.
    Profiles &profiles = Profiles::instance();
    QMenu *profile = menu->addMenu(theme().icon("artist", a), tr("Profile (%1)").arg(profiles.current()));
    for (const QString &name : profiles.names()) {
        QAction *act = profile->addAction(name, this, [this, name] {
            Profiles::instance().switchTo(name);
            m_toast->show(tr("Profile: %1").arg(name), {}, {}, 1800);
        });
        act->setCheckable(true);
        act->setChecked(name == profiles.current());
    }
    profile->addSeparator();
    profile->addAction(theme().icon("plus", a), tr("New Profile…"), this, [this] { (new NewProfileOverlay(this))->open(); });
    if (profiles.names().size() > 1) {
        QMenu *del = profile->addMenu(theme().icon("trash", a), tr("Delete Profile"));
        for (const QString &name : profiles.names())
            if (name != profiles.current())
                del->addAction(name, this, [this, name] {
                    if (QMessageBox::question(this, tr("Delete profile"),
                                              tr("Delete the profile “%1” and its settings?").arg(name)) == QMessageBox::Yes)
                        Profiles::instance().remove(name);
                });
    }
    menu->addAction(m_actShortcuts);
    menu->addAction(theme().icon("brush", a), tr("Customize Player"), this, &MainWindow::customizePlayer);
    QMenu *plugins = menu->addMenu(theme().icon("puzzle", a), tr("Plugins && Extensions"));
    plugins->addAction(theme().icon("puzzle", a), tr("Plugins…"), this, [this] { (new PluginsOverlay(this))->open(); });
    plugins->addAction(theme().icon("tune", a), tr("Audio and Video Filters…"), this,
                       [this] { m_settings->open(SettingsOverlay::Extensions); });
    menu->addAction(m_actSettings);
    menu->addAction(m_actAbout);
    menu->addSeparator();
    menu->addAction(m_actQuit);
    return menu;
}

// Right-click on the video (the "right-click menu" design).
QMenu *MainWindow::buildContextMenu()
{
    const QColor a = theme().accent();
    auto *menu = new QMenu(this);
    menu->addAction(m_actPlay);
    menu->addAction(m_actStop);
    menu->addAction(m_actPrev);
    menu->addAction(m_actNext);
    menu->addAction(m_actRecord);
    menu->addAction(m_actHideUi);

    // Playback
    QMenu *playback = menu->addMenu(theme().icon("playback", a), tr("Playback"));
    QMenu *speed = playback->addMenu(theme().icon("speed", a), tr("Speed"));
    auto *speedGroup = new QActionGroup(speed);
    for (double r : {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0}) {
        QAction *act = speed->addAction(QString("%1×").arg(r), this, [this, r] { m_player->setRate(float(r)); });
        act->setCheckable(true);
        act->setChecked(qAbs(m_player->rate() - r) < 0.01);
        speedGroup->addAction(act);
    }
    if (m_player->isDisc())
        playback->addAction(theme().icon("menu-disc", a), tr("Disc Menu"), this, [this] { m_player->discMenu(); });
    const QList<Player::Track> titles = m_player->titles();
    if (titles.size() > 1) {
        QMenu *tmenu = playback->addMenu(theme().icon("film", a), tr("Title"));
        for (const auto &t : titles) {
            QAction *act = tmenu->addAction(t.name, this, [this, id = t.id] { m_player->setTitle(id); });
            act->setCheckable(true);
            act->setChecked(t.id == m_player->currentTitle());
        }
    }
    const QList<Player::Track> chapters = m_player->chapters();
    if (chapters.size() > 1) {
        QMenu *cmenu = playback->addMenu(theme().icon("chapters", a), tr("Chapter"));
        for (const auto &c : chapters) {
            QAction *act = cmenu->addAction(c.name, this, [this, id = c.id] { m_player->setChapter(id); });
            act->setCheckable(true);
            act->setChecked(c.id == m_player->currentChapter());
        }
    }
    if (m_player->hasMedia())
        playback->addAction(theme().icon("bookmark", a), tr("Add Bookmark") + "\tCtrl+Shift+B", this, [this] {
            m_panel->addBookmark();
            openPlayerPanel(PlayerPanel::BookmarksTab);
        });
    playback->addAction(theme().icon("bookmark", a), tr("Show Bookmarks"), this, [this] {
        if (!(m_panelOpen && m_panel->tab() == PlayerPanel::BookmarksTab && m_panel->isVisible()))
            openPlayerPanel(PlayerPanel::BookmarksTab);
    });
    playback->addAction(theme().icon("chapters", a), tr("Show Chapters"), this, [this] {
        if (!(m_panelOpen && m_panel->tab() == PlayerPanel::ChaptersTab && m_panel->isVisible()))
            openPlayerPanel(PlayerPanel::ChaptersTab);
    });
    playback->addAction(theme().icon("jump-fwd", a), tr("Jump Forward") + "\t→",
                        [this] { m_player->seekRelative(prefs().integer("controls/seekStep") * 1000); });
    playback->addAction(theme().icon("jump-back", a), tr("Jump Backward") + "\t←",
                        [this] { m_player->seekRelative(-prefs().integer("controls/seekStep") * 1000); });
    QMenu *repeat = playback->addMenu(theme().icon("repeat", a), tr("Repeat"));
    const QStringList repeatNames = {tr("Off"), tr("Repeat One"), tr("Repeat All")};
    for (int i = 0; i < 3; ++i) {
        QAction *act = repeat->addAction(repeatNames[i], this, [i] { prefs().set("playback/repeat", i); });
        act->setCheckable(true);
        act->setChecked(prefs().integer("playback/repeat") == i);
    }
    QAction *shuffle = playback->addAction(theme().icon("shuffle", a), tr("Shuffle"), this,
                                           [] { prefs().set("playback/shuffle", !prefs().flag("playback/shuffle")); });
    shuffle->setCheckable(true);
    shuffle->setChecked(prefs().flag("playback/shuffle"));
    playback->addAction(theme().icon("settings", a), tr("Playback Settings…"), this,
                        [this] { m_quick->openAt(m_controls->settingsButton(), QuickSettings::PlaybackTab); });

    // Video
    QMenu *video = menu->addMenu(theme().icon("video", a), tr("Video"));
    video->addAction(m_actFullscreen);
    QMenu *aspect = video->addMenu(theme().icon("aspect", a), tr("Aspect Ratio"));
    for (const QString &r : VideoWidget::aspectRatios()) {
        QAction *act = aspect->addAction(r, this, [this, r] { m_video->setAspectRatio(r); });
        act->setCheckable(true);
        act->setChecked(m_video->aspectRatio() == r);
    }
    QMenu *vtrack = video->addMenu(theme().icon("film", a), tr("Video Track"));
    for (const auto &t : m_player->videoTracks()) {
        QAction *act = vtrack->addAction(t.name, this, [this, id = t.id] { m_player->setVideoTrack(id); });
        act->setCheckable(true);
        act->setChecked(t.id == m_player->videoTrack());
    }
    vtrack->setEnabled(!vtrack->isEmpty());
    video->addAction(m_actSnapshot);
    video->addAction(theme().icon("wand", a), tr("Video Effects…"), this,
                     [this] { m_quick->openAt(m_controls->settingsButton(), QuickSettings::VideoTab); });

    // Audio
    QMenu *audio = menu->addMenu(theme().icon("audio", a), tr("Audio"));
    QMenu *atrack = audio->addMenu(theme().icon("note", a), tr("Audio Track"));
    for (const auto &t : m_player->audioTracks()) {
        QAction *act = atrack->addAction(t.name, this, [this, id = t.id] { m_player->setAudioTrack(id); });
        act->setCheckable(true);
        act->setChecked(t.id == m_player->audioTrack());
    }
    atrack->setEnabled(!atrack->isEmpty());
    audio->addAction(m_actMute);
    audio->addAction(theme().icon("volume", a), tr("Volume Up") + "\t↑",
                     [this] { m_player->setVolume(m_player->volume() + prefs().integer("controls/volumeStep")); });
    audio->addAction(theme().icon("volume-low", a), tr("Volume Down") + "\t↓",
                     [this] { m_player->setVolume(m_player->volume() - prefs().integer("controls/volumeStep")); });
    QAction *eq = audio->addAction(theme().icon("eq", a), tr("Equalizer"), this, [] {
        fx().d().eqEnabled = !fx().data().eqEnabled;
        fx().commit(Effects::Equalizer);
    });
    eq->setCheckable(true);
    eq->setChecked(fx().data().eqEnabled);
    audio->addAction(theme().icon("wand", a), tr("Audio Effects…"), this,
                     [this] { m_quick->openAt(m_controls->audioButton(), QuickSettings::AudioTab); });

    // Subtitle
    QMenu *subs = menu->addMenu(theme().icon("subtitles", a), tr("Subtitle"));
    QMenu *strack = subs->addMenu(theme().icon("subtitles", a), tr("Subtitle Track"));
    for (const auto &t : m_player->subtitleTracks()) {
        QAction *act = strack->addAction(t.name, this, [this, id = t.id] { m_player->setSubtitleTrack(id); });
        act->setCheckable(true);
        act->setChecked(t.id == m_player->subtitleTrack());
    }
    strack->setEnabled(!strack->isEmpty());
    subs->addAction(theme().icon("file", a), tr("Add Subtitle File…"), this, &MainWindow::openSubtitle);
    subs->addAction(theme().icon("globe", a), tr("Download Subtitles…"), this, [this] { m_subtitles->open(); })
        ->setEnabled(m_player->hasMedia());
    subs->addAction(theme().icon("settings", a), tr("Subtitle Settings…"), this,
                    [this] { m_quick->openAt(m_controls->subtitleButton(), QuickSettings::SubtitlesTab); });

    // More settings
    QMenu *more = menu->addMenu(theme().icon("settings", a), tr("More Settings"));
    more->addAction(m_actSettings);
    QMenu *themeMenu = more->addMenu(theme().icon("eye", a), tr("Theme"));
    const QStringList modes = Theme::modeNames();
    for (int i = 0; i < modes.size(); ++i) {
        QAction *act = themeMenu->addAction(modes[i], this, [i] { Theme::pick(Theme::Mode(i)); });
        act->setCheckable(true);
        act->setChecked(theme().mode() == i);
    }
    QMenu *accent = more->addMenu(theme().icon("tune", a), tr("Accent Color"));
    const auto colors = Theme::accentChoices();
    const auto names = Theme::accentNames();
    for (int i = 0; i < colors.size(); ++i) {
        QPixmap swatch(40, 40);
        swatch.fill(colors[i]);
        QAction *act = accent->addAction(QIcon(swatch), names[i], this, [i] { Theme::pickAccent(i); });
        act->setCheckable(true);
        act->setChecked(theme().accentIndex() == i);
    }
    more->addAction(m_actSidebar);
    more->addAction(m_actLibrary);

    // Playlist
    QMenu *playlist = menu->addMenu(theme().icon("playlist", a), tr("Playlist"));
    playlist->addAction(theme().icon("queue", a), tr("Show Play Queue"), this, [this] { openPlayerPanel(PlayerPanel::QueueTab); });
    playlist->addAction(theme().icon("playlist", a), tr("Show Playlists"), this, [this] { openSidePanel(Section::Playlists); });
    if (!m_current.isEmpty()) {
        QMenu *addTo = playlist->addMenu(theme().icon("plus", a), tr("Add Current to Playlist"));
        for (const QString &name : library().playlistNames())
            addTo->addAction(name, this, [this, name] {
                library().addToPlaylist(name, {m_current});
                m_toast->show(tr("Added to “%1”").arg(name), {}, {}, 1800);
            });
        if (!library().playlistNames().isEmpty())
            addTo->addSeparator();
        addTo->addAction(tr("New Playlist…"), this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Create playlist"), tr("Playlist name:"),
                                                       QLineEdit::Normal, tr("New playlist"), &ok);
            if (ok)
                library().addToPlaylist(library().createPlaylist(name), {m_current});
        });
    }
    QAction *save = playlist->addAction(theme().icon("check", a), tr("Save Queue as Playlist…"), this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save queue"), tr("Playlist name:"), QLineEdit::Normal,
                                                   tr("Queue"), &ok);
        if (ok)
            library().addToPlaylist(library().createPlaylist(name), m_queue);
    });
    save->setEnabled(!m_queue.isEmpty());

    // Open media
    QMenu *open = menu->addMenu(theme().icon("open-media", a), tr("Open Media"));
    addOpenEntries(open);

    menu->addAction(m_actQuit);
    return menu;
}

// ---------------------------------------------------------------------------
// Window behaviour

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if ((obj == m_videoArea && event->type() == QEvent::Resize) ||
        (obj == m_controls && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) ||
        (obj == m_title && (event->type() == QEvent::Show || event->type() == QEvent::Hide)))
        QMetaObject::invokeMethod(this, &MainWindow::placeVideoOverlays, Qt::QueuedConnection);
    // A keyboard shortcut is about to run: let its message say which key it was.
    if (event->type() == QEvent::Shortcut && qobject_cast<QAction *>(obj) && prefs().flag("osd/showKeys")) {
        m_toast->setKeyHint(static_cast<QShortcutEvent *>(event)->key().toString(QKeySequence::NativeText));
        QTimer::singleShot(0, this, [this] { m_toast->setKeyHint({}); });
    }
    // Edge resizing for the frameless window.
    if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove) && !isMaximized() &&
        !isFullScreen() && obj->isWidgetType() && static_cast<QWidget *>(obj)->window() == this) {
        auto *me = static_cast<QMouseEvent *>(event);
        const QPoint p = mapFromGlobal(me->globalPosition().toPoint());
        Qt::Edges edges;
        if (p.x() <= kResizeBorder)
            edges |= Qt::LeftEdge;
        if (p.x() >= width() - kResizeBorder)
            edges |= Qt::RightEdge;
        if (p.y() <= kResizeBorder)
            edges |= Qt::TopEdge;
        if (p.y() >= height() - kResizeBorder)
            edges |= Qt::BottomEdge;
        if (event->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton && edges && windowHandle()) {
            windowHandle()->startSystemResize(edges);
            return true;
        }
        if (event->type() == QEvent::MouseMove && !(me->buttons() & Qt::LeftButton)) {
            static bool overridden = false;
            Qt::CursorShape shape = Qt::ArrowCursor;
            if ((edges & Qt::LeftEdge && edges & Qt::TopEdge) || (edges & Qt::RightEdge && edges & Qt::BottomEdge))
                shape = Qt::SizeFDiagCursor;
            else if ((edges & Qt::RightEdge && edges & Qt::TopEdge) || (edges & Qt::LeftEdge && edges & Qt::BottomEdge))
                shape = Qt::SizeBDiagCursor;
            else if (edges & (Qt::LeftEdge | Qt::RightEdge))
                shape = Qt::SizeHorCursor;
            else if (edges & (Qt::TopEdge | Qt::BottomEdge))
                shape = Qt::SizeVerCursor;
            if (edges && !overridden) {
                QApplication::setOverrideCursor(shape);
                overridden = true;
            } else if (edges && overridden) {
                QApplication::changeOverrideCursor(shape);
            } else if (!edges && overridden) {
                QApplication::restoreOverrideCursor();
                overridden = false;
            }
        }
    }
    if (event->type() == QEvent::MouseMove && (m_fullscreen || m_pip) && obj->isWidgetType() &&
        static_cast<QWidget *>(obj)->window() == this)
        overlayHover(m_videoArea->mapFromGlobal(static_cast<QMouseEvent *>(event)->globalPosition().toPoint()));
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange) {
        m_title->setMaximized(isMaximized());
        if (isMinimized() && prefs().flag("ui/pauseOnMinimize") && m_player->isPlaying())
            m_player->pause();
        if (!isFullScreen() && m_fullscreen)
            setFullscreen(false); // left fullscreen through the compositor
    }
    QMainWindow::changeEvent(e);
}

void MainWindow::saveSession()
{
    QSettings s;
    s.setValue("session/queue", m_queue);
    s.setValue("session/index", m_index);
    s.setValue("session/time", m_player->time());
}

void MainWindow::restoreSession()
{
    QSettings s;
    const QStringList queue = s.value("session/queue").toStringList();
    const int index = s.value("session/index", -1).toInt();
    if (queue.isEmpty() || index < 0 || index >= queue.size() || !m_queue.isEmpty())
        return;
    m_queue = queue;
    m_index = index;
    m_current = queue.at(index);
    m_player->open(m_current, s.value("session/time").toLongLong(), true);
    updateQueueViews();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    savePosition();
    saveSession();
    QSettings s;
    if (m_fullscreen)
        setFullscreen(false);
    s.setValue("window/geometry", saveGeometry());
    if (m_inhibitCookie) {
        QDBusInterface iface("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                             "org.freedesktop.ScreenSaver");
        iface.call("UnInhibit", m_inhibitCookie);
        m_inhibitCookie = 0;
    }
    m_player->stop();
    QMainWindow::closeEvent(e);
}
