// Developer-only UI scripting (qmake CONFIG+=devhooks): KONOS_SCRIPT="cmd:arg;wait:ms;…"
// drives the window for tests and screenshots. Not in release builds.
#ifdef KONOS_DEVHOOKS
#include "albumpanel.h"
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
#include <QProcess>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QTreeWidget>
#include <QMimeData>
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
    const QString script = qEnvironmentVariable("KONOS_SCRIPT");
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
            } else if (cmd == "selfupdate") { // selfupdate:url[|size|sha256] (download it over $APPIMAGE)
                const QStringList a = arg.split('|');
                UpdateCheck::instance().setAppImageUrl(a.value(0), a.size() > 1 ? a[1].toLongLong() : -1, a.value(2));
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
            } else if (cmd == "albumsong") { // albumsong:i[,menu] (click, or right-click, a song in the open album)
                if (auto *panel = m_library->findChild<AlbumTracksPanel *>()) {
                    const int i = arg.section(',', 0, 0).toInt();
                    // Rows go down the first column, then the second.
                    const int cols = panel->width() >= 760 ? 2 : 1;
                    const int rows = (panel->songCount() + cols - 1) / cols;
                    const int colWidth = (panel->width() - 48 - (cols - 1) * 48) / cols;
                    const QPoint at(24 + (i / rows) * (colWidth + 48) + 120, 104 + (i % rows) * 40 + 20);
                    if (arg.section(',', 1) == "menu") {
                        onNextMenu([](QMenu *m) {
                            for (QAction *a : m->actions())
                                qInfo() << "menu item" << a->text();
                        });
                        QTest::mouseClick(panel, Qt::RightButton, Qt::NoModifier, at);
                    } else
                        QTest::mouseClick(panel, Qt::LeftButton, Qt::NoModifier, at);
                } else
                    qInfo() << "no album open";
            } else if (cmd == "rows") { // print the library's rows in order
                if (auto *view = m_library->findChild<QListView *>("entries"))
                    for (int i = 0; i < view->model()->rowCount(); ++i)
                        qInfo() << "row" << i << view->model()->index(i, 0).data().toString()
                                << view->visualRect(view->model()->index(i, 0)) << "viewport" << view->viewport()->width()
                                << "grid" << view->gridSize() << "spacing" << view->spacing();
            } else if (cmd == "feedparse") { // feedparse:file (an RSS or M3U file on disk)
                QFile f(arg);
                if (!f.open(QIODevice::ReadOnly))
                    qInfo() << "feedparse: can't read" << arg;
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
            } else if (cmd == "libmenu") { // libmenu:Action text, or Submenu>Action text (on the selection via the right-click menu)
                if (auto *view = m_library->findChild<QListView *>("entries")) {
                    const QModelIndexList sel = view->selectionModel()->selectedIndexes();
                    if (!sel.isEmpty()) {
                        onNextMenu([arg](QMenu *m) {
                            QList<QAction *> actions = m->actions();
                            const QStringList steps = arg.split('>');
                            const QString want = steps.last();
                            for (int i = 0; i + 1 < steps.size(); ++i) // down through the submenus
                                for (QAction *a : actions)
                                    if (a->text() == steps[i] && a->menu()) {
                                        for (QAction *sub : a->menu()->actions())
                                            qInfo() << "submenu item" << steps[i] << sub->text();
                                        actions = a->menu()->actions();
                                        break;
                                    }
                            for (QAction *a : actions)
                                if (a->text() == want) {
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
                        qInfo() << "drag" << m->urls() << m->data("application/x-konos-queue-rows");
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
                emit UpdateCheck::instance().finished(true, arg, "https://github.com/" KONOS_RELEASES_REPO "/releases", {}, false);
            else if (cmd == "shortcuts")
                showShortcuts();
            else if (cmd == "customize")
                customizePlayer();
            else if (cmd == "ctl") { // ctl:insert,id,left|right,index · ctl:remove,left|right,index · ctl:save · ctl:print
                if (auto *ed = findChild<CustomizePlayerOverlay *>()) {
                    const QStringList a = arg.split(',');
                    if (a[0] == "insert")
                        ed->insert(a.value(1), a.value(2) == "left", a.value(3).toInt());
                    else if (a[0] == "remove")
                        ed->remove(a.value(1) == "left", a.value(2).toInt());
                    qInfo() << "ctl left" << ed->items(true) << "right" << ed->items(false);
                    if (a[0] == "save")
                        for (QPushButton *b : ed->findChildren<QPushButton *>())
                            if (b->text() == tr("Save")) {
                                b->click();
                                break;
                            }
                }
                qInfo() << "bar left" << savedControls(true) << "right" << savedControls(false);
            } else if (cmd == "plugins")
                (new PluginsOverlay(this))->open();
            else if (cmd == "profile") // profile:Name (switch) · profile:+Name (create) · profile:-Name (delete)
            {
                if (arg.startsWith('+'))
                    Profiles::instance().create(arg.mid(1));
                else if (arg.startsWith('-'))
                    Profiles::instance().remove(arg.mid(1));
                else
                    Profiles::instance().switchTo(arg);
                qInfo() << "profile" << Profiles::instance().current() << Profiles::instance().names();
            } else if (cmd == "onboarding") { // onboarding[:step]
                auto *o = new Onboarding(this);
                o->open();
                o->setStep(arg.toInt());
            }
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
            } else if (cmd == "dialogbutton") { // dialogbutton:Button text (in the open message box)
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    qInfo() << "dialog" << box->text() << box->informativeText();
                    for (QAbstractButton *b : box->buttons())
                        if (b->text() == arg)
                            QMetaObject::invokeMethod(b, &QAbstractButton::click, Qt::QueuedConnection);
                } else
                    qInfo() << "no dialog open";
            } else if (cmd == "trackdrop") { // trackdrop:from,to (a row of Edit Metadata's songs dropped on another)
                if (auto *t = findChild<QTreeWidget *>("TrackTable")) {
                    t->setCurrentItem(t->topLevelItem(arg.section(',', 0, 0).toInt()));
                    const QPointF at = t->visualItemRect(t->topLevelItem(arg.section(',', 1).toInt())).center();
                    QMimeData mime;
                    mime.setData("application/x-qabstractitemmodeldatalist", "row");
                    QDropEvent drop(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(t->viewport(), &drop);
                    for (int i = 0; i < t->topLevelItemCount(); ++i)
                        qInfo() << "track" << t->topLevelItem(i)->text(0) << t->topLevelItem(i)->text(1) << t->topLevelItem(i)->text(3);
                }
            } else if (cmd == "overlayfield") { // overlayfield:n=text (the nth text field in the open overlay)
                QList<QLineEdit *> fields;
                for (auto *overlay : findChildren<PanelOverlay *>())
                    if (overlay->isVisible())
                        for (auto *e : overlay->findChildren<QLineEdit *>())
                            if (e->isVisible())
                                fields << e;
                if (QLineEdit *e = fields.value(arg.section('=', 0, 0).toInt()))
                    e->setText(arg.section('=', 1));
                qInfo() << "fields" << fields.size();
            } else if (cmd == "trackedit") { // trackedit:row,column[,text] (type into a song's cell in Edit Metadata)
                if (auto *t = findChild<QTreeWidget *>("TrackTable"))
                    if (QTreeWidgetItem *item = t->topLevelItem(arg.section(',', 0, 0).toInt())) {
                        const int col = arg.section(',', 1, 1).toInt();
                        t->editItem(item, col);
                        if (arg.count(',') >= 2)
                            if (auto *e = t->findChild<QLineEdit *>()) {
                                e->selectAll();
                                QTest::keyClicks(e, arg.section(',', 2));
                            }
                    }
            } else if (cmd == "dialogshot") { // dialogshot:path
                if (QWidget *w = QApplication::activeModalWidget())
                    w->grab().save(arg);
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
#endif // KONOS_DEVHOOKS
