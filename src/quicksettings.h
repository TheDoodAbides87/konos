#pragma once

#include <QElapsedTimer>
#include <QWidget>

class Dropdown;
class Player;
class QLineEdit;
class QStackedWidget;
class SegmentedControl;
class SliderBar;
class ToggleSwitch;
class VideoWidget;

// Popup above the control bar: Settings (Playback / Video tabs), or the
// Audio or Subtitles menu on its own.
class QuickSettings : public QWidget {
    Q_OBJECT
public:
    enum Tab { PlaybackTab, VideoTab, AudioTab, SubtitlesTab };

    QuickSettings(Player *player, VideoWidget *video, QWidget *parent);
    void openAt(QWidget *anchor, Tab tab);
    void setSleepMinutes(int minutes);

signals:
    void advancedSettingsRequested(int tab);
    void snapshotRequested();
    void sleepTimerRequested(int minutes);
    void loadSubtitleRequested();
    void findSubtitlesRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QWidget *buildPlayback();
    QWidget *buildVideo();
    QWidget *buildAudio();
    QWidget *buildSubtitles();
    QWidget *buildFooter();
    void setTab(int tab);
    void refreshTracks();
    void refreshValues();
    void refreshDevices();

    Player *m_player;
    VideoWidget *m_video;
    QWidget *m_panel;
    QStackedWidget *m_stack;
    QList<class TabButton *> m_tabs;
    class QLabel *m_titleIcon, *m_titleText;
    QWidget *m_footer = nullptr;
    class QPushButton *m_advanced = nullptr;

    SliderBar *m_speed = nullptr;
    QLineEdit *m_speedBox = nullptr;
    SegmentedControl *m_repeat = nullptr;
    ToggleSwitch *m_shuffle = nullptr;
    Dropdown *m_sleep = nullptr;
    Dropdown *m_videoTrack = nullptr, *m_aspect = nullptr, *m_deinterlace = nullptr, *m_zoom = nullptr;
    SegmentedControl *m_rotate = nullptr;
    ToggleSwitch *m_flipH = nullptr, *m_flipV = nullptr, *m_hdr = nullptr;
    Dropdown *m_stereo = nullptr, *m_eqPreset = nullptr;
    class ChoiceList *m_audioList = nullptr, *m_deviceList = nullptr, *m_subEmbedded = nullptr, *m_subExternal = nullptr;
    SliderBar *m_audioDelay = nullptr;
    QLineEdit *m_audioDelayBox = nullptr;
    SliderBar *m_subDelay = nullptr, *m_subSpeed = nullptr, *m_subDuration = nullptr;
    QLineEdit *m_subDelayBox = nullptr, *m_subSpeedBox = nullptr, *m_subDurationBox = nullptr;
    Dropdown *m_preset = nullptr;
    bool m_refreshing = false;
    QElapsedTimer m_closedAt;
    bool contains(QObject *obj) const;
};
