#pragma once

#include <QWidget>

class Player;

// "Show Metadata": a card in the top-left of the video with the title, artist
// or album, and the picture and sound details (size, codecs, frame rate).
class MediaInfoCard : public QWidget {
    Q_OBJECT
public:
    MediaInfoCard(Player *player, QWidget *parent);
    void refresh(); // re-read the media's details and resize

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Player *m_player;
    QString m_title, m_subtitle, m_details;
};
