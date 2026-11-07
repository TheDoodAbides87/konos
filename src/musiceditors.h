#pragma once

#include <QJsonObject>
#include <QPointer>
#include <QStringList>
#include <QWidget>
#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QVBoxLayout;

// A panel over the window with a dimmed backdrop, like Settings: title, close
// button, Esc or a click outside closes it. Deletes itself when closed.
class PanelOverlay : public QWidget {
    Q_OBJECT
public:
    PanelOverlay(const QString &title, QSize maxSize, QWidget *window);
    void open();
    void close();

protected:
    QVBoxLayout *body() const { return m_body; }
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void layoutPanel();
    QWidget *m_panel;
    QVBoxLayout *m_body;
    QSize m_max;
};

// Music → Artists → right-click → "Choose Artist Image…": pick one of the
// pictures Deezer has for that name (or similar names), or a file.
class ArtistPictureOverlay : public PanelOverlay {
    Q_OBJECT
public:
    ArtistPictureOverlay(const QString &artist, QWidget *window);

private:
    void search();
    void use(const QString &urlOrFile);

    QString m_artist;
    QLineEdit *m_query;
    QListWidget *m_results;
    QLabel *m_status;
    QPushButton *m_use;
    int m_generation = 0;
};

// Music → Albums → right-click → "Edit Album Info…": edit the album's tags,
// optionally filled in from a MusicBrainz release, and write them to the files.
class AlbumEditorOverlay : public PanelOverlay {
    Q_OBJECT
public:
    AlbumEditorOverlay(const QStringList &tracks, QWidget *window);

private:
    void searchMusicBrainz();
    void loadRelease(const QString &id);
    void fill(const QJsonObject &release);
    void save();
    // Queued so requests go out at most once a second, as MusicBrainz asks.
    void musicBrainz(const QString &path, const QList<QPair<QString, QString>> &query,
                     std::function<void(const QJsonObject &reply, const QString &error)> done);

    QStringList m_tracks;
    QLineEdit *m_album, *m_albumArtist, *m_year, *m_genre;
    QTreeWidget *m_table;
    QLineEdit *m_query;
    QListWidget *m_releases;
    QCheckBox *m_saveCover;
    QLabel *m_status;
    QPushButton *m_save;
    QString m_releaseId;
    qint64 m_nextRequest = 0; // earliest time (ms since epoch) for the next request
    int m_generation = 0;
};
