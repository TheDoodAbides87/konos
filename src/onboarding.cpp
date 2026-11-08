#include "onboarding.h"
#include "filedialogs.h"
#include "prefs.h"
#include "shortcuts.h"
#include "theme.h"
#include "widgets.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDir>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {
class Logo : public QWidget {
public:
    Logo() { setFixedSize(96, 96); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        Theme::paintLogo(&p, QRectF(rect()), theme().accent());
    }
};

QPushButton *button(const QString &text, bool accent = false)
{
    auto *b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedSize(accent ? 110 : 96, 38);
    if (accent)
        b->setProperty("role", "accent");
    return b;
}

QLabel *heading(const QString &text)
{
    auto *l = new QLabel(text);
    l->setAlignment(Qt::AlignCenter);
    l->setStyleSheet("font-size: 24px;");
    return l;
}

QLabel *dim(const QString &text)
{
    return Dialog::caption(text);
}

// A key cap like the ones in Settings → Controls.
QLabel *keyCap(const QString &text)
{
    auto *k = new QLabel(text);
    k->setAlignment(Qt::AlignCenter);
    k->setMinimumWidth(34);
    k->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 5px; padding: 4px 8px;")
                         .arg(theme().pal().surface.name(), theme().pal().border.name()));
    return k;
}
} // namespace

bool Onboarding::wanted()
{
    return qApp->property("konosFirstRun").toBool() && !QSettings().value("onboarding/done").toBool();
}

Onboarding::Onboarding(QWidget *window) : QWidget(window)
{
    hide();
    window->installEventFilter(this);
    m_card = new QWidget(this);
    m_card->setObjectName("SettingsPanel");
    m_card->setAttribute(Qt::WA_StyledBackground);
    auto *v = new QVBoxLayout(m_card);
    v->setContentsMargins(48, 36, 36, 30);
    m_steps = new QStackedWidget;
    v->addWidget(m_steps, 1);

    // 1. Welcome, and a folder to start with.
    {
        auto *page = new QWidget;
        auto *l = new QVBoxLayout(page);
        l->setContentsMargins(60, 10, 60, 0);
        l->setSpacing(14);
        l->addStretch();
        l->addWidget(new Logo, 0, Qt::AlignHCenter);
        l->addWidget(heading(tr("Welcome to Konos!")));
        auto *about = dim(tr("Konos is a media player for videos, music, discs and streams. It's an overhaul of VLC: "
                             "VLC's engine plays everything, with a new interface on top. It's in early "
                             "development, so expect changes."));
        about->setAlignment(Qt::AlignCenter);
        l->addWidget(about);
        l->addSpacing(18);
        l->addWidget(dim(tr("Choose a media folder to get started with")));
        auto *row = new QHBoxLayout;
        const QStringList folders = prefs().list("library/videoFolders");
        m_folder = new QLineEdit(folders.value(0, QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)));
        m_folder->setFixedHeight(38);
        auto *browse = new QPushButton(tr("Browse"));
        browse->setCursor(Qt::PointingHandCursor);
        browse->setFixedHeight(38);
        row->addWidget(m_folder, 1);
        row->addWidget(browse);
        l->addLayout(row);
        l->addStretch();
        auto *foot = new QHBoxLayout;
        auto *skip = new QPushButton(tr("Skip"));
        skip->setProperty("role", "link");
        skip->setCursor(Qt::PointingHandCursor);
        auto *next = button(tr("Next"), true);
        foot->addWidget(skip);
        foot->addStretch();
        foot->addWidget(next);
        l->addLayout(foot);
        m_steps->addWidget(page);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString dir = FileDialogs::existingDirectory(this->window(), tr("Choose a media folder"), m_folder->text());
            if (!dir.isEmpty())
                m_folder->setText(dir);
        });
        connect(next, &QPushButton::clicked, this, [this] { setStep(1); });
        connect(skip, &QPushButton::clicked, this, [this] {
            m_folder->clear(); // keep the folders as they are
            finish();
        });
    }

    // 2. Theme and accent colour; the app behind changes as you pick.
    {
        auto *page = new QWidget;
        auto *l = new QVBoxLayout(page);
        l->setSpacing(22);
        l->addStretch();
        l->addWidget(heading(tr("Pick your theme and colour.")));
        auto *cards = new QHBoxLayout;
        cards->setSpacing(24);
        cards->addStretch();
        auto *group = new QButtonGroup(page);
        const QStringList names = Theme::modeNames();
        for (int i = 0; i < names.size(); ++i) {
            auto *card = new ThemeCard(names[i], i);
            card->setChecked(theme().mode() == i);
            group->addButton(card, i);
            cards->addWidget(card);
        }
        cards->addStretch();
        l->addLayout(cards);
        connect(group, &QButtonGroup::idClicked, this, [](int i) { Theme::pick(Theme::Mode(i)); });
        auto *swatches = new AccentSwatches;
        connect(swatches, &AccentSwatches::picked, this, &Theme::pickAccent);
        l->addWidget(swatches, 0, Qt::AlignHCenter);
        auto *note = dim(tr("Both can be changed any time in Settings → General."));
        note->setAlignment(Qt::AlignCenter);
        l->addWidget(note);
        l->addStretch();
        auto *foot = new QHBoxLayout;
        foot->addStretch();
        auto *back = button(tr("Back"));
        auto *next = button(tr("Next"), true);
        foot->addWidget(back);
        foot->addWidget(next);
        l->addLayout(foot);
        m_steps->addWidget(page);
        connect(back, &QPushButton::clicked, this, [this] { setStep(0); });
        connect(next, &QPushButton::clicked, this, [this] { setStep(2); });
    }

    // 3. The basic keys (as they are now, so changed keys show too).
    {
        auto *page = new QWidget;
        auto *l = new QVBoxLayout(page);
        l->setSpacing(22);
        l->addStretch();
        l->addWidget(heading(tr("The basic keyboard shortcuts.")));
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(18);
        grid->setVerticalSpacing(12);
        const QList<QPair<QString, QStringList>> basics = {
            {tr("Play / pause"), {"play"}},          {tr("Fullscreen"), {"fullscreen"}},
            {tr("Seek back / forward"), {"seekBack", "seekFwd"}}, {tr("Volume down / up"), {"volDown", "volUp"}},
            {tr("Previous / next item"), {"prev", "next"}},       {tr("Library / player"), {"library"}},
            {tr("Next subtitle track"), {"subTrack"}},            {tr("Take a snapshot"), {"snapshot"}},
            {tr("Next audio track"), {"audioTrack"}},             {tr("Record"), {"record"}},
        };
        for (int i = 0; i < basics.size(); ++i) {
            auto *name = dim(basics[i].first);
            name->setWordWrap(false);
            auto *keys = new QHBoxLayout;
            keys->setSpacing(6);
            for (const QString &id : basics[i].second) {
                const QKeySequence k = Shortcuts::instance().keys(id).value(0);
                keys->addWidget(keyCap(k.isEmpty() ? tr("none") : Shortcuts::keyText(k)));
            }
            keys->addStretch();
            const int row = i / 2, col = (i % 2) * 2;
            grid->addWidget(name, row, col);
            grid->addLayout(keys, row, col + 1);
        }
        grid->setColumnMinimumWidth(1, 150);
        grid->setColumnMinimumWidth(2, 60);
        auto *center = new QHBoxLayout;
        center->addStretch();
        center->addLayout(grid);
        center->addStretch();
        l->addLayout(center);
        l->addStretch();
        auto *foot = new QHBoxLayout;
        foot->addWidget(dim(tr("Find them all, and change them, in the menu → Keyboard Shortcuts.")), 1);
        auto *back = button(tr("Back"));
        auto *done = button(tr("Finish"), true);
        foot->addWidget(back);
        foot->addWidget(done);
        l->addLayout(foot);
        m_steps->addWidget(page);
        connect(back, &QPushButton::clicked, this, [this] { setStep(1); });
        connect(done, &QPushButton::clicked, this, &Onboarding::finish);
    }
    connect(&theme(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void Onboarding::open()
{
    layoutCard();
    setStep(0);
    show();
    raise();
}

void Onboarding::setStep(int step)
{
    m_steps->setCurrentIndex(step);
    update(); // the dimming changes with the step
}

void Onboarding::finish()
{
    // The folder goes first in Videos (Music finds music on its own).
    const QString dir = QDir::cleanPath(m_folder->text().trimmed().replace(QRegularExpression("^~"), QDir::homePath()));
    if (!m_folder->text().trimmed().isEmpty() && QDir(dir).exists()) {
        QStringList folders = prefs().list("library/videoFolders");
        folders.removeAll(dir);
        folders.prepend(dir);
        prefs().set("library/videoFolders", folders);
    }
    QSettings().setValue("onboarding/done", true);
    hide();
    emit finished();
    deleteLater();
}

void Onboarding::layoutCard()
{
    setGeometry(parentWidget()->rect());
    const int w = qMin(960, width() - 40), h = qMin(640, height() - 40);
    m_card->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);
}

bool Onboarding::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == parentWidget() && event->type() == QEvent::Resize && isVisible())
        layoutCard();
    return QWidget::eventFilter(obj, event);
}

void Onboarding::paintEvent(QPaintEvent *)
{
    // The theme step lets the app show through, so picks can be seen on it.
    QPainter(this).fillRect(rect(), m_steps->currentIndex() == 1 ? QColor(0, 0, 0, 90) : QColor(0, 0, 0, 215));
}
