#pragma once

#include "musiceditors.h" // PanelOverlay

class ControlPalette;
class ControlStrip;

// Main menu → "Customize Player" (from the redesign video, 18:56): every item
// the playback bar can show, in a box; drag them into the bar, around it, or
// back into the box to take them off.
class CustomizePlayerOverlay : public PanelOverlay {
    Q_OBJECT
public:
    explicit CustomizePlayerOverlay(QWidget *window);
    // For tests: the layout being edited, and changing it without dragging.
    QStringList items(bool left) const;
    void insert(const QString &id, bool left, int index);
    void remove(bool left, int index);

private:
    ControlPalette *m_palette;
    ControlStrip *m_strip;
};

// The bar as it will look: chips left and right of a stand-in seek bar.
class ControlStrip : public QWidget {
    Q_OBJECT
public:
    explicit ControlStrip(QWidget *parent = nullptr);
    QStringList left, right;
    void insert(const QString &id, bool toLeft, int index); // moves it if it's there already
    void removeAt(bool fromLeft, int index);
    QSize sizeHint() const override { return {900, 64}; }

signals:
    void changed();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dragLeaveEvent(QDragLeaveEvent *) override;
    void dropEvent(QDropEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    struct Chip {
        QString id;
        bool left;
        int index;
        QRect rect;
    };
    QList<Chip> chips() const;
    QRect seekRect() const;
    void dropTarget(const QPoint &pos, bool &toLeft, int &index) const;
    int m_pressed = -1;
    QPoint m_pressPos;
    bool m_dropLeft = true;
    int m_dropIndex = -1; // -1: not dragging over
};

// The box of every item.
class ControlPalette : public QWidget {
    Q_OBJECT
public:
    ControlPalette(ControlStrip *strip, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

private:
    QRect tileRect(int i) const;
    int columns() const;
    ControlStrip *m_strip;
    int m_pressed = -1;
    QPoint m_pressPos;
};
