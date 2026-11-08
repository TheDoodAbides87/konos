#pragma once

#include <QAbstractButton>
#include <QComboBox>
#include <QLabel>
#include <QRadioButton>
#include <QVector>
#include <QWidget>
#include <functional>

class QLineEdit;
class QVBoxLayout;

// Flat icon button; optionally with a text label (expanded navigation items).
class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit IconButton(const QString &icon, QWidget *parent = nullptr);

    void setIconName(const QString &name);
    QString iconName() const { return m_icon; }
    void setIconPixelSize(int size);
    void setLabel(const QString &label);
    // "Active" draws the icon in the accent colour (current nav section, toggled feature).
    void setActive(bool active);
    bool isActive() const { return m_active; }
    // Selected draws a filled background (settings navigation).
    void setSelectedLook(bool selected);
    void setHoverBackground(bool on) { m_hoverBg = on; }
    void setIconColor(const QColor &color);
    void setLabelPixelSize(int px) { m_labelPx = px; update(); }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QString m_icon;
    QString m_label;
    int m_iconSize = 20;
    int m_labelPx = 15;
    bool m_active = false;
    bool m_selected = false;
    bool m_hover = false;
    bool m_hoverBg = true;
    QColor m_iconColor;
};

// The accent pill play/pause button.
class PlayButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit PlayButton(QWidget *parent = nullptr);
    void setPlaying(bool playing);
    QSize sizeHint() const override { return {56, 28}; }

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override { m_hover = true; update(); }
    void leaveEvent(QEvent *) override { m_hover = false; update(); }

private:
    bool m_playing = false;
    bool m_hover = false;
};

// Track + dark-accent fill + accent pill handle, as used everywhere in the design.
class SliderBar : public QWidget {
    Q_OBJECT
public:
    explicit SliderBar(Qt::Orientation orientation = Qt::Horizontal, QWidget *parent = nullptr);

    void setRange(double min, double max);
    double minimum() const { return m_min; }
    double maximum() const { return m_max; }
    void setValue(double value);
    double value() const { return m_value; }
    void setStep(double step) { m_step = step; }
    void setTicks(int count) { m_ticks = count; update(); }
    void setThickness(int px);
    void setHandleOvershoot(int px) { m_overshoot = px; updateGeometry(); update(); }
    void setRadius(int px) { m_radius = px; update(); }
    void setNeutral(bool neutral) { m_neutral = neutral; update(); }
    // Optional mark on the track (e.g. 0 dB / 100 %).
    void setMarker(double value) { m_marker = value; update(); }
    // Report the value under the pointer while hovering or dragging (seek bar previews).
    void setHoverTracking(bool on) { m_hoverTracking = on; setMouseTracking(on); }
    bool isDragging() const { return m_dragging; }
    // Draw the track as a waveform (bars of these 0..1 peaks) instead of a bar.
    void setWaveform(const QVector<float> &peaks) { m_wave = peaks; update(); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void valueChanged(double value); // user interaction only
    void sliderPressed();
    void sliderReleased();
    void hovered(double value, const QPoint &pos); // pos in widget coordinates
    void hoverEnded();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    double valueAt(const QPointF &pos) const;
    QRectF trackRect() const;
    void userSet(double v);

    Qt::Orientation m_orientation;
    double m_min = 0, m_max = 1, m_value = 0, m_step = 0;
    double m_marker = qQNaN();
    int m_ticks = 0;
    int m_thickness = 22;
    int m_overshoot = 4;
    int m_radius = 7;
    bool m_neutral = false;
    bool m_dragging = false;
    bool m_hoverTracking = false;
    QVector<float> m_wave;
    qreal m_hoverX = -1;
};

// Wedge-shaped volume meter from the control bar.
class VolumeBar : public QWidget {
    Q_OBJECT
public:
    explicit VolumeBar(QWidget *parent = nullptr);
    void setValue(int value);
    int value() const { return m_value; }
    void setMuted(bool muted) { m_muted = muted; update(); }
    QSize sizeHint() const override { return {92, 24}; }

signals:
    void valueChanged(int value);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    void setFromX(qreal x);
    int m_value = 80;
    int m_max = 125;
    bool m_muted = false;
};

// OFF [■]  /  ON [■] toggle.
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
public:
    explicit ToggleSwitch(QWidget *parent = nullptr);
    QSize sizeHint() const override { return {60, 28}; }

protected:
    void paintEvent(QPaintEvent *) override;
};

// Row of mutually exclusive options (Dark | Light | System).
class SegmentedControl : public QWidget {
    Q_OBJECT
public:
    explicit SegmentedControl(const QStringList &options, QWidget *parent = nullptr);
    void setCurrentIndex(int index);
    int currentIndex() const { return m_current; }
    QSize sizeHint() const override;

signals:
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    QRectF segmentRect(int i) const;
    QStringList m_options;
    int m_current = 0;
};

// Accent colour picker swatches.
class AccentSwatches : public QWidget {
    Q_OBJECT
public:
    explicit AccentSwatches(QWidget *parent = nullptr);
    QSize sizeHint() const override;

signals:
    void picked(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    QRectF cell(int i) const;
};

// Combo box drawn like the design's dropdowns.
class Dropdown : public QComboBox {
    Q_OBJECT
public:
    explicit Dropdown(QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    // The open list is at least as wide as its longest item (plus room for the
    // check mark), even when the closed box is narrower.
    void showPopup() override;

protected:
    void paintEvent(QPaintEvent *) override;
};

// "#FF0000 [■]" colour field.
class ColorField : public QWidget {
    Q_OBJECT
public:
    explicit ColorField(const QColor &color = Qt::red, QWidget *parent = nullptr);
    void setColor(const QColor &color);
    QColor color() const { return m_color; }

signals:
    void colorChanged(const QColor &color);

private:
    QLineEdit *m_edit;
    class QPushButton *m_swatch;
    QColor m_color;
    void updateSwatch();
};

// ⓘ with a tooltip.
class InfoIcon : public QLabel {
public:
    explicit InfoIcon(const QString &tip, QWidget *parent = nullptr);
};

// Theme preview card with accent outline when selected.
class ThemeCard : public QAbstractButton {
    Q_OBJECT
public:
    ThemeCard(const QString &label, int kind, QWidget *parent = nullptr); // kind: 0 dark, 1 light, 2 system, 3 omarchy
    QSize sizeHint() const override { return {92, 118}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int m_kind;
};

// Radio button with the accent ring/dot from the design.
class RadioDot : public QRadioButton {
    Q_OBJECT
public:
    explicit RadioDot(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
};

// Card with a "Collapse Settings ⌄" header that hides/shows its contents.
class CollapsiblePanel : public QWidget {
    Q_OBJECT
public:
    explicit CollapsiblePanel(QWidget *parent = nullptr);
    QVBoxLayout *body() const { return m_body; }
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }
    void setTexts(const QString &expanded, const QString &collapsed);
    void setHeaderCentered(bool centered);

signals:
    void toggled(bool expanded);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void refreshHeader();
    QWidget *m_content;
    QVBoxLayout *m_body;
    QAbstractButton *m_header;
    bool m_expanded = true;
    QString m_expandedText = tr("Collapse Settings");
    QString m_collapsedText = tr("Expand Settings");
};

// "Label .......... value" with a slider underneath.
class ValueSlider : public QWidget {
    Q_OBJECT
public:
    ValueSlider(const QString &label, double min, double max, double value,
                std::function<QString(double)> format, QWidget *parent = nullptr);
    SliderBar *slider() const { return m_slider; }
    void setValue(double v);
    double value() const;
    void setTicks(int n) { m_slider->setTicks(n); }

signals:
    void valueChanged(double value);

private:
    SliderBar *m_slider;
    QLabel *m_value;
    std::function<QString(double)> m_format;
};

// Helpers for building settings pages.
QWidget *settingRow(const QString &label, QWidget *control, const QString &info = QString(),
                    QWidget *parent = nullptr);
QLabel *sectionHeading(const QString &text, QWidget *parent = nullptr);
QWidget *hline(QWidget *parent = nullptr);

// The parts every dialog is built from, so they all look alike.
class QLineEdit;
class QPushButton;
namespace Dialog {
QPushButton *button(const QString &text, bool accent = false); // 36 px high; accent: the main action
QLineEdit *field(const QString &placeholder = QString());
QLabel *caption(const QString &text, bool wrap = true); // dim text
} // namespace Dialog
QString formatTime(qint64 ms, bool forceHours = false);

// Tab with icon + label and an accent underline when checked.
class TabButton : public QAbstractButton {
    Q_OBJECT
public:
    TabButton(const QString &icon, const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_icon;
};

// A short list of options with a check mark on the chosen one (audio and
// subtitle tracks, output devices). Rows are clickable.
class ChoiceList : public QWidget {
    Q_OBJECT
public:
    explicit ChoiceList(QWidget *parent = nullptr);
    void setItems(const QList<QPair<QString, QVariant>> &items, const QVariant &current);
    void setEmptyText(const QString &text);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

signals:
    void chosen(const QVariant &value);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    static constexpr int kRow = 38;
    QList<QPair<QString, QVariant>> m_items;
    QVariant m_current;
    QString m_empty;
    int m_hover = -1;
};
