#pragma once

#include "opensubtitles.h"

#include <QWidget>
#include <functional>

class Dropdown;
class Player;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

// "Add Subtitles": pairs each video in scope (this file, its folder or the play
// queue) with a subtitle from the computer, OpenSubtitles or a link, saves it
// next to the video as "<video>.<lang>.srt" so it loads automatically next
// time, and adds it to what's playing now.
class SubtitleOverlay : public QWidget {
    Q_OBJECT
public:
    enum Scope { ThisFile, ThisFolder, ThisPlaylist };
    enum Source { FromComputer, FromOpenSubtitles, FromLink };

    SubtitleOverlay(Player *player, QWidget *parent);
    // The play queue, for "To This Playlist".
    void setQueueProvider(std::function<QStringList()> provider) { m_queue = std::move(provider); }
    void open(Source source = FromOpenSubtitles);
    void close();

signals:
    void settingsRequested(); // "Add a key in Settings"
    void message(const QString &text);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    struct Row {
        QString video;    // local path, or the location for non-file media
        QString subtitle; // chosen local file (computer) or saved file
        QList<OpenSubtitles::Result> results;
        int chosen = -1;
        enum State { Idle, Searching, Found, Missing, Working, Done, Failed } state = Idle;
        QString note;
        class MatchRow *view = nullptr;
    };

    void layoutPanel();
    QStringList videosInScope() const;
    void rebuild();
    void searchAll();
    void chooseFiles();
    void apply();
    void downloadNext();
    void fetchLink();
    bool save(Row &row, const QByteArray &data, const QString &fileName);
    void updateRow(int i);
    void updateFooter();
    void pickAlternative(int i);
    QString language() const;
    OpenSubtitles::Query queryFor(const QString &video) const;
    QString defaultSearchText() const;

    Player *m_player;
    std::function<QStringList()> m_queue;
    QWidget *m_panel;
    Dropdown *m_scope, *m_source, *m_language;
    QLineEdit *m_search;
    QWidget *m_list;
    QVBoxLayout *m_rows;
    QLabel *m_status;
    QPushButton *m_action;
    QPushButton *m_settingsLink;
    QList<Row> m_items;
    QString m_autoSearch; // what the search box was filled with, to tell user edits apart
    int m_generation = 0; // bumps on every rebuild; stale replies are ignored
    int m_downloading = -1;
    bool m_opening = false;
};
