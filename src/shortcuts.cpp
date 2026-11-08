#include "shortcuts.h"
#include "theme.h"
#include "widgets.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStyle>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// Shortcuts

Shortcuts &Shortcuts::instance()
{
    static Shortcuts s;
    return s;
}

Shortcuts::Shortcuts()
{
    using K = QKeySequence;
    const QString basic = tr("Basic"), playback = tr("Playback"), av = tr("Audio & Video"), open = tr("Open & Save");
    m_defs = {
        {"fullscreen", basic, tr("Toggle fullscreen"), {K(Qt::Key_F), K(Qt::Key_F11)}},
        {"escape", basic, tr("Exit fullscreen / close panel"), {K(Qt::Key_Escape)}},
        {"hideUi", basic, tr("Hide the interface"), {K("Ctrl+\\")}},
        {"sidebar", basic, tr("Show or hide the sidebar"), {K("Ctrl+B")}},
        {"library", basic, tr("Switch between library and player"), {K("Ctrl+L")}},
        {"settings", basic, tr("Settings"), {K("Ctrl+P")}},
        {"shortcuts", basic, tr("Keyboard shortcuts"), {}},
        {"quit", basic, tr("Quit"), {K("Ctrl+Q")}},

        {"play", playback, tr("Play / pause"), {K(Qt::Key_Space)}},
        {"stop", playback, tr("Stop playback"), {K(Qt::Key_S)}},
        {"next", playback, tr("Next item"), {K(Qt::Key_N)}},
        {"prev", playback, tr("Previous item"), {K(Qt::Key_P)}},
        {"seekFwd", playback, tr("Seek forward"), {K(Qt::Key_Right)}},
        {"seekBack", playback, tr("Seek back"), {K(Qt::Key_Left)}},
        {"jumpFwd", playback, tr("Jump forward 1 minute"), {K("Ctrl+Right")}},
        {"jumpBack", playback, tr("Jump back 1 minute"), {K("Ctrl+Left")}},
        {"faster", playback, tr("Faster"), {K(Qt::Key_BracketRight)}},
        {"slower", playback, tr("Slower"), {K(Qt::Key_BracketLeft)}},
        {"normalSpeed", playback, tr("Normal speed"), {K(Qt::Key_Equal)}},
        {"bookmark", playback, tr("Add a bookmark"), {K("Ctrl+Shift+B")}},

        {"volUp", av, tr("Volume up"), {K(Qt::Key_Up)}},
        {"volDown", av, tr("Volume down"), {K(Qt::Key_Down)}},
        {"mute", av, tr("Mute"), {K(Qt::Key_M)}},
        {"audioTrack", av, tr("Next audio track"), {K(Qt::Key_B)}},
        {"subTrack", av, tr("Next subtitle track"), {K(Qt::Key_V)}},
        {"aspect", av, tr("Next aspect ratio"), {K(Qt::Key_A)}},
        {"snapshot", av, tr("Take a snapshot"), {K("Shift+S")}},
        {"record", av, tr("Start or stop recording"), {K(Qt::Key_R)}},

        {"open", open, tr("Open file"), {K("Ctrl+O")}},
        {"openMany", open, tr("Open multiple files"), {K("Ctrl+Shift+O")}},
        {"openDir", open, tr("Open directory"), {K("Ctrl+F")}},
        {"disc", open, tr("Open disc"), {K("Ctrl+D")}},
        {"network", open, tr("Open network stream"), {K("Ctrl+N")}},
        {"capture", open, tr("Open capture device"), {K("Ctrl+C")}},
        {"clipboard", open, tr("Open location from clipboard"), {K("Ctrl+V")}},
        {"savePlaylist", open, tr("Save playlist to file"), {K("Ctrl+Y")}},
        {"convert", open, tr("Convert / save"), {K("Ctrl+R")}},
        {"stream", open, tr("Stream"), {K("Ctrl+S")}},
        {"about", open, tr("About Konos"), {K("Shift+F1")}},
    };
}

const Shortcuts::Def *Shortcuts::def(const QString &id) const
{
    for (const Def &d : m_defs)
        if (d.id == id)
            return &d;
    return nullptr;
}

QList<QKeySequence> Shortcuts::keys(const QString &id) const
{
    QSettings s;
    const QString key = "keys/" + id;
    if (!s.contains(key)) {
        const Def *d = def(id);
        return d ? d->defaults : QList<QKeySequence>{};
    }
    QList<QKeySequence> out;
    for (const QString &k : s.value(key).toStringList())
        if (!k.isEmpty())
            out << QKeySequence::fromString(k, QKeySequence::PortableText);
    return out;
}

Shortcuts::KeyMap Shortcuts::all() const
{
    KeyMap m;
    for (const Def &d : m_defs)
        m.insert(d.id, keys(d.id));
    return m;
}

Shortcuts::KeyMap Shortcuts::defaults() const
{
    KeyMap m;
    for (const Def &d : m_defs)
        m.insert(d.id, d.defaults);
    return m;
}

void Shortcuts::setAll(const KeyMap &keys)
{
    QSettings s;
    for (const Def &d : m_defs) {
        QList<QKeySequence> k = keys.value(d.id);
        k.removeAll(QKeySequence());
        if (k == d.defaults) {
            s.remove("keys/" + d.id);
            continue;
        }
        QStringList text;
        for (const QKeySequence &seq : k)
            text << seq.toString(QKeySequence::PortableText);
        s.setValue("keys/" + d.id, text.isEmpty() ? QStringList{QString()} : text);
    }
    emit changed();
}

void Shortcuts::bind(QAction *action, const QString &id)
{
    action->setShortcuts(keys(id));
    action->setProperty("shortcutId", id);
    connect(this, &Shortcuts::changed, action, [action, id] { action->setShortcuts(instance().keys(id)); });
}

QString Shortcuts::keyText(const QKeySequence &seq)
{
    if (seq.isEmpty())
        return {};
    const QKeyCombination c = seq[0];
    QStringList parts;
    const Qt::KeyboardModifiers m = c.keyboardModifiers();
    if (m & Qt::ControlModifier)
        parts << tr("Ctrl");
    if (m & Qt::AltModifier)
        parts << tr("Alt");
    if (m & Qt::ShiftModifier)
        parts << tr("Shift");
    if (m & Qt::MetaModifier)
        parts << tr("Super");
    switch (c.key()) {
    case Qt::Key_Left: parts << "←"; break;
    case Qt::Key_Right: parts << "→"; break;
    case Qt::Key_Up: parts << "↑"; break;
    case Qt::Key_Down: parts << "↓"; break;
    case Qt::Key_Escape: parts << tr("Esc"); break;
    default: parts << QKeySequence(c.key()).toString(QKeySequence::NativeText);
    }
    return parts.join(" + ");
}

QString Shortcuts::text(const QString &id) const
{
    QStringList out;
    for (const QKeySequence &k : keys(id))
        out << keyText(k);
    return out.join(" / ");
}

// ---------------------------------------------------------------------------
// KeyField: shows a key; click it, then press the new key.

class KeyField : public QWidget {
    Q_OBJECT
public:
    explicit KeyField(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(36);
        setMinimumWidth(170);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(tr("Click, then press the new key. Backspace removes it, Esc cancels."));
    }
    void setKey(const QKeySequence &k)
    {
        m_key = k;
        update();
    }
    bool isCapturing() const { return m_capturing; }

signals:
    void captured(const QKeySequence &seq); // empty: removed

protected:
    bool event(QEvent *e) override
    {
        // While capturing, keys are for this field, not the window's shortcuts.
        if (m_capturing && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)) {
            if (e->type() == QEvent::ShortcutOverride) {
                e->accept();
                return true;
            }
            keyPressEvent(static_cast<QKeyEvent *>(e));
            return true;
        }
        return QWidget::event(e);
    }
    void mousePressEvent(QMouseEvent *) override
    {
        m_capturing = !m_capturing;
        if (m_capturing)
            setFocus(Qt::MouseFocusReason);
        update();
    }
    void focusOutEvent(QFocusEvent *) override
    {
        m_capturing = false;
        update();
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (!m_capturing) {
            if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) {
                m_capturing = true;
                update();
                return;
            }
            QWidget::keyPressEvent(e);
            return;
        }
        const int key = e->key();
        if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta ||
            key == Qt::Key_AltGr || key == Qt::Key_Super_L || key == Qt::Key_Super_R || key == Qt::Key_unknown)
            return; // wait for the real key
        m_capturing = false;
        update();
        if (key == Qt::Key_Escape && e->modifiers() == Qt::NoModifier)
            return; // cancelled
        if ((key == Qt::Key_Backspace || key == Qt::Key_Delete) && e->modifiers() == Qt::NoModifier) {
            emit captured({});
            return;
        }
        // Shift+S stays "Shift+S" (not "Shift+s" or "S" alone).
        Qt::KeyboardModifiers mods = e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier |
                                                       Qt::MetaModifier);
        emit captured(QKeySequence(QKeyCombination(mods, Qt::Key(key))));
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Palette &c = theme().pal();
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(m_capturing ? QPen(theme().accent(), 1.5) : Qt::NoPen);
        p.setBrush(c.input);
        p.drawRoundedRect(r, 6, 6);
        const QString text = m_capturing ? tr("Press a key…") : Shortcuts::keyText(m_key);
        p.setPen(m_capturing ? theme().accent() : c.text);
        p.drawText(rect().adjusted(10, 0, -34, 0), Qt::AlignCenter, text);
        p.drawPixmap(width() - 28, (height() - 16) / 2, theme().pixmap("edit", c.textDim, 16));
    }

private:
    QKeySequence m_key;
    bool m_capturing = false;
};

// ---------------------------------------------------------------------------
// KeyboardShortcutsOverlay


KeyboardShortcutsOverlay::KeyboardShortcutsOverlay(QWidget *window)
    : PanelOverlay(tr("Keyboard Shortcuts"), {900, 820}, window), m_keys(Shortcuts::instance().all())
{
    auto *bar = new QHBoxLayout;
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search keyboard shortcuts…"));
    m_search->setProperty("role", "search");
    m_search->setFixedHeight(40);
    m_search->setClearButtonEnabled(true);
    auto *icon = new QLabel(m_search);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    icon->setGeometry(12, 11, 18, 18);
    icon->setPixmap(theme().pixmap("search", theme().pal().textDim, 18, 2.0));
    m_searchIn = new Dropdown;
    m_searchIn->addItems({tr("All fields"), tr("Action"), tr("Hotkey")});
    m_searchIn->setFixedWidth(150);
    bar->addWidget(m_search, 1);
    bar->addWidget(m_searchIn);
    body()->addLayout(bar);

    auto *columns = new QHBoxLayout;
    columns->setContentsMargins(4, 4, 22, 0);
    auto columnLabel = [](const QString &t) {
        auto *l = new QLabel(t);
        l->setProperty("role", "dim");
        l->setAlignment(Qt::AlignCenter);
        return l;
    };
    auto *action = columnLabel(tr("Action"));
    action->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    columns->addWidget(action, 1);
    auto *k1 = columnLabel(tr("Hotkey"));
    auto *k2 = columnLabel(tr("Second hotkey"));
    k1->setFixedWidth(200);
    k2->setFixedWidth(200);
    columns->addWidget(k1);
    columns->addWidget(k2);
    body()->addLayout(columns);
    body()->addWidget(hline());

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *list = new QWidget;
    auto *v = new QVBoxLayout(list);
    v->setContentsMargins(4, 0, 14, 0);
    v->setSpacing(6);
    QString group;
    QLabel *heading = nullptr;
    for (const Shortcuts::Def &d : Shortcuts::instance().defs()) {
        if (d.group != group) {
            group = d.group;
            if (heading)
                v->addSpacing(8);
            heading = new QLabel(group);
            heading->setProperty("role", "dim");
            heading->setStyleSheet("font-size: 15px; padding: 8px 0 4px 0;");
            v->addWidget(heading);
            m_headings << heading;
        }
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(20);
        h->addWidget(new QLabel(d.label), 1);
        Row r{d.id, row, {new KeyField, new KeyField}, heading};
        for (int slot = 0; slot < 2; ++slot) {
            r.fields[slot]->setFixedWidth(190);
            h->addWidget(r.fields[slot]);
            connect(r.fields[slot], &KeyField::captured, this,
                    [this, id = d.id, slot](const QKeySequence &seq) { assign(id, slot, seq); });
        }
        v->addWidget(row);
        m_rows << r;
    }
    v->addStretch();
    scroll->setWidget(list);
    body()->addWidget(scroll, 1);

    auto *foot = new QHBoxLayout;
    m_status = new QLabel(tr("Click a key to change it. Backspace removes it; Esc cancels."));
    m_status->setProperty("role", "dim");
    m_status->setWordWrap(true);
    foot->addWidget(m_status, 1);
    auto *reset = Dialog::button(tr("Reset to Defaults"));
    auto *cancel = Dialog::button(tr("Cancel"));
    auto *save = Dialog::button(tr("Save"), true);
    foot->addWidget(reset);
    foot->addWidget(cancel);
    foot->addWidget(save);
    body()->addLayout(foot);

    connect(reset, &QPushButton::clicked, this, [this] {
        m_keys = Shortcuts::instance().defaults();
        refreshFields();
        m_status->setText(tr("Back to the default keys. Save to keep them."));
    });
    connect(cancel, &QPushButton::clicked, this, &PanelOverlay::close);
    connect(save, &QPushButton::clicked, this, [this] {
        Shortcuts::instance().setAll(m_keys);
        close();
    });
    connect(m_search, &QLineEdit::textChanged, this, &KeyboardShortcutsOverlay::filter);
    connect(m_searchIn, &QComboBox::currentIndexChanged, this, &KeyboardShortcutsOverlay::filter);
    refreshFields();
    m_search->setFocus();
}

void KeyboardShortcutsOverlay::assign(const QString &id, int slot, const QKeySequence &seq)
{
    QList<QKeySequence> &mine = m_keys[id];
    while (mine.size() <= slot)
        mine << QKeySequence();
    if (!seq.isEmpty()) {
        // A key does one thing: take it away from whatever had it.
        for (auto it = m_keys.begin(); it != m_keys.end(); ++it) {
            for (int i = 0; i < it.value().size(); ++i) {
                if (it.value()[i] == seq && !(it.key() == id && i == slot)) {
                    it.value()[i] = QKeySequence();
                    const Shortcuts::Def *other = Shortcuts::instance().def(it.key());
                    m_status->setText(tr("%1 was the key for “%2”; it's been removed there.")
                                          .arg(Shortcuts::keyText(seq), other ? other->label : it.key()));
                }
            }
        }
    } else {
        m_status->setText(tr("Key removed."));
    }
    mine[slot] = seq;
    // Keep the main key first.
    for (auto it = m_keys.begin(); it != m_keys.end(); ++it) {
        it.value().removeAll(QKeySequence());
    }
    refreshFields();
}

void KeyboardShortcutsOverlay::refreshFields()
{
    for (const Row &r : m_rows) {
        const QList<QKeySequence> k = m_keys.value(r.id);
        for (int slot = 0; slot < 2; ++slot)
            r.fields[slot]->setKey(k.value(slot));
    }
    filter();
}

void KeyboardShortcutsOverlay::filter()
{
    const QString t = m_search->text().trimmed();
    const int in = m_searchIn->currentIndex(); // 0 all, 1 action, 2 hotkey
    QSet<QLabel *> shown;
    for (const Row &r : m_rows) {
        bool match = t.isEmpty();
        if (!match && in != 2) {
            const Shortcuts::Def *d = Shortcuts::instance().def(r.id);
            match = d && (d->label.contains(t, Qt::CaseInsensitive) || d->group.contains(t, Qt::CaseInsensitive));
        }
        if (!match && in != 1) {
            for (const QKeySequence &k : m_keys.value(r.id)) {
                const QString text = Shortcuts::keyText(k);
                // "s" finds S, not every Shift combination.
                if (text.compare(t, Qt::CaseInsensitive) == 0 ||
                    (t.size() > 1 && text.contains(t, Qt::CaseInsensitive)))
                    match = true;
            }
        }
        r.widget->setVisible(match);
        if (match)
            shown.insert(r.heading);
    }
    for (QLabel *h : m_headings)
        h->setVisible(shown.contains(h));
}

#include "shortcuts.moc"
