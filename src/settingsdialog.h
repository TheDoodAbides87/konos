#pragma once

#include <QWidget>

class IconButton;
class Player;
class QLabel;
class QLineEdit;
class QStackedWidget;

// Full settings window shown as an overlay over the main window
// (search + App/Media/System navigation on the left, pages on the right).
class SettingsOverlay : public QWidget {
    Q_OBJECT
public:
    enum Page { General, Interface, Privacy, Controls, Playback, Video, Audio, Subtitles, Library,
                Network, Performance, Recording, Extensions, PageCount };

    SettingsOverlay(Player *player, QWidget *parent);
    void open(Page page = General);
    void close();

signals:
    void openUrlRequested(const QString &url);
    void recordToggled();
    void shortcutsRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void setPage(int page);
    void filter(const QString &text);
    void layoutPanel();
    QWidget *wrap(QWidget *content);

    QWidget *pageGeneral();
    QWidget *pageInterface();
    QWidget *pagePrivacy();
    QWidget *pageControls();
    QWidget *pagePlayback();
    QWidget *pageVideo();
    QWidget *pageAudio();
    QWidget *pageSubtitles();
    QWidget *pageLibrary();
    QWidget *pageNetwork();
    QWidget *pagePerformance();
    QWidget *pageRecording();
    QWidget *pageExtensions();

    Player *m_player;
    QWidget *m_panel;
    QLineEdit *m_search;
    QLabel *m_title;
    QStackedWidget *m_stack;
    QList<IconButton *> m_nav;
    QList<QLabel *> m_groupLabels;
    QStringList m_titles;
    QList<QStringList> m_keywords;
    bool m_extensionsLoaded = false;
};
