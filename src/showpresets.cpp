#include "showpresets.h"
#include "effects.h"
#include "player.h"
#include "prefs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMessageBox>
#include <QStandardPaths>
#include <QUrl>

static QString presetsFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/presets.json";
}

ShowPresets &ShowPresets::instance()
{
    static ShowPresets s;
    return s;
}

ShowPresets::ShowPresets()
{
    load();
}

void ShowPresets::load()
{
    QFile f(presetsFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject presets = root.value("presets").toObject();
    for (auto it = presets.begin(); it != presets.end(); ++it)
        m_presets.insert(it.key(), it.value().toObject());
    const QJsonObject folders = root.value("folders").toObject();
    for (auto it = folders.begin(); it != folders.end(); ++it)
        m_folders.insert(it.key(), it.value().toString());
    m_base = root.value("base").toObject();
    const QJsonObject tracks = root.value("folderTracks").toObject();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        FolderTracks t;
        t.subRule = o.value("sub").toString();
        t.audioRule = o.value("audio").toString();
        const QJsonObject subs = o.value("subFiles").toObject(), audio = o.value("audioFiles").toObject();
        for (auto f = subs.begin(); f != subs.end(); ++f)
            t.subFiles.insert(f.key(), f.value().toString());
        for (auto f = audio.begin(); f != audio.end(); ++f)
            t.audioFiles.insert(f.key(), f.value().toString());
        m_folderTracks.insert(it.key(), t);
    }
    m_active = root.value("active").toString();
    if (!m_presets.contains(m_active))
        m_active.clear();
}

void ShowPresets::save() const
{
    QJsonObject presets, folders;
    for (auto it = m_presets.begin(); it != m_presets.end(); ++it)
        presets.insert(it.key(), it.value());
    for (auto it = m_folders.begin(); it != m_folders.end(); ++it)
        folders.insert(it.key(), it.value());
    QJsonObject tracks;
    for (auto it = m_folderTracks.begin(); it != m_folderTracks.end(); ++it) {
        QJsonObject subs, audio;
        for (auto f = it->subFiles.begin(); f != it->subFiles.end(); ++f)
            subs.insert(f.key(), f.value());
        for (auto f = it->audioFiles.begin(); f != it->audioFiles.end(); ++f)
            audio.insert(f.key(), f.value());
        tracks.insert(it.key(), QJsonObject{{"sub", it->subRule}, {"audio", it->audioRule}, {"subFiles", subs}, {"audioFiles", audio}});
    }
    const QJsonObject root{{"presets", presets}, {"folders", folders}, {"base", m_base}, {"active", m_active},
                           {"folderTracks", tracks}};
    QDir().mkpath(QFileInfo(presetsFile()).absolutePath());
    QFile f(presetsFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson());
}

QStringList ShowPresets::names() const
{
    QStringList out = m_presets.keys();
    out.sort(Qt::CaseInsensitive);
    return out;
}

static QString folderOf(const QString &location)
{
    if (location.isEmpty())
        return {};
    const QUrl url(location);
    if (url.isValid() && url.scheme().size() > 1 && !url.isLocalFile())
        return {}; // streams have no show folder
    const QString path = url.isLocalFile() ? url.toLocalFile() : location;
    return QFileInfo(path).absolutePath();
}

QString ShowPresets::presetFor(const QString &location) const
{
    QString dir = folderOf(location);
    while (!dir.isEmpty()) {
        const QString name = m_folders.value(dir);
        if (!name.isEmpty() && m_presets.contains(name))
            return name;
        const QString up = QFileInfo(dir).absolutePath();
        if (up == dir)
            break;
        dir = up;
    }
    return {};
}

QString ShowPresets::currentFolder() const
{
    return m_player ? folderOf(m_player->location()) : QString();
}

// ---------------------------------------------------------------------------

QJsonObject ShowPresets::capture(bool withDelays) const
{
    const EffectsData &e = fx().data();
    QJsonObject o{
        {"subFont", e.subFont},
        {"subColor", e.subColor.name()},
        {"subOutline", e.subOutline.name()},
        {"subSize", e.subSize},
        {"subSizePixels", e.subSizePixels},
        {"subOutlineThickness", e.subOutlineThickness},
        {"subBackgroundOpacity", e.subBackgroundOpacity},
        {"subMargin", e.subMargin},
        {"subEmbeddedStyle", e.subEmbeddedStyle},
        {"subDefault", prefs().string("subs/default")},
        {"audioDefault", prefs().string("audio/default")},
    };
    if (m_player) {
        o.insert("subSpeed", double(m_player->subtitleSpeed()));
        o.insert("subDuration", double(m_player->subtitleDurationFactor()));
        if (withDelays) {
            o.insert("subDelay", double(m_player->subtitleDelay()));
            o.insert("audioDelay", double(m_player->audioDelay()));
        }
    }
    return o;
}

void ShowPresets::applyStyle(const QJsonObject &o, bool live)
{
    if (o.isEmpty())
        return;
    EffectsData &e = fx().d();
    const EffectsData before = e;
    e.subFont = o.value("subFont").toString(e.subFont);
    e.subColor = QColor(o.value("subColor").toString(e.subColor.name()));
    e.subOutline = QColor(o.value("subOutline").toString(e.subOutline.name()));
    e.subSize = o.value("subSize").toInt(e.subSize);
    e.subSizePixels = o.value("subSizePixels").toInt(e.subSizePixels);
    e.subOutlineThickness = o.value("subOutlineThickness").toInt(e.subOutlineThickness);
    e.subBackgroundOpacity = o.value("subBackgroundOpacity").toInt(e.subBackgroundOpacity);
    e.subMargin = o.value("subMargin").toInt(e.subMargin);
    e.subEmbeddedStyle = o.value("subEmbeddedStyle").toBool(e.subEmbeddedStyle);
    const bool styleChanged = e.subFont != before.subFont || e.subColor != before.subColor ||
                              e.subOutline != before.subOutline || e.subSize != before.subSize ||
                              e.subSizePixels != before.subSizePixels ||
                              e.subOutlineThickness != before.subOutlineThickness ||
                              e.subBackgroundOpacity != before.subBackgroundOpacity ||
                              e.subEmbeddedStyle != before.subEmbeddedStyle;
    if (styleChanged)
        fx().commit(Effects::SubtitleStyle, live);
    else if (e.subMargin != before.subMargin)
        fx().commit(Effects::SubtitleStyle, false); // position applies live without reopening
    if (m_player)
        m_player->setSubtitleMargin(e.subMargin);
    if (o.contains("subDefault"))
        prefs().set("subs/default", o.value("subDefault").toString());
    if (o.contains("audioDefault"))
        prefs().set("audio/default", o.value("audioDefault").toString());
    if (m_player && o.contains("subSpeed"))
        m_player->setSubtitleSpeed(float(o.value("subSpeed").toDouble(1.0)));
    if (m_player && o.contains("subDuration"))
        m_player->setSubtitleDurationFactor(float(o.value("subDuration").toDouble(0.0)));
}

void ShowPresets::applyTiming(const QJsonObject &o)
{
    if (!m_player)
        return;
    if (o.contains("subDelay"))
        m_player->setSubtitleDelay(qint64(o.value("subDelay").toDouble()));
    if (o.contains("audioDelay"))
        m_player->setAudioDelay(qint64(o.value("audioDelay").toDouble()));
}

void ShowPresets::beforeOpen(const QString &location)
{
    const QString name = presetFor(location);
    if (!name.isEmpty()) {
        if (m_active.isEmpty())
            m_base = capture(false); // what to return to after this show
        m_active = name;
        applyStyle(m_presets.value(name), false);
    } else {
        if (!m_active.isEmpty()) {
            m_active.clear();
            applyStyle(m_base, false);
        }
        m_base = capture(false);
    }
    save();
    emit changed();
}

void ShowPresets::afterOpen()
{
    if (!m_active.isEmpty())
        applyTiming(m_presets.value(m_active));
}

void ShowPresets::choose(const QString &name)
{
    const QString folder = currentFolder();
    if (name.isEmpty()) {
        if (!folder.isEmpty())
            for (const QString &key : m_folders.keys())
                if (key == folder || folder.startsWith(key + "/"))
                    m_folders.remove(key);
        if (!m_active.isEmpty()) {
            m_active.clear();
            applyStyle(m_base, true);
        }
    } else if (m_presets.contains(name)) {
        if (!folder.isEmpty())
            m_folders.insert(folder, name);
        if (m_active.isEmpty())
            m_base = capture(false);
        m_active = name;
        applyStyle(m_presets.value(name), true);
        applyTiming(m_presets.value(name));
    }
    save();
    emit changed();
}

bool ShowPresets::saveAs(const QString &name)
{
    const QString n = name.trimmed();
    if (n.isEmpty())
        return false;
    m_presets.insert(n, capture(true));
    if (const QString folder = currentFolder(); !folder.isEmpty())
        m_folders.insert(folder, n);
    m_active = n;
    save();
    emit changed();
    return true;
}

void ShowPresets::update()
{
    if (m_active.isEmpty())
        return;
    m_presets.insert(m_active, capture(true));
    save();
    emit changed();
}

void ShowPresets::remove(const QString &name)
{
    m_presets.remove(name);
    for (const QString &key : m_folders.keys())
        if (m_folders.value(key) == name)
            m_folders.remove(key);
    if (m_active == name)
        m_active.clear(); // keep the settings as they are now
    save();
    emit changed();
}

// ---------------------------------------------------------------------------
// Folder track settings

void ShowPresets::setFolderTracks(const QString &folder, const FolderTracks &tracks)
{
    if (tracks.isEmpty())
        m_folderTracks.remove(folder);
    else
        m_folderTracks.insert(folder, tracks);
    save();
    emit changed();
}

QString ShowPresets::attachedFile(const QString &location, bool subtitles) const
{
    const QString folder = folderOf(location);
    if (folder.isEmpty() || !m_folderTracks.contains(folder))
        return {};
    const FolderTracks &t = m_folderTracks[folder];
    const QString file = (subtitles ? t.subFiles : t.audioFiles).value(QFileInfo(location).fileName());
    return QFileInfo::exists(file) ? file : QString();
}

QString ShowPresets::folderRule(bool subtitles) const
{
    QString dir = currentFolder();
    while (!dir.isEmpty()) {
        const FolderTracks t = m_folderTracks.value(dir);
        const QString rule = subtitles ? t.subRule : t.audioRule;
        if (!rule.isEmpty())
            return rule;
        const QString up = QFileInfo(dir).absolutePath();
        if (up == dir)
            break;
        dir = up;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Default tracks

static QLocale::Language languageOf(const QString &code)
{
    if (code.isEmpty())
        return QLocale::AnyLanguage;
    return QLocale::codeToLanguage(code.toLower(), QLocale::AnyLanguageCode);
}

QString ShowPresets::describeRule(const QString &rule)
{
    if (rule.isEmpty())
        return tr("Automatic");
    if (rule == "off")
        return tr("None");
    if (rule.startsWith("track:"))
        return tr("Track %1").arg(rule.mid(6));
    if (rule.startsWith("lang:"))
        return QLocale::languageToString(languageOf(rule.mid(5)));
    return rule;
}

void ShowPresets::applyDefaultTracks()
{
    if (!m_player)
        return;
    QHash<int, QString> langs;
    for (const Player::TrackInfo &t : m_player->trackInfo())
        langs.insert(t.id, t.language);

    // Returns the chosen track id, -1 for none, or -2 for "leave it alone".
    auto pick = [&](const QString &rule, const QList<Player::Track> &tracks) -> int {
        if (rule.isEmpty())
            return -2;
        if (rule == "off")
            return -1;
        QList<Player::Track> real;
        for (const Player::Track &t : tracks)
            if (t.id >= 0)
                real << t;
        if (rule.startsWith("track:")) {
            const int n = rule.mid(6).toInt();
            return n >= 1 && n <= real.size() ? real.at(n - 1).id : -2;
        }
        if (rule.startsWith("lang:")) {
            const QLocale::Language want = languageOf(rule.mid(5));
            const QString wantName = QLocale::languageToString(want);
            for (const Player::Track &t : real) // tagged language first
                if (languageOf(langs.value(t.id)) == want)
                    return t.id;
            for (const Player::Track &t : real) // then the name ("… [French]", "file.fr.srt")
                if (t.name.contains(wantName, Qt::CaseInsensitive) ||
                    t.name.contains("." + rule.mid(5) + ".", Qt::CaseInsensitive) ||
                    t.name.contains("[" + rule.mid(5) + "]", Qt::CaseInsensitive))
                    return t.id;
        }
        return -2;
    };
    const QString subRule = folderRule(true), audioRule = folderRule(false);
    const int sub = pick(subRule.isEmpty() ? prefs().string("subs/default") : subRule, m_player->subtitleTracks());
    if (sub != -2 && sub != m_player->subtitleTrack())
        m_player->setSubtitleTrack(sub);
    const int audio = pick(audioRule.isEmpty() ? prefs().string("audio/default") : audioRule, m_player->audioTracks());
    if (audio >= 0 && audio != m_player->audioTrack())
        m_player->setAudioTrack(audio);
}

// ---------------------------------------------------------------------------

PresetPicker::PresetPicker(QWidget *parent) : Dropdown(parent)
{
    rebuild();
    connect(&ShowPresets::instance(), &ShowPresets::changed, this, &PresetPicker::rebuild);
    connect(this, &QComboBox::activated, this, [this](int i) {
        if (m_rebuilding)
            return;
        ShowPresets &sp = ShowPresets::instance();
        const QString data = itemData(i).toString();
        if (data == "::new") {
            bool ok = false;
            const QString name = QInputDialog::getText(window(), tr("Save Preset"),
                                                       tr("Name (for example, the show's title):"),
                                                       QLineEdit::Normal, QString(), &ok);
            if (ok && !name.trimmed().isEmpty())
                sp.saveAs(name);
        } else if (data == "::update") {
            sp.update();
        } else if (data == "::delete") {
            if (QMessageBox::question(window(), tr("Delete Preset"), tr("Delete the preset “%1”?").arg(sp.active())) ==
                QMessageBox::Yes)
                sp.remove(sp.active());
        } else {
            sp.choose(data);
        }
        rebuild();
    });
}

void PresetPicker::rebuild()
{
    m_rebuilding = true;
    const QSignalBlocker block(this);
    ShowPresets &sp = ShowPresets::instance();
    clear();
    addItem(tr("No Preset"), QString());
    for (const QString &name : sp.names())
        addItem(name, name);
    insertSeparator(count());
    addItem(tr("Save as New Preset…"), QString("::new"));
    if (!sp.active().isEmpty()) {
        addItem(tr("Update “%1”").arg(sp.active()), QString("::update"));
        addItem(tr("Delete “%1”").arg(sp.active()), QString("::delete"));
    }
    setCurrentIndex(qMax(0, findData(sp.active())));
    setToolTip(tr("Subtitle look and timing, default tracks and audio delay, remembered for this show's folder"));
    m_rebuilding = false;
}

// ---------------------------------------------------------------------------

DefaultTrackPicker::DefaultTrackPicker(bool subtitles, Player *player, QWidget *parent)
    : Dropdown(parent), m_subtitles(subtitles), m_player(player)
{
    rebuild();
    const char *key = subtitles ? "subs/default" : "audio/default";
    connect(this, &QComboBox::currentIndexChanged, this, [this, key](int i) {
        if (m_rebuilding || i < 0)
            return;
        prefs().set(key, itemData(i).toString());
        ShowPresets::instance().applyDefaultTracks(); // try it on what's playing now
    });
    connect(&prefs(), &Prefs::changed, this, [this, key](const QString &k) {
        if (k == key)
            rebuild();
    });
    connect(player, &Player::tracksChanged, this, &DefaultTrackPicker::rebuild);
}

void DefaultTrackPicker::rebuild()
{
    m_rebuilding = true;
    const QSignalBlocker block(this);
    const QString rule = prefs().string(m_subtitles ? "subs/default" : "audio/default");
    clear();
    addItem(ShowPresets::describeRule(""), QString());
    if (m_subtitles)
        addItem(ShowPresets::describeRule("off"), QString("off"));
    // Track numbers: as many as the current item has (at least 3).
    int count = 0;
    const QList<Player::Track> tracks = m_subtitles ? m_player->subtitleTracks() : m_player->audioTracks();
    for (const Player::Track &t : tracks)
        count += t.id >= 0;
    for (int n = 1; n <= qMax(3, count); ++n)
        addItem(ShowPresets::describeRule(QString("track:%1").arg(n)), QString("track:%1").arg(n));
    // Languages: those in the current item first, then common ones.
    QStringList codes;
    for (const Player::TrackInfo &t : m_player->trackInfo())
        if ((t.type == (m_subtitles ? Player::TrackInfo::Subtitle : Player::TrackInfo::Audio)) && !t.language.isEmpty()) {
            const QLocale::Language l = QLocale::codeToLanguage(t.language.toLower(), QLocale::AnyLanguageCode);
            if (l != QLocale::AnyLanguage && l != QLocale::C)
                codes << QLocale(l).name().section('_', 0, 0);
        }
    codes << QStringList{"en", "es", "fr", "de", "it", "pt", "ja", "ko", "zh", "ru", "ar", "hi", "nl", "sv", "pl"};
    if (rule.startsWith("lang:"))
        codes.prepend(rule.mid(5));
    codes.removeDuplicates();
    QList<QPair<QString, QString>> langs;
    for (const QString &c : codes)
        langs << qMakePair(ShowPresets::describeRule("lang:" + c), c);
    std::sort(langs.begin(), langs.end());
    insertSeparator(this->count());
    for (const auto &[name, code] : langs)
        addItem(name, "lang:" + code);
    int at = findData(rule);
    if (at < 0) {
        addItem(ShowPresets::describeRule(rule), rule);
        at = this->count() - 1;
    }
    setCurrentIndex(at);
    m_rebuilding = false;
}
