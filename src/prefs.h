#pragma once

#include <QObject>
#include <QVariant>

// Application preferences backed by QSettings, with change notification.
class Prefs : public QObject {
    Q_OBJECT
public:
    static Prefs &instance();

    QVariant get(const QString &key) const;
    void set(const QString &key, const QVariant &value);

    bool flag(const QString &key) const { return get(key).toBool(); }
    int integer(const QString &key) const { return get(key).toInt(); }
    QString string(const QString &key) const { return get(key).toString(); }
    QStringList list(const QString &key) const { return get(key).toStringList(); }

signals:
    void changed(const QString &key);

private:
    Prefs() = default;
};

inline Prefs &prefs() { return Prefs::instance(); }
