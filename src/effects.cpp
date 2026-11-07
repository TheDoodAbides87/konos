#include "effects.h"

#include <QSettings>
#include <vlc/vlc.h>

bool EffectsData::videoAdjustDefault() const
{
    return hue == 0.0f && brightness == 1.0f && contrast == 1.0f && saturation == 1.0f && gamma == 1.0f;
}

bool EffectsData::adjustActive() const
{
    return !videoAdjustDefault();
}

Effects &Effects::instance()
{
    static Effects e;
    return e;
}

void Effects::commit(Group group, bool live)
{
    save();
    emit changed(group, live);
}

void Effects::resetGroup(Group group)
{
    const EffectsData defaults;
    switch (group) {
    case Equalizer:
        m_data.eqEnabled = defaults.eqEnabled;
        m_data.eqPreset = defaults.eqPreset;
        m_data.eqPreamp = defaults.eqPreamp;
        m_data.eqBands = defaults.eqBands;
        m_data.eqTwoPass = defaults.eqTwoPass;
        break;
    case AudioFilters:
        m_data.compEnabled = false;
        m_data.compRmsPeak = defaults.compRmsPeak;
        m_data.compAttack = defaults.compAttack;
        m_data.compRelease = defaults.compRelease;
        m_data.compThreshold = defaults.compThreshold;
        m_data.compRatio = defaults.compRatio;
        m_data.compKnee = defaults.compKnee;
        m_data.compMakeup = defaults.compMakeup;
        m_data.spatialEnabled = false;
        m_data.widenEnabled = false;
        m_data.normalize = false;
        break;
    case VideoAdjust:
        m_data.hue = defaults.hue;
        m_data.brightness = defaults.brightness;
        m_data.contrast = defaults.contrast;
        m_data.saturation = defaults.saturation;
        m_data.gamma = defaults.gamma;
        m_data.sharpen = defaults.sharpen;
        m_data.debandRadius = defaults.debandRadius;
        m_data.grain = defaults.grain;
        break;
    case VideoFilters:
        m_data.extractEnabled = false;
        m_data.thresholdEnabled = false;
        m_data.negate = false;
        m_data.posterize = false;
        m_data.gradient = false;
        m_data.sepia = 0;
        break;
    case Crop:
        m_data.cropTop = m_data.cropBottom = m_data.cropLeft = m_data.cropRight = 0;
        break;
    case SubtitleStyle:
        m_data.subFont = defaults.subFont;
        m_data.subColor = defaults.subColor;
        m_data.subOutline = defaults.subOutline;
        m_data.subSize = defaults.subSize;
        m_data.subSizePixels = defaults.subSizePixels;
        m_data.subOutlineThickness = defaults.subOutlineThickness;
        m_data.subBackgroundOpacity = defaults.subBackgroundOpacity;
        m_data.subMargin = defaults.subMargin;
        m_data.subEmbeddedStyle = defaults.subEmbeddedStyle;
        break;
    }
    commit(group);
}

QStringList Effects::eqPresetNames()
{
    QStringList names;
    const unsigned count = libvlc_audio_equalizer_get_preset_count();
    for (unsigned i = 0; i < count; ++i)
        names << QString::fromUtf8(libvlc_audio_equalizer_get_preset_name(i));
    return names;
}

QVector<float> Effects::eqPresetBands(int index)
{
    QVector<float> bands(10, 0.0f);
    if (libvlc_equalizer_t *eq = libvlc_audio_equalizer_new_from_preset(unsigned(index))) {
        for (int i = 0; i < bands.size(); ++i)
            bands[i] = libvlc_audio_equalizer_get_amp_at_index(eq, unsigned(i));
        libvlc_audio_equalizer_release(eq);
    }
    return bands;
}

void Effects::applyEqPreset(int index)
{
    m_data.eqPreset = index;
    if (index >= 0) {
        libvlc_equalizer_t *eq = libvlc_audio_equalizer_new_from_preset(unsigned(index));
        if (eq) {
            m_data.eqPreamp = libvlc_audio_equalizer_get_preamp(eq);
            for (int i = 0; i < m_data.eqBands.size(); ++i)
                m_data.eqBands[i] = libvlc_audio_equalizer_get_amp_at_index(eq, unsigned(i));
            libvlc_audio_equalizer_release(eq);
        }
    }
    commit(Equalizer);
}

#define LOAD(field) m_data.field = s.value(#field, m_data.field).value<decltype(m_data.field)>()
#define SAVE(field) s.setValue(#field, m_data.field)

#define FIELDS(X)                                                                                        \
    X(eqEnabled); X(eqPreset); X(eqPreamp); X(eqTwoPass); X(compEnabled); X(compRmsPeak);               \
    X(compAttack); X(compRelease); X(compThreshold); X(compRatio); X(compKnee); X(compMakeup);           \
    X(spatialEnabled); X(spatialMode); X(reverbSize); X(reverbWidth); X(reverbWet); X(reverbDry);         \
    X(reverbDamp); X(widenEnabled); X(widenDelay); X(widenFeedback); X(widenCrossfeed); X(widenDry);      \
    X(normalize); X(hue); X(brightness); X(contrast); X(saturation); X(gamma); X(sharpen);               \
    X(debandRadius); X(grain); X(extractEnabled); X(extractColor); X(thresholdEnabled);                  \
    X(thresholdColor); X(thresholdSaturation); X(thresholdSimilarity); X(negate); X(posterize);          \
    X(gradient); X(gradientMode); X(gradientColor); X(gradientCartoon); X(sepia); X(cropTop);            \
    X(cropBottom); X(cropLeft); X(cropRight); X(cropLinkVertical); X(cropLinkHorizontal); X(subFont);    \
    X(subColor); X(subOutline); X(subSize); X(subSizePixels); X(subOutlineThickness);                   \
    X(subBackgroundOpacity); X(subMargin); X(subEmbeddedStyle)

void Effects::load()
{
    QSettings s;
    s.beginGroup("effects");
    FIELDS(LOAD);
    if (!s.contains("subBackgroundOpacity") && s.value("subBackground").toBool())
        m_data.subBackgroundOpacity = 160; // was an on/off switch
    const QVariantList bands = s.value("eqBands").toList();
    if (bands.size() == m_data.eqBands.size())
        for (int i = 0; i < bands.size(); ++i)
            m_data.eqBands[i] = bands[i].toFloat();
    s.endGroup();
}

void Effects::save() const
{
    QSettings s;
    s.beginGroup("effects");
    FIELDS(SAVE);
    QVariantList bands;
    for (float b : m_data.eqBands)
        bands << b;
    s.setValue("eqBands", bands);
    s.endGroup();
}
