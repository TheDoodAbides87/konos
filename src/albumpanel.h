#pragma once

#include <QStringList>
#include <QWidget>

class IconButton;

// An album's songs, opened under its tile in the album grid: name and artist
// with play and shuffle, then the songs in one or two columns (number, title,
// length, ⋯), and the year at the bottom. A click on a song adds it to the
// queue; right-click (or ⋯) offers Play Now and Play After.
class AlbumTracksPanel : public QWidget {
    Q_OBJECT
public:
    AlbumTracksPanel(const QString &album, const QString &artist, const QStringList &tracks, QWidget *parent = nullptr);

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    void setNowPlaying(const QString &path);
    int songCount() const { return int(m_songs.size()); }

signals:
    void closeRequested();
    void playAllRequested(const QStringList &tracks, bool shuffle);
    void enqueueRequested(const QString &path);
    void playNowRequested(const QString &path);
    void playAfterRequested(const QString &path);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    struct Song {
        QString path, title;
        int number = 0;
        qint64 ms = 0;
    };
    int columns(int width) const;
    int rowsPerColumn(int width) const;
    QRect songRect(int i) const;
    QRect moreRect(int i) const;
    int songAt(const QPoint &pos) const;
    void showMenu(int i, const QPoint &globalPos);

    QString m_album, m_artist, m_footer, m_now;
    QList<Song> m_songs;
    IconButton *m_play, *m_shuffle, *m_close;
    int m_hover = -1;
};
