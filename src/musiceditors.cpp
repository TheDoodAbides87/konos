#include "datafile.h"
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
#include <QMessageBox>
#include <QBuffer>
#include <QImage>
#include <QSet>
#include <QSharedPointer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QCollator>
#include <QRegularExpressionValidator>
#include <QDropEvent>
#include <QMimeData>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

using Dialog::button;
using Dialog::field;

namespace {


QLabel *caption(const QString &text)
{
    return Dialog::caption(text, false);
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

const char *const kUserAgent = "Konos/" KONOS_VERSION " ( desktop media player )";

// A request to the MusicBrainz web service. Queued so requests go out at most
// once a second, as MusicBrainz asks (shared by every editor).
void musicBrainz(QObject *context, const QString &path, const QList<QPair<QString, QString>> &query,
                 std::function<void(const QJsonObject &reply, const QString &error)> done, int attempt = 0)
{
    static qint64 nextRequest = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // When it's busy (503, its rate limit), wait longer and ask again.
    const qint64 wait = qMax<qint64>(0, nextRequest - now) + attempt * 1500;
    nextRequest = qMax(now, nextRequest) + 1100 + attempt * 1500;
    QPointer<QObject> self(context);
    QTimer::singleShot(int(wait), context, [self, path, query, done, attempt] {
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
        req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
        req.setRawHeader("Accept", "application/json");
        req.setTransferTimeout(20000);
        QNetworkReply *reply = ArtistImages::instance().network()->get(req);
        QObject::connect(reply, &QNetworkReply::finished, self, [self, reply, path, query, done, attempt] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 503 && attempt < 3)
                musicBrainz(self, path, query, done, attempt + 1);
            else if (status == 503)
                done({}, QObject::tr("MusicBrainz is busy; try again in a moment."));
            else if (reply->error() != QNetworkReply::NoError)
                done({}, QObject::tr("Couldn't reach MusicBrainz (%1).").arg(reply->errorString()));
            else
                done(QJsonDocument::fromJson(reply->readAll()).object(), QString());
        });
    });
}

// A picture from the web (following redirects, as the Cover Art Archive's do).
QNetworkReply *download(const QString &url, int timeoutMs = 60000)
{
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(timeoutMs);
    return ArtistImages::instance().network()->get(req);
}

// A cover from the Cover Art Archive ("…/front"): 1200 pixels across, else
// 500, else the picture as uploaded (older uploads lack the bigger sizes).
void downloadCover(QObject *context, const QString &front, std::function<void(const QByteArray &, const QString &)> done,
                   int step = 0)
{
    static const char *const sizes[] = {"-1200", "-500", ""};
    QNetworkReply *reply = download(front + sizes[step]);
    QObject::connect(reply, &QNetworkReply::finished, context, [context, reply, front, done, step] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError)
            done(reply->readAll(), {});
        else if (status == 404 && step < 2)
            downloadCover(context, front, done, step + 1);
        else
            done({}, QObject::tr("Couldn't download the cover (%1).").arg(reply->errorString()));
    });
}

// Edit Metadata's songs. The number, title and artist columns are places on
// the album; the files are what goes in them. Dragging a row onto another
// moves its file there (the files between shift along), so files whose tags
// got mixed up can be lined up with the right titles. Each row keeps its
// file's path in column 3's UserRole.
class TrackTable : public QTreeWidget {
public:
    TrackTable()
    {
        setObjectName("TrackTable");
        setDragEnabled(true);
        setAcceptDrops(true);
        viewport()->setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::InternalMove);
        setDropIndicatorShown(true);
        setSelectionMode(QAbstractItemView::SingleSelection);
    }

    void moveFile(int from, int to)
    {
        const int n = topLevelItemCount();
        if (from == to || from < 0 || to < 0 || from >= n || to >= n)
            return;
        QList<std::pair<QString, QString>> files; // name, path
        for (int i = 0; i < n; ++i)
            files.append({topLevelItem(i)->text(3), topLevelItem(i)->data(3, Qt::UserRole).toString()});
        files.move(from, to);
        setFiles(files);
        setCurrentItem(topLevelItem(to));
    }

    // Files in the order of their names (01 …, 02 …), for files named right but tagged wrong.
    void orderFilesByName()
    {
        QStringList paths;
        for (int i = 0; i < topLevelItemCount(); ++i)
            paths << topLevelItem(i)->data(3, Qt::UserRole).toString();
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::stable_sort(paths.begin(), paths.end(), [&collator](const QString &a, const QString &b) {
            return collator.compare(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
        });
        QList<std::pair<QString, QString>> files;
        for (const QString &p : paths)
            files.append({QFileInfo(p).fileName(), p});
        setFiles(files);
    }

protected:
    void dropEvent(QDropEvent *e) override
    {
        // The file moves to the row it's dropped on; the rows themselves stay.
        const QModelIndex at = indexAt(e->position().toPoint());
        const int from = currentIndex().row();
        const int to = at.isValid() ? at.row() : topLevelItemCount() - 1;
        e->setDropAction(Qt::IgnoreAction);
        e->accept();
        stopAutoScroll();
        setState(NoState);
        if (e->mimeData()->hasFormat("application/x-qabstractitemmodeldatalist"))
            moveFile(from, to);
        viewport()->update();
    }

private:
    void setFiles(const QList<std::pair<QString, QString>> &files)
    {
        for (int i = 0; i < files.size() && i < topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = topLevelItem(i);
            item->setText(3, files[i].first);
            item->setToolTip(3, files[i].second);
            item->setData(3, Qt::UserRole, files[i].second);
        }
    }
};

// "Edit Metadata", or "Edit Metadata — 3 albums" for songs from several.
QString editorTitle(const QStringList &tracks)
{
    QSet<QString> keys;
    for (const QString &p : tracks) {
        const MusicIndex::Track *t = musicIndex().track(p);
        keys.insert(t ? t->album + '\n' + t->displayAlbumArtist() : p);
    }
    return keys.size() > 1 ? QObject::tr("Edit Metadata — %1 albums").arg(keys.size()) : QObject::tr("Edit Metadata");
}

// The note under editors that write into files.
QWidget *overwriteNote(const QString &text)
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    auto *icon = new QLabel;
    icon->setPixmap(theme().pixmap("info", theme().accent(), 16, w->devicePixelRatioF()));
    icon->setAlignment(Qt::AlignTop);
    h->addWidget(icon);
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    h->addWidget(label, 1);
    return w;
}
} // namespace

bool confirmOverwrite(QWidget *parent, const QString &title, const QString &what, int songs)
{
    QMessageBox box(QMessageBox::Warning, title,
                    (songs == 1 ? QObject::tr("This writes %1 into the song file itself.")
                                : QObject::tr("This writes %1 into all %2 song files themselves.").arg(what).arg(songs))
                        .arg(what),
                    QMessageBox::NoButton, parent);
    box.setInformativeText(QObject::tr("It replaces what's there now and can't be undone."));
    QPushButton *go = box.addButton(QObject::tr("Write to Files"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == go;
}

QString embedAlbumArt(const QStringList &tracks, const QByteArray &image, bool folderCopy)
{
    QImage picture;
    if (!picture.loadFromData(image))
        return QObject::tr("That isn't a picture Konos can read.");
    // JPEG and PNG go in as they are; anything else becomes a JPEG.
    QByteArray data = image, mime = "image/jpeg";
    if (image.startsWith("\x89PNG")) {
        mime = "image/png";
    } else if (!image.startsWith("\xFF\xD8")) {
        data.clear();
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "JPEG", 92);
    }
    QByteArray jpeg = data; // for the .jpg copies
    if (mime != "image/jpeg") {
        jpeg.clear();
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, "JPEG", 92);
    }

    QStringList failed;
    QSet<QString> copies, folders;
    for (const QString &path : tracks) {
        TagLib::FileRef file(QFile::encodeName(path).constData());
        TagLib::VariantMap cover;
        cover.insert("data", TagLib::ByteVector(data.constData(), uint(data.size())));
        cover.insert("mimeType", TagLib::String(mime.constData()));
        cover.insert("pictureType", TagLib::String("Front Cover"));
        cover.insert("description", TagLib::String());
        if (file.isNull() || !file.setComplexProperties("PICTURE", {cover}) || !file.save())
            failed << QFileInfo(path).fileName();
        // What Konos shows is VLC's copy of the cover (in its cache), which
        // VLC wouldn't read again: replace that too.
        for (const QString &art : {library().meta(path).art,
                                   musicIndex().track(path) ? musicIndex().track(path)->art : QString()})
            if (art.contains("/vlc/art/"))
                copies.insert(art);
        folders.insert(QFileInfo(path).absolutePath());
    }
    for (const QString &art : copies)
        DataFile::write(art, jpeg);
    if (folderCopy)
        for (const QString &dir : folders)
            DataFile::write(dir + "/cover.jpg", jpeg); // replaces one there only once it's complete
    for (const QString &p : tracks)
        Thumbnailer::instance().forget(p);
    for (const QString &dir : folders)
        Thumbnailer::instance().forget(dir);
    musicIndex().rescan();
    if (failed.isEmpty())
        return {};
    return QObject::tr("Couldn't write the cover into %1. Check the files aren't read-only.").arg(failed.join(", "));
}

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
    : PanelOverlay(editorTitle(tracks), {1180, 800}, window), m_tracks(tracks)
{
    // Album fields: what the songs share. Where they differ (songs from
    // several albums), the field starts empty and is left alone on saving
    // unless something is typed into it.
    const MusicIndex::Track *first = tracks.isEmpty() ? nullptr : musicIndex().track(tracks.first());
    QStringList albums, albumArtists, genres, years, albumKeys;
    for (const QString &p : tracks) {
        const MusicIndex::Track *t = musicIndex().track(p);
        albums << (t ? t->album : QString());
        // Untagged: the album artist worked out from the album, so saving
        // writes what Konos shows (and groups by).
        albumArtists << (t ? (t->albumArtist.isEmpty() ? t->impliedAlbumArtist : t->albumArtist) : QString());
        genres << (t ? t->genre : QString());
        years << (t && t->year ? QString::number(t->year) : QString());
        albumKeys << (t ? t->album + '\n' + t->displayAlbumArtist() : p);
    }
    albumKeys.removeDuplicates();
    m_oneAlbum = albumKeys.size() <= 1;
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(6);
    m_album = field();
    m_albumArtist = field();
    m_year = field();
    m_year->setMaximumWidth(110);
    m_year->setValidator(new QRegularExpressionValidator(QRegularExpression("\\d{0,4}"), m_year));
    m_genre = field();
    auto fillIn = [this](QLineEdit *f, QStringList values) {
        values.removeDuplicates();
        if (values.size() > 1) {
            f->setPlaceholderText(tr("Several — kept as they are"));
            m_mixed << f;
        } else {
            f->setText(values.value(0));
        }
    };
    fillIn(m_album, albums);
    fillIn(m_albumArtist, albumArtists);
    fillIn(m_genre, genres);
    fillIn(m_year, years);
    if (first && !m_mixed.contains(m_albumArtist) && m_albumArtist->text().isEmpty())
        m_albumArtist->setPlaceholderText(first->displayAlbumArtist());
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
    auto *songsBar = new QHBoxLayout;
    songsBar->addWidget(caption(m_oneAlbum ? tr("Songs — double-click a name to edit it; drag a row to move its file")
                                           : tr("Songs — double-click a name to edit it")), 1);
    auto *byName = button(tr("Order Files by Name"));
    byName->setToolTip(tr("Put the files in the order of their names (01, 02, …), for files named right but tagged wrong"));
    songsBar->addWidget(byName);
    left->addLayout(songsBar);
    auto *table = new TrackTable;
    m_table = table;
    connect(byName, &QPushButton::clicked, this, [table] { table->orderFilesByName(); });
    if (!m_oneAlbum) { // moving files about only makes sense within one album
        byName->hide();
        table->setDragEnabled(false);
        table->setAcceptDrops(false);
    }
    m_table->setRootIsDecorated(false);
    m_table->setColumnCount(4);
    m_table->setHeaderLabels({tr("#"), tr("Title"), tr("Artist"), tr("File")});
    m_table->header()->setStretchLastSection(false);
    m_table->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_table->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_table->setColumnWidth(2, 130);
    m_table->setColumnWidth(3, 220);
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
        item->setData(3, Qt::UserRole, p);
        item->setForeground(3, theme().pal().textMuted);
    }
    connect(m_table, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (column <= 2)
            m_table->editItem(item, column);
    });
    left->addWidget(m_table, 1);
    split->addLayout(left, 4);

    // MusicBrainz: search, pick a release, and the fields fill in (one album
    // at a time, so not for songs from several).
    auto *rightBox = new QWidget;
    auto *right = new QVBoxLayout(rightBox);
    right->setContentsMargins(0, 0, 0, 0);
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
    m_embedCover = new QCheckBox(tr("Embed its cover in the files too"));
    m_embedCover->setEnabled(false);
    right->addWidget(m_embedCover);
    split->addWidget(rightBox, 2);
    if (!m_oneAlbum) {
        rightBox->hide();
        auto *note = Dialog::caption(tr("These songs are from %1 albums. Filling in from MusicBrainz works on one "
                                        "album at a time: open Edit Metadata on a single album for that.")
                                         .arg(albumKeys.size()));
        note->setAlignment(Qt::AlignTop);
        note->setMaximumWidth(300);
        split->addWidget(note, 0);
    }
    body()->addLayout(split, 1);
    body()->addWidget(overwriteNote(tr("Saving writes these tags into the files themselves, replacing the tags they "
                                       "have now (and their cover, if you embed one). This can't be undone.")));

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
    if (m_oneAlbum && !m_query->text().isEmpty())
        QTimer::singleShot(0, this, &AlbumEditorOverlay::searchMusicBrainz);
}

void AlbumEditorOverlay::musicBrainz(const QString &path, const QList<QPair<QString, QString>> &query,
                                     std::function<void(const QJsonObject &, const QString &)> done)
{
    ::musicBrainz(this, path, query, std::move(done));
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
                    // Its cover goes in by choice, ticked when the songs have none yet.
                    const QJsonObject art = release.value("cover-art-archive").toObject();
                    bool hasCover = false;
                    for (const QString &p : m_tracks)
                        if (const auto *t = musicIndex().track(p); t && !t->art.isEmpty())
                            hasCover = true;
                    m_embedCover->setEnabled(art.value("front").toBool());
                    m_embedCover->setChecked(art.value("front").toBool() && !hasCover);
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
    // A cell still being typed in counts, Enter pressed or not.
    for (QLineEdit *editor : m_table->viewport()->findChildren<QLineEdit *>(Qt::FindDirectChildrenOnly))
        QMetaObject::invokeMethod(m_table, "commitData", Q_ARG(QWidget *, editor));
    if (!confirmOverwrite(window(), tr("Edit Metadata"),
                          m_embedCover->isChecked() ? tr("these tags and the cover") : tr("these tags"),
                          int(m_tracks.size())))
        return;
    m_save->setEnabled(false);
    QStringList failed;
    const int rows = m_table->topLevelItemCount();
    for (int i = 0; i < rows; ++i) {
        const QTreeWidgetItem *item = m_table->topLevelItem(i);
        const QString path = item->data(3, Qt::UserRole).toString(); // the file now in this place
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
        // A field the songs differ on stays as each song has it, unless filled in.
        auto putShared = [&](const char *key, QLineEdit *f) {
            if (!m_mixed.contains(f) || !f->text().trimmed().isEmpty())
                put(key, f->text());
        };
        putShared("ALBUM", m_album);
        putShared("ALBUMARTIST", m_albumArtist);
        putShared("DATE", m_year);
        putShared("GENRE", m_genre);
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
    if (!m_embedCover->isChecked() || m_releaseId.isEmpty()) {
        finish();
        return;
    }
    m_status->setText(tr("Downloading the cover…"));
    QPointer<AlbumEditorOverlay> self(this);
    downloadCover(this, "https://coverartarchive.org/release/" + m_releaseId + "/front",
                  [self, finish, tracks = m_tracks](const QByteArray &image, QString error) mutable {
        if (!self)
            return;
        if (error.isEmpty())
            error = embedAlbumArt(tracks, image, false);
        if (!error.isEmpty()) {
            self->m_save->setEnabled(true);
            self->m_status->setText(error);
            return;
        }
        finish();
    });
}

// ---------------------------------------------------------------------------
// AlbumArtOverlay

AlbumArtOverlay::AlbumArtOverlay(const QStringList &tracks, QWidget *window)
    : PanelOverlay(tr("Download Album Art"), {980, 760}, window), m_tracks(tracks)
{
    if (const MusicIndex::Track *t = tracks.isEmpty() ? nullptr : musicIndex().track(tracks.first())) {
        m_album = t->album;
        m_artist = t->albumArtist.isEmpty() ? t->artist : t->albumArtist;
    }
    auto *bar = new QHBoxLayout;
    m_query = field(tr("Album and artist"));
    m_query->setText((m_album + " " + m_artist).trimmed());
    auto *find = button(tr("Search"));
    bar->addWidget(m_query, 1);
    bar->addWidget(find);
    body()->addLayout(bar);

    m_results = new QListWidget;
    m_results->setViewMode(QListView::IconMode);
    m_results->setIconSize({170, 170});
    m_results->setGridSize({196, 250});
    m_results->setResizeMode(QListView::Adjust);
    m_results->setMovement(QListView::Static);
    m_results->setWordWrap(true);
    m_results->setUniformItemSizes(true);
    body()->addWidget(m_results, 1);

    m_folderCopy = new QCheckBox(tr("Also save it as cover.jpg in the album folder"));
    // Ticked when there's a cover.jpg already, so it doesn't go on showing the old one.
    m_folderCopy->setChecked(!tracks.isEmpty() && QFile::exists(QFileInfo(tracks.first()).absolutePath() + "/cover.jpg"));
    body()->addWidget(m_folderCopy);
    body()->addWidget(overwriteNote(tr("The cover is embedded in the song files themselves, replacing any cover "
                                       "they have now. This can't be undone.")));

    auto *foot = new QHBoxLayout;
    auto *fromFile = button(tr("From File…"));
    foot->addWidget(fromFile);
    m_status = new QLabel;
    m_status->setProperty("role", "dim");
    m_status->setWordWrap(true);
    foot->addWidget(m_status, 1);
    auto *cancel = button(tr("Cancel"));
    m_use = button(tr("Embed in Files"), true);
    m_use->setFixedWidth(150);
    m_use->setEnabled(false);
    foot->addWidget(cancel);
    foot->addWidget(m_use);
    body()->addLayout(foot);

    connect(find, &QPushButton::clicked, this, &AlbumArtOverlay::search);
    connect(m_query, &QLineEdit::returnPressed, this, &AlbumArtOverlay::search);
    connect(m_results, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item) { m_use->setEnabled(item && !item->isHidden()); });
    connect(m_results, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { apply(item->data(Qt::UserRole).toString()); });
    connect(m_use, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *item = m_results->currentItem())
            apply(item->data(Qt::UserRole).toString());
    });
    connect(cancel, &QPushButton::clicked, this, &AlbumArtOverlay::close);
    connect(fromFile, &QPushButton::clicked, this, [this] {
        const QString file = FileDialogs::openFile(parentWidget(), tr("Choose a cover for %1").arg(m_album),
                                                   QFileInfo(m_tracks.value(0)).absolutePath(),
                                                   tr("Images (*.jpg *.jpeg *.png *.webp *.bmp)"));
        if (!file.isEmpty())
            apply(file);
    });
    if (!m_query->text().isEmpty())
        QTimer::singleShot(0, this, &AlbumArtOverlay::search);
}

void AlbumArtOverlay::search()
{
    const QString text = m_query->text().trimmed();
    if (text.isEmpty())
        return;
    const int generation = ++m_generation;
    m_results->clear();
    m_use->setEnabled(false);
    m_status->setText(tr("Searching MusicBrainz…"));
    // The album and artist fields when it's still what was filled in, else free text.
    auto quoted = [](QString s) { return "\"" + s.replace('"', ' ') + "\""; };
    QString query = text;
    if (!m_album.isEmpty() && text == (m_album + " " + m_artist).trimmed())
        query = "releasegroup:" + quoted(m_album) + (m_artist.isEmpty() ? QString() : " AND artist:" + quoted(m_artist));
    // Release groups: an album once, however many pressings it had.
    musicBrainz(this, "release-group/", {{"query", query}, {"limit", "24"}},
                [this, generation](const QJsonObject &reply, const QString &error) {
        if (generation != m_generation)
            return;
        if (!error.isEmpty()) {
            m_status->setText(error);
            return;
        }
        // Albums with this very title first (MusicBrainz's order otherwise).
        QJsonArray groups;
        QList<QJsonValue> rest;
        for (const QJsonValue &v : reply.value("release-groups").toArray()) {
            const QJsonObject g = v.toObject();
            if (g.value("primary-type").toString() == "Album" &&
                g.value("title").toString().compare(m_album, Qt::CaseInsensitive) == 0)
                groups.append(v);
            else
                rest << v;
        }
        for (const QJsonValue &v : rest)
            groups.append(v);
        if (groups.isEmpty()) {
            m_status->setText(tr("No albums found. Try different words, or use a file."));
            return;
        }
        m_status->setText(tr("Looking for covers…"));
        QPixmap blank(170, 170);
        blank.fill(theme().tone(QColor(0x22, 0x22, 0x22)));
        auto pending = QSharedPointer<int>::create(int(groups.size()));
        for (const QJsonValue &v : groups) {
            const QJsonObject g = v.toObject();
            const QString id = g.value("id").toString();
            QStringList info;
            if (!g.value("first-release-date").toString().isEmpty())
                info << g.value("first-release-date").toString().left(4);
            if (!g.value("primary-type").toString().isEmpty())
                info << g.value("primary-type").toString();
            auto *item = new QListWidgetItem(QIcon(blank), g.value("title").toString() + "\n" +
                                                               credit(g.value("artist-credit").toArray()) +
                                                               (info.isEmpty() ? QString() : " · " + info.join(" · ")));
            item->setData(Qt::UserRole, "https://coverartarchive.org/release-group/" + id + "/front");
            item->setToolTip(item->text());
            item->setHidden(true); // until its cover turns up (not every album has one)
            m_results->addItem(item);
            QNetworkReply *thumb = download("https://coverartarchive.org/release-group/" + id + "/front-250", 30000);
            connect(thumb, &QNetworkReply::finished, this, [this, thumb, item, generation, pending] {
                thumb->deleteLater();
                if (generation != m_generation)
                    return;
                QPixmap pm;
                if (thumb->error() == QNetworkReply::NoError && pm.loadFromData(thumb->readAll())) {
                    item->setIcon(QIcon(pm));
                    item->setHidden(false);
                }
                int shown = 0;
                for (int i = 0; i < m_results->count(); ++i)
                    shown += !m_results->item(i)->isHidden();
                if (--*pending == 0) {
                    m_status->setText(shown ? tr("Pick the cover that matches your copy.")
                                            : tr("None of these albums has a cover in the Cover Art Archive. "
                                                 "Try different words, or use a file."));
                } else if (shown) {
                    m_status->setText(tr("Pick the cover that matches your copy."));
                }
            });
        }
    });
}

void AlbumArtOverlay::apply(const QString &urlOrFile)
{
    if (urlOrFile.isEmpty() || !confirmOverwrite(window(), tr("Download Album Art"), tr("this cover"), int(m_tracks.size())))
        return;
    m_use->setEnabled(false);
    QPointer<AlbumArtOverlay> self(this);
    auto embed = [self](const QByteArray &image) {
        if (!self)
            return;
        const QString error = embedAlbumArt(self->m_tracks, image, self->m_folderCopy->isChecked());
        if (error.isEmpty()) {
            self->close();
            return;
        }
        self->m_status->setText(error);
        self->m_use->setEnabled(self->m_results->currentItem());
    };
    if (!urlOrFile.startsWith("https://")) {
        QFile f(urlOrFile);
        if (!f.open(QIODevice::ReadOnly)) {
            m_status->setText(tr("Couldn't read %1.").arg(QFileInfo(urlOrFile).fileName()));
            m_use->setEnabled(m_results->currentItem());
            return;
        }
        m_status->setText(tr("Embedding…"));
        embed(f.readAll());
        return;
    }
    m_status->setText(tr("Downloading the cover…"));
    downloadCover(this, urlOrFile, [self, embed](const QByteArray &image, const QString &error) {
        if (!self)
            return;
        if (!error.isEmpty()) {
            self->m_status->setText(error);
            self->m_use->setEnabled(self->m_results->currentItem());
            return;
        }
        self->m_status->setText(tr("Embedding…"));
        embed(image);
    });
}
