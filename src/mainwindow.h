#pragma once

#include "disc.h"
#include "libraryview.h"

#include <QMainWindow>
#include <QTimer>

class ControlBar;
class NavBar;
class Player;
class QStackedWidget;
class QVBoxLayout;
class QMenu;
class QuickSettings;
class SettingsOverlay;
class TitleBar;
class Toast;
class VideoWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Opens files/URLs given on the command line or dropped on the window.
    void openLocations(const QStringList &locations, bool enqueue = false);

protected:
    void closeEvent(QCloseEvent *) override;
    void changeEvent(QEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    enum class View { Library, Player };

    void buildUi();
    void createActions();
    void connectPlayer();
    void applyTheme();

    void showLibrary(Section section);
    void showPlayer();
    void setView(View view);
    void updateLayout();
    void toggleSidebar();
    void openSidePanel(Section section);
    void openPlayerPanel(int tab); // PlayerPanel::Tab; the current one again closes it
    void setFullscreen(bool on);
    void setHideUi(bool on);
    void showControlsTemporarily();
    void placeOverlayControls();
    void placeVideoOverlays(); // subtitle timeline and metadata card
    void overlayHover(const QPoint &pos); // pointer position over the video area
    void setTitleOverlay(bool on);
    void setPip(bool on);
    void hyprlandPip(bool on, QSize size);

    void playItems(const QStringList &items, int index);
    void enqueue(const QStringList &items);
    void playIndex(int index);
    void next(bool userInitiated = true);
    void previous();
    void onEndReached();
    void savePosition();
    void updateQueueViews();
    void updateTitle();

    void openFiles();
    void openFolder();
    void openNetwork();
    void openDisc();
    void playDisc(const Disc::Drive &drive);
    void addDiscEntries(QMenu *menu);
    void openSubtitle();
    void takeSnapshot();
    void toggleRecording();
    void setSleepTimer(int minutes);
    void showError(const QString &message);
    void updateScreensaverInhibit();

    QMenu *buildMainMenu();
#ifdef VMP_DEVHOOKS
    void runDevScript();
#endif
    void restoreSession();
    void saveSession();

    Player *m_player;
    VideoWidget *m_video;
    QWidget *m_videoArea;
    TitleBar *m_title;
    NavBar *m_rail;
    NavBar *m_nav;
    QWidget *m_navHolder;
    LibraryView *m_library;
    LibraryView *m_side;
    class PlayerPanel *m_panel;
    QStackedWidget *m_content;
    ControlBar *m_controls;
    QVBoxLayout *m_rootLayout;
    QuickSettings *m_quick;
    SettingsOverlay *m_settings;
    class SubtitleOverlay *m_subtitles;
    class SubtitleTimeline *m_timeline;
    class MediaInfoCard *m_infoCard;
    QTimer m_infoTimer;     // how long the metadata card stays after the pointer moves
    QTimer m_defaultsTimer; // waits for the tracks to settle before picking defaults
    QString m_defaultsFor;  // the item default tracks were picked for
    QSet<QString> m_plainDone; // "location#track" swapped for a plain copy
    void usePlainSubtitles();
    Toast *m_updateToast = nullptr;
    Toast *m_toast;

    View m_view = View::Library;
    class MiniPlayer *m_mini = nullptr;
    bool m_miniDismissed = false; // closed for the current item
    bool m_libraryQueue = false;  // the queue panel beside the library
    void updateMiniPlayer();
    bool m_sidebar = true;     // rail + side panel visible in player view
    bool m_sidePanel = true;   // side panel (beyond the rail) open
    bool m_panelOpen = false;  // queue / bookmarks / chapters panel on the right
    bool m_fullscreen = false;
    bool m_hideUi = false;
    bool m_wasMaximized = false;
    bool m_pip = false; // picture in picture
    QByteArray m_pipRestore;
    bool m_pipWasMaximized = false;
    bool m_pipWasFloating = false;
    QTimer m_awayTimer;
    bool m_nearTop = false, m_nearBottom = false;

    QStringList m_queue;
    int m_index = -1;
    QList<int> m_shuffleHistory;
    QString m_current;

    QTimer m_positionTimer;
    QTimer m_hideTimer;
    QTimer m_sleepTimer;
    QTimer m_reinitTimer;
    bool m_sleepAtEnd = false;
    int m_sleepMinutes = 0;
    uint m_inhibitCookie = 0;

    // Actions (also provide the keyboard shortcuts)
    QAction *m_actPlay, *m_actStop, *m_actPrev, *m_actNext, *m_actRecord, *m_actHideUi, *m_actQuit;
    QAction *m_actFullscreen, *m_actSnapshot, *m_actMute, *m_actOpen, *m_actOpenNet, *m_actSettings;
    QAction *m_actSidebar, *m_actLibrary, *m_actDisc, *m_actMenuEnter;
};
