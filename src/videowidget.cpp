#include "videowidget.h"
#include "effects.h"
#include "player.h"
#include "theme.h"

#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QPainterPath>
#include <QDebug>

VideoWidget::VideoWidget(Player *player, QWidget *parent) : QOpenGLWidget(parent), m_player(player)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(false); // the main window handles drops
    connect(player, &Player::frameReady, this, qOverload<>(&QWidget::update));
    connect(player, &Player::metaChanged, this, qOverload<>(&QWidget::update));
    connect(player, &Player::mediaChanged, this, qOverload<>(&QWidget::update));
    connect(player, &Player::stateChanged, this, qOverload<>(&QWidget::update));
    connect(&theme(), &Theme::changed, this, qOverload<>(&QWidget::update));
    connect(&fx(), &Effects::changed, this, [this](Effects::Group g) {
        if (g == Effects::Crop)
            update();
    });
}

VideoWidget::~VideoWidget()
{
    makeCurrent();
    releaseGL();
    doneCurrent();
}

static const char *vertexShader = R"(
attribute vec2 position;
attribute vec2 texcoord;
varying vec2 uv;
void main()
{
    uv = texcoord;
    gl_Position = vec4(position, 0.0, 1.0);
})";

// Limited-range YUV to RGB. Textures are as wide as the buffer pitch, so the
// horizontal coordinate is scaled into the visible part (x) and clamped (y).
static const char *fragmentShader = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec2 uv;
uniform sampler2D planeY;
uniform sampler2D planeU;
uniform sampler2D planeV;
uniform vec2 scaleY;
uniform vec2 scaleC;
uniform vec4 coef; // r = v, g = u, g = v, b = u
uniform float hdr;  // 1: the picture is PQ / BT.2020, tone-map it to SDR
vec3 pqToLinear(vec3 e)
{
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / 78.84375));
    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), vec3(1.0 / 0.1593017578125));
}
void main()
{
    float y = texture2D(planeY, vec2(min(uv.x * scaleY.x, scaleY.y), uv.y)).r;
    vec2 c = vec2(min(uv.x * scaleC.x, scaleC.y), uv.y);
    float u = texture2D(planeU, c).r - 0.5;
    float v = texture2D(planeV, c).r - 0.5;
    y = 1.164383 * (y - 0.062745);
    vec3 rgb = vec3(y + coef.x * v, y - coef.y * u - coef.z * v, y + coef.w * u);
    if (hdr > 0.5) {
        // PQ to light (1.0 = 203 nit reference white), BT.2020 to BT.709
        // primaries, then a soft roll-off for highlights up to ~1000 nits.
        vec3 l = pqToLinear(rgb) * (10000.0 / 203.0);
        l = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083, 1.1187) * l;
        l = max(l, 0.0);
        const float white = 1000.0 / 203.0;
        l = l * (1.0 + l / (white * white)) / (1.0 + l);
        rgb = pow(l, vec3(1.0 / 2.2));
    }
    gl_FragColor = vec4(rgb, 1.0);
})";

void VideoWidget::initializeGL()
{
    initializeOpenGLFunctions();
    m_program = new QOpenGLShaderProgram;
    m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader);
    m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader);
    m_program->bindAttributeLocation("position", 0);
    m_program->bindAttributeLocation("texcoord", 1);
    if (!m_program->link()) {
        qWarning() << "video shader failed to link:" << m_program->log();
        delete m_program;
        m_program = nullptr;
    }
    glGenTextures(3, m_textures);
    for (GLuint t : m_textures) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    m_uploaded = 0;
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &VideoWidget::releaseGL, Qt::DirectConnection);
}

void VideoWidget::releaseGL()
{
    if (!m_program)
        return;
    glDeleteTextures(3, m_textures);
    delete m_program;
    m_program = nullptr;
    m_planeSizes[0] = m_planeSizes[1] = m_planeSizes[2] = QSize();
    m_uploaded = 0;
}

bool VideoWidget::paintVideo()
{
    if (!m_program)
        return false;
    bool painted = false;
    m_player->withFrame([&](const Player::Frame &f) {
        // Upload only new pictures; repaints for UI changes reuse the textures.
        if (f.serial != m_uploaded) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            for (int i = 0; i < 3; ++i) {
                const QSize size(f.pitches[i], i == 0 ? f.size.height() : f.size.height() / 2);
                glBindTexture(GL_TEXTURE_2D, m_textures[i]);
                if (size != m_planeSizes[i]) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, size.width(), size.height(), 0, GL_LUMINANCE,
                                 GL_UNSIGNED_BYTE, f.planes[i]);
                    m_planeSizes[i] = size;
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.width(), size.height(), GL_LUMINANCE,
                                    GL_UNSIGNED_BYTE, f.planes[i]);
                }
            }
            // Back to the default: Qt's text drawing uploads its glyphs with
            // this setting too, and garbles them at 1.
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            m_uploaded = f.serial;
        }

        // Crop and aspect work in display coordinates; texture coordinates are 0..1.
        const QRectF src = sourceRect(f.displaySize);
        const QRectF target = videoRect(orientedSize(src.size()));
        const qreal w = width(), h = height();
        const GLfloat l = GLfloat(2 * target.left() / w - 1), r = GLfloat(2 * target.right() / w - 1);
        const GLfloat t = GLfloat(1 - 2 * target.top() / h), b = GLfloat(1 - 2 * target.bottom() / h);
        const GLfloat position[] = {l, t, r, t, l, b, r, b};
        // Each screen corner (top-left, top-right, bottom-left, bottom-right)
        // samples the source corner that rotation and flips bring there.
        GLfloat texcoord[8];
        for (int i = 0; i < 4; ++i) {
            const QPointF s = sourceCorner(QPointF(i % 2, i / 2));
            texcoord[2 * i] = GLfloat((src.left() + s.x() * src.width()) / f.displaySize.width());
            texcoord[2 * i + 1] = GLfloat((src.top() + s.y() * src.height()) / f.displaySize.height());
        }

        m_program->bind();
        for (int i = 0; i < 3; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        }
        glActiveTexture(GL_TEXTURE0);
        m_program->setUniformValue("planeY", 0);
        m_program->setUniformValue("planeU", 1);
        m_program->setUniformValue("planeV", 2);
        const float wy = float(f.size.width()), wc = float(f.size.width() / 2);
        m_program->setUniformValue("scaleY", wy / f.pitches[0], (wy - 0.5f) / f.pitches[0]);
        m_program->setUniformValue("scaleC", wc / f.pitches[1], (wc - 0.5f) / f.pitches[1]);
        m_program->setUniformValue("hdr", m_hdr ? 1.0f : 0.0f);
        if (m_hdr) // BT.2020 non-constant luminance
            m_program->setUniformValue("coef", 1.678674f, 0.187326f, 0.650424f, 2.141772f);
        else if (f.bt709)
            m_program->setUniformValue("coef", 1.792741f, 0.213249f, 0.532909f, 2.112402f);
        else
            m_program->setUniformValue("coef", 1.596027f, 0.391762f, 0.812968f, 2.017232f);
        m_program->enableAttributeArray(0);
        m_program->enableAttributeArray(1);
        m_program->setAttributeArray(0, GL_FLOAT, position, 2);
        m_program->setAttributeArray(1, GL_FLOAT, texcoord, 2);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_program->disableAttributeArray(0);
        m_program->disableAttributeArray(1);
        m_program->release();
        painted = true;
    });
    return painted;
}

QStringList VideoWidget::aspectRatios()
{
    return {"Auto", "16:9", "4:3", "16:10", "21:9", "2.35:1", "2.39:1", "1:1", "5:4", "Fill", "Stretch"};
}

void VideoWidget::setAspectRatio(const QString &ratio)
{
    m_aspect = ratio;
    update();
}

void VideoWidget::setRotation(int quarterTurns)
{
    m_rotation = ((quarterTurns % 4) + 4) % 4;
    update();
}

void VideoWidget::setFlip(bool horizontal, bool vertical)
{
    m_flipH = horizontal;
    m_flipV = vertical;
    update();
}

QList<QPair<QString, double>> VideoWidget::zoomLevels()
{
    return {{tr("Fit to Window"), 0}, {"125%", 1.25}, {"150%", 1.5}, {"200%", 2.0}, {"300%", 3.0}};
}

void VideoWidget::setZoom(double zoom)
{
    m_zoom = zoom;
    update();
}

void VideoWidget::setHdrToneMapping(bool on)
{
    m_hdr = on;
    update();
}

QSizeF VideoWidget::orientedSize(const QSizeF &source) const
{
    return m_rotation % 2 ? source.transposed() : source;
}

// Screen position (0..1 in the picture) to source position (0..1): undo the
// clockwise rotation, then the flips (which apply to the picture as shown).
QPointF VideoWidget::sourceCorner(QPointF p) const
{
    if (m_flipH)
        p.setX(1 - p.x());
    if (m_flipV)
        p.setY(1 - p.y());
    for (int i = 0; i < m_rotation; ++i) // one clockwise quarter turn back
        p = QPointF(p.y(), 1 - p.x());
    return p;
}

QRectF VideoWidget::videoRect(const QSizeF &source) const
{
    const QSizeF area = size();
    if (m_aspect == "Stretch" || source.isEmpty())
        return QRectF(QPointF(0, 0), area);
    double ar = source.width() / source.height();
    if (m_aspect != "Auto" && m_aspect != "Fill") {
        const QStringList parts = m_aspect.split(':');
        if (parts.size() == 2 && parts[1].toDouble() > 0)
            ar = parts[0].toDouble() / parts[1].toDouble();
    }
    if (m_aspect != "Auto" && m_aspect != "Fill" && m_rotation % 2)
        ar = 1 / ar; // a forced ratio describes the unrotated picture
    QSizeF target(area.width(), area.width() / ar);
    if (m_aspect == "Fill" ? target.height() < area.height() : target.height() > area.height())
        target = QSizeF(area.height() * ar, area.height());
    if (m_zoom > 0)
        target *= m_zoom;
    return QRectF(QPointF((area.width() - target.width()) / 2, (area.height() - target.height()) / 2), target);
}

QRectF VideoWidget::sourceRect(const QSize &frame) const
{
    const EffectsData &e = fx().data();
    QRectF src(0, 0, frame.width(), frame.height());
    src.adjust(e.cropLeft, e.cropTop, -e.cropRight, -e.cropBottom);
    if (src.width() < 16 || src.height() < 16)
        src = QRectF(0, 0, frame.width(), frame.height());
    return src;
}

std::optional<QPointF> VideoWidget::framePoint(const QPoint &pos) const
{
    const QSize frame = m_player->videoSize();
    if (frame.isEmpty())
        return std::nullopt;
    const QRectF src = sourceRect(frame);
    const QRectF target = videoRect(orientedSize(src.size()));
    if (!target.contains(pos))
        return std::nullopt;
    const QPointF s = sourceCorner(QPointF((pos.x() - target.x()) / target.width(), (pos.y() - target.y()) / target.height()));
    return QPointF(src.x() + s.x() * src.width(), src.y() + s.y() * src.height());
}

void VideoWidget::paintGL()
{
    // Fullscreen: pure black around the picture, whatever the theme's colour.
    const QColor bg = window()->isFullScreen() ? QColor(Qt::black) : theme().pal().videoBg;
    glClearColor(GLfloat(bg.redF()), GLfloat(bg.greenF()), GLfloat(bg.blueF()), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (paintVideo())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (m_player->hasMedia() && m_player->state() != Player::Error)
        paintAudio(p);
    else
        paintIdle(p);
}

void VideoWidget::paintAudio(QPainter &p)
{
    // Audio-only media: album art (or a note) with title and artist.
    const Palette &c = theme().pal();
    p.setRenderHint(QPainter::Antialiasing);
    if (m_artPath != m_player->artworkPath()) {
        m_artPath = m_player->artworkPath();
        m_art = m_artPath.isEmpty() ? QPixmap() : QPixmap(m_artPath);
    }
    const int side = qMin(qMin(width(), height()) / 2, 360);
    const QRectF art((width() - side) / 2.0, (height() - side) / 2.0 - 40, side, side);
    QPainterPath clip;
    clip.addRoundedRect(art, 12, 12);
    if (!m_art.isNull()) {
        p.save();
        p.setClipPath(clip);
        const QSizeF s = QSizeF(m_art.size()).scaled(art.size(), Qt::KeepAspectRatioByExpanding);
        p.drawPixmap(QRectF(art.center().x() - s.width() / 2, art.center().y() - s.height() / 2, s.width(), s.height()),
                     m_art, QRectF(m_art.rect()));
        p.restore();
    } else {
        p.fillPath(clip, theme().tone(QColor(0x1c, 0x1c, 0x1c)));
        const int is = side / 3;
        p.drawPixmap(QPointF(art.center().x() - is / 2.0, art.center().y() - is / 2.0),
                     theme().pixmap("note", theme().accent(), is, devicePixelRatioF()));
    }
    QFont f = font();
    f.setPixelSize(20);
    f.setWeight(QFont::Medium);
    p.setFont(f);
    p.setPen(theme().tone(QColor(0xe6, 0xe6, 0xe6)));
    const QRectF titleRect(0, art.bottom() + 20, width(), 30);
    p.drawText(titleRect, Qt::AlignCenter, QFontMetrics(f).elidedText(m_player->title(), Qt::ElideRight, width() - 80));
    QStringList sub;
    if (!m_player->artist().isEmpty())
        sub << m_player->artist();
    if (!m_player->album().isEmpty())
        sub << m_player->album();
    if (!sub.isEmpty()) {
        f.setPixelSize(15);
        f.setWeight(QFont::Normal);
        p.setFont(f);
        p.setPen(c.textMuted);
        p.drawText(QRectF(0, titleRect.bottom() + 4, width(), 24), Qt::AlignCenter, sub.join("  ·  "));
    }
}

void VideoWidget::paintIdle(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing);
    const int s = qMin(110, height() / 5);
    const QRectF logo((width() - s) / 2.0, height() / 2.0 - s - 10, s, s);
    QColor accent = theme().accent();
    accent.setAlphaF(0.85);
    Theme::paintLogo(&p, logo, accent);
    QFont f = font();
    f.setPixelSize(15);
    p.setFont(f);
    p.setPen(theme().tone(QColor(0x80, 0x80, 0x80)));
    const QString text = m_player->state() == Player::Error
                             ? tr("This media couldn't be played.")
                             : tr("Drop media here, or press Ctrl+O to open a file");
    p.drawText(QRectF(0, logo.bottom() + 28, width(), 24), Qt::AlignCenter, text);
}

QImage VideoWidget::snapshot() const
{
    QImage img = m_player->snapshot();
    if (img.isNull())
        return img;
    const QRect src = sourceRect(img.size()).toAlignedRect();
    if (src != img.rect())
        img = img.copy(src);
    if (m_rotation) // as shown: rotated, then flipped
        img = img.transformed(QTransform().rotate(90 * m_rotation));
    if (m_flipH || m_flipV)
        img = img.flipped((m_flipH ? Qt::Horizontal : Qt::Orientations()) | (m_flipV ? Qt::Vertical : Qt::Orientations()));
    return img;
}

void VideoWidget::mousePressEvent(QMouseEvent *e)
{
    m_pressPos = e->pos();
    if (e->button() == Qt::RightButton)
        emit contextMenuRequested(e->globalPosition().toPoint());
}

void VideoWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && (e->pos() - m_pressPos).manhattanLength() < 6)
        emit clicked(e->pos());
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        emit doubleClicked();
}

void VideoWidget::mouseMoveEvent(QMouseEvent *e)
{
    emit mouseMoved(e->pos());
}

void VideoWidget::wheelEvent(QWheelEvent *e)
{
    const QPoint d = e->angleDelta();
    if (qAbs(d.x()) > qAbs(d.y()))
        emit wheelScrolledSideways(-d.x());
    else
        emit wheelScrolled(d.y());
    e->accept();
}
