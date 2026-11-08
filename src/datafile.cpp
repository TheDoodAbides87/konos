#include "datafile.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace DataFile {

static QStringList &asideList()
{
    static QStringList list;
    return list;
}

QStringList setAside()
{
    return asideList();
}

bool write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        f.cancelWriting();
        qWarning("Couldn't save %s: %s", qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    if (!f.commit()) {
        qWarning("Couldn't save %s: %s", qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    return true;
}

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = f.readAll();
    f.close();
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error == QJsonParseError::NoError && doc.isObject())
        return doc.object();
    // Keep it for recovery instead of letting the next save replace it.
    const QString aside = path + ".broken-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
    QFile::rename(path, aside);
    asideList() << aside;
    qWarning("%s couldn't be read (%s); kept as %s and starting fresh", qPrintable(path),
             data.isEmpty() ? "empty" : qPrintable(error.errorString()), qPrintable(aside));
    return {};
}

} // namespace DataFile
