#pragma once

#include <QWidget>

class IconButton;
class Player;
class QLabel;
class QVBoxLayout;

// The player in a corner of the library while something plays: the video
// itself (or the cover), what's playing, and buttons to go back to the player
// or put it away until the next item. Drag it by its bottom strip to another
// corner. The video widget is lent to it by the main window.
class MiniPlayer : public QWidget {
    Q_OBJECT
public:
    MiniPlayer(Player *player, QWidget *parent);
    QVBoxLayout *videoHost() const { return m_hostLayout; }
    void place(); // in its corner of the parent

signals:
    void expandRequested();
    void closeRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void updateText();

    Player *m_player;
    QWidget *m_host;
    QVBoxLayout *m_hostLayout;
    QWidget *m_strip;
    QLabel *m_title, *m_artist;
    IconButton *m_expand, *m_close;
    QPoint m_dragFrom; // pointer position in this widget when a drag began
    bool m_dragging = false;
};
