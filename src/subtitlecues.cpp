#include "subtitlecues.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringDecoder>

SubtitleCues &SubtitleCues::instance()
{
    static SubtitleCues s;
    return s;
}

QString SubtitleCues::embeddedKey(const QString &video, int index)
{
    return QString("embedded:%1:%2").arg(index).arg(video);
}

SubtitleCues::State SubtitleCues::state(const QString &key)
{
    if (m_cues.contains(key))
        return Ready;
    if (m_failures.contains(key))
        return Failed;
    if (m_loading.contains(key))
        return Loading;
    if (key.startsWith("embedded:"))
        extract(key);
    else
        loadFile(key);
    return m_cues.contains(key) ? Ready : m_failures.contains(key) ? Failed : Loading;
}

// ---------------------------------------------------------------------------
// Parsing

static QString decodeText(const QByteArray &data)
{
    // UTF-8 (with or without BOM) or UTF-16 with BOM; otherwise Windows-1252,
    // which is what most older subtitle files are.
    auto utf = QStringDecoder::encodingForData(data);
    QStringDecoder dec(utf.value_or(QStringConverter::Utf8));
    QString text = dec(data);
    if (dec.hasError() && !utf) {
        QStringDecoder latin("windows-1252");
        text = latin.isValid() ? latin(data) : QString::fromLatin1(data);
    }
    return text;
}

static QString cleanLine(QString t)
{
    static const QRegularExpression assTags(R"(\{[^}]*\})"), htmlTags(R"(<[^>]*>)");
    t.replace(assTags, QString());
    t.replace(htmlTags, QString());
    t.replace("\\N", " ").replace("\\n", " ").replace("\\h", " ");
    t.replace('\n', ' ');
    return t.simplified();
}

QList<SubtitleCues::Cue> SubtitleCues::parse(const QByteArray &data)
{
    const QString text = decodeText(data);
    QList<Cue> out;

    if (text.contains("[Events]")) {
        // SSA/ASS: "Dialogue: layer,0:00:01.23,0:00:03.45,Style,Name,…,Text"
        static const QRegularExpression time(R"((\d+):(\d\d):(\d\d)[.:](\d\d))");
        auto ms = [](const QString &s) -> qint64 {
            const auto m = time.match(s);
            if (!m.hasMatch())
                return -1;
            return ((m.captured(1).toLongLong() * 60 + m.captured(2).toInt()) * 60 + m.captured(3).toInt()) * 1000 +
                   m.captured(4).toInt() * 10;
        };
        int textField = 9;
        bool inEvents = false;
        for (const QString &raw : text.split('\n')) {
            const QString line = raw.trimmed();
            if (line.startsWith('[')) {
                inEvents = line.compare("[Events]", Qt::CaseInsensitive) == 0;
                continue;
            }
            if (!inEvents)
                continue;
            if (line.startsWith("Format:", Qt::CaseInsensitive)) {
                const QStringList fields = line.mid(7).split(',');
                for (int i = 0; i < fields.size(); ++i)
                    if (fields[i].trimmed().compare("Text", Qt::CaseInsensitive) == 0)
                        textField = i;
                continue;
            }
            if (!line.startsWith("Dialogue:", Qt::CaseInsensitive))
                continue;
            const QStringList parts = line.mid(9).split(',');
            if (parts.size() <= textField)
                continue;
            const qint64 a = ms(parts[1]), b = ms(parts[2]);
            if (a < 0 || b <= a)
                continue;
            const QString t = cleanLine(parts.mid(textField).join(','));
            if (!t.isEmpty())
                out.append({a, b, t});
        }
    } else {
        // SRT / WebVTT: "00:00:01,234 --> 00:00:03,456" then text up to a blank line.
        static const QRegularExpression cue(
            R"(((?:\d+:)?\d{1,2}:\d\d[.,]\d{1,3})\s*-->\s*((?:\d+:)?\d{1,2}:\d\d[.,]\d{1,3}))");
        auto ms = [](QString s) -> qint64 {
            s.replace(',', '.');
            const QStringList hms = s.section('.', 0, 0).split(':');
            qint64 secs = 0;
            for (const QString &p : hms)
                secs = secs * 60 + p.toLongLong();
            const QString frac = (s.section('.', 1) + "000").left(3);
            return secs * 1000 + frac.toInt();
        };
        const QStringList lines = text.split('\n');
        for (int i = 0; i < lines.size(); ++i) {
            const auto m = cue.match(lines[i]);
            if (!m.hasMatch())
                continue;
            QStringList body;
            int j = i + 1;
            for (; j < lines.size() && !lines[j].trimmed().isEmpty(); ++j)
                body << lines[j].trimmed();
            const QString t = cleanLine(body.join(' '));
            const qint64 a = ms(m.captured(1)), b = ms(m.captured(2));
            if (!t.isEmpty() && b > a)
                out.append({a, b, t});
            i = j - 1;
        }
    }
    std::sort(out.begin(), out.end(), [](const Cue &a, const Cue &b) { return a.start < b.start; });
    return out;
}

// ---------------------------------------------------------------------------

void SubtitleCues::loadFile(const QString &key)
{
    QFile f(key);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 20 * 1024 * 1024) {
        m_failures.insert(key, tr("Couldn't read the subtitle file."));
        return;
    }
    const QList<Cue> cues = parse(f.readAll());
    if (cues.isEmpty())
        m_failures.insert(key, tr("No timed lines found in this subtitle file."));
    else
        m_cues.insert(key, cues);
}

QString SubtitleCues::plainFile(const QString &key)
{
    if (!m_cues.contains(key))
        return {};
    const QString base = key.startsWith("embedded:") ? QFileInfo(key.section(':', 2)).completeBaseName()
                                                     : QFileInfo(key).completeBaseName();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/subtitles/plain/" +
                        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex();
    const QString path = dir + "/" + base + ".plain.srt";
    if (QFile::exists(path))
        return path;
    QDir().mkpath(dir);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    auto stamp = [](qint64 ms) {
        return QString("%1:%2:%3,%4")
            .arg(ms / 3600000, 2, 10, QChar('0'))
            .arg(ms / 60000 % 60, 2, 10, QChar('0'))
            .arg(ms / 1000 % 60, 2, 10, QChar('0'))
            .arg(ms % 1000, 3, 10, QChar('0'));
    };
    QString out;
    int n = 0;
    for (const Cue &c : m_cues.value(key))
        out += QString("%1\n%2 --> %3\n%4\n\n").arg(++n).arg(stamp(c.start), stamp(c.end), c.text);
    f.write(out.toUtf8());
    return path;
}

QString SubtitleCues::cacheFile(const QString &video, int index) const
{
    const QFileInfo fi(video);
    const QByteArray id = video.toUtf8() + QByteArray::number(fi.lastModified().toSecsSinceEpoch()) +
                          QByteArray::number(fi.size()) + "#" + QByteArray::number(index);
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/subtitles/" +
           QCryptographicHash::hash(id, QCryptographicHash::Md5).toHex() + ".srt";
}

void SubtitleCues::extract(const QString &key)
{
    const int index = key.section(':', 1, 1).toInt();
    const QString video = key.section(':', 2);
    const QString cached = cacheFile(video, index);
    if (QFile f(cached); f.open(QIODevice::ReadOnly)) {
        const QList<Cue> cues = parse(f.readAll());
        if (!cues.isEmpty()) {
            m_cues.insert(key, cues);
            return;
        }
    }
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty() || !QFileInfo(video).isFile()) {
        m_failures.insert(key, ffmpeg.isEmpty() ? tr("Install ffmpeg to see the timing of subtitles inside videos.")
                                                : tr("Timing is only available for files on this computer."));
        return;
    }
    m_loading.insert(key);
    auto *proc = new QProcess(this);
    proc->setProgram(ffmpeg);
    proc->setArguments({"-nostdin", "-v", "error", "-i", video, "-map", QString("0:s:%1").arg(index), "-f", "srt",
                        "-"});
    connect(proc, &QProcess::finished, this, [this, proc, key, cached] {
        m_loading.remove(key);
        const QByteArray out = proc->readAllStandardOutput();
        const QList<Cue> cues = proc->exitStatus() == QProcess::NormalExit ? parse(out) : QList<Cue>();
        if (cues.isEmpty()) {
            m_failures.insert(key, tr("No timing available for this track (it may be picture-based)."));
        } else {
            m_cues.insert(key, cues);
            QDir().mkpath(QFileInfo(cached).absolutePath());
            if (QFile f(cached); f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(out);
        }
        proc->deleteLater();
        emit ready(key);
    });
    proc->start();
}
