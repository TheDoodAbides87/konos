#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include "widgets.h"

class Player;

// Per-show presets: subtitle timing and look, default subtitle and audio
// tracks and audio delay, saved under a name and linked to a show's folder.
// Opening anything in (or under) a linked folder brings its preset back;
// leaving it restores the settings from before.
//
// Everything a preset holds is otherwise ordinary "current" state (Effects
// for the look, prefs for the default tracks, the player for delays), so the
// menus edit it the same way whether or not a preset is active.
class ShowPresets : public QObject {
    Q_OBJECT
public:
    static ShowPresets &instance();
    void setPlayer(Player *player) { m_player = player; }

    QStringList names() const;
    QString active() const { return m_active; }
    // The preset linked to this file's folder or the nearest folder above it.
    QString presetFor(const QString &location) const;

    // Around Player::open(): style before (it's read when the item opens),
    // delays after (opening a new item resets them).
    void beforeOpen(const QString &location);
    void afterOpen();

    // From the menus, for what's playing now.
    void choose(const QString &name); // empty: no preset for this show
    bool saveAs(const QString &name);
    void update();                    // the active preset := current settings
    void remove(const QString &name);

    // Picks the default subtitle and audio tracks ("subs/default",
    // "audio/default") once the current item's tracks are known.
    void applyDefaultTracks();
    // Rule text for menus: "" (leave it to the file), "off", "track:N", "lang:xx".
    static QString describeRule(const QString &rule);

    // A library folder's own track settings (the library's ⋯ → Edit Subtitle
    // Tracks / Edit Audio Tracks): default track rules that win over the
    // global ones, and files attached to its videos (file name → subtitle or
    // audio file), added whenever that video opens.
    struct FolderTracks {
        QString subRule, audioRule; // empty: use the global default
        QHash<QString, QString> subFiles, audioFiles;
        bool isEmpty() const { return subRule.isEmpty() && audioRule.isEmpty() && subFiles.isEmpty() && audioFiles.isEmpty(); }
    };
    FolderTracks folderTracks(const QString &folder) const { return m_folderTracks.value(folder); }
    void setFolderTracks(const QString &folder, const FolderTracks &tracks);
    QString attachedFile(const QString &location, bool subtitles) const;

signals:
    void changed();

private:
    ShowPresets();
    QJsonObject capture(bool withDelays) const;
    void applyStyle(const QJsonObject &o, bool live);
    void applyTiming(const QJsonObject &o);
    QString currentFolder() const;
    void load();
    void save() const;

    Player *m_player = nullptr;
    QHash<QString, QJsonObject> m_presets;
    QHash<QString, QString> m_folders; // folder → preset name
    QHash<QString, FolderTracks> m_folderTracks;
    QString folderRule(bool subtitles) const; // for what's playing, from its folder or one above
    QJsonObject m_base;                // settings to go back to after a preset
    QString m_active;
};

// "Preset for This Show" dropdown: the presets, plus save/update/delete.
class PresetPicker : public Dropdown {
    Q_OBJECT
public:
    explicit PresetPicker(QWidget *parent = nullptr);

private:
    void rebuild();
    bool m_rebuilding = false;
};

// Default track dropdown for subtitles or audio: leave it to the file, none
// (subtitles), the Nth track, or a language.
class DefaultTrackPicker : public Dropdown {
    Q_OBJECT
public:
    DefaultTrackPicker(bool subtitles, Player *player, QWidget *parent = nullptr);
    void rebuild();

private:
    bool m_subtitles;
    Player *m_player;
    bool m_rebuilding = false;
};
