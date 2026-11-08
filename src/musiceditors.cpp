#include "musiceditors.h"
#include "artistimages.h"
#include "filedialogs.h"
#include "library.h"
#include "musicindex.h"
#include "theme.h"
#include "version.h"
#include "widgets.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

namespace {
QPushButton *button(const QString &text, bool accent = false)
{
    auto *b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(36);
    if (accent) {
        b->setProperty("role", "accent");
        b->style()->unpolish(b);
        b->style()->polish(b);
    }
    return b;
}

QLineEdit *field(const QString &placeholder = QString())
{
    auto *e = new QLineEdit;
    e->setFixedHeight(36);
    e->setPlaceholderText(placeholder);
    return e;
}

QLabel *caption(const QString &text)
{
    auto *l = new QLabel(text);
    l->setProperty("role", "dim");
    return l;
}

// "Bathory", "Simon & Garfunkel", "A feat. B" from a MusicBrainz artist-credit.
QString credit(const QJsonArray &credits)
{
    QString out;
    for (const QJsonValue &v : credits)
        out += v.toObject().value("name").toString() + v.toObject().value("joinphrase").toString();
    return out.trimmed();
}

TagLib::String tl(const QString &s) { return TagLib::String(s.toStdString(), TagLib::String::UTF8); }
} // namespace

// ---------------------------------------------------------------------------
// PanelOverlay

PanelOverlay::PanelOverlay(const QString &title, QSize maxSize, QWidget *window)
    : QWidget(window), m_max(maxSize)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);
    window->installEventFilter(this);
    m_panel = new QWidget(this);
    m_panel->setObjectName("SettingsPanel");
    m_panel->setAttribute(Qt::WA_StyledBackground);
    auto *v = new QVBoxLayout(m_panel);
    v->setContentsMargins(20, 14, 20, 20);
    v->setSpacing(14);
    auto *head = new QHBoxLayout;
    auto *label = new QLabel(title);
    label->setProperty("role", "title");
    head->addWidget(label, 1);
    auto *closeButton = new IconButton("close");
    closeButton->setToolTip(tr("Close (Esc)"));
    connect(closeButton, &IconButton::clicked, this, &PanelOverlay::close);
    head->addWidget(closeButton);
    v->addLayout(head);
    m_body = new QVBoxLayout;
    m_body->setSpacing(12);
    v->addLayout(m_body, 1);
    hide();
}

void PanelOverlay::open()
{
    layoutPanel();
    show();
    raise();
    setFocus();
}

void PanelOverlay::close()
{
    if (parentWidget())
        parentWidget()->setFocus();
    QWidget::close();
}

void PanelOverlay::layoutPanel()
{
    setGeometry(parentWidget()->rect());
    const int w = qMin(m_max.width(), width() - 60);
    const int h = qMin(m_max.height(), height() - 60);
    m_panel->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);
}

bool PanelOverlay::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == parentWidget() && event->type() == QEvent::Resize && isVisible())
        layoutPanel();
    return QWidget::eventFilter(obj, event);
}

void PanelOverlay::paintEvent(QPaintEvent *)
{
    QPainter(this).fillRect(rect(), QColor(0, 0, 0, 150));
}

void PanelOverlay::mousePressEvent(QMouseEvent *e)
{
    if (!m_panel->geometry().contains(e->position().toPoint()))
        close();
}

void PanelOverlay::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------------------
// ArtistPictureOverlay

ArtistPictureOverlay::ArtistPictureOverlay(const QString &artist, QWidget *window)
    : PanelOverlay(tr("Artist Image — %1").arg(artist), {900, 680}, window), m_artist(artist)
{
    auto *bar = new QHBoxLayout;
    m_query = field(tr("Artist name"));
    m_query->setText(artist);
    auto *find = button(tr("Search"));
    bar->addWidget(m_query, 1);
    bar->addWidget(find);
    body()->addLayout(bar);

    m_results = new QListWidget;
    m_results->setViewMode(QListView::IconMode);
    m_results->setIconSize({150, 150});
    m_results->setGridSize({176, 206});
    m_results->setResizeMode(QListView::Adjust);
    m_results->setMovement(QListView::Static);
    m_results->setWordWrap(true);
    m_results->setUniformItemSizes(true);
    body()->addWidget(m_results, 1);

    auto *foot = new QHBoxLayout;
    auto *fromFile = button(tr("From File…"));
    foot->addWidget(fromFile);
    if (ArtistImages::instance().hasChoice(artist)) {
        auto *automatic = button(tr("Automatic"));
        automatic->setToolTip(tr("Forget the picture you chose and find one again"));
        connect(automatic, &QPushButton::clicked, this, [this] {
            ArtistImages::instance().resetChoice(m_artist);
            close();
        });
        foot->addWidget(automatic);
    }
    m_status = new QLabel;
    m_status->setProperty("role", "dim");
    foot->addWidget(m_status, 1);
    m_use = button(tr("Use Picture"), true);
    m_use->setEnabled(false);
    foot->addWidget(m_use);
    body()->addLayout(foot);

    connect(find, &QPushButton::clicked, this, &ArtistPictureOverlay::search);
    connect(m_query, &QLineEdit::returnPressed, this, &ArtistPictureOverlay::search);
    connect(m_results, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item) { m_use->setEnabled(item); });
    connect(m_results, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { use(item->data(Qt::UserRole).toString()); });
    connect(m_use, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *item = m_results->currentItem())
            use(item->data(Qt::UserRole).toString());
    });
    connect(fromFile, &QPushButton::clicked, this, [this] {
        const QString file = FileDialogs::openFile(parentWidget(), tr("Choose a picture of %1").arg(m_artist), QDir::homePath(),
                                                   tr("Images (*.jpg *.jpeg *.png *.webp *.bmp)"));
        if (!file.isEmpty())
            use(file);
    });
    QTimer::singleShot(0, this, &ArtistPictureOverlay::search);
}

void ArtistPictureOverlay::search()
{
    const QString query = m_query->text().trimmed();
    if (query.isEmpty())
        return;
    const int generation = ++m_generation;
    m_results->clear();
    m_use->setEnabled(false);
    m_status->setText(tr("Searching Deezer…"));
    ArtistImages::instance().search(query, [this, generation](const QList<ArtistImages::Candidate> &found,
                                                              const QString &error) {
        if (generation != m_generation)
            return;
        if (!error.isEmpty()) {
            m_status->setText(error);
            return;
        }
        m_status->setText(found.isEmpty() ? tr("No pictures found. Try another spelling, or use a file.")
                                          : tr("Pick the right picture."));
        QPixmap blank(150, 150);
        blank.fill(theme().tone(QColor(0x22, 0x22, 0x22)));
        for (const auto &c : found) {
            auto *item = new QListWidgetItem(QIcon(blank), c.fans > 0 ? tr("%1\n%2 fans").arg(c.name, QLocale().toString(c.fans))
                                                                      : c.name);
            item->setData(Qt::UserRole, c.picture);
            item->setToolTip(c.name);
            m_results->addItem(item);
            // Thumbnails arrive one by one.
            QNetworkReply *reply = ArtistImages::instance().network()->get(QNetworkRequest(QUrl(c.thumb)));
            const int row = m_results->count() - 1;
            connect(reply, &QNetworkReply::finished, this, [this, reply, row, generation] {
                reply->deleteLater();
                QPixmap pm;
                if (generation == m_generation && pm.loadFromData(reply->readAll()) && row < m_results->count())
                    m_results->item(row)->setIcon(QIcon(pm));
            });
        }
    });
}

void ArtistPictureOverlay::use(const QString &urlOrFile)
{
    if (urlOrFile.isEmpty())
        return;
    m_use->setEnabled(false);
    m_status->setText(tr("Saving…"));
    QPointer<ArtistPictureOverlay> self(this);
    ArtistImages::instance().choose(m_artist, urlOrFile, [self](const QString &error) {
        if (!self)
            return;
        if (error.isEmpty()) {
            self->close();
            return;
        }
        self->m_status->setText(error);
        self->m_use->setEnabled(self->m_results->currentItem());
    });
}

// ---------------------------------------------------------------------------
// AlbumEditorOverlay

AlbumEditorOverlay::AlbumEditorOverlay(const QStringList &tracks, QWidget *window)
    : PanelOverlay(tr("Edit Album Info"), {1180, 780}, window), m_tracks(tracks)
{
    // Album fields, filled from the first song's tags.
    const MusicIndex::Track *first = tracks.isEmpty() ? nullptr : musicIndex().track(tracks.first());
    int year = 0;
    for (const QString &p : tracks)
        if (const auto *t = musicIndex().track(p); t && t->year && (!year || t->year < year))
            year = t->year;
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(6);
    m_album = field();
    m_albumArtist = field();
    m_year = field();
    m_year->setMaximumWidth(110);
    m_year->setInputMask("9999");
    m_genre = field();
    if (first) {
        m_album->setText(first->album);
        m_albumArtist->setText(first->albumArtist);
        m_albumArtist->setPlaceholderText(first->artist);
        m_genre->setText(first->genre);
    }
    if (year)
        m_year->setText(QString::number(year));
    grid->addWidget(caption(tr("Album")), 0, 0);
    grid->addWidget(caption(tr("Album artist")), 0, 1);
    grid->addWidget(caption(tr("Year")), 0, 2);
    grid->addWidget(caption(tr("Genre")), 0, 3);
    grid->addWidget(m_album, 1, 0);
    grid->addWidget(m_albumArtist, 1, 1);
    grid->addWidget(m_year, 1, 2);
    grid->addWidget(m_genre, 1, 3);
    grid->setColumnStretch(0, 3);
    grid->setColumnStretch(1, 3);
    grid->setColumnStretch(3, 2);
    body()->addLayout(grid);

    auto *split = new QHBoxLayout;
    split->setSpacing(18);

    // Songs: number, title, artist (double-click to edit), file.
    auto *left = new QVBoxLayout;
    left->addWidget(caption(tr("Songs — double-click to edit")));
    m_table = new QTreeWidget;
    m_table->setObjectName("TrackTable");
    m_table->setRootIsDecorated(false);
    m_table->setColumnCount(4);
    m_table->setHeaderLabels({tr("#"), tr("Title"), tr("Artist"), tr("File")});
    m_table->header()->setStretchLastSection(false);
    m_table->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_table->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_table->setColumnWidth(2, 140);
    m_table->setColumnWidth(3, 110);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (const QString &p : tracks) {
        const MusicIndex::Track *t = musicIndex().track(p);
        auto *item = new QTreeWidgetItem(m_table);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setText(0, t && t->number ? QString::number(t->number) : QString());
        item->setText(1, t ? t->displayTitle() : QFileInfo(p).completeBaseName());
        item->setText(2, t ? t->artist : QString());
        item->setText(3, QFileInfo(p).fileName());
        item->setToolTip(3, p);
        item->setForeground(3, theme().pal().textMuted);
    }
    connect(m_table, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (column <= 2)
            m_table->editItem(item, column);
    });
    left->addWidget(m_table, 1);
    split->addLayout(left, 4);

    // MusicBrainz: search, pick a release, and the fields fill in.
    auto *right = new QVBoxLayout;
    right->addWidget(caption(tr("Fill in from MusicBrainz")));
    auto *bar = new QHBoxLayout;
    m_query = field(tr("Album and artist"));
    m_query->setText((m_album->text() + " " + (m_albumArtist->text().isEmpty() ? (first ? first->artist : QString())
                                                                               : m_albumArtist->text()))
                         .trimmed());
    auto *find = button(tr("Search"));
    bar->addWidget(m_query, 1);
    bar->addWidget(find);
    right->addLayout(bar);
    m_releases = new QListWidget;
    m_releases->setWordWrap(true);
    right->addWidget(m_releases, 1);
    m_saveCover = new QCheckBox(tr("Save its cover as cover.jpg in the album folder"));
    m_saveCover->setEnabled(false);
    right->addWidget(m_saveCover);
    split->addLayout(right, 2);
    body()->addLayout(split, 1);

    auto *foot = new QHBoxLayout;
    m_status = new QLabel(tracks.size() == 1 ? tr("1 song") : tr("%1 songs").arg(tracks.size()));
    m_status->setProperty("role", "dim");
    m_status->setWordWrap(true);
    foot->addWidget(m_status, 1);
    auto *cancel = button(tr("Cancel"));
    m_save = button(tr("Save to Files"), true);
    foot->addWidget(cancel);
    foot->addWidget(m_save);
    body()->addLayout(foot);

    connect(find, &QPushButton::clicked, this, &AlbumEditorOverlay::searchMusicBrainz);
    connect(m_query, &QLineEdit::returnPressed, this, &AlbumEditorOverlay::searchMusicBrainz);
    connect(m_releases, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { loadRelease(item->data(Qt::UserRole).toString()); });
    connect(cancel, &QPushButton::clicked, this, &AlbumEditorOverlay::close);
    connect(m_save, &QPushButton::clicked, this, &AlbumEditorOverlay::save);
    if (!m_query->text().isEmpty())
        QTimer::singleShot(0, this, &AlbumEditorOverlay::searchMusicBrainz);
}

void AlbumEditorOverlay::musicBrainz(const QString &path, const QList<QPair<QString, QString>> &query,
                                     std::function<void(const QJsonObject &, const QString &)> done)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 wait = qMax<qint64>(0, m_nextRequest - now);
    m_nextRequest = qMax(now, m_nextRequest) + 1100;
    QPointer<AlbumEditorOverlay> self(this);
    QTimer::singleShot(int(wait), this, [self, path, query, done] {
        if (!self)
            return;
        QUrl url("https://musicbrainz.org/ws/2/" + path);
        QUrlQuery q;
        for (const auto &kv : query)
            q.addQueryItem(kv.first, kv.second);
        q.addQueryItem("fmt", "json");
        url.setQuery(q);
        QNetworkRequest req(url);
        // MusicBrainz asks every program to name itself.
        req.setHeader(QNetworkRequest::UserAgentHeader, "Konos/" KONOS_VERSION " ( desktop media player )");
        req.setRawHeader("Accept", "application/json");
        req.setTransferTimeout(20000);
        QNetworkReply *reply = ArtistImages::instance().network()->get(req);
        QObject::connect(reply, &QNetworkReply::finished, self, [reply, done] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 503)
                done({}, tr("MusicBrainz is busy; try again in a moment."));
            else if (reply->error() != QNetworkReply::NoError)
                done({}, tr("Couldn't reach MusicBrainz (%1).").arg(reply->errorString()));
            else
                done(QJsonDocument::fromJson(reply->readAll()).object(), QString());
        });
    });
}

void AlbumEditorOverlay::searchMusicBrainz()
{
    const QString text = m_query->text().trimmed();
    if (text.isEmpty())
        return;
    const int generation = ++m_generation;
    m_releases->clear();
    m_status->setText(tr("Searching MusicBrainz…"));
    // Search the album and artist fields when both are known, else free text.
    QString query = text;
    const QString album = m_album->text().trimmed(), artist = m_albumArtist->text().trimmed().isEmpty()
                                                                  ? m_albumArtist->placeholderText().trimmed()
                                                                  : m_albumArtist->text().trimmed();
    auto quoted = [](QString s) { return "\"" + s.replace('"', ' ') + "\""; };
    if (!album.isEmpty() && text == (album + " " + artist).trimmed())
        query = "release:" + quoted(album) + (artist.isEmpty() ? QString() : " AND artist:" + quoted(artist));
    musicBrainz("release/", {{"query", query}, {"limit", "25"}}, [this, generation](const QJsonObject &reply,
                                                                                  const QString &error) {
        if (generation != m_generation)
            return;
        if (!error.isEmpty()) {
            m_status->setText(error);
            return;
        }
        const QJsonArray releases = reply.value("releases").toArray();
        for (const QJsonValue &v : releases) {
            const QJsonObject r = v.toObject();
            QStringList formats;
            for (const QJsonValue &m : r.value("media").toArray())
                if (!m.toObject().value("format").toString().isEmpty())
                    formats << m.toObject().value("format").toString();
            formats.removeDuplicates();
            QStringList info;
            if (!r.value("date").toString().isEmpty())
                info << r.value("date").toString().left(4);
            if (!r.value("country").toString().isEmpty())
                info << r.value("country").toString();
            if (!formats.isEmpty())
                info << formats.join(" + ");
            info << (r.value("track-count").toInt() == 1 ? tr("1 track") : tr("%1 tracks").arg(r.value("track-count").toInt()));
            auto *item = new QListWidgetItem(r.value("title").toString() + " — " + credit(r.value("artist-credit").toArray()) +
                                             "\n" + info.join(" · "));
            item->setData(Qt::UserRole, r.value("id").toString());
            m_releases->addItem(item);
        }
        m_status->setText(releases.isEmpty() ? tr("No releases found. Try different words.")
                                             : tr("Pick the release that matches your copy (%1 songs here).")
                                                   .arg(m_tracks.size()));
    });
}

void AlbumEditorOverlay::loadRelease(const QString &id)
{
    if (id.isEmpty())
        return;
    const int generation = ++m_generation;
    m_status->setText(tr("Loading the release…"));
    musicBrainz("release/" + id, {{"inc", "recordings+artist-credits+genres+release-groups"}},
                [this, generation, id](const QJsonObject &release, const QString &error) {
                    if (generation != m_generation)
                        return;
                    if (!error.isEmpty()) {
                        m_status->setText(error);
                        return;
                    }
                    m_releaseId = id;
                    const QJsonObject art = release.value("cover-art-archive").toObject();
                    m_saveCover->setEnabled(art.value("front").toBool());
                    m_saveCover->setChecked(art.value("front").toBool() &&
                                            !QFile::exists(QFileInfo(m_tracks.value(0)).absolutePath() + "/cover.jpg"));
                    fill(release);
                });
}

void AlbumEditorOverlay::fill(const QJsonObject &release)
{
    m_album->setText(release.value("title").toString());
    m_albumArtist->setText(credit(release.value("artist-credit").toArray()));
    // The original release year, not this pressing's.
    const QString date = release.value("release-group").toObject().value("first-release-date").toString();
    const QString year = (date.isEmpty() ? release.value("date").toString() : date).left(4);
    if (!year.isEmpty())
        m_year->setText(year);
    QJsonArray genres = release.value("genres").toArray();
    if (genres.isEmpty())
        genres = release.value("release-group").toObject().value("genres").toArray();
    int best = -1;
    for (const QJsonValue &g : genres)
        if (g.toObject().value("count").toInt() > best) {
            best = g.toObject().value("count").toInt();
            m_genre->setText(g.toObject().value("name").toString());
        }

    // All discs' tracks in order.
    struct Song {
        int position;
        QString title, artist;
    };
    QList<Song> songs;
    for (const QJsonValue &m : release.value("media").toArray())
        for (const QJsonValue &t : m.toObject().value("tracks").toArray()) {
            const QJsonObject o = t.toObject();
            songs.append({int(songs.size()) + 1, o.value("title").toString(), credit(o.value("artist-credit").toArray())});
        }
    // Same number of songs: match in order. Otherwise by track number.
    const int rows = m_table->topLevelItemCount();
    int matched = 0;
    for (int i = 0; i < rows; ++i) {
        QTreeWidgetItem *item = m_table->topLevelItem(i);
        int at = -1;
        if (songs.size() == rows)
            at = i;
        else if (const int n = item->text(0).toInt(); n >= 1 && n <= songs.size())
            at = n - 1;
        if (at < 0)
            continue;
        item->setText(0, QString::number(songs[at].position));
        item->setText(1, songs[at].title);
        item->setText(2, songs[at].artist);
        ++matched;
    }
    m_status->setText(songs.size() == rows
                          ? tr("Filled in from MusicBrainz. Check it over, then Save to Files.")
                          : tr("This release has %1 tracks and you have %2; matched %3 by track number. Check it over.")
                                .arg(songs.size())
                                .arg(rows)
                                .arg(matched));
}

void AlbumEditorOverlay::save()
{
    m_save->setEnabled(false);
    QStringList failed;
    const int rows = m_table->topLevelItemCount();
    for (int i = 0; i < rows && i < m_tracks.size(); ++i) {
        const QString path = m_tracks[i];
        const QTreeWidgetItem *item = m_table->topLevelItem(i);
        TagLib::FileRef file(QFile::encodeName(path).constData());
        if (file.isNull() || !file.tag()) {
            failed << QFileInfo(path).fileName();
            continue;
        }
        TagLib::PropertyMap props = file.properties();
        auto put = [&props](const char *key, const QString &value) {
            if (value.trimmed().isEmpty())
                props.erase(key);
            else
                props.replace(key, TagLib::StringList(tl(value.trimmed())));
        };
        put("TITLE", item->text(1));
        put("ARTIST", item->text(2));
        put("TRACKNUMBER", item->text(0));
        put("ALBUM", m_album->text());
        put("ALBUMARTIST", m_albumArtist->text());
        put("DATE", m_year->text());
        put("GENRE", m_genre->text());
        if (!m_releaseId.isEmpty())
            put("MUSICBRAINZ_ALBUMID", m_releaseId);
        file.setProperties(props);
        if (!file.save())
            failed << QFileInfo(path).fileName();
    }

    auto finish = [this, failed] {
        musicIndex().rescan(); // the files changed, so they're read again
        if (failed.isEmpty()) {
            close();
            return;
        }
        m_save->setEnabled(true);
        m_status->setText(tr("Couldn't write to %1. Check the files aren't read-only.").arg(failed.join(", ")));
    };
    if (!m_saveCover->isChecked() || m_releaseId.isEmpty()) {
        finish();
        return;
    }
    m_status->setText(tr("Downloading the cover…"));
    QNetworkRequest req(QUrl("https://coverartarchive.org/release/" + m_releaseId + "/front-500"));
    req.setHeader(QNetworkRequest::UserAgentHeader, "Konos/" KONOS_VERSION " ( desktop media player )");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);
    QNetworkReply *reply = ArtistImages::instance().network()->get(req);
    QPointer<AlbumEditorOverlay> self(this);
    connect(reply, &QNetworkReply::finished, this, [self, reply, finish, tracks = m_tracks] {
        reply->deleteLater();
        if (!self)
            return;
        if (reply->error() != QNetworkReply::NoError)
            qWarning() << "cover download failed:" << reply->error() << reply->errorString();
        if (reply->error() == QNetworkReply::NoError) {
            QFile cover(QFileInfo(tracks.value(0)).absolutePath() + "/cover.jpg");
            if (cover.open(QIODevice::WriteOnly | QIODevice::Truncate))
                cover.write(reply->readAll());
            for (const QString &p : tracks)
                Thumbnailer::instance().forget(p);
        }
        finish();
    });
}
