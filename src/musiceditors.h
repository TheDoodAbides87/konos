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

// Writing to an album's files can't be undone: asks first, naming what gets
// replaced. True to go ahead.
bool confirmOverwrite(QWidget *parent, const QString &title, const QString &what, int songs);

// Puts a cover picture into each song file, replacing any cover there, and
// refreshes the copy Konos shows so it changes at once. folderCopy also saves
// it as cover.jpg beside the songs. Returns an error to show, or nothing.
QString embedAlbumArt(const QStringList &tracks, const QByteArray &image, bool folderCopy);

// Music → album → right-click → Edit → "Edit Metadata…": edit the album's
// tags, optionally filled in from a MusicBrainz release (and its cover), and
// write them into the files.
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
    QCheckBox *m_embedCover;
    QList<QLineEdit *> m_mixed; // album fields the songs differ on
    bool m_oneAlbum = true;
    QLabel *m_status;
    QPushButton *m_save;
    QString m_releaseId;
    int m_generation = 0;
};

// Music → album → right-click → Edit → "Download Album Art…": covers from the
// Cover Art Archive (found through MusicBrainz), or a picture file, embedded
// into the album's songs.
class AlbumArtOverlay : public PanelOverlay {
    Q_OBJECT
public:
    AlbumArtOverlay(const QStringList &tracks, QWidget *window);

private:
    void search();
    void apply(const QString &urlOrFile);

    QStringList m_tracks;
    QString m_album, m_artist;
    QLineEdit *m_query;
    QListWidget *m_results;
    QCheckBox *m_folderCopy;
    QLabel *m_status;
    QPushButton *m_use;
    int m_generation = 0;
};
