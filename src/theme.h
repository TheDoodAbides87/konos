#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QPixmap>

class QFileSystemWatcher;
class QTimer;

// Colour roles used throughout the UI. Values come from the redesign mockups.
struct Palette {
    QColor window;      // main content background
    QColor chrome;      // title bar, nav rail, control bar
    QColor panel;       // side panels / cards
    QColor surface;     // raised elements (dropdowns, buttons)
    QColor hover;       // hover highlight
    QColor selected;    // selected nav item
    QColor input;       // text fields, slider tracks (the near-black wells)
    QColor border;      // hairlines
    QColor text;        // primary text
    QColor textDim;     // secondary text / icons
    QColor textMuted;   // tertiary text
    QColor videoBg;     // behind the video
    QColor knobOff;     // toggle switch knob when off
};

class Theme : public QObject {
    Q_OBJECT
public:
    // Omarchy only exists in the Omarchy build (qmake CONFIG+=omarchy): colours
    // come from the desktop's current theme and follow it when it changes.
    enum Mode { Dark = 0, Light = 1, System = 2, Omarchy = 3 };

    static Theme &instance();
    static QList<QColor> accentChoices();
    static QStringList accentNames();
    static QStringList modeNames();

    // Set and remember the theme mode / accent (prefs keys differ per build).
    static void pick(Mode mode);
    static void pickAccent(int index);
    static void loadSaved();

    QColor accent() const;
    // Darkened accent used for slider fills and selected backgrounds.
    QColor accentFill() const;
    // Colour used for icons/text drawn on top of an accent background.
    QColor onAccent() const;
    QColor volumeColor() const;

    int accentIndex() const { return m_accent; }
    void setAccentIndex(int index);

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    bool isDark() const;

    const Palette &pal() const;

    // Maps one of the stock greys onto the active palette's background-to-text
    // ramp, so hand-picked shades keep their contrast under an Omarchy theme.
    // Returns the colour unchanged otherwise.
    QColor tone(const QColor &stock) const;

    // The desktop theme's palette and accent, or null when there isn't one.
    const Palette *omarchyPalette() const { return m_omLoaded ? &m_omarchy : nullptr; }
    QColor omarchyAccent() const { return m_omAccent; }

    QString styleSheet() const;
    // Application palette, for what the style sheet doesn't reach (dialogs, views).
    class QPalette palette() const;

    // Renders one of the built-in line icons in the given colour.
    QIcon icon(const QString &name, const QColor &color = QColor()) const;
    QPixmap pixmap(const QString &name, const QColor &color, int size, qreal dpr = 2.0) const;

    // Paints the Konos mark (a split K: stem, gap, chevron arms) in the accent colour.
    static void paintLogo(class QPainter *p, const QRectF &rect, const QColor &accent);

signals:
    void changed();

private:
    Theme();
    bool omarchyActive() const;
    void loadOmarchy();
    int m_accent = 0;
    Mode m_mode = Dark;
    Palette m_dark;
    Palette m_light;

    // Omarchy desktop theme (Omarchy build only).
    bool m_omLoaded = false;
    bool m_omDark = true;
    QColor m_omAccent;
    QColor m_omGreen;
    Palette m_omarchy;
    QFileSystemWatcher *m_omWatcher = nullptr;
    QTimer *m_omReload = nullptr;
};

inline Theme &theme() { return Theme::instance(); }

// Mix two colours; t = 0 gives a, t = 1 gives b.
QColor mixColor(const QColor &a, const QColor &b, qreal t);
