#pragma once

#include <QString>
#include <QStringList>

class QWidget;

// File pickers styled like the rest of the app (Qt's own dialog with the
// app's theme, icons and sidebar places). Same arguments as QFileDialog's
// static functions; an empty result means cancelled.
namespace FileDialogs {

QStringList openFiles(QWidget *parent, const QString &title, const QString &dir, const QString &filter);
QString openFile(QWidget *parent, const QString &title, const QString &dir, const QString &filter);
QString existingDirectory(QWidget *parent, const QString &title, const QString &dir = {});
QString saveFile(QWidget *parent, const QString &title, const QString &path);

} // namespace FileDialogs
