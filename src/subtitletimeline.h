#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

class Player;

// A strip above the controls showing when subtitle lines appear around the
// current moment, over the sound's waveform. Dragging it sideways moves the
// subtitles earlier or later (the subtitle delay), so they can be lined up
// with what's said without typing numbers. Scroll to zoom.
class SubtitleTimeline : public QWidget {
    Q_OBJECT
public:
    SubtitleTimeline(Player *player, QWidget *parent);
    void refresh(); // the subtitle track or media changed

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    qint64 now() const; // playback time, smoothed between player updates
    QRectF lane() const;
    QRectF resetRect() const;

    Player *m_player;
    QString m_key;    // SubtitleCues key of the selected track, empty for none
    QString m_status; // shown when there are no cues to draw
    qint64 m_span = 20000; // ms across the strip
    qint64 m_time = 0;
    QElapsedTimer m_since; // since m_time was reported
    QTimer m_tick;
    bool m_dragging = false;
    int m_dragX = 0;
    qint64 m_dragDelay = 0;
};
