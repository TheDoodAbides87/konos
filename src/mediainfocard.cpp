#include "mediainfocard.h"
#include "player.h"
#include "theme.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

MediaInfoCard::MediaInfoCard(Player *player, QWidget *parent) : QWidget(parent), m_player(player)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    for (auto signal : {&Player::mediaChanged, &Player::metaChanged, &Player::tracksChanged})
        connect(player, signal, this, &MediaInfoCard::refresh);
}

static QString channelsText(int n)
{
    switch (n) {
    case 1:
        return QObject::tr("Mono");
    case 2:
        return QObject::tr("Stereo");
    case 6:
        return "5.1";
    case 8:
        return "7.1";
    default:
        return n > 0 ? QObject::tr("%1 ch").arg(n) : QString();
    }
}

void MediaInfoCard::refresh()
{
    m_title = m_player->title();
    QStringList sub;
    if (!m_player->artist().isEmpty())
        sub << m_player->artist();
    if (!m_player->album().isEmpty())
        sub << m_player->album();
    const QString loc = m_player->location();
    const QUrl url(loc);
    const QString path = url.isLocalFile() ? url.toLocalFile() : loc;
    const QFileInfo fi(path);
    if (sub.isEmpty() && fi.exists())
        sub << fi.dir().dirName();
    m_subtitle = sub.join(" · ");

    QStringList video, audio;
    const int vt = m_player->videoTrack(), at = m_player->audioTrack();
    for (const Player::TrackInfo &t : m_player->trackInfo()) {
        if (t.type == Player::TrackInfo::Video && (video.isEmpty() && (vt < 0 || t.id == vt))) {
            if (t.width > 0)
                video << QString("%1×%2").arg(t.width).arg(t.height);
            video << t.codec.section(" - ", 0, 0); // "H264 - MPEG-4 AVC (part 10)" → "H264"
            if (t.fps > 0)
                video << tr("%1 fps").arg(QLocale().toString(t.fps, 'g', 5));
        } else if (t.type == Player::TrackInfo::Audio && audio.isEmpty() && (at < 0 || t.id == at)) {
            audio << t.codec.section(" - ", 0, 0) << channelsText(t.channels);
            if (t.sampleRate > 0)
                audio << tr("%1 kHz").arg(QLocale().toString(t.sampleRate / 1000.0, 'g', 3));
            if (const auto l = QLocale::codeToLanguage(t.language.toLower(), QLocale::AnyLanguageCode);
                l != QLocale::AnyLanguage && l != QLocale::C)
                audio << QLocale::languageToString(l);
        }
    }
    QStringList details;
    video.removeAll(QString());
    audio.removeAll(QString());
    if (!video.isEmpty())
        details << video.join(" · ");
    if (!audio.isEmpty())
        details << audio.join(" · ");
    if (fi.isFile())
        details << QLocale().formattedDataSize(fi.size(), 1);
    m_details = details.join("\n");

    // Size to fit, up to half the parent's width.
    QFont tf = font(), sf = font();
    tf.setPixelSize(16);
    tf.setWeight(QFont::DemiBold);
    sf.setPixelSize(12);
    const int maxW = parentWidget() ? qMax(280, parentWidget()->width() * 2 / 3) : 600;
    int widest = std::max(QFontMetrics(tf).horizontalAdvance(m_title), QFontMetrics(sf).horizontalAdvance(m_subtitle));
    for (const QString &line : m_details.split('\n'))
        widest = std::max(widest, QFontMetrics(sf).horizontalAdvance(line));
    const int lines = m_details.isEmpty() ? 0 : int(m_details.count('\n')) + 1;
    resize(qMin(maxW, 44 + widest), 12 + 24 + (m_subtitle.isEmpty() ? 0 : 18) + lines * 18 + 10);
    update();
}

void MediaInfoCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath bg;
    bg.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    p.fillPath(bg, QColor(12, 12, 12, 200));
    QFont tf = font(), sf = font();
    tf.setPixelSize(16);
    tf.setWeight(QFont::DemiBold);
    sf.setPixelSize(12);
    const int textW = width() - 32;
    int y = 12;
    p.setFont(tf);
    p.setPen(QColor(0xf4, 0xf4, 0xf4));
    p.drawText(QRect(16, y, textW, 22), Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetrics(tf).elidedText(m_title, Qt::ElideRight, textW));
    y += 24;
    p.setFont(sf);
    if (!m_subtitle.isEmpty()) {
        p.setPen(QColor(0xc8, 0xc8, 0xc8));
        p.drawText(QRect(16, y, textW, 18), Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(sf).elidedText(m_subtitle, Qt::ElideRight, textW));
        y += 18;
    }
    p.setPen(theme().accent().lighter(115));
    for (const QString &line : m_details.split('\n')) {
        p.drawText(QRect(16, y, textW, 18), Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(sf).elidedText(line, Qt::ElideRight, textW));
        y += 18;
    }
}
