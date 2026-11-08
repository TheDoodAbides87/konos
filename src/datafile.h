#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Saving and loading Konos's own files safely.
namespace DataFile {

// Writes the whole file or nothing: the data goes to a temporary file that
// replaces the old one only once it's complete, so a crash, a full disk or
// a power cut mid-save leaves the previous version intact. Makes the folder.
bool write(const QString &path, const QByteArray &data);

// Reads a JSON file saved with write(). A file that exists but can't be read
// as JSON is moved aside as "<name>.broken-<date>" (and a warning logged)
// rather than being overwritten by the next save, so it can be recovered.
QJsonObject readJson(const QString &path);

// The files readJson() set aside this run, to tell the user about.
QStringList setAside();

} // namespace DataFile
