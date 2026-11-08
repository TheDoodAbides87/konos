#pragma once

#include "musiceditors.h" // PanelOverlay

#include <QObject>

// Main menu → Profile (from the redesign video, 19:24): named sets of
// settings (Movies, Shows, Music, Work, or your own). Switching saves the
// settings in use to the current profile and loads the other one: playback,
// video and audio settings, effects, subtitles, keys, the playback bar's
// layout, theme. Library folders, window size, sign-ins and update checks
// are shared by every profile. Each profile is a file in the config folder's
// "profiles" folder; a new one starts as a copy of the settings in use.
class Profiles : public QObject {
    Q_OBJECT
public:
    static Profiles &instance();
    QStringList names() const;
    QString current() const;
    void switchTo(const QString &name);
    bool create(const QString &name); // a copy of the settings in use; switches to it
    bool remove(const QString &name);  // not the current one
    static bool isShared(const QString &key);

signals:
    void changed();

private:
    Profiles() = default;
    QString fileFor(const QString &name) const;
    void saveCurrent() const;
    void setNames(const QStringList &names);
};

// "New Profile…": its name.
class NewProfileOverlay : public PanelOverlay {
    Q_OBJECT
public:
    explicit NewProfileOverlay(QWidget *window);
};
