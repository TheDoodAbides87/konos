#pragma once

#include "libraryview.h"

#include <QScrollArea>
#include <QTimer>

class QLabel;
class QVBoxLayout;
class HomeRow;

// The library's Home: "Continue Watching" (unfinished videos), "Listen Again"
// (recent audio, with cover and artist) and "Frequently Visited Folders", each a
// single row of cards that shows as many as fit.
class HomePage : public QScrollArea {
    Q_OBJECT
public:
    explicit HomePage(QWidget *parent = nullptr);

    void reload();
    void setFilter(const QString &text);
    int itemCount() const;

signals:
    void playRequested(const QStringList &items, int index);
    void enqueueRequested(const QStringList &items);
    void folderRequested(const QString &path);
    void sectionRequested(Section section); // a row's "›" header

private:
    void showMenu(const QString &path, EntryKind kind, const QPoint &globalPos);

    HomeRow *m_continue, *m_listen, *m_folders;
    QLabel *m_empty;
    QString m_filter;
    QTimer m_reload; // progress changes can move videos in or out of Continue Watching
    QStringList continueWatching() const;
    QStringList m_shownContinue; // what Continue Watching shows now
};
