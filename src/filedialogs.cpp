#include "filedialogs.h"
#include "theme.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QHeaderView>
#include <QMimeDatabase>
#include <QListView>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QTreeView>
#include <QUrl>

namespace {

// Line icons from the app's set instead of the desktop icon theme.
class IconProvider : public QFileIconProvider {
public:
    QIcon icon(IconType type) const override
    {
        switch (type) {
        case Computer:
        case Drive:
            return theme().icon("disc");
        case Folder:
            return folder();
        case Network:
            return theme().icon("network");
        default:
            return theme().icon("file");
        }
    }

    QIcon icon(const QFileInfo &info) const override
    {
        if (info.isRoot())
            return theme().icon("disc");
        if (info.isDir())
            return info.absoluteFilePath() == QDir::homePath() ? theme().icon("home", theme().accent()) : folder();
        static const QMimeDatabase db;
        const QString mime = db.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name();
        if (mime.startsWith("video/"))
            return theme().icon("film");
        if (mime.startsWith("audio/"))
            return theme().icon("note");
        static const QStringList subs = {"srt", "ass", "ssa", "sub", "vtt", "idx", "smi"};
        if (subs.contains(info.suffix().toLower()))
            return theme().icon("subtitles");
        if (mime.contains("mpegurl") || mime.contains("xspf") || mime.contains("scpls"))
            return theme().icon("playlist");
        return theme().icon("file");
    }

private:
    static QIcon folder() { return theme().icon("folder-fill", theme().pal().textDim); }
};

QList<QUrl> places()
{
    QList<QUrl> urls{QUrl::fromLocalFile(QDir::homePath())};
    for (auto location : {QStandardPaths::MoviesLocation, QStandardPaths::MusicLocation,
                          QStandardPaths::DownloadLocation, QStandardPaths::DesktopLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty() && path != QDir::homePath() && QDir(path).exists())
            urls << QUrl::fromLocalFile(path);
    }
    urls << QUrl("file:"); // "Computer": drives and mounts
    return urls;
}

void prepare(QFileDialog &d)
{
    d.setOption(QFileDialog::DontUseNativeDialog);
    d.setObjectName("FileDialog");
    static IconProvider icons; // icons are drawn on request, so they follow the theme
    d.setIconProvider(&icons);
    d.setSidebarUrls(places());
    d.setViewMode(QFileDialog::Detail);
    d.resize(900, 560);
    if (auto *box = d.findChild<QDialogButtonBox *>()) {
        for (auto which : {QDialogButtonBox::Open, QDialogButtonBox::Save})
            if (QPushButton *b = box->button(which)) {
                b->setProperty("role", "accent");
                b->style()->unpolish(b); // re-read the style sheet for the new role
                b->style()->polish(b);
            }
        for (QPushButton *b : box->findChildren<QPushButton *>())
            b->setCursor(Qt::PointingHandCursor);
    }
    if (auto *tree = d.findChild<QTreeView *>("treeView")) {
        tree->setAlternatingRowColors(false);
        tree->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        tree->header()->setMinimumSectionSize(80);
        // Name takes the spare width; size, type and date fit their contents.
        tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column < 4; ++column)
            tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
        tree->header()->setStretchLastSection(false);
    }
    if (auto *sidebar = d.findChild<QListView *>("sidebar")) {
        sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sidebar->setIconSize(QSize(18, 18));
    }
    if (auto *splitter = d.findChild<QSplitter *>("splitter"))
        splitter->setSizes({180, d.width() - 180});
}

bool run(QFileDialog &d)
{
    return d.exec() == QDialog::Accepted;
}

} // namespace

namespace FileDialogs {

QStringList openFiles(QWidget *parent, const QString &title, const QString &dir, const QString &filter)
{
    QFileDialog d(parent, title, dir, filter);
    d.setFileMode(QFileDialog::ExistingFiles);
    prepare(d);
    return run(d) ? d.selectedFiles() : QStringList();
}

QString openFile(QWidget *parent, const QString &title, const QString &dir, const QString &filter)
{
    QFileDialog d(parent, title, dir, filter);
    d.setFileMode(QFileDialog::ExistingFile);
    prepare(d);
    return run(d) ? d.selectedFiles().value(0) : QString();
}

QString existingDirectory(QWidget *parent, const QString &title, const QString &dir)
{
    QFileDialog d(parent, title, dir.isEmpty() ? QDir::homePath() : dir);
    d.setFileMode(QFileDialog::Directory);
    d.setOption(QFileDialog::ShowDirsOnly);
    prepare(d);
    // A folder picker has no file types to choose from.
    for (const char *name : {"fileTypeCombo", "fileTypeLabel"})
        if (auto *w = d.findChild<QWidget *>(name))
            w->hide();
    return run(d) ? d.selectedFiles().value(0) : QString();
}

QString saveFile(QWidget *parent, const QString &title, const QString &path)
{
    QFileDialog d(parent, title, path);
    d.setAcceptMode(QFileDialog::AcceptSave);
    prepare(d);
    return run(d) ? d.selectedFiles().value(0) : QString();
}

} // namespace FileDialogs
