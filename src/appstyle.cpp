#include "appstyle.h"
#include "theme.h"

#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStyleFactory>
#include <QComboBox>
#include <QStyleOptionMenuItem>

static constexpr int kItemHeight = 42;
static constexpr int kIconX = 18;
static constexpr int kIconSize = 20;
static constexpr int kTextX = 54;

AppStyle::AppStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}

void AppStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);
    if (qobject_cast<QMenu *>(widget)) {
        // Rounded menus need a translucent, shadowless popup.
        widget->setAttribute(Qt::WA_TranslucentBackground);
        widget->setWindowFlag(Qt::FramelessWindowHint);
        widget->setWindowFlag(Qt::NoDropShadowWindowHint);
    }
}

void AppStyle::drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *p,
                             const QWidget *widget) const
{
    if (element == PE_PanelMenu || element == PE_FrameMenu) {
        // Rounded menu panel (menus are translucent popups, see polish()).
        if (element == PE_FrameMenu)
            return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(theme().pal().border, 1));
        p->setBrush(theme().tone(theme().isDark() ? QColor(0x23, 0x23, 0x23) : QColor(0xfa, 0xfa, 0xfa)));
        p->drawRoundedRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        p->restore();
        return;
    }
    QProxyStyle::drawPrimitive(element, option, p, widget);
}

void AppStyle::drawControl(ControlElement element, const QStyleOption *option, QPainter *p,
                           const QWidget *widget) const
{
    if (element == CE_MenuEmptyArea)
        return;
    if (element != CE_MenuItem) {
        QProxyStyle::drawControl(element, option, p, widget);
        return;
    }
    const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
    if (!mi) {
        QProxyStyle::drawControl(element, option, p, widget);
        return;
    }

    const Palette &c = theme().pal();
    const QRect r = mi->rect;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    if (mi->menuItemType == QStyleOptionMenuItem::Separator) {
        p->fillRect(QRect(r.left() + 14, r.center().y(), r.width() - 28, 1), c.border);
        p->restore();
        return;
    }

    const bool enabled = mi->state & State_Enabled;
    const bool selected = (mi->state & State_Selected) && enabled;
    if (selected) {
        QPainterPath path;
        path.addRoundedRect(QRectF(r.adjusted(6, 1, -6, -1)), 6, 6);
        p->fillPath(path, c.hover);
    }

    // Icon (already coloured by whoever created the action).
    // Dropdowns may ask for wider pictures (equalizer curves).
    const auto *combo = qobject_cast<const QComboBox *>(widget);
    const QSize iconSize = combo && combo->iconSize().width() > kIconSize ? combo->iconSize() : QSize(kIconSize, kIconSize);
    int iconRight = r.left() + kIconX + kIconSize;
    if (!mi->icon.isNull()) {
        const QRect iconRect(r.left() + (iconSize.width() > kIconSize ? 18 : kIconX),
                             r.center().y() - iconSize.height() / 2 + 1, iconSize.width(), iconSize.height());
        mi->icon.paint(p, iconRect, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);
        iconRight = iconRect.right();
    }

    QString text = mi->text;
    QString shortcut;
    const int tab = text.indexOf('\t');
    if (tab >= 0) {
        shortcut = text.mid(tab + 1);
        text = text.left(tab);
    }

    QFont f = mi->font;
    f.setPixelSize(15);
    p->setFont(f);
    QColor textColor = selected ? c.text : c.textDim;
    if (!enabled)
        textColor = c.textMuted;
    p->setPen(textColor);
    // Dropdown lists reserve an icon column even when no item has an icon.
    const bool noIcons = mi->icon.isNull() && (mi->maxIconWidth == 0 || qobject_cast<const QComboBox *>(widget));
    const int textLeft = noIcons ? r.left() + 20 : qMax(r.left() + kTextX, iconRight + 14);
    // Up to the check mark / chevron / shortcut column on the right.
    QRect textRect(textLeft, r.top(), r.right() - 40 - textLeft, r.height());
    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextShowMnemonic, text);

    int rightEdge = r.right() - 18;
    if (mi->menuItemType == QStyleOptionMenuItem::SubMenu) {
        const QPixmap chev = theme().pixmap("chevron-right", c.textDim, 18);
        p->drawPixmap(rightEdge - 18, r.center().y() - 8, chev);
    } else if (!shortcut.isEmpty()) {
        p->setPen(c.textMuted);
        p->drawText(QRect(r.left(), r.top(), rightEdge - r.left(), r.height()),
                    Qt::AlignVCenter | Qt::AlignRight, shortcut);
    } else if (mi->checked) {
        const QPixmap check = theme().pixmap("check", theme().accent(), 18);
        p->drawPixmap(rightEdge - 18, r.center().y() - 8, check);
    }
    p->restore();
}

QSize AppStyle::sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size,
                                 const QWidget *widget) const
{
    if (type == CT_MenuItem) {
        if (const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            if (mi->menuItemType == QStyleOptionMenuItem::Separator)
                return QSize(size.width(), 13);
            QFont f = mi->font;
            f.setPixelSize(15);
            const QFontMetrics fm(f);
            QString text = mi->text;
            QString shortcut;
            const int tab = text.indexOf('\t');
            if (tab >= 0) {
                shortcut = text.mid(tab + 1);
                text = text.left(tab);
            }
            const int w = kTextX + fm.horizontalAdvance(text) + 48 +
                          (shortcut.isEmpty() ? 24 : fm.horizontalAdvance(shortcut) + 20);
            return QSize(qMax(w, 240), kItemHeight);
        }
    }
    return QProxyStyle::sizeFromContents(type, option, size, widget);
}

int AppStyle::pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const
{
    switch (metric) {
    case PM_MenuHMargin:
        return 0;
    case PM_MenuVMargin:
        return 6;
    case PM_MenuPanelWidth:
        return 1;
    case PM_SubMenuOverlap:
        return 4;
    case PM_SmallIconSize:
        return 20;
    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

int AppStyle::styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                        QStyleHintReturn *returnData) const
{
    if (hint == SH_Menu_SubMenuPopupDelay)
        return 120;
    if (hint == SH_ToolTip_WakeUpDelay)
        return 500;
    if (hint == SH_DialogButtonBox_ButtonsHaveIcons)
        return 0;
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

QIcon AppStyle::standardIcon(StandardPixmap icon, const QStyleOption *option, const QWidget *widget) const
{
    // The file dialog's toolbar and places, drawn with the app's line icons.
    const char *name = nullptr;
    switch (icon) {
    case SP_ArrowBack: name = "arrow-left"; break;
    case SP_ArrowForward: name = "arrow-right"; break;
    case SP_FileDialogToParent: name = "folder-up"; break;
    case SP_FileDialogNewFolder: name = "folder-plus"; break;
    case SP_FileDialogListView: name = "grid"; break;
    case SP_FileDialogDetailedView: name = "listview"; break;
    case SP_DirIcon:
    case SP_DirClosedIcon:
    case SP_DirOpenIcon: name = "folder-fill"; break;
    case SP_DirHomeIcon: name = "home"; break;
    case SP_ComputerIcon:
    case SP_DriveHDIcon:
    case SP_DriveCDIcon:
    case SP_DriveDVDIcon: name = "disc"; break;
    case SP_FileIcon: name = "file"; break;
    case SP_TrashIcon: name = "trash"; break;
    default: break;
    }
    if (name)
        return theme().icon(name);
    return QProxyStyle::standardIcon(icon, option, widget);
}
