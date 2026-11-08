#include "profiles.h"
#include "effects.h"
#include "prefs.h"
#include "shortcuts.h"
#include "theme.h"
#include "widgets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>

Profiles &Profiles::instance()
{
    static Profiles p;
    return p;
}

bool Profiles::isShared(const QString &key)
{
    static const QStringList shared = {"profiles/", "session/", "window/", "updates/", "opensubtitles/",
                                       "onboarding/", "library/", "sort/"};
    for (const QString &prefix : shared)
        if (key.startsWith(prefix))
            return true;
    return false;
}

QStringList Profiles::names() const
{
    const QStringList saved = QSettings().value("profiles/names").toStringList();
    return saved.isEmpty() ? QStringList{tr("Movies"), tr("Shows"), tr("Music"), tr("Work")} : saved;
}

QString Profiles::current() const
{
    const QString c = QSettings().value("profiles/current").toString();
    return c.isEmpty() ? names().value(0) : c;
}

void Profiles::setNames(const QStringList &names)
{
    QSettings().setValue("profiles/names", names);
}

QString Profiles::fileFor(const QString &name) const
{
    QString safe = name;
    safe.replace(QRegularExpression("[^\\w\\- ]"), "_");
    return QFileInfo(QSettings().fileName()).path() + "/profiles/" + safe + ".conf";
}

void Profiles::saveCurrent() const
{
    QDir().mkpath(QFileInfo(fileFor(current())).path());
    QSettings main;
    QSettings out(fileFor(current()), QSettings::IniFormat);
    out.clear();
    for (const QString &key : main.allKeys())
        if (!isShared(key))
            out.setValue(key, main.value(key));
    out.sync();
}

void Profiles::switchTo(const QString &name)
{
    if (name == current() || !names().contains(name))
        return;
    saveCurrent();
    QSettings main;
    QStringList changedKeys;
    if (QFile::exists(fileFor(name))) {
        QSettings in(fileFor(name), QSettings::IniFormat);
        const QStringList wanted = in.allKeys();
        for (const QString &key : main.allKeys())
            if (!isShared(key) && !wanted.contains(key)) {
                main.remove(key);
                changedKeys << key;
            }
        for (const QString &key : wanted) {
            if (isShared(key))
                continue;
            const QVariant v = in.value(key);
            if (!main.contains(key) || main.value(key).toString() != v.toString() || v.typeId() == QMetaType::QStringList) {
                main.setValue(key, v);
                changedKeys << key;
            }
        }
    }
    // A profile used for the first time starts as a copy of the settings in use.
    main.setValue("profiles/current", name);
    main.sync();

    // Tell everything that reads these settings.
    for (const QString &key : changedKeys)
        emit prefs().changed(key);
    Theme::loadSaved();
    fx().load();
    for (auto g : {Effects::Equalizer, Effects::AudioFilters, Effects::VideoAdjust, Effects::VideoFilters,
                   Effects::Crop, Effects::SubtitleStyle})
        fx().commit(g);
    emit Shortcuts::instance().changed();
    emit changed();
}

bool Profiles::create(const QString &rawName)
{
    const QString name = rawName.trimmed();
    if (name.isEmpty() || names().contains(name, Qt::CaseInsensitive))
        return false;
    saveCurrent();
    QFile::remove(fileFor(name)); // a leftover from a deleted profile of that name
    setNames(names() << name);
    switchTo(name); // no file yet: it keeps the settings in use
    return true;
}

bool Profiles::remove(const QString &name)
{
    if (name == current() || !names().contains(name) || names().size() < 2)
        return false;
    QStringList list = names();
    list.removeAll(name);
    setNames(list);
    QFile::remove(fileFor(name));
    emit changed();
    return true;
}

// ---------------------------------------------------------------------------
// NewProfileOverlay

NewProfileOverlay::NewProfileOverlay(QWidget *window) : PanelOverlay(tr("New Profile"), {520, 270}, window)
{
    auto *intro = Dialog::caption(tr("A new profile starts as a copy of the settings in use (“%1”). Change "
                                "anything after switching to it; the other profiles keep theirs.")
                                 .arg(Profiles::instance().current()));
    body()->addWidget(intro);
    auto *name = new QLineEdit;
    name->setPlaceholderText(tr("Name, like “Late Night”"));
    name->setFixedHeight(38);
    body()->addWidget(name);
    auto *status = new QLabel;
    status->setProperty("role", "dim");
    body()->addWidget(status);
    body()->addStretch();
    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *cancel = Dialog::button(tr("Cancel"));
    auto *create = Dialog::button(tr("Create and Switch"), true);
    foot->addWidget(cancel);
    foot->addWidget(create);
    body()->addLayout(foot);
    connect(cancel, &QPushButton::clicked, this, &PanelOverlay::close);
    auto go = [this, name, status] {
        if (Profiles::instance().create(name->text()))
            close();
        else
            status->setText(name->text().trimmed().isEmpty() ? tr("Give it a name first.")
                                                             : tr("There's already a profile called that."));
    };
    connect(create, &QPushButton::clicked, this, go);
    connect(name, &QLineEdit::returnPressed, this, go);
    name->setFocus();
}
