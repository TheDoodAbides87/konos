#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPixmap>
#include <optional>

class Player;

class QOpenGLShaderProgram;

// Paints the current video frame, album art for audio, or the idle screen.
// Frames arrive as planar YUV; colour conversion, scaling, the aspect-ratio
// override and crop all happen in one shader pass on the GPU.
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit VideoWidget(Player *player, QWidget *parent = nullptr);
    ~VideoWidget() override;

    static QStringList aspectRatios();
    void setAspectRatio(const QString &ratio);
    QString aspectRatio() const { return m_aspect; }
    // Picture transforms, done while painting (no reopening). Rotation is in
    // quarter turns clockwise; zoom 0 = fit the window, else a scale on top of it.
    void setRotation(int quarterTurns);
    int rotation() const { return m_rotation; }
    void setFlip(bool horizontal, bool vertical);
    bool flipH() const { return m_flipH; }
    bool flipV() const { return m_flipV; }
    void setZoom(double zoom);
    double zoom() const { return m_zoom; }
    static QList<QPair<QString, double>> zoomLevels();
    // Tone-maps HDR (PQ, BT.2020) pictures to SDR: for videos that look washed out.
    void setHdrToneMapping(bool on);
    bool hdrToneMapping() const { return m_hdr; }
    QImage snapshot() const;
    // Maps a widget position to video frame pixels; null if outside the picture.
    std::optional<QPointF> framePoint(const QPoint &pos) const;

signals:
    void clicked(const QPoint &pos);
    void doubleClicked();
    void contextMenuRequested(const QPoint &globalPos);
    void mouseMoved(const QPoint &pos);
    void wheelScrolled(int delta);
    void wheelScrolledSideways(int delta); // positive: to the right

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    bool paintVideo();
    void releaseGL();
    void paintAudio(class QPainter &p);
    void paintIdle(class QPainter &p);
    QRectF videoRect(const QSizeF &source) const;
    QRectF sourceRect(const QSize &frame) const;
    QSizeF orientedSize(const QSizeF &source) const; // width and height swap at 90° and 270°
    QPointF sourceCorner(QPointF screen) const;      // see the .cpp

    Player *m_player;
    QString m_aspect = "Auto";
    int m_rotation = 0;
    bool m_flipH = false, m_flipV = false, m_hdr = false;
    double m_zoom = 0;
    QPixmap m_art;
    QString m_artPath;
    QPoint m_pressPos;

    QOpenGLShaderProgram *m_program = nullptr;
    GLuint m_textures[3] = {0, 0, 0};
    QSize m_planeSizes[3];
    quint64 m_uploaded = 0; // serial of the frame currently in the textures
};
