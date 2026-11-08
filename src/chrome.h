#pragma once

#include "libraryview.h"

#include <QTimer>
#include <QHash>
#include <QWidget>

class IconButton;
class PlayButton;
class Player;
class QLabel;
class SliderBar;
class VolumeBar;

// Custom title bar: logo, menu, sidebar toggle, location, centred title, window buttons.
class TitleBar : public QWidget {
    Q_OBJECT
public:
    enum Mode { LibraryMode, PlayerMode, MinimalMode, PipMode };

    explicit TitleBar(QWidget *parent = nullptr);
    void setMode(Mode mode);
    void setTitle(const QString &title);
    void setLocation(Section section);
    void setSidebarActive(bool active);
    void setMaximized(bool maximized);
    void setRecording(bool recording);
    void setOverlay(bool overlay); // shown over fullscreen video
    Mode mode() const { return m_mode; }

signals:
    void menuRequested(const QPoint &globalPos);
    void sidebarToggled();
    void locationClicked();
    void minimizeRequested();
    void maximizeRequested();
    void fullscreenRequested();
    void pipRequested();
    void closeRequested();

protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QWidget *m_logo;
    IconButton *m_menu, *m_sidebar, *m_location;
    QWidget *m_separator;
    QLabel *m_title, *m_rec;
    IconButton *m_min, *m_pip, *m_max, *m_full, *m_close;
    Mode m_mode = LibraryMode;
    bool m_overlay = false;
};

// Navigation: collapsed icon rail (player mode) or expanded labelled panel (library mode).
class NavBar : public QWidget {
    Q_OBJECT
public:
    NavBar(bool expanded, QWidget *parent = nullptr);
    void setCurrent(Section section, bool highlight = true);

signals:
    void sectionClicked(Section section);
    void addClicked(const QPoint &globalPos);
    void pinClicked(const Library::Pin &pin);

private:
    void rebuildPins();
    QList<QPair<Section, IconButton *>> m_items;
    class QVBoxLayout *m_pins;
    QWidget *m_pinRule;
    bool m_expanded;
};

// Bottom transport bar.
// What the playback bar can show. Menu → Customize Player picks the items and
// their order, left and right of the seek bar (prefs "ui/controlsLeft" and
// "ui/controlsRight", comma-separated ids). "spacer" and "divider" can repeat.
struct ControlItem {
    QString id, label, icon;
    bool text = false; // shows words (a name) rather than an icon
};
const QList<ControlItem> &controlItems();
const ControlItem *controlItem(const QString &id);
QStringList defaultControls(bool left);
QStringList savedControls(bool left);

class ControlBar : public QWidget {
    Q_OBJECT
public:
    explicit ControlBar(Player *player, QWidget *parent = nullptr);
    void setQueueActive(bool active);
    void setOverlay(bool overlay);
    QWidget *audioButton() const;
    QWidget *subtitleButton() const;
    QWidget *settingsButton() const;
    QWidget *itemWidget(const QString &id) const; // nullptr when it isn't in the bar
    void setFavorite(bool on);
    void rebuild(); // from the saved layout

signals:
    void actionRequested(const QString &id); // the items without a signal of their own
    void previousClicked();
    void nextClicked();
    void audioClicked();
    void subtitlesClicked();
    void settingsClicked();
    void queueClicked();

protected:
    void paintEvent(QPaintEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void hideEvent(QHideEvent *e) override;

private:
    void updateTime(qint64 ms);
    void updateState();
    void showSeekPreview(double ms, const QPoint &pos);
    void hideSeekPreview();
    QWidget *item(const QString &id); // made on first use
    void updateLabels();
    QWidget *anchor(QWidget *preferred) const;

    Player *m_player;
    IconButton *m_prev, *m_next, *m_volIcon, *m_audio, *m_subs, *m_settings, *m_queue;
    PlayButton *m_play;
    QLabel *m_time, *m_duration;
    SliderBar *m_seek;
    VolumeBar *m_volume;
    class SeekPreviewer *m_previewer;
    class SeekPreviewPopup *m_preview = nullptr;
    QTimer m_seekThrottle;
    qint64 m_pendingSeek = -1;
    bool m_overlay = false;
    class QHBoxLayout *m_layout;
    QWidget *m_volumeBox;
    QHash<QString, QWidget *> m_items;
    QList<QWidget *> m_transient; // this layout's dividers
    QStringList m_shown;
};

// Short message shown over the video ("Resumed at 12:34 · Start over").
class Toast : public QWidget {
    Q_OBJECT
public:
    explicit Toast(QWidget *parent);
    enum Place { Top, Bottom }; // Bottom: just above the playback controls
    void show(const QString &text, const QString &action = QString(), std::function<void()> onAction = {},
              int ms = 3500);
    void showAt(Place place, const QString &text, const QString &action = QString(),
                std::function<void()> onAction = {}, int ms = 3500);
    // The key that triggered the next message ("Pressed C"); empty for none.
    void setKeyHint(const QString &key) { m_keyHint = key; }
    void setBottomMargin(int px) { m_bottomMargin = px; }

private:
    void reposition();
    bool eventFilter(QObject *obj, QEvent *event) override;
    QLabel *m_text, *m_key;
    QString m_keyHint;
    Place m_place = Top;
    int m_bottomMargin = 28;
    class QPushButton *m_action;
    std::function<void()> m_onAction;
    QTimer m_timer;
};
