#pragma once

#include "musiceditors.h" // PanelOverlay

#include <QHash>
#include <QKeySequence>
#include <QObject>

class QAction;
class QScrollArea;

// Every keyboard shortcut VMP has, with its default keys and the keys the
// user picked instead (stored under "keys/<id>" in the settings file; only
// changed ones are stored). Each action has up to two keys.
class Shortcuts : public QObject {
    Q_OBJECT
public:
    struct Def {
        QString id, group, label;
        QList<QKeySequence> defaults;
    };
    using KeyMap = QHash<QString, QList<QKeySequence>>;

    static Shortcuts &instance();
    const QList<Def> &defs() const { return m_defs; }
    const Def *def(const QString &id) const;
    QList<QKeySequence> keys(const QString &id) const;
    KeyMap all() const;
    KeyMap defaults() const;
    void setAll(const KeyMap &keys);
    // The action's shortcuts follow the user's choice from now on.
    void bind(QAction *action, const QString &id);
    // "Ctrl + Shift + O", "→", "Space".
    static QString keyText(const QKeySequence &seq);
    QString text(const QString &id) const; // all keys, " / " between them

signals:
    void changed();

private:
    Shortcuts();
    QList<Def> m_defs;
};

class KeyField;

// Main menu → "Keyboard Shortcuts": every shortcut, searchable, each with a
// main and a second key that can be changed.
class KeyboardShortcutsOverlay : public PanelOverlay {
    Q_OBJECT
public:
    explicit KeyboardShortcutsOverlay(QWidget *window);

private:
    void assign(const QString &id, int slot, const QKeySequence &seq);
    void refreshFields();
    void filter();

    struct Row {
        QString id;
        QWidget *widget;
        KeyField *fields[2];
        QLabel *heading; // the group's heading
    };
    Shortcuts::KeyMap m_keys;
    QList<Row> m_rows;
    QList<QLabel *> m_headings;
    QLineEdit *m_search;
    class Dropdown *m_searchIn;
    QLabel *m_status;
};
