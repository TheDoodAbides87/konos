#include "subtitleoverlay.h"
#include "filedialogs.h"
#include "library.h"
#include "player.h"
#include "prefs.h"
#include "theme.h"
#include "widgets.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// Languages offered for downloads, as OpenSubtitles codes.
const QList<QPair<QString, QString>> &languages()
{
    static const QList<QPair<QString, QString>> l = {
        {"en", "English"},     {"es", "Spanish"},     {"fr", "French"},     {"de", "German"},
        {"it", "Italian"},     {"pt-pt", "Portuguese"}, {"pt-br", "Portuguese (Brazil)"},
        {"nl", "Dutch"},       {"pl", "Polish"},      {"ru", "Russian"},    {"uk", "Ukrainian"},
        {"sv", "Swedish"},     {"da", "Danish"},      {"no", "Norwegian"},  {"fi", "Finnish"},
        {"cs", "Czech"},       {"hu", "Hungarian"},   {"ro", "Romanian"},   {"el", "Greek"},
        {"tr", "Turkish"},     {"ar", "Arabic"},      {"he", "Hebrew"},     {"hi", "Hindi"},
        {"id", "Indonesian"},  {"vi", "Vietnamese"},  {"th", "Thai"},       {"ja", "Japanese"},
        {"ko", "Korean"},      {"zh-cn", "Chinese (Simplified)"},           {"zh-tw", "Chinese (Traditional)"},
    };
    return l;
}

QString localPath(const QString &location)
{
    const QUrl url(location);
    if (url.isValid() && url.scheme().size() > 1)
        return url.isLocalFile() ? url.toLocalFile() : QString();
    return QFileInfo(location).isFile() ? location : QString();
}

// "1 subtitle" / "3 subtitles" (there are no translation files to do it).
QString count(int n, const QString &one, const QString &many)
{
    return (n == 1 ? one : many).arg(n);
}

QString shortName(const QString &location)
{
    const QString path = localPath(location);
    return path.isEmpty() ? QUrl(location).fileName().isEmpty() ? location : QUrl(location).fileName()
                          : QFileInfo(path).fileName();
}

} // namespace

// One "video - - - - subtitle" line.
class MatchRow : public QWidget {
public:
    std::function<void()> clicked;
    QString video, subtitle;
    bool dimSubtitle = false, choosable = false, done = false;

    MatchRow()
    {
        setFixedHeight(54);
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Palette &c = theme().pal();
        const qreal dpr = devicePixelRatioF();
        const int mid = height() / 2;
        const int half = width() / 2;
        if (choosable && underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(c.hover);
            p.drawRoundedRect(QRectF(half - 6, 6, width() - half, height() - 12), 6, 6);
        }
        QFont f = font();
        f.setPixelSize(15);
        p.setFont(f);
        const QFontMetrics fm(f);

        p.drawPixmap(QPointF(30, mid - 12), theme().pixmap("film", c.textDim, 24, dpr));
        const QString left = fm.elidedText(video, Qt::ElideMiddle, half - 120);
        p.setPen(c.text);
        p.drawText(QRectF(70, 0, half - 110, height()), Qt::AlignVCenter | Qt::AlignLeft, left);

        const int lineFrom = 70 + fm.horizontalAdvance(left) + 18;
        const int lineTo = half - 24;
        if (lineTo > lineFrom) {
            QPen dash(c.border, 1.2, Qt::CustomDashLine);
            dash.setDashPattern({6, 6});
            p.setPen(dash);
            p.drawLine(QPointF(lineFrom, mid + 0.5), QPointF(lineTo, mid + 0.5));
        }

        const QColor iconColor = done ? theme().accent() : c.textDim;
        p.drawPixmap(QPointF(half, mid - 12), theme().pixmap(done ? "check" : "subtitles", iconColor, 24, dpr));
        p.setPen(dimSubtitle ? c.textMuted : c.text);
        p.drawText(QRectF(half + 40, 0, width() - half - 80, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(subtitle, Qt::ElideMiddle, width() - half - 80));
        if (choosable)
            p.drawPixmap(QPointF(width() - 34, mid - 9), theme().pixmap("chevron-down", c.textDim, 18, dpr));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (choosable && e->position().x() > width() / 2 - 6 && clicked)
            clicked();
    }
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }
};

SubtitleOverlay::SubtitleOverlay(Player *player, QWidget *parent) : QWidget(parent), m_player(player)
{
    hide();
    setFocusPolicy(Qt::StrongFocus);
    if (parent)
        parent->installEventFilter(this);

    m_panel = new QWidget(this);
    m_panel->setObjectName("SettingsPanel");
    m_panel->setAttribute(Qt::WA_StyledBackground);
    auto *v = new QVBoxLayout(m_panel);
    v->setContentsMargins(18, 14, 18, 18);
    v->setSpacing(14);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(tr("Add Subtitles"));
    title->setProperty("role", "title");
    head->addWidget(title);
    head->addStretch();
    auto *closeButton = new IconButton("close");
    closeButton->setToolTip(tr("Close (Esc)"));
    connect(closeButton, &IconButton::clicked, this, &SubtitleOverlay::close);
    head->addWidget(closeButton);
    v->addLayout(head);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(12);
    m_scope = new Dropdown;
    m_scope->addItems({tr("To This File"), tr("To This Folder"), tr("To This Playlist")});
    m_scope->setMinimumWidth(150);
    m_scope->setMaximumWidth(220);
    m_source = new Dropdown;
    m_source->addItems({tr("From Computer"), tr("From OpenSubtitles"), tr("From Link …")});
    m_source->setMinimumWidth(150);
    m_source->setMaximumWidth(220);
    m_search = new QLineEdit;
    m_search->setProperty("role", "search");
    m_search->setFixedHeight(40);
    m_search->setClearButtonEnabled(true);
    auto *icon = new QLabel(m_search);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    icon->setGeometry(12, 11, 18, 18);
    auto refreshIcon = [icon] { icon->setPixmap(theme().pixmap("search", theme().pal().textDim, 18, 2.0)); };
    refreshIcon();
    connect(&theme(), &Theme::changed, icon, refreshIcon);
    m_language = new Dropdown;
    for (const auto &[code, name] : languages())
        m_language->addItem(tr(name.toUtf8().constData()), code);
    m_language->setMinimumWidth(130);
    m_language->setMaximumWidth(220);
    m_search->setMinimumWidth(220);
    bar->addWidget(m_scope, 1);
    bar->addWidget(m_source, 1);
    bar->addWidget(m_search, 3);
    bar->addWidget(m_language, 1);
    v->addLayout(bar);

    m_list = new QWidget;
    m_list->setObjectName("SubtitleList");
    m_rows = new QVBoxLayout(m_list);
    m_rows->setContentsMargins(0, 10, 0, 10);
    m_rows->setSpacing(0);
    m_rows->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(m_list);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *well = new QWidget;
    well->setObjectName("SubtitleWell");
    well->setAttribute(Qt::WA_StyledBackground);
    well->setStyleSheet(QString()); // colours come from the app style sheet
    auto *wellLay = new QVBoxLayout(well);
    wellLay->setContentsMargins(0, 0, 0, 0);
    wellLay->addWidget(scroll);
    v->addWidget(well, 1);

    auto *foot = new QHBoxLayout;
    m_status = new QLabel;
    m_status->setProperty("role", "dim");
    m_status->setWordWrap(true);
    m_settingsLink = new QPushButton(tr("Open Settings"));
    m_settingsLink->setProperty("role", "link");
    m_settingsLink->setCursor(Qt::PointingHandCursor);
    connect(m_settingsLink, &QPushButton::clicked, this, [this] {
        close();
        emit settingsRequested();
    });
    m_action = new QPushButton;
    m_action->setProperty("role", "accent");
    m_action->setCursor(Qt::PointingHandCursor);
    m_action->setMinimumWidth(170);
    foot->addWidget(m_status, 1);
    foot->addWidget(m_settingsLink);
    foot->addWidget(m_action);
    v->addLayout(foot);

    m_language->setCurrentIndex(qMax(0, m_language->findData(prefs().string("opensubtitles/language"))));
    connect(m_scope, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_opening)
            rebuild();
    });
    connect(m_source, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_opening)
            rebuild();
    });
    connect(m_language, &QComboBox::currentIndexChanged, this, [this] {
        prefs().set("opensubtitles/language", language());
        if (!m_opening && m_source->currentIndex() == FromOpenSubtitles)
            searchAll();
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (m_source->currentIndex() == FromOpenSubtitles)
            searchAll();
        else if (m_source->currentIndex() == FromLink)
            apply();
    });
    connect(m_action, &QPushButton::clicked, this, &SubtitleOverlay::apply);
    connect(&OpenSubtitles::instance(), &OpenSubtitles::accountChanged, this, &SubtitleOverlay::updateFooter);
}

QString SubtitleOverlay::language() const
{
    return m_language->currentData().toString();
}

void SubtitleOverlay::open(Source source)
{
    if (prefs().string("opensubtitles/language").isEmpty()) {
        // First use: the preferred subtitle language, else the system's.
        QString code = prefs().string("subs/language").section(',', 0, 0).trimmed().toLower();
        if (m_language->findData(code) < 0) {
            const QLocale locale = QLocale::system();
            code = locale.name().section('_', 0, 0);
            if (code == "pt" || code == "zh")
                code = locale.name().toLower().replace('_', '-');
        }
        m_language->setCurrentIndex(qMax(0, m_language->findData(code)));
    }
    m_opening = true;
    m_source->setCurrentIndex(source);
    m_opening = false;
    rebuild();
    layoutPanel();
    show();
    raise();
    setFocus();
}

void SubtitleOverlay::close()
{
    ++m_generation; // ignore replies still on their way
    m_downloading = -1;
    hide();
    if (parentWidget())
        parentWidget()->setFocus();
}

QStringList SubtitleOverlay::videosInScope() const
{
    const QString current = m_player->location();
    const QString path = localPath(current);
    QStringList out;
    switch (Scope(m_scope->currentIndex())) {
    case ThisFile:
        if (!current.isEmpty())
            out << (path.isEmpty() ? current : path);
        break;
    case ThisFolder:
        if (!path.isEmpty()) {
            const QDir dir = QFileInfo(path).dir();
            for (const QString &name : dir.entryList(QDir::Files))
                if (Library::isVideoFile(name))
                    out << dir.filePath(name);
        }
        break;
    case ThisPlaylist:
        if (m_queue)
            for (const QString &item : m_queue()) {
                const QString p = localPath(item);
                if (!p.isEmpty() && Library::isVideoFile(p))
                    out << p;
            }
        break;
    }
    if (m_scope->currentIndex() != ThisPlaylist) {
        QCollator collator;
        collator.setNumericMode(true);
        std::sort(out.begin(), out.end(), collator);
    }
    return out;
}

QString SubtitleOverlay::defaultSearchText() const
{
    const QStringList videos = videosInScope();
    if (videos.isEmpty())
        return m_player->title();
    const OpenSubtitles::Query q = OpenSubtitles::guess(videos.first());
    QString text = q.text.isEmpty() ? m_player->title() : q.text;
    if (q.season >= 0)
        text += m_scope->currentIndex() == ThisFile ? QStringLiteral(" S%1E%2").arg(q.season, 2, 10, QChar('0')).arg(q.episode, 2, 10, QChar('0'))
                                                    : QStringLiteral(" S%1").arg(q.season, 2, 10, QChar('0'));
    return text;
}

OpenSubtitles::Query SubtitleOverlay::queryFor(const QString &video) const
{
    OpenSubtitles::Query q = OpenSubtitles::guess(localPath(video));
    if (localPath(video).isEmpty())
        q.path.clear();
    q.language = language();
    // An edited search box overrides the title (and season) for every file.
    const QString typed = m_search->text().simplified();
    if (!typed.isEmpty() && typed != m_autoSearch) {
        static const QRegularExpression se(R"(\b[Ss](\d{1,2})(?:[Ee](\d{1,3}))?\b)");
        QString text = typed;
        if (const auto m = se.match(text); m.hasMatch()) {
            if (q.season < 0)
                q.season = m.captured(1).toInt();
            if (q.episode < 0 && !m.captured(2).isEmpty())
                q.episode = m.captured(2).toInt();
            text.remove(m.capturedStart(), m.capturedLength());
        }
        q.text = text.simplified();
    } else if (q.text.isEmpty()) {
        q.text = m_player->title();
    }
    return q;
}

void SubtitleOverlay::rebuild()
{
    ++m_generation;
    m_downloading = -1;
    for (Row &r : m_items)
        delete r.view;
    m_items.clear();

    const Source source = Source(m_source->currentIndex());
    // A link is one subtitle file: it can only go with the current video.
    if (source == FromLink && m_scope->currentIndex() != ThisFile) {
        m_opening = true;
        m_scope->setCurrentIndex(ThisFile);
        m_opening = false;
    }
    m_scope->setEnabled(source != FromLink);
    m_language->setVisible(source == FromOpenSubtitles);

    m_search->blockSignals(true);
    if (source == FromLink) {
        m_search->setPlaceholderText(tr("Paste a subtitle link (https://…/file.srt)"));
        m_search->clear();
        m_autoSearch.clear();
    } else {
        m_search->setPlaceholderText(source == FromComputer ? tr("Title") : tr("Search OpenSubtitles"));
        m_autoSearch = defaultSearchText();
        m_search->setText(m_autoSearch);
    }
    m_search->setEnabled(source != FromComputer);
    m_search->blockSignals(false);

    for (const QString &video : videosInScope()) {
        Row row;
        row.video = video;
        row.view = new MatchRow;
        row.view->video = shortName(video);
        m_items << row;
        m_rows->insertWidget(m_rows->count() - 1, row.view);
        const int i = int(m_items.size()) - 1;
        row.view->clicked = [this, i] { pickAlternative(i); };
        updateRow(i);
    }
    if (source == FromOpenSubtitles)
        searchAll();
    updateFooter();
}

void SubtitleOverlay::searchAll()
{
    const int generation = ++m_generation;
    m_downloading = -1;
    if (!OpenSubtitles::instance().hasKey()) {
        updateFooter();
        return;
    }
    for (int i = 0; i < m_items.size(); ++i) {
        Row &row = m_items[i];
        row.results.clear();
        row.chosen = -1;
        row.state = Row::Searching;
        updateRow(i);
        OpenSubtitles::instance().search(queryFor(row.video), [this, i, generation](const auto &results, const QString &error) {
            if (generation != m_generation || i >= m_items.size())
                return;
            Row &r = m_items[i];
            r.results = results;
            r.chosen = results.isEmpty() ? -1 : 0;
            r.state = results.isEmpty() ? (error.isEmpty() ? Row::Missing : Row::Failed) : Row::Found;
            r.note = error;
            updateRow(i);
            updateFooter();
        });
    }
    updateFooter();
}

void SubtitleOverlay::updateRow(int i)
{
    Row &r = m_items[i];
    MatchRow *v = r.view;
    v->dimSubtitle = r.state != Row::Found && r.state != Row::Done && !(r.state == Row::Idle && !r.subtitle.isEmpty());
    v->done = r.state == Row::Done;
    v->choosable = r.state == Row::Found && r.results.size() > 1;
    switch (r.state) {
    case Row::Idle:
        v->subtitle = r.subtitle.isEmpty() ? (m_source->currentIndex() == FromComputer ? tr("No file chosen") : QString())
                                           : QFileInfo(r.subtitle).fileName();
        break;
    case Row::Searching: v->subtitle = tr("Searching…"); break;
    case Row::Found: {
        const OpenSubtitles::Result &res = r.results[r.chosen];
        v->subtitle = res.fileName.isEmpty() ? res.release : res.fileName;
        break;
    }
    case Row::Missing: v->subtitle = tr("No subtitles found"); break;
    case Row::Working: v->subtitle = tr("Downloading…"); break;
    case Row::Done: v->subtitle = QFileInfo(r.subtitle).fileName(); break;
    case Row::Failed: v->subtitle = r.note.isEmpty() ? tr("Failed") : r.note; break;
    }
    v->update();
}

void SubtitleOverlay::pickAlternative(int i)
{
    Row &r = m_items[i];
    if (r.results.size() < 2)
        return;
    QMenu menu(this);
    for (int k = 0; k < qMin(15, int(r.results.size())); ++k) {
        const OpenSubtitles::Result &res = r.results[k];
        QString label = res.fileName.isEmpty() ? res.release : res.fileName;
        QStringList info;
        if (res.hashMatch)
            info << tr("matches this file");
        if (res.hearingImpaired)
            info << tr("SDH");
        info << count(res.downloads, tr("%1 download"), tr("%1 downloads"));
        QAction *a = menu.addAction(label + QStringLiteral("  ·  ") + info.join(QStringLiteral(" · ")));
        a->setCheckable(true);
        a->setChecked(k == r.chosen);
        connect(a, &QAction::triggered, this, [this, i, k] {
            m_items[i].chosen = k;
            updateRow(i);
        });
    }
    menu.exec(QCursor::pos());
}

void SubtitleOverlay::updateFooter()
{
    const Source source = Source(m_source->currentIndex());
    const bool busy = m_downloading >= 0;
    m_settingsLink->setVisible(source == FromOpenSubtitles && !OpenSubtitles::instance().hasKey());
    int ready = 0, searching = 0, done = 0;
    for (const Row &r : std::as_const(m_items)) {
        ready += r.state == Row::Found || (r.state == Row::Idle && !r.subtitle.isEmpty());
        searching += r.state == Row::Searching;
        done += r.state == Row::Done;
    }
    QString status;
    switch (source) {
    case FromComputer:
        if (m_scope->currentIndex() == ThisFile)
            m_action->setText(tr("Choose File…"));
        else
            m_action->setText(ready == 0 ? tr("Choose Files…") : count(ready, tr("Add %1 Subtitle"), tr("Add %1 Subtitles")));
        status = m_scope->currentIndex() == ThisFile
                     ? tr("Pick a subtitle file to add to this video.")
                     : tr("Pick subtitle files; they're matched to videos by episode number or name and saved next to them.");
        m_action->setEnabled(!m_items.isEmpty());
        break;
    case FromOpenSubtitles:
        if (!OpenSubtitles::instance().hasKey()) {
            status = tr("OpenSubtitles needs a free API key from opensubtitles.com (Settings → Subtitles).");
            m_action->setText(tr("Download"));
            m_action->setEnabled(false);
            break;
        }
        m_action->setText(busy ? tr("Downloading…") : ready == 0 && done > 0 ? tr("Done") : tr("Download %1").arg(ready));
        m_action->setEnabled(!busy && (ready > 0 || done > 0));
        if (searching > 0)
            status = tr("Searching OpenSubtitles…");
        else if (done > 0)
            status = count(done, tr("%1 subtitle saved next to the video."), tr("%1 subtitles saved next to the videos."));
        else
            status = OpenSubtitles::instance().signedIn()
                         ? tr("Signed in as %1. Click a subtitle to pick another release.").arg(OpenSubtitles::instance().user())
                         : tr("Not signed in: OpenSubtitles allows only a few downloads a day without an account.");
        break;
    case FromLink:
        m_action->setText(tr("Download && Add"));
        m_action->setEnabled(!busy && !m_items.isEmpty());
        status = tr("Downloads a .srt/.ass/.vtt file and adds it to this video.");
        break;
    }
    if (m_items.isEmpty())
        status = tr("Nothing to add subtitles to: play a video file first.");
    if (!m_status->property("sticky").toBool())
        m_status->setText(status);
}

void SubtitleOverlay::apply()
{
    m_status->setProperty("sticky", false);
    switch (Source(m_source->currentIndex())) {
    case FromComputer:
        chooseFiles();
        break;
    case FromOpenSubtitles: {
        bool ready = false;
        for (const Row &r : std::as_const(m_items))
            ready |= r.state == Row::Found;
        if (!ready) { // "Done"
            close();
            break;
        }
        m_downloading = 0;
        downloadNext();
        break;
    }
    case FromLink:
        fetchLink();
        break;
    }
}

void SubtitleOverlay::chooseFiles()
{
    if (m_items.isEmpty())
        return;
    const QString filter = tr("Subtitles (*.srt *.ass *.ssa *.sub *.vtt *.idx *.smi *.txt);;All files (*)");
    const QString first = localPath(m_items.first().video);
    const QString dir = first.isEmpty() ? QDir::homePath() : QFileInfo(first).absolutePath();
    if (m_scope->currentIndex() == ThisFile) {
        const QString file = FileDialogs::openFile(window(), tr("Add subtitle file"), dir, filter);
        if (file.isEmpty())
            return;
        if (m_player->addSubtitleFile(file)) {
            emit message(tr("Subtitle added"));
            close();
        } else {
            m_status->setText(tr("libVLC couldn't add that file."));
        }
        return;
    }
    // Several videos: either the subtitles were chosen already (then this
    // button saves them), or pick them now and match.
    bool chosen = false;
    for (const Row &r : std::as_const(m_items))
        chosen |= r.state == Row::Idle && !r.subtitle.isEmpty();
    if (chosen) {
        int saved = 0;
        for (int i = 0; i < m_items.size(); ++i) {
            Row &r = m_items[i];
            if (r.state != Row::Idle || r.subtitle.isEmpty())
                continue;
            QFile in(r.subtitle);
            if (in.open(QIODevice::ReadOnly) && save(r, in.readAll(), QFileInfo(r.subtitle).fileName()))
                ++saved;
            updateRow(i);
        }
        m_status->setText(count(saved, tr("%1 subtitle saved next to the video."), tr("%1 subtitles saved next to the videos.")));
        m_status->setProperty("sticky", true);
        updateFooter();
        return;
    }
    const QStringList files = FileDialogs::openFiles(window(), tr("Choose subtitle files"), dir, filter);
    if (files.isEmpty())
        return;
    // Match by season/episode, else by shared file name.
    for (int i = 0; i < m_items.size(); ++i) {
        Row &r = m_items[i];
        const OpenSubtitles::Query v = OpenSubtitles::guess(r.video);
        const QString base = QFileInfo(r.video).completeBaseName().toLower();
        for (const QString &f : files) {
            const OpenSubtitles::Query s = OpenSubtitles::guess(f);
            const bool episode = v.episode >= 0 && s.episode == v.episode && (v.season < 0 || s.season == v.season);
            const bool name = QFileInfo(f).completeBaseName().toLower().startsWith(base);
            if (episode || name || (files.size() == 1 && m_items.size() == 1)) {
                r.subtitle = f;
                break;
            }
        }
        r.state = Row::Idle;
        updateRow(i);
    }
    updateFooter();
}

bool SubtitleOverlay::save(Row &row, const QByteArray &data, const QString &fileName)
{
    // "<video>.<lang>.<ext>" next to the video, so libVLC picks it up next
    // time; videos in read-only places get theirs in the app's data folder.
    QString ext = QFileInfo(fileName).suffix().toLower();
    if (!Library::isSubtitleFile("x." + ext))
        ext = "srt";
    const QString video = localPath(row.video);
    const QString lang = m_source->currentIndex() == FromOpenSubtitles ? language() : QString();
    const QString base = video.isEmpty() ? QFileInfo(QUrl(row.video).path()).completeBaseName()
                                         : QFileInfo(video).completeBaseName();
    const QString name = base + (lang.isEmpty() ? QString() : "." + lang) + "." + ext;
    QString dir = video.isEmpty() ? QString() : QFileInfo(video).absolutePath();
    if (dir.isEmpty() || !QFileInfo(dir).isWritable()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/subtitles";
        QDir().mkpath(dir);
    }
    const QString target = dir + "/" + name;
    if (QFileInfo(row.subtitle).absoluteFilePath() != QFileInfo(target).absoluteFilePath()) {
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(data) != data.size()) {
            row.state = Row::Failed;
            row.note = tr("Couldn't save %1").arg(target);
            return false;
        }
    }
    row.subtitle = target;
    row.state = Row::Done;
    // Playing now: load it straight away.
    const QString playing = localPath(m_player->location());
    if (row.video == m_player->location() || (!playing.isEmpty() && playing == video))
        m_player->addSubtitleFile(target);
    return true;
}

void SubtitleOverlay::downloadNext()
{
    while (m_downloading >= 0 && m_downloading < m_items.size() && m_items[m_downloading].state != Row::Found)
        ++m_downloading;
    if (m_downloading < 0 || m_downloading >= m_items.size()) {
        m_downloading = -1;
        updateFooter();
        return;
    }
    const int i = m_downloading;
    const int generation = m_generation;
    Row &row = m_items[i];
    row.state = Row::Working;
    updateRow(i);
    updateFooter();
    const qint64 fileId = row.results[row.chosen].fileId;
    OpenSubtitles::instance().download(fileId, [this, i, generation](const QByteArray &data, const QString &fileName,
                                                                     int remaining, const QString &error) {
        if (generation != m_generation || i >= m_items.size())
            return;
        Row &r = m_items[i];
        if (!error.isEmpty()) {
            r.state = Row::Found; // can be retried
            updateRow(i);
            m_downloading = -1;
            m_status->setText(error);
            m_status->setProperty("sticky", true);
            updateFooter();
            return;
        }
        save(r, data, fileName);
        updateRow(i);
        if (remaining >= 0) {
            m_status->setText(count(remaining, tr("Saved. %1 download left today."), tr("Saved. %1 downloads left today.")));
            m_status->setProperty("sticky", true);
        }
        ++m_downloading;
        downloadNext();
    });
}

void SubtitleOverlay::fetchLink()
{
    const QUrl url = QUrl::fromUserInput(m_search->text().trimmed());
    if (!url.isValid() || !(url.scheme() == "http" || url.scheme() == "https") || m_items.isEmpty()) {
        m_status->setText(tr("Paste an http(s) link to a subtitle file."));
        return;
    }
    static QNetworkAccessManager *net = new QNetworkAccessManager(qApp);
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "VMP");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);
    m_downloading = 0;
    m_items[0].state = Row::Working;
    updateRow(0);
    updateFooter();
    const int generation = m_generation;
    QNetworkReply *reply = net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, generation] {
        reply->deleteLater();
        if (generation != m_generation || m_items.isEmpty())
            return;
        m_downloading = -1;
        Row &r = m_items[0];
        const QByteArray data = reply->readAll();
        // A subtitle is text; refuse pages and archives.
        const QByteArray head = data.left(512);
        const bool binary = head.contains('\0') || head.startsWith("PK") || head.trimmed().startsWith("<");
        if (reply->error() != QNetworkReply::NoError || data.isEmpty() || data.size() > 20 * 1024 * 1024 || binary) {
            r.state = Row::Failed;
            r.note = reply->error() != QNetworkReply::NoError ? reply->errorString()
                                                              : tr("That link isn't a subtitle file");
        } else {
            save(r, data, url.fileName());
        }
        updateRow(0);
        updateFooter();
    });
}

void SubtitleOverlay::layoutPanel()
{
    if (!parentWidget())
        return;
    setGeometry(parentWidget()->rect());
    const int w = qMin(1180, width() - 60);
    const int h = qMin(760, height() - 60);
    m_panel->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);
}

bool SubtitleOverlay::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == parentWidget() && event->type() == QEvent::Resize && isVisible())
        layoutPanel();
    return QWidget::eventFilter(obj, event);
}

void SubtitleOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 150));
}

void SubtitleOverlay::mousePressEvent(QMouseEvent *e)
{
    if (!m_panel->geometry().contains(e->position().toPoint()))
        close();
}

void SubtitleOverlay::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(e);
}
