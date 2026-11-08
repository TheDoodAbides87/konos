#include "datafile.h"
#include "playerpanel.h"
#include "libraryview.h"
#include "player.h"
#include "theme.h"
#include "widgets.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr int TimeRole = Qt::UserRole;
constexpr int CurrentRole = Qt::UserRole + 1;

QString bookmarksFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/bookmarks.json";
}

// 00:00:42:023, like VLC's bookmarks.
QString stamp(qint64 ms)
{
    ms = qMax<qint64>(0, ms);
    return QString("%1:%2:%3:%4")
        .arg(ms / 3600000, 2, 10, QChar('0'))
        .arg(ms / 60000 % 60, 2, 10, QChar('0'))
        .arg(ms / 1000 % 60, 2, 10, QChar('0'))
        .arg(ms % 1000, 3, 10, QChar('0'));
}

// A row with the name on the left and its time on the right.
class TimedRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {0, 52}; }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const Palette &c = theme().pal();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect.adjusted(0, 2, 0, -2);
        const bool hover = option.state & QStyle::State_MouseOver;
        const bool selected = option.state & QStyle::State_Selected;
        if (hover || selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? c.selected : c.hover);
            p->drawRoundedRect(r, 6, 6);
        }
        const bool current = index.data(CurrentRole).toBool();
        QFont f = option.font;
        f.setPixelSize(15);
        p->setFont(f);
        const QString time = stamp(index.data(TimeRole).toLongLong());
        QFont tf = f;
        tf.setFeature(QFont::Tag("tnum"), 1); // digits line up
        const int tw = QFontMetrics(tf).horizontalAdvance(time);
        const QRect inner = r.adjusted(14, 0, -14, 0);
        const QRect textRect = inner.adjusted(0, 0, -tw - 16, 0);
        p->setPen(current ? theme().accent() : c.text);
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(f).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width()));
        p->setFont(tf);
        p->setPen(current ? theme().accent() : c.textDim);
        p->drawText(inner, Qt::AlignVCenter | Qt::AlignRight, time);
        p->restore();
    }
};

QListWidget *makeList(QWidget *parent)
{
    auto *list = new QListWidget(parent);
    list->setItemDelegate(new TimedRowDelegate(list));
    list->setFrameShape(QFrame::NoFrame);
    list->setMouseTracking(true);
    list->viewport()->setAttribute(Qt::WA_Hover);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    list->viewport()->setCursor(Qt::PointingHandCursor);
    return list;
}

QLabel *makeEmpty(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("role", "dim");
    label->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    label->setWordWrap(true);
    label->setContentsMargins(0, 40, 0, 0);
    return label;
}

// "2 Bookmarks" … [−] [+] above a list.
QHBoxLayout *headerRow(QLabel *count)
{
    auto *h = new QHBoxLayout;
    h->setContentsMargins(6, 0, 0, 10);
    h->setSpacing(4);
    count->setProperty("role", "dim");
    h->addWidget(count);
    h->addStretch();
    return h;
}
} // namespace

// ---------------------------------------------------------------------------
// Bookmarks

Bookmarks &Bookmarks::instance()
{
    static Bookmarks b;
    return b;
}

Bookmarks::Bookmarks()
{
    const QJsonObject root = DataFile::readJson(bookmarksFile());
    for (auto it = root.begin(); it != root.end(); ++it) {
        QList<Mark> marks;
        for (const QJsonValue &v : it.value().toArray()) {
            const QJsonObject o = v.toObject();
            marks.append({o.value("name").toString(), qint64(o.value("ms").toDouble())});
        }
        if (!marks.isEmpty())
            m_marks.insert(it.key(), marks);
    }
}

QList<Bookmarks::Mark> Bookmarks::marks(const QString &location) const
{
    return m_marks.value(location);
}

void Bookmarks::set(const QString &location, QList<Mark> marks)
{
    std::stable_sort(marks.begin(), marks.end(), [](const Mark &a, const Mark &b) { return a.ms < b.ms; });
    if (marks.isEmpty())
        m_marks.remove(location);
    else
        m_marks.insert(location, marks);
    save();
}

void Bookmarks::save() const
{
    QJsonObject root;
    for (auto it = m_marks.begin(); it != m_marks.end(); ++it) {
        QJsonArray arr;
        for (const Mark &m : it.value())
            arr.append(QJsonObject{{"name", m.name}, {"ms", double(m.ms)}});
        root.insert(it.key(), arr);
    }
    DataFile::write(bookmarksFile(), QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// ---------------------------------------------------------------------------
// PlayerPanel

PlayerPanel::PlayerPanel(Player *player, QWidget *parent) : QWidget(parent), m_player(player)
{
    setObjectName("SidePanel");
    setAttribute(Qt::WA_StyledBackground);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *tabRow = new QWidget(this);
    tabRow->setObjectName("PanelTabs");
    tabRow->setAttribute(Qt::WA_StyledBackground);
    auto *tabs = new QHBoxLayout(tabRow);
    tabs->setContentsMargins(8, 0, 8, 0);
    tabs->setSpacing(0);
    const QList<QPair<QString, QString>> defs = {
        {"queue", tr("Queue")}, {"bookmark", tr("Bookmarks")}, {"chapters", tr("Chapters")}};
    for (int i = 0; i < defs.size(); ++i) {
        auto *t = new TabButton(defs[i].first, defs[i].second, tabRow);
        tabs->addWidget(t, 1);
        m_tabs << t;
        connect(t, &QAbstractButton::clicked, this, [this, i] { setTab(Tab(i)); });
    }
    v->addWidget(tabRow);
    auto restyle = [tabRow] {
        tabRow->setStyleSheet(QString("QWidget#PanelTabs { background: %1; border-bottom: 1px solid %2; }")
                                  .arg(theme().pal().chrome.name(), theme().pal().border.name()));
    };
    restyle();
    connect(&theme(), &Theme::changed, this, restyle);

    m_stack = new QStackedWidget(this);
    m_queue = new LibraryView(LibraryView::QueueTab, m_stack);
    m_queue->showSection(Section::Queue);
    m_stack->addWidget(m_queue);
    m_stack->addWidget(buildBookmarks());
    m_stack->addWidget(buildChapters());
    v->addWidget(m_stack, 1);

    connect(m_player, &Player::mediaChanged, this, [this] {
        refreshBookmarks();
        refreshChapters();
    });
    connect(m_player, &Player::chaptersChanged, this, &PlayerPanel::refreshChapters);
    connect(m_player, &Player::lengthChanged, this, &PlayerPanel::refreshChapters);
    connect(m_player, &Player::stateChanged, this, [this] { m_markAdd->setEnabled(m_player->hasMedia()); });
    setTab(QueueTab);
    refreshBookmarks();
    refreshChapters();
}

void PlayerPanel::setTab(Tab tab)
{
    m_tab = tab;
    for (int i = 0; i < m_tabs.size(); ++i)
        m_tabs[i]->setChecked(i == int(tab));
    m_stack->setCurrentIndex(int(tab));
    if (tab == ChaptersTab)
        refreshChapters();
    emit tabChanged(tab);
}

void PlayerPanel::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refreshBookmarks();
    refreshChapters();
}

// --- Bookmarks ---

QWidget *PlayerPanel::buildBookmarks()
{
    auto *page = new QWidget(m_stack);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(18, 16, 18, 0);
    v->setSpacing(0);
    m_markCount = new QLabel(page);
    QHBoxLayout *h = headerRow(m_markCount);
    m_markRemove = new IconButton("minus", page);
    m_markRemove->setToolTip(tr("Remove the selected bookmark"));
    m_markAdd = new IconButton("plus", page);
    m_markAdd->setToolTip(tr("Bookmark the current moment"));
    h->addWidget(m_markRemove);
    h->addWidget(m_markAdd);
    v->addLayout(h);
    v->addWidget(hline(page));
    v->addSpacing(8);

    m_marks = makeList(page);
    m_marks->setEditTriggers(QAbstractItemView::EditKeyPressed);
    v->addWidget(m_marks, 1);
    m_markEmpty = makeEmpty(tr("No bookmarks yet.\nPress + to mark the current moment."), page);
    v->addWidget(m_markEmpty, 1);

    connect(m_markAdd, &IconButton::clicked, this, &PlayerPanel::addBookmark);
    connect(m_markRemove, &IconButton::clicked, this, &PlayerPanel::removeBookmark);
    connect(m_marks, &QListWidget::itemSelectionChanged, this,
            [this] { m_markRemove->setEnabled(!m_marks->selectedItems().isEmpty()); });
    connect(m_marks, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (m_player->location() == m_markLocation)
            m_player->seek(item->data(TimeRole).toLongLong());
    });
    connect(m_marks, &QListWidget::itemChanged, this, [this] { storeBookmarks(); });
    connect(m_marks, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_marks->itemAt(pos);
        if (!item)
            return;
        m_marks->setCurrentItem(item);
        QMenu menu(this);
        const QColor a = theme().accent();
        menu.addAction(theme().icon("play", a), tr("Jump Here"), this,
                       [this, item] { m_player->seek(item->data(TimeRole).toLongLong()); });
        menu.addAction(theme().icon("edit", a), tr("Rename"), this, [this, item] { renameBookmark(item); });
        menu.addAction(theme().icon("trash", a), tr("Remove"), this, &PlayerPanel::removeBookmark);
        menu.exec(m_marks->viewport()->mapToGlobal(pos));
    });
    // Delete removes, F2 renames (edit trigger above).
    auto *del = new QAction(m_marks);
    del->setShortcut(QKeySequence::Delete);
    del->setShortcutContext(Qt::WidgetShortcut);
    connect(del, &QAction::triggered, this, &PlayerPanel::removeBookmark);
    m_marks->addAction(del);
    return page;
}

void PlayerPanel::refreshBookmarks()
{
    m_markLocation = m_player->location();
    const QList<Bookmarks::Mark> marks = Bookmarks::instance().marks(m_markLocation);
    {
        const QSignalBlocker block(m_marks);
        m_marks->clear();
        for (const Bookmarks::Mark &m : marks) {
            auto *item = new QListWidgetItem(m.name, m_marks);
            item->setData(TimeRole, m.ms);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
    }
    m_markCount->setText(marks.size() == 1 ? tr("1 Bookmark") : tr("%1 Bookmarks").arg(marks.size()));
    m_marks->setVisible(!marks.isEmpty());
    m_markEmpty->setVisible(marks.isEmpty());
    m_markEmpty->setText(m_player->hasMedia() ? tr("No bookmarks yet.\nPress + to mark the current moment.")
                                              : tr("Play something to bookmark moments in it."));
    m_markAdd->setEnabled(m_player->hasMedia());
    m_markRemove->setEnabled(false);
}

void PlayerPanel::addBookmark()
{
    if (!m_player->hasMedia())
        return;
    if (m_markLocation != m_player->location())
        refreshBookmarks();
    QList<Bookmarks::Mark> marks = Bookmarks::instance().marks(m_markLocation);
    // "Title 1", "Title 2", …: one past the highest number used so far.
    QString base = m_player->title().trimmed();
    if (base.isEmpty())
        base = tr("Bookmark");
    int n = 0;
    for (const Bookmarks::Mark &m : marks)
        if (m.name.startsWith(base + " "))
            n = qMax(n, m.name.mid(base.size() + 1).toInt());
    const qint64 at = m_player->time();
    marks.append({QString("%1 %2").arg(base).arg(n + 1), at});
    Bookmarks::instance().set(m_markLocation, marks);
    refreshBookmarks();
    for (int i = 0; i < m_marks->count(); ++i)
        if (m_marks->item(i)->data(TimeRole).toLongLong() == at) {
            m_marks->setCurrentRow(i);
            m_marks->scrollToItem(m_marks->item(i));
        }
}

void PlayerPanel::removeBookmark()
{
    QListWidgetItem *item = m_marks->currentItem();
    if (!item || !item->isSelected())
        return;
    delete item;
    storeBookmarks();
    refreshBookmarks();
}

void PlayerPanel::renameBookmark(QListWidgetItem *item)
{
    m_marks->setCurrentItem(item);
    m_marks->editItem(item);
}

void PlayerPanel::storeBookmarks()
{
    QList<Bookmarks::Mark> marks;
    for (int i = 0; i < m_marks->count(); ++i) {
        const QListWidgetItem *item = m_marks->item(i);
        QString name = item->text().trimmed();
        if (name.isEmpty())
            name = tr("Bookmark");
        marks.append({name, item->data(TimeRole).toLongLong()});
    }
    Bookmarks::instance().set(m_markLocation, marks);
}

// --- Chapters ---

QWidget *PlayerPanel::buildChapters()
{
    auto *page = new QWidget(m_stack);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(18, 16, 18, 0);
    v->setSpacing(0);
    m_chapterCount = new QLabel(page);
    QHBoxLayout *h = headerRow(m_chapterCount);
    h->addSpacing(32); // same height as the bookmarks header with its buttons
    m_chapterCount->setMinimumHeight(32);
    v->addLayout(h);
    v->addWidget(hline(page));
    v->addSpacing(8);
    m_chapters = makeList(page);
    m_chapters->setEditTriggers(QAbstractItemView::NoEditTriggers);
    v->addWidget(m_chapters, 1);
    m_chapterEmpty = makeEmpty(QString(), page);
    v->addWidget(m_chapterEmpty, 1);
    connect(m_chapters, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        m_player->setChapter(item->data(Qt::UserRole + 2).toInt());
    });
    return page;
}

void PlayerPanel::refreshChapters()
{
    const QList<Player::Chapter> chapters = m_player->chapterList();
    const int current = m_player->currentChapter();
    // Keep the list (and its scroll position) when only the current one moved.
    bool same = m_chapters->count() == chapters.size();
    for (int i = 0; same && i < chapters.size(); ++i)
        same = m_chapters->item(i)->text() == chapters[i].name &&
               m_chapters->item(i)->data(TimeRole).toLongLong() == chapters[i].start;
    if (!same) {
        m_chapters->clear();
        for (const Player::Chapter &c : chapters) {
            auto *item = new QListWidgetItem(c.name, m_chapters);
            item->setData(TimeRole, c.start);
            item->setData(Qt::UserRole + 2, c.id);
        }
    }
    for (int i = 0; i < m_chapters->count(); ++i)
        m_chapters->item(i)->setData(CurrentRole, m_chapters->item(i)->data(Qt::UserRole + 2).toInt() == current);
    m_chapterCount->setText(chapters.size() == 1 ? tr("1 Chapter") : tr("%1 Chapters").arg(chapters.size()));
    m_chapters->setVisible(!chapters.isEmpty());
    m_chapterEmpty->setVisible(chapters.isEmpty());
    m_chapterEmpty->setText(!m_player->hasMedia() ? tr("Play something to see its chapters.")
                            : m_player->inDiscMenu() ? tr("Chapters show once the disc's menu is left.")
                                                     : tr("This media has no chapters."));
}
