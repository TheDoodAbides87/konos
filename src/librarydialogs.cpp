#include "librarydialogs.h"
#include "durations.h"
#include "filedialogs.h"
#include "library.h"
#include "musicindex.h"
#include "netfeeds.h"
#include "prefs.h"
#include "showpresets.h"
#include "theme.h"
#include "widgets.h"

#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyle>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

using Dialog::button;
using Dialog::caption;
using Dialog::field;
using FileDialogs::mediaFilter;
namespace {




QString lengthText(qint64 ms)
{
    if (ms <= 0)
        return {};
    const qint64 s = ms / 1000;
    return s >= 3600 ? QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                     : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QStringList urlsFrom(const QMimeData *mime)
{
    QStringList out;
    for (const QUrl &u : mime->urls())
        out << (u.isLocalFile() ? u.toLocalFile() : u.toString());
    return out;
}
} // namespace

// ---------------------------------------------------------------------------
// NewPlaylistOverlay

NewPlaylistOverlay::NewPlaylistOverlay(const QStringList &items, const QString &suggestedName, QWidget *window)
    : PanelOverlay(tr("New Playlist"), {620, 640}, window)
{
    body()->addWidget(caption(tr("Name")));
    m_name = field(tr("New playlist"));
    m_name->setText(suggestedName);
    body()->addWidget(m_name);

    auto *head = new QHBoxLayout;
    m_count = caption(QString());
    head->addWidget(m_count, 1);
    auto *addFiles = button(tr("Add Files…"));
    auto *addFolder = button(tr("Add Folder…"));
    auto *remove = button(tr("Remove"));
    head->addWidget(addFiles);
    head->addWidget(addFolder);
    head->addWidget(remove);
    body()->addLayout(head);

    m_list = new QListWidget;
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragDropMode(QAbstractItemView::InternalMove); // reorder
    m_list->setAcceptDrops(true);
    m_list->viewport()->setAcceptDrops(true);
    m_list->viewport()->installEventFilter(this);
    body()->addWidget(m_list, 1);
    body()->addWidget(caption(tr("Drag files or folders here from the library or your file manager.")));

    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *cancel = button(tr("Cancel"));
    m_create = button(tr("Create Playlist"), true);
    foot->addWidget(cancel);
    foot->addWidget(m_create);
    body()->addLayout(foot);

    connect(addFiles, &QPushButton::clicked, this, [this] {
        addItems(FileDialogs::openFiles(this, tr("Add to playlist"),
                                        QStandardPaths::writableLocation(QStandardPaths::MusicLocation), mediaFilter()));
    });
    connect(addFolder, &QPushButton::clicked, this, [this] {
        const QString dir = FileDialogs::existingDirectory(this, tr("Add folder to playlist"));
        if (!dir.isEmpty())
            addItems(library().mediaInFolder(dir));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        qDeleteAll(m_list->selectedItems());
        updateCount();
    });
    connect(cancel, &QPushButton::clicked, this, &PanelOverlay::close);
    connect(m_create, &QPushButton::clicked, this, &NewPlaylistOverlay::create);
    connect(m_name, &QLineEdit::returnPressed, this, &NewPlaylistOverlay::create);
    addItems(items);
    m_name->setFocus();
}

void NewPlaylistOverlay::addItems(const QStringList &items)
{
    for (const QString &loc : library().expand(items)) {
        auto *it = new QListWidgetItem(theme().icon(Library::kindFor(loc) == EntryKind::Audio ? "note" : Library::isUrl(loc) ? "stream" : "film",
                                                    theme().pal().textDim),
                                       library().streamName(loc).isEmpty() ? Library::displayName(loc) : library().streamName(loc));
        it->setData(Qt::UserRole, loc);
        it->setToolTip(loc);
        m_list->addItem(it);
    }
    updateCount();
}

void NewPlaylistOverlay::updateCount()
{
    const int n = m_list->count();
    m_count->setText(n == 0 ? tr("No files yet") : n == 1 ? tr("1 file") : tr("%1 files").arg(n));
}

bool NewPlaylistOverlay::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_list->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto *e = static_cast<QDragMoveEvent *>(event);
            if (e->source() != m_list && e->mimeData()->hasUrls()) {
                e->setDropAction(Qt::CopyAction);
                e->accept();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto *e = static_cast<QDropEvent *>(event);
            if (e->source() != m_list && e->mimeData()->hasUrls()) {
                addItems(urlsFrom(e->mimeData()));
                e->setDropAction(Qt::CopyAction);
                e->accept();
                return true;
            }
        }
    }
    return PanelOverlay::eventFilter(obj, event);
}

void NewPlaylistOverlay::create()
{
    QString name = m_name->text().trimmed();
    if (name.isEmpty())
        name = tr("New playlist");
    QStringList items;
    for (int i = 0; i < m_list->count(); ++i)
        items << m_list->item(i)->data(Qt::UserRole).toString();
    const QString created = library().createPlaylist(name);
    if (!items.isEmpty())
        library().addToPlaylist(created, items);
    close();
}

// ---------------------------------------------------------------------------
// NetworkStreamOverlay

NetworkStreamOverlay::NetworkStreamOverlay(QWidget *window, OpenFn open, BrowseFn browse)
    : PanelOverlay(tr("Open Network Stream"), {640, 520}, window), m_open(std::move(open)), m_browse(std::move(browse))
{
    body()->addWidget(caption(tr("Addresses, one per line: http(s), rtsp, rtmp, udp, smb, ftp, … A link to an M3U "
                                 "channel list opens its channels; a podcast feed opens its episodes.")));
    m_urls = new QPlainTextEdit;
    m_urls->setPlaceholderText("https://example.com/stream.m3u8\nhttps://example.com/channels.m3u");
    m_urls->setTabChangesFocus(true);
    body()->addWidget(m_urls, 1);
    body()->addWidget(caption(tr("Save as playlist (optional)")));
    m_playlist = field(tr("Playlist name"));
    body()->addWidget(m_playlist);

    auto *foot = new QHBoxLayout;
    m_status = caption(QString());
    foot->addWidget(m_status, 1);
    m_queue = button(tr("Add to Queue"));
    m_play = button(tr("Play"), true);
    foot->addWidget(m_queue);
    foot->addWidget(m_play);
    body()->addLayout(foot);

    connect(m_play, &QPushButton::clicked, this, [this] { go(true); });
    connect(m_queue, &QPushButton::clicked, this, [this] { go(false); });
    connect(m_playlist, &QLineEdit::returnPressed, this, [this] { go(true); });
    m_urls->setFocus();
}

void NetworkStreamOverlay::setUrls(const QStringList &urls)
{
    m_urls->setPlainText(urls.join('\n'));
    m_status->setText(tr("From the clipboard. Check the addresses, then play them."));
    m_play->setFocus();
}

void NetworkStreamOverlay::go(bool play)
{
    QStringList urls;
    for (const QString &line : m_urls->toPlainText().split('\n')) {
        const QString u = line.trimmed();
        if (u.isEmpty())
            continue;
        if (!Library::isUrl(u)) {
            m_status->setText(tr("“%1” isn't a network address.").arg(u.left(60)));
            return;
        }
        urls << u;
    }
    if (urls.isEmpty()) {
        m_status->setText(tr("Enter an address first."));
        return;
    }
    const QString playlist = m_playlist->text().trimmed();
    auto finish = [this, urls, playlist, play] {
        for (const QString &u : urls)
            library().addStream(u);
        if (!playlist.isEmpty())
            library().addToPlaylist(library().createPlaylist(playlist), urls);
        m_open(urls, play);
        close();
    };
    if (urls.size() > 1 || !playlist.isEmpty()) {
        finish();
        return;
    }
    // One link: see whether it's a list of channels or a podcast first.
    m_status->setText(tr("Opening…"));
    m_play->setEnabled(false);
    m_queue->setEnabled(false);
    QPointer<NetworkStreamOverlay> self(this);
    NetFeeds::instance().probe(urls.first(), [self, finish, url = urls.first()](const QString &kind, const QString &title) {
        if (!self)
            return;
        if (kind == "channels") {
            library().addChannelList(url);
            self->m_browse(kind, url);
            self->close();
        } else if (kind == "podcast") {
            library().addPodcast(url, title);
            self->m_browse(kind, url);
            self->close();
        } else {
            finish();
        }
    });
}

// ---------------------------------------------------------------------------
// MediaInfoOverlay

MediaInfoOverlay::MediaInfoOverlay(const QString &location, QWidget *window)
    : PanelOverlay(tr("Info"), {640, 560}, window)
{
    auto *host = new QWidget;
    m_grid = new QGridLayout(host);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setHorizontalSpacing(18);
    m_grid->setVerticalSpacing(10);
    m_grid->setColumnStretch(1, 1);
    body()->addWidget(host);
    body()->addStretch();

    const QLocale loc;
    const bool url = Library::isUrl(location);
    const QFileInfo fi(location);
    if (url) {
        const QString name = library().streamName(location);
        addRow(tr("Name"), name.isEmpty() ? Library::displayName(location) : name);
        addRow(tr("Address"), location);
        addRow(tr("Type"), tr("Network stream"));
    } else if (fi.isDir()) {
        int videos = 0, audio = 0, folders = 0, watched = 0;
        qint64 bytes = 0, length = 0;
        bool lengthKnown = true;
        for (const QFileInfo &f : QDir(location).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (f.isDir()) {
                ++folders;
                continue;
            }
            if (!Library::isMedia(f.filePath()))
                continue;
            Library::isAudioFile(f.filePath()) ? ++audio : ++videos;
            bytes += f.size();
            watched += library().progress(f.filePath()) >= 0.95;
            const qint64 ms = Durations::instance().duration(f.filePath());
            ms < 0 ? void(lengthKnown = false) : void(length += ms);
        }
        addRow(tr("Name"), fi.fileName());
        addRow(tr("Location"), fi.absolutePath());
        QStringList contents;
        if (videos)
            contents << (videos == 1 ? tr("1 video") : tr("%1 videos").arg(videos));
        if (audio)
            contents << (audio == 1 ? tr("1 audio file") : tr("%1 audio files").arg(audio));
        if (folders)
            contents << (folders == 1 ? tr("1 folder") : tr("%1 folders").arg(folders));
        addRow(tr("Contains"), contents.isEmpty() ? tr("Nothing playable") : contents.join(", "));
        if (videos + audio > 0) {
            addRow(tr("Size"), loc.formattedDataSize(bytes));
            if (length > 0)
                addRow(tr("Length"), lengthText(length) + (lengthKnown ? QString() : tr(" (still counting)")));
            addRow(tr("Watched"), tr("%1 of %2").arg(watched).arg(videos + audio));
        }
        addRow(tr("Modified"), loc.toString(fi.lastModified(), QLocale::LongFormat));
    } else {
        const MusicIndex::Track *t = musicIndex().track(location);
        addRow(tr("Name"), fi.fileName());
        addRow(tr("Location"), fi.absolutePath());
        addRow(tr("Type"), Library::isAudioFile(location) ? tr("Audio") : tr("Video"));
        addRow(tr("Size"), loc.formattedDataSize(fi.size()));
        const qint64 ms = t && t->duration > 0 ? t->duration : Durations::instance().duration(location);
        if (ms > 0)
            addRow(tr("Length"), lengthText(ms));
        addRow(tr("Modified"), loc.toString(fi.lastModified(), QLocale::LongFormat));
        const double p = library().progress(location);
        addRow(tr("Progress"), p >= 0.95 ? tr("Watched") : p > 0.01 ? tr("%1% (resumes at %2)").arg(int(p * 100)).arg(lengthText(library().resumeTime(location)))
                                                       : tr("Not started"));
        if (t) {
            addRow(tr("Title"), t->displayTitle());
            addRow(tr("Artist"), t->displayArtist());
            addRow(tr("Album"), t->displayAlbum());
            if (!t->albumArtist.isEmpty() && t->albumArtist != t->artist)
                addRow(tr("Album artist"), t->albumArtist);
            if (t->number)
                addRow(tr("Track"), QString::number(t->number));
            if (t->year)
                addRow(tr("Year"), QString::number(t->year));
            addRow(tr("Genre"), t->displayGenre());
        }
        const QString sub = ShowPresets::instance().attachedFile(location, true);
        if (!sub.isEmpty())
            addRow(tr("Subtitle file"), QFileInfo(sub).fileName());
        const QString audio = ShowPresets::instance().attachedFile(location, false);
        if (!audio.isEmpty())
            addRow(tr("Audio file"), QFileInfo(audio).fileName());
    }
    if (!url && !fi.exists())
        addRow(tr("Note"), tr("This file isn't there any more."));
}

void MediaInfoOverlay::addRow(const QString &label, const QString &value)
{
    if (value.isEmpty())
        return;
    auto *l = new QLabel(label);
    l->setProperty("role", "dim");
    auto *v = new QLabel(value);
    v->setWordWrap(true);
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_grid->addWidget(l, m_rows, 0, Qt::AlignTop);
    m_grid->addWidget(v, m_rows, 1);
    ++m_rows;
}

// ---------------------------------------------------------------------------
// FolderTracksOverlay

namespace {
// "S01E05", "1x05", "Episode 5", "E05" → a comparable key; empty if none.
QString episodeKey(const QString &name)
{
    static const QRegularExpression se(R"([Ss](\d{1,2})[ ._-]?[Ee](\d{1,3}))");
    static const QRegularExpression x(R"((?:^|[^\d])(\d{1,2})x(\d{2,3})(?:[^\d]|$))");
    static const QRegularExpression ep(R"((?:[Ee]pisode|[Ee]p|\b[Ee])[ ._-]?(\d{1,3}))");
    if (auto m = se.match(name); m.hasMatch())
        return QString("s%1e%2").arg(m.captured(1).toInt()).arg(m.captured(2).toInt());
    if (auto m = x.match(name); m.hasMatch())
        return QString("s%1e%2").arg(m.captured(1).toInt()).arg(m.captured(2).toInt());
    if (auto m = ep.match(name); m.hasMatch())
        return QString("e%1").arg(m.captured(1).toInt());
    return {};
}
} // namespace

FolderTracksOverlay::FolderTracksOverlay(const QString &folder, bool subtitles, QWidget *window)
    : PanelOverlay(subtitles ? tr("Subtitle Tracks — %1").arg(QFileInfo(folder).fileName())
                             : tr("Audio Tracks — %1").arg(QFileInfo(folder).fileName()),
                   {820, 660}, window),
      m_folder(folder), m_subtitles(subtitles)
{
    const ShowPresets::FolderTracks saved = ShowPresets::instance().folderTracks(folder);

    auto *ruleRow = new QHBoxLayout;
    auto *ruleLabel = new QLabel(subtitles ? tr("Turn on automatically") : tr("Pick automatically"));
    ruleRow->addWidget(ruleLabel);
    ruleRow->addStretch();
    m_rule = new Dropdown;
    m_rule->setMinimumWidth(260);
    const QString global = prefs().string(subtitles ? "subs/default" : "audio/default");
    m_rule->addItem(tr("As in Settings (%1)").arg(ShowPresets::describeRule(global)), QString());
    if (subtitles)
        m_rule->addItem(ShowPresets::describeRule("off"), QString("off"));
    for (int n = 1; n <= 4; ++n)
        m_rule->addItem(ShowPresets::describeRule(QString("track:%1").arg(n)), QString("track:%1").arg(n));
    QList<QPair<QString, QString>> langs;
    for (const char *c : {"en", "es", "fr", "de", "it", "pt", "ja", "ko", "zh", "ru", "ar", "hi", "nl", "sv", "pl"})
        langs << qMakePair(ShowPresets::describeRule(QString("lang:") + c), QString("lang:") + c);
    std::sort(langs.begin(), langs.end());
    m_rule->insertSeparator(m_rule->count());
    for (const auto &[name, rule] : langs)
        m_rule->addItem(name, rule);
    const QString rule = subtitles ? saved.subRule : saved.audioRule;
    if (m_rule->findData(rule) < 0)
        m_rule->addItem(ShowPresets::describeRule(rule), rule);
    m_rule->setCurrentIndex(qMax(0, m_rule->findData(rule)));
    ruleRow->addWidget(m_rule);
    body()->addLayout(ruleRow);
    body()->addWidget(caption(subtitles ? tr("For every video in this folder and the folders inside it. Overrides "
                                             "Settings → Subtitles → Turn On Automatically.")
                                        : tr("For every video in this folder and the folders inside it. Overrides "
                                             "Settings → Audio → Pick Automatically.")));
    body()->addWidget(hline(this));

    auto *head = new QHBoxLayout;
    head->addWidget(new QLabel(subtitles ? tr("Subtitle file for each video") : tr("Extra audio file for each video")), 1);
    auto *find = button(tr("Find Automatically"));
    find->setToolTip(tr("Look in this folder and the folders inside it, matching by episode number or name"));
    auto *elsewhere = button(tr("Search Folder…"));
    auto *choose = button(tr("Choose File…"));
    auto *clear = button(tr("Remove"));
    head->addWidget(find);
    head->addWidget(elsewhere);
    head->addWidget(choose);
    head->addWidget(clear);
    body()->addLayout(head);

    m_table = new QTreeWidget;
    m_table->setColumnCount(2);
    m_table->setHeaderLabels({tr("Video"), subtitles ? tr("Subtitle file") : tr("Audio file")});
    m_table->setRootIsDecorated(false);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    const QHash<QString, QString> files = subtitles ? saved.subFiles : saved.audioFiles;
    for (const QString &video : library().mediaInFolder(folder, 0)) {
        auto *item = new QTreeWidgetItem(m_table, {QFileInfo(video).fileName(), QString()});
        item->setData(0, Qt::UserRole, video);
        setFile(item, files.value(QFileInfo(video).fileName()));
    }
    body()->addWidget(m_table, 1);

    auto *foot = new QHBoxLayout;
    m_status = caption(m_table->topLevelItemCount() == 0 ? tr("This folder has no videos of its own.") : QString());
    foot->addWidget(m_status, 1);
    auto *cancel = button(tr("Cancel"));
    auto *saveBtn = button(tr("Save"), true);
    foot->addWidget(cancel);
    foot->addWidget(saveBtn);
    body()->addLayout(foot);

    connect(find, &QPushButton::clicked, this, [this] { findAutomatically(m_folder); });
    connect(elsewhere, &QPushButton::clicked, this, [this] {
        const QString dir = FileDialogs::existingDirectory(this, m_subtitles ? tr("Folder with subtitle files")
                                                                             : tr("Folder with audio files"), m_folder);
        if (!dir.isEmpty())
            findAutomatically(dir);
    });
    connect(choose, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem *it = m_table->currentItem())
            chooseFor(it);
    });
    connect(m_table, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) { chooseFor(it); });
    connect(clear, &QPushButton::clicked, this, [this] {
        for (QTreeWidgetItem *it : m_table->selectedItems())
            setFile(it, QString());
    });
    connect(cancel, &QPushButton::clicked, this, &PanelOverlay::close);
    connect(saveBtn, &QPushButton::clicked, this, &FolderTracksOverlay::save);
}

bool FolderTracksOverlay::candidate(const QString &file) const
{
    return m_subtitles ? Library::isSubtitleFile(file) : Library::isAudioFile(file);
}

bool FolderTracksOverlay::autodetected(const QString &video, const QString &file) const
{
    // What VLC adds on its own, by Settings → Subtitles → "Load subtitle files
    // automatically" (only files next to the video are counted here).
    const QFileInfo v(video), f(file);
    if (!m_subtitles || v.absolutePath() != f.absolutePath())
        return false;
    const QString name = f.completeBaseName(), base = v.completeBaseName();
    switch (prefs().integer("subs/fuzzy")) {
    case 1: return true;
    case 2: return name.contains(base, Qt::CaseInsensitive);
    case 3: return name.startsWith(base, Qt::CaseInsensitive);
    case 4: return name.compare(base, Qt::CaseInsensitive) == 0;
    default: return false;
    }
}

void FolderTracksOverlay::setFile(QTreeWidgetItem *item, const QString &file)
{
    item->setData(1, Qt::UserRole, file);
    item->setText(1, file.isEmpty() ? QString() : QDir(m_folder).relativeFilePath(file));
    item->setToolTip(1, file);
}

void FolderTracksOverlay::chooseFor(QTreeWidgetItem *item)
{
    const QString filter = m_subtitles ? tr("Subtitles (*.srt *.ass *.ssa *.sub *.vtt *.idx *.smi *.txt);;All files (*)")
                                       : tr("Audio (*.mp3 *.flac *.ogg *.opus *.m4a *.aac *.wav *.ac3 *.dts *.eac3 *.mka);;All files (*)");
    const QString file = FileDialogs::openFile(this, m_subtitles ? tr("Subtitle file for %1").arg(item->text(0))
                                                                 : tr("Audio file for %1").arg(item->text(0)),
                                               m_folder, filter);
    if (!file.isEmpty())
        setFile(item, file);
}

void FolderTracksOverlay::findAutomatically(const QString &searchIn)
{
    QStringList found;
    QDirIterator it(searchIn, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && found.size() < 5000) {
        const QString f = it.next();
        if (candidate(f) && QDir(searchIn).relativeFilePath(f).count('/') <= 3)
            found << f;
    }
    found.sort(Qt::CaseInsensitive);
    int matched = 0, automatic = 0;
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_table->topLevelItem(i);
        const QString video = item->data(0, Qt::UserRole).toString();
        const QString base = QFileInfo(video).completeBaseName();
        const QString key = episodeKey(base);
        QString pick;
        for (const QString &f : found) {
            if (f == video)
                continue;
            const QString name = QFileInfo(f).completeBaseName();
            if ((!key.isEmpty() && episodeKey(name) == key) || name.startsWith(base, Qt::CaseInsensitive)) {
                pick = f;
                break;
            }
        }
        if (pick.isEmpty())
            continue;
        if (autodetected(video, pick)) {
            ++automatic; // nothing to attach: it shows up anyway
            continue;
        }
        setFile(item, pick);
        ++matched;
    }
    QStringList parts;
    parts << (matched == 1 ? tr("Matched 1 file.") : tr("Matched %1 files.").arg(matched));
    if (automatic)
        parts << tr("%1 already load on their own (same name as the video).").arg(automatic);
    if (!matched && !automatic)
        parts = {m_subtitles ? tr("No matching subtitle files found.") : tr("No matching audio files found.")};
    m_status->setText(parts.join(' '));
}

void FolderTracksOverlay::save()
{
    ShowPresets::FolderTracks t = ShowPresets::instance().folderTracks(m_folder);
    QHash<QString, QString> &files = m_subtitles ? t.subFiles : t.audioFiles;
    files.clear();
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_table->topLevelItem(i);
        const QString file = item->data(1, Qt::UserRole).toString();
        if (!file.isEmpty())
            files.insert(item->text(0), file);
    }
    (m_subtitles ? t.subRule : t.audioRule) = m_rule->currentData().toString();
    ShowPresets::instance().setFolderTracks(m_folder, t);
    close();
}
