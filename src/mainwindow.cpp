#include "mainwindow.h"
#include "mediadialogs.h"
#include "shortcuts.h"
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

static QString mediaFilter();

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("VMP"));
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
#ifdef VMP_DEVHOOKS
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
                    m_updateToast->showAt(Toast::Bottom, tr("VMP %1 is out.").arg(version), tr("Update Now"), [this] {
                        m_updateToast->showAt(Toast::Bottom, tr("Downloading the update…"), {}, {}, 600000);
                        UpdateCheck::instance().installUpdate();
                    }, 20000);
                    return;
                }
                m_updateToast->showAt(Toast::Bottom,
                                      UpdateCheck::updatedByPacman()
                                          ? tr("VMP %1 is out. Update with: sudo pacman -Syu").arg(version)
                                          : tr("VMP %1 is out.").arg(version),
                                      tr("What's New"), [page] { QDesktopServices::openUrl(QUrl(page)); }, 15000);
            });
    connect(&UpdateCheck::instance(), &UpdateCheck::progress, this, [this](int percent) {
        m_updateToast->showAt(Toast::Bottom, tr("Downloading the update… %1%").arg(percent), {}, {}, 600000);
    });
    connect(&UpdateCheck::instance(), &UpdateCheck::installed, this, [this](bool ok, const QString &error) {
        m_updateToast->raise();
        if (ok)
            m_updateToast->showAt(Toast::Bottom, tr("Update installed. It starts the next time you open VMP."), {}, {}, 10000);
        else
            m_updateToast->showAt(Toast::Bottom, error, {}, {}, 10000);
    });
    if (qEnvironmentVariableIsEmpty("VMP_SCRIPT")) // not during scripted test runs
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
                mediaFilter());
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
    m_actQuit = make("quit", tr("Quit VMP"), "quit", [this] { close(); });
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
    m_actAbout = make("info", tr("About VMP"), "about", [this] { (new AboutOverlay(this))->open(); });
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
    key("aspect", [this] {
        const QStringList ratios = VideoWidget::aspectRatios();
        const QString next = ratios.value((ratios.indexOf(prefs().string("video/aspect")) + 1) % ratios.size());
        prefs().set("video/aspect", next);
        m_toast->show(tr("Aspect ratio: %1").arg(next == "Auto" ? tr("Auto") : next), {}, {}, 1400);
    });
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
    QString t = QStringLiteral("VMP");
    if (m_player->hasMedia() && m_view == View::Player) {
        t = m_player->title();
        if (!m_player->artist().isEmpty())
            t = m_player->artist() + " — " + t;
    }
    m_title->setTitle(t);
    setWindowTitle(m_player->hasMedia() ? m_player->title() + " — VMP" : QStringLiteral("VMP"));
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

static QString mediaFilter()
{
    return QObject::tr("Media files (*.mp4 *.mkv *.avi *.mov *.webm *.m4v *.wmv *.flv *.mpg *.mpeg *.ts *.m2ts "
                       "*.3gp *.ogv *.vob *.mp3 *.flac *.ogg *.opus *.m4a *.aac *.wav *.wma *.aiff *.ape *.mka "
                       "*.m3u *.m3u8 *.pls *.xspf);;All files (*)");
}

void MainWindow::openFiles()
{
    const QStringList files = FileDialogs::openFiles(
        this, tr("Open media"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation), mediaFilter());
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

void MainWindow::openFile()
{
    const QString file = FileDialogs::openFile(this, tr("Open file"),
                                               QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
                                               mediaFilter());
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
        m_toast->show(tr("Saved %n item(s) to %1", nullptr, m_queue.size()).arg(QFileInfo(path).fileName()));
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
        m_toast->show(tr("Sleep timer: pausing in %n minute(s)", "", minutes));
    } else if (minutes < 0) {
        m_toast->show(tr("Sleep timer: will pause at the end of this item"));
    } else {
        m_toast->show(tr("Sleep timer off"), {}, {}, 1500);
    }
}

void MainWindow::showError(const QString &message)
{
    if (prefs().flag("startup/confirmDialogs"))
        QMessageBox::warning(this, QStringLiteral("VMP"), message);
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
        const QDBusReply<uint> reply = iface.call("Inhibit", QStringLiteral("VMP"), tr("Playing video"));
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
    menu->addAction(m_actShortcuts);
    QMenu *plugins = menu->addMenu(theme().icon("puzzle", a), tr("Plugins && Extensions"));
    plugins->addAction(theme().icon("puzzle", a), tr("Audio and Video Filters…"), this,
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

// ---------------------------------------------------------------------------
// Developer hook (only in builds made with CONFIG+=devhooks): drive the UI from
// the VMP_SCRIPT environment variable to capture screenshots.

#ifdef VMP_DEVHOOKS
#include <QProcess>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
// Runs `fn` on the next menu that opens (synthetic input can't keep a
// Wayland popup open, but its Show event still arrives).
static void onNextMenu(std::function<void(QMenu *)> fn)
{
    struct Catcher : QObject {
        std::function<void(QMenu *)> fn;
        bool eventFilter(QObject *o, QEvent *e) override
        {
            if (e->type() == QEvent::Show)
                if (auto *m = qobject_cast<QMenu *>(o)) {
                    qApp->removeEventFilter(this);
                    fn(m);
                    deleteLater();
                }
            return false;
        }
    };
    auto *c = new Catcher;
    c->fn = std::move(fn);
    qApp->installEventFilter(c);
}
void MainWindow::runDevScript()
{
    const QString script = qEnvironmentVariable("VMP_SCRIPT");
    if (script.isEmpty())
        return;
    auto sectionFrom = [](const QString &s) {
        const QStringList names = {"home", "videos", "music", "network", "playlists", "recent", "queue"};
        return Section(qMax(0, int(names.indexOf(s.toLower()))));
    };
    int at = 300;
    for (const QString &raw : script.split(';', Qt::SkipEmptyParts)) {
        const QString step = raw.trimmed();
        const QString cmd = step.section(':', 0, 0);
        const QString arg = step.section(':', 1);
        if (cmd == "wait") {
            at += arg.toInt();
            continue;
        }
        QTimer::singleShot(at, this, [this, cmd, arg, sectionFrom] {
            if (cmd == "library")
                showLibrary(sectionFrom(arg));
            else if (cmd == "browse")
                m_library->showFolder(sectionFrom(arg.section(',', 0, 0)), arg.section(',', 1)), setView(View::Library);
            else if (cmd == "play")
                playItems(arg.split(','), 0);
            else if (cmd == "disc")
                openDisc();
            else if (cmd == "nav") {
                const QStringList names = {"activate", "up", "down", "left", "right"};
                m_player->navigate(Player::Nav(qMax(0, int(names.indexOf(arg)))));
            } else if (cmd == "discmenu")
                m_player->discMenu();
            else if (cmd == "chapter")
                arg == "next" ? next(true) : previous();
            else if (cmd == "titles") {
                for (const auto &t : m_player->titles())
                    qInfo() << "title" << t.id << t.name << (t.id == m_player->currentTitle() ? "*" : "");
                for (const auto &c : m_player->chapters())
                    qInfo() << "chapter" << c.id << c.name << (c.id == m_player->currentChapter() ? "*" : "");
                qInfo() << "inMenu" << m_player->inDiscMenu();
            } else if (cmd == "tracks") {
                for (const auto &t : m_player->subtitleTracks())
                    qInfo() << "spu" << t.id << t.name;
                for (const auto &t : m_player->audioTracks())
                    qInfo() << "audio" << t.id << t.name;
                m_player->debugTracks();
            } else if (cmd == "addsub")
                m_player->addSubtitleFile(arg);
            else if (cmd == "click") {
                const QPoint p(arg.section(',', 0, 0).toInt(), arg.section(',', 1, 1).toInt());
                if (const auto fp = m_video->framePoint(p))
                    m_player->pointerOnVideo(*fp, true);
            }
            else if (cmd == "musicview") { // musicview:lib|side,view[,group key[,album name]]
                LibraryView *lv = arg.section(',', 0, 0) == "side" ? m_side : m_library;
                const QString key = arg.section(',', 2, 2);
                lv->showMusic(arg.section(',', 1, 1), arg.count(',') >= 2 ? key : QString(),
                              arg.count(',') >= 3 ? arg.section(',', 3) + "\n" : QString());
            }
            else if (cmd == "artistpic") // artistpic:Name
                (new ArtistPictureOverlay(arg, this))->open();
            else if (cmd == "albumedit") { // albumedit:Album name (no album artist)
                QStringList tracks;
                for (const auto &g : musicIndex().groups(MusicIndex::Album))
                    if (g.name == arg)
                        for (const auto &t : musicIndex().tracksIn(MusicIndex::Album, g.key))
                            tracks << t.path;
                (new AlbumEditorOverlay(tracks, this))->open();
            } else if (cmd == "overlaypick") { // overlaypick:row (click a result in the open overlay)
                for (auto *list : findChildren<QListWidget *>())
                    if (list->isVisible() && list->window() == this && qobject_cast<PanelOverlay *>(list->parentWidget()->parentWidget()) &&
                        arg.toInt() < list->count()) {
                        list->setCurrentRow(arg.toInt());
                        emit list->itemClicked(list->item(arg.toInt()));
                    }
            } else if (cmd == "overlaybutton") { // overlaybutton:Button text
                for (auto *b : findChildren<QPushButton *>())
                    if (b->isVisible() && b->text() == arg)
                        b->click();
            } else if (cmd == "popupshot") { // popupshot:path[,item text] (that combo's list; default: artist art)
                const QString path = arg.section(',', 0, 0);
                const QString item = arg.contains(',') ? arg.section(',', 1) : tr("Artist image");
                for (auto *dd : findChildren<Dropdown *>())
                    if (dd->isVisible() && dd->findText(item) >= 0) {
                        const QString arg = path;
                        dd->showPopup();
                        QTimer::singleShot(400, this, [dd, arg] {
                            dd->view()->window()->grab().save(arg);
                            dd->hidePopup();
                        });
                        break;
                    }
            }
            else if (cmd == "key") { // key:Ctrl+Shift+B (as if pressed: goes through the shortcut)
                const QKeySequence seq(arg);
                for (QAction *a : actions())
                    if (a->shortcuts().contains(seq)) {
                        QShortcutEvent ev(seq, 0);
                        QCoreApplication::sendEvent(a, &ev);
                        break;
                    }
            } else if (cmd == "libclick") { // libclick:row[,ctrl|shift|double|menu] (real mouse input on a library row)
                if (auto *view = m_library->findChild<QListView *>("entries")) {
                    const QString how = arg.section(',', 1);
                    const QRect r = view->visualRect(view->model()->index(arg.section(',', 0, 0).toInt(), 0));
                    const Qt::KeyboardModifiers mods = how == "ctrl" ? Qt::ControlModifier
                                                       : how == "shift" ? Qt::ShiftModifier : Qt::NoModifier;
                    if (how == "double") { // what a real double-click delivers: press, release, double-click, release
                        const QPointF pt = r.center();
                        const QPointF gp = view->viewport()->mapToGlobal(pt);
                        for (QEvent::Type t : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease, QEvent::MouseButtonDblClick,
                                               QEvent::MouseButtonRelease}) {
                            QMouseEvent me(t, pt, gp, Qt::LeftButton,
                                           t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
                            QApplication::sendEvent(view->viewport(), &me);
                        }
                    }
                    else if (how == "menu") {
                        onNextMenu([](QMenu *m) {
                            for (QAction *a : m->actions())
                                qInfo() << "menu item" << a->text();
                        });
                        emit view->customContextMenuRequested(r.center());
                    } else
                        QTest::mouseClick(view->viewport(), Qt::LeftButton, mods, r.center());
                    qInfo() << "selected" << view->selectionModel()->selectedIndexes().size();
                }
            } else if (cmd == "progress") { // progress:path,ratio (0..1)
                library().setPosition(arg.section(',', 0, -2), qint64(arg.section(',', -1).toDouble() * 600000), 600000);
            } else if (cmd == "search") { // search:text (typed into the library's search field)
                for (auto *e : m_library->findChildren<QLineEdit *>())
                    if (e->placeholderText() == tr("Search") && e->isVisible())
                        e->setText(arg);
            } else if (cmd == "probe") { // probe:url (what the network dialog decides a link is)
                NetFeeds::instance().probe(arg, [arg](const QString &kind, const QString &title) {
                    qInfo() << "probe" << arg << kind << title;
                });
            } else if (cmd == "selfupdate") { // selfupdate:url (download it over $APPIMAGE)
                UpdateCheck::instance().setAppImageUrl(arg);
                connect(&UpdateCheck::instance(), &UpdateCheck::installed, this,
                        [](bool ok, const QString &e) { qInfo() << "installed" << ok << e; });
                UpdateCheck::instance().installUpdate();
            } else if (cmd == "viewmode") { // viewmode:0|1|2 (grid, list, details)
                m_library->setViewMode(arg.toInt());
            } else if (cmd == "sortset") { // sortset:key,desc (SortKey in libraryview.cpp)
                m_library->setSort(arg.section(',', 0, 0).toInt(), arg.section(',', 1) == "1");
            } else if (cmd == "network") { // network:path ("", radio, podcasts, m3u:<url>, rss:<url>)
                showLibrary(Section::Network);
                m_library->showNetwork(arg);
            } else if (cmd == "newplaylist") { // newplaylist[:file,file]
                (new NewPlaylistOverlay(arg.isEmpty() ? QStringList() : arg.split(','), {}, this))->open();
            } else if (cmd == "netdialog") {
                openNetwork();
            } else if (cmd == "info") { // info:path
                (new MediaInfoOverlay(arg, this))->open();
            } else if (cmd == "foldertracks") { // foldertracks:folder,sub|audio
                (new FolderTracksOverlay(arg.section(',', 0, -2), arg.section(',', -1) == "sub", this))->open();
            } else if (cmd == "pinadd") { // pinadd:kind|target|name|section
                const QStringList p = arg.split('|');
                library().pin({p.value(0), p.value(1), p.value(2), p.value(3).toInt()});
            } else if (cmd == "pinopen") { // pinopen:N (click the Nth pin in the sidebar)
                const auto pins = library().pins();
                if (arg.toInt() < pins.size())
                    emit m_nav->pinClicked(pins.at(arg.toInt()));
            } else if (cmd == "moremenu") { // print the library's ⋯ menu
                for (auto *b : m_library->findChildren<IconButton *>())
                    if (b->toolTip() == tr("More") && b->isVisible()) {
                        onNextMenu([](QMenu *m) {
                            for (QAction *a : m->actions())
                                if (!a->isSeparator())
                                    qInfo() << "menu item" << a->text();
                            QTimer::singleShot(0, m, &QMenu::close);
                        });
                        b->click();
                        break;
                    }
            } else if (cmd == "sortmenu") { // print the library's sort menu
                for (auto *b : m_library->findChildren<IconButton *>())
                    if (b->toolTip() == tr("Sort") && b->isVisible()) {
                        onNextMenu([](QMenu *m) {
                            for (QAction *a : m->actions())
                                if (!a->isSeparator())
                                    qInfo() << "menu item" << a->text() << (a->isChecked() ? "*" : "");
                            QTimer::singleShot(0, m, &QMenu::close);
                        });
                        b->click();
                        break;
                    }
            } else if (cmd == "libqueue") {
                emit m_controls->queueClicked();
            } else if (cmd == "rows") { // print the library's rows in order
                if (auto *view = m_library->findChild<QListView *>("entries"))
                    for (int i = 0; i < view->model()->rowCount(); ++i)
                        qInfo() << "row" << i << view->model()->index(i, 0).data().toString();
            } else if (cmd == "feedparse") { // feedparse:file (an RSS or M3U file on disk)
                QFile f(arg);
                f.open(QIODevice::ReadOnly);
                const QByteArray data = f.readAll();
                qInfo() << "channels" << NetFeeds::isChannelList(data) << "feed" << NetFeeds::isFeed(data);
                const auto items = NetFeeds::isFeed(data) ? NetFeeds::parseRss(data).items : NetFeeds::parseM3u(data, QUrl::fromLocalFile(arg));
                if (NetFeeds::isFeed(data))
                    qInfo() << "title" << NetFeeds::parseRss(data).title << "image" << NetFeeds::parseRss(data).image;
                for (const auto &it : items)
                    qInfo() << "item" << it.title << it.url << it.detail << it.image;
            } else if (cmd == "tilebtn") { // tilebtn:row,hover|play|more[,menu shot path] (real input on a tile's buttons)
                if (auto *view = m_library->findChild<QListView *>("entries")) {
                    const QModelIndex idx = view->model()->index(arg.section(',', 0, 0).toInt(), 0);
                    const QString how = arg.section(',', 1, 1), shotPath = arg.section(',', 2);
                    auto *d = static_cast<EntryDelegate *>(view->itemDelegate());
                    const QRect r = view->visualRect(idx);
                    const QPointF pt = d->tileButtonRect(r, how == "more" ? EntryDelegate::MoreButton : EntryDelegate::PlayButton).center();
                    const QPointF gp = view->viewport()->mapToGlobal(pt);
                    QHoverEvent hover(QEvent::HoverMove, pt, gp, pt);
                    QApplication::sendEvent(view->viewport(), &hover);
                    QMouseEvent move(QEvent::MouseMove, pt, gp, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(view->viewport(), &move);
                    if (how == "more")
                        onNextMenu([shotPath](QMenu *m) {
                            for (QAction *a : m->actions())
                                qInfo() << "menu item" << a->text();
                            QTimer::singleShot(0, m, [m, shotPath] {
                                if (!shotPath.isEmpty()) {
                                    // A popup can't be grabbed on Wayland: paint a copy.
                                    QPixmap pm(m->size() * m->devicePixelRatio());
                                    pm.setDevicePixelRatio(m->devicePixelRatio());
                                    pm.fill(Qt::transparent);
                                    m->render(&pm);
                                    pm.save(shotPath);
                                }
                                m->close();
                            });
                        });
                    if (how != "hover")
                        for (QEvent::Type t : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                            QMouseEvent me(t, pt, gp, Qt::LeftButton, t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                                           Qt::NoModifier);
                            QApplication::sendEvent(view->viewport(), &me);
                        }
                    qInfo() << "tilebtn" << how << "selected" << view->selectionModel()->selectedIndexes().size()
                            << "queue" << m_queue.size() << "index" << m_index;
                }
            } else if (cmd == "libmenu") { // libmenu:Action text (run it on the selection via the right-click menu)
                if (auto *view = m_library->findChild<QListView *>("entries")) {
                    const QModelIndexList sel = view->selectionModel()->selectedIndexes();
                    if (!sel.isEmpty()) {
                        onNextMenu([arg](QMenu *m) {
                            for (QAction *a : m->actions())
                                if (a->text() == arg) {
                                    QMetaObject::invokeMethod(a, &QAction::trigger, Qt::QueuedConnection);
                                    break;
                                }
                            QMetaObject::invokeMethod(m, &QMenu::close, Qt::QueuedConnection);
                        });
                        emit view->customContextMenuRequested(view->visualRect(sel.first()).center());
                    }
                }
            } else if (cmd == "libmime") { // what dragging the current selection would carry
                if (auto *view = m_library->findChild<QListView *>("entries"))
                    if (QMimeData *m = view->model()->mimeData(view->selectionModel()->selectedIndexes())) {
                        qInfo() << "drag" << m->urls() << m->data("application/x-vmp-queue-rows");
                        delete m;
                    }
            } else if (cmd == "winsize") { // winsize:w,h
                showNormal();
                resize(arg.section(',', 0, 0).toInt(), arg.section(',', 1).toInt());
            } else if (cmd == "videomove") // as if the pointer moved over the video
                emit m_video->mouseMoved(QPoint(m_video->width() / 2, m_video->height() / 2));
            else if (cmd == "rotate")
                m_video->setRotation(arg.toInt());
            else if (cmd == "flip")
                m_video->setFlip(arg.contains('h'), arg.contains('v'));
            else if (cmd == "hdr")
                m_video->setHdrToneMapping(arg == "1");
            else if (cmd == "zoom")
                m_video->setZoom(arg.toDouble());
            else if (cmd == "presetsave")
                ShowPresets::instance().saveAs(arg);
            else if (cmd == "presetuse")
                ShowPresets::instance().choose(arg);
            else if (cmd == "presetinfo")
                qInfo() << "preset active" << ShowPresets::instance().active() << "for" << m_player->location()
                        << ShowPresets::instance().presetFor(m_player->location()) << "subDelay" << m_player->subtitleDelay()
                        << "px" << fx().data().subSizePixels << "spu" << m_player->subtitleTrack()
                        << "audioDefault" << prefs().string("audio/default") << "subDefault" << prefs().string("subs/default");
            else if (cmd == "subdelay")
                m_player->setSubtitleDelay(arg.toLongLong());
            else if (cmd == "queuedump")
                qInfo() << "queue" << m_index << m_queue;
            else if (cmd == "pip")
                setPip(!m_pip);
            else if (cmd == "fshover") // fshover:y (pointer at that height over the video)
                overlayHover(QPoint(m_videoArea->width() / 2, arg.toInt() < 0 ? m_videoArea->height() + arg.toInt() : arg.toInt()));
            else if (cmd == "queueadd") // queueadd:at,path (as if dropped on the queue)
                emit m_panel->queue()->queueInsertRequested({arg.section(',', 1)}, arg.section(',', 0, 0).toInt());
            else if (cmd == "dropqueue") { // dropqueue:y,path (a real drop event on the queue list)
                QListView *view = m_panel->queue()->findChild<QListView *>();
                auto *mime = new QMimeData;
                mime->setUrls({QUrl::fromLocalFile(arg.section(',', 1))});
                const QPointF at(40, arg.section(',', 0, 0).toInt());
                QDragEnterEvent enter(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(view->viewport(), &enter);
                QDropEvent drop(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(view->viewport(), &drop);
                qInfo() << "drop accepted" << drop.isAccepted();
                delete mime;
            } else if (cmd == "queuemove") // queuemove:from,to
                { // queuemove:row[ row…],at
                QList<int> rows;
                for (const QString &r : arg.section(',', 0, 0).split(' ', Qt::SkipEmptyParts))
                    rows << r.toInt();
                emit m_panel->queue()->queueMoveRequested(rows, arg.section(',', 1).toInt());
            }
            else if (cmd == "audiodev")
                for (const auto &d : m_player->audioDevices())
                    qInfo() << "device" << d.id << d.name << (d.id == m_player->audioDevice() ? "*" : "");
            else if (cmd == "panel") // panel:queue|bookmarks|chapters (toggles like the buttons)
                openPlayerPanel(qMax(0, int(QStringList{"queue", "bookmarks", "chapters"}.indexOf(arg))));
            else if (cmd == "bookmark")
                m_panel->addBookmark();
            else if (cmd == "pref") // pref:key=value
                prefs().set(arg.section('=', 0, 0), arg.section('=', 1));
            else if (cmd == "enqueue")
                enqueue(arg.split(','));
            else if (cmd == "side")
                openSidePanel(sectionFrom(arg));
            else if (cmd == "sidebar")
                toggleSidebar();
            else if (cmd == "player")
                showPlayer();
            else if (cmd == "quick")
                m_quick->openAt(m_controls->settingsButton(), QuickSettings::Tab(arg.toInt()));
            else if (cmd == "quickscroll") { // quickscroll:px (the open quick-settings page)
                for (auto *area : m_quick->findChildren<QScrollArea *>())
                    if (area->isVisible())
                        area->verticalScrollBar()->setValue(arg.toInt());
            } else if (cmd == "closequick")
                m_quick->hide();
            else if (cmd == "settings")
                m_settings->open(SettingsOverlay::Page(arg.toInt()));
            else if (cmd == "pageshot") { // pageshot:path (the whole settings page showing, not just what fits)
                for (QScrollArea *sa : m_settings->findChildren<QScrollArea *>())
                    if (sa->isVisible() && sa->widget())
                        sa->widget()->grab().save(arg);
            }
            else if (cmd == "checkupdate") { // checkupdate (prints what the update check found)
                connect(&UpdateCheck::instance(), &UpdateCheck::finished, this,
                        [](bool newer, const QString &v, const QString &page, const QString &err, bool manual) {
                            qInfo() << "update" << newer << v << page << err << manual;
                        });
                UpdateCheck::instance().checkNow();
            } else if (cmd == "versioncmp") { // versioncmp:a b|a b|… (prints the comparison of each pair)
                for (const QString &pair : arg.split('|'))
                    qInfo() << "cmp" << pair << UpdateCheck::compare(pair.section(' ', 0, 0), pair.section(' ', 1, 1));
            } else if (cmd == "fakeupdate") // fakeupdate:version (the notice a newer release would show)
                emit UpdateCheck::instance().finished(true, arg, "https://github.com/" VMP_RELEASES_REPO "/releases", {}, false);
            else if (cmd == "shortcuts")
                showShortcuts();
            else if (cmd == "keyset") { // keyset:id=Ctrl+K,Alt+K (empty: none; "default": the default)
                Shortcuts::KeyMap keys = Shortcuts::instance().all();
                const QString id = arg.section('=', 0, 0), value = arg.section('=', 1);
                QList<QKeySequence> list;
                if (value == "default")
                    list = Shortcuts::instance().defaults().value(id);
                else
                    for (const QString &k : value.split(',', Qt::SkipEmptyParts))
                        list << QKeySequence::fromString(k, QKeySequence::PortableText);
                keys[id] = list;
                Shortcuts::instance().setAll(keys);
                qInfo() << "keyset" << id << Shortcuts::instance().text(id);
            } else if (cmd == "capture")
                openCapture();
            else if (cmd == "clipboard") { // clipboard:text (put on the clipboard, then "Open Location from Clipboard")
                QApplication::clipboard()->setText(arg);
                openClipboard();
            } else if (cmd == "convert") // convert:file|file
                openConvert(arg.split('|', Qt::SkipEmptyParts));
            else if (cmd == "transcode") { // transcode:source|profile index|destination (prints progress, then the result)
                auto *job = new SoutJob(this);
                const auto p = ConvertOverlay::profiles().value(arg.section('|', 1, 1).toInt());
                QStringList options;
                if (!p.video)
                    options << ":no-sout-video" << ":no-sout-spu";
                QElapsedTimer *clock = new QElapsedTimer;
                clock->start();
                connect(job, &SoutJob::finished, this, [job, clock](bool ok, const QString &error) {
                    qInfo("transcode finished ok=%d %s in %lld ms", ok, qPrintable(error), clock->elapsed());
                    delete clock;
                    job->deleteLater();
                });
                qInfo("transcode start=%d sout=%s", job->start(arg.section('|', 0, 0), ConvertOverlay::soutFor(p, arg.section('|', 2), 0), options),
                      qPrintable(ConvertOverlay::soutFor(p, arg.section('|', 2), 0)));
            } else if (cmd == "stream" || cmd == "streamcast")
                openStream(cmd == "streamcast");
            else if (cmd == "about")
                (new AboutOverlay(this))->open();
            else if (cmd == "saveplaylist") // saveplaylist:path (.m3u8 or .xspf)
                qInfo("saveplaylist %d", writePlaylist(arg));
            else if (cmd == "closesettings")
                m_settings->close();
            else if (cmd == "menu" || cmd == "ctxmenu") { // menu: the title bar's; ctxmenu: right-click
                // Render the menu offscreen (Wayland won't map unsolicited popups).
                QMenu *menu = cmd == "menu" ? buildMainMenu() : buildContextMenu();
                menu->ensurePolished();
                menu->adjustSize();
                menu->grab().save(arg.isEmpty() ? "/tmp/vlct/menu.png" : arg);
                if (QMenu *sub = menu->actions().value(6) ? menu->actions().value(6)->menu() : nullptr) {
                    sub->ensurePolished();
                    sub->adjustSize();
                    sub->grab().save("/tmp/vlct/submenu.png");
                }
                menu->deleteLater();
            } else if (cmd == "seek")
                m_player->seek(arg.toLongLong());
            else if (cmd == "oshash") {
                const auto q = OpenSubtitles::guess(arg);
                qInfo("oshash %s | %s | %d | %d | %d", qPrintable(OpenSubtitles::movieHash(arg)), qPrintable(q.text),
                      q.season, q.episode, q.year);
            }
            else if (cmd == "ossignin") // ossignin:user,password
                OpenSubtitles::instance().signIn(arg.section(',', 0, 0), arg.section(',', 1), [](const QString &error) {
                    qInfo("ossignin: %s", error.isEmpty() ? "ok" : qPrintable(error));
                });
            else if (cmd == "subs")
                m_subtitles->open(SubtitleOverlay::Source(arg.toInt())); // subs:1 OpenSubtitles
            else if (cmd == "subsapply") {
                for (auto *b : m_subtitles->findChildren<QPushButton *>())
                    if (b->property("role") == "accent")
                        b->click();
            } else if (cmd == "subshot") {
                m_subtitles->grab().save(arg);
            }
            else if (cmd == "subscope") {
                if (auto *d = m_subtitles->findChildren<Dropdown *>().value(0))
                    d->setCurrentIndex(arg.toInt());
            }
            else if (cmd == "open")
                QTimer::singleShot(0, this, arg == "folder" ? &MainWindow::openFolder : &MainWindow::openFiles);
            else if (cmd == "closedialog") {
                if (QWidget *w = QApplication::activeModalWidget())
                    w->close();
            } else if (cmd == "hover") {
                // hover:0.4  synthetic pointer over the seek bar at 40 %; hover:off leaves
                if (auto *bar = m_controls->findChild<SliderBar *>()) {
                    if (arg == "off") {
                        QEvent leave(QEvent::Leave);
                        QCoreApplication::sendEvent(bar, &leave);
                    } else {
                        const QPointF pos(bar->width() * arg.toDouble(), bar->height() / 2.0);
                        QMouseEvent move(QEvent::MouseMove, pos, bar->mapToGlobal(pos), Qt::NoButton, Qt::NoButton,
                                         Qt::NoModifier);
                        QCoreApplication::sendEvent(bar, &move);
                    }
                }
            }
            else if (cmd == "pause")
                m_player->pause();
            else if (cmd == "accent") {
                Theme::pickAccent(arg.toInt());
            } else if (cmd == "theme") {
                theme().setMode(Theme::Mode(arg.toInt()));
            } else if (cmd == "fullscreen")
                setFullscreen(!m_fullscreen);
            else if (cmd == "hideui")
                setHideUi(!m_hideUi);
            else if (cmd == "fx") {
                // fx:sepia=120  fx:comp=1  fx:eq=1
                const QString key = arg.section('=', 0, 0);
                const double v = arg.section('=', 1).toDouble();
                if (key == "sepia") { fx().d().sepia = int(v); fx().commit(Effects::VideoFilters); }
                if (key == "comp") { fx().d().compEnabled = v > 0; fx().commit(Effects::AudioFilters); }
                if (key == "eq") { fx().d().eqEnabled = v > 0; fx().commit(Effects::Equalizer); }
                if (key == "reverb") { fx().d().spatialEnabled = v > 0; fx().d().spatialMode = 1; fx().commit(Effects::AudioFilters); }
                if (key == "widen") { fx().d().widenEnabled = v > 0; fx().commit(Effects::AudioFilters); }
                if (key == "bright") { fx().d().brightness = float(v); fx().commit(Effects::VideoAdjust); }
                if (key == "subpx") { fx().d().subSizePixels = int(v); fx().commit(Effects::SubtitleStyle); }
                if (key == "subthick") { fx().d().subOutlineThickness = int(v); fx().commit(Effects::SubtitleStyle); }
                if (key == "subbg") { fx().d().subBackgroundOpacity = int(v); fx().commit(Effects::SubtitleStyle); }
                if (key == "submargin") { fx().d().subMargin = int(v); m_player->setSubtitleMargin(int(v)); }
                if (key == "subown") { fx().d().subEmbeddedStyle = v > 0; fx().commit(Effects::SubtitleStyle); }
                if (key == "subcolor") { fx().d().subColor = QColor::fromRgb(QRgb(v)); fx().commit(Effects::SubtitleStyle); }
            } else if (cmd == "record")
                toggleRecording();
            else if (cmd == "snapshot")
                takeSnapshot();
            else if (cmd == "dump")
                m_player->debugDump();
            else if (cmd == "shot")
            {
                // This window only, never the screen. grab() leaves the GL
                // video surface black, so paint its framebuffer in on top.
                QPixmap pm = grab();
                if (m_video->isVisible()) {
                    QPainter painter(&pm);
                    const QPoint at = m_video->mapTo(this, QPoint(0, 0));
                    painter.drawImage(QRect(at, m_video->size()), m_video->grabFramebuffer());
                    // Things floating over the video (messages, fullscreen bars) go on top again.
                    for (QWidget *child : m_videoArea->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
                        if (child != m_video && child->isVisible())
                            painter.drawPixmap(child->mapTo(this, QPoint(0, 0)), child->grab());
                    for (QWidget *side : {static_cast<QWidget *>(m_side), static_cast<QWidget *>(m_panel),
                                          static_cast<QWidget *>(m_quick)})
                        if (side->isVisible())
                            painter.drawPixmap(side->mapTo(this, QPoint(0, 0)), side->grab());
                }
                pm.save(arg);
            }
            else if (cmd == "quit")
                close();
        });
        at += 150;
    }
}
#endif
