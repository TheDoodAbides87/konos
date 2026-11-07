#pragma once

#include <QColor>
#include <QObject>
#include <QVector>

// Every audio/video effect parameter the UI exposes. Shared between the quick
// settings popup and the settings window; persisted with QSettings.
struct EffectsData {
    // Equalizer (VLC's 10-band equalizer filter)
    bool eqEnabled = false;
    int eqPreset = -1; // -1 = custom / none
    float eqPreamp = 12.0f;
    QVector<float> eqBands = QVector<float>(10, 0.0f);
    bool eqTwoPass = false;

    // Dynamic range compressor
    bool compEnabled = false;
    float compRmsPeak = 0.2f;    // 0..1
    float compAttack = 25.0f;    // 1.5..400 ms
    float compRelease = 100.0f;  // 2..800 ms
    float compThreshold = -11.f; // -30..0 dB
    float compRatio = 4.0f;      // 1..20
    float compKnee = 5.0f;       // 1..10 dB
    float compMakeup = 7.0f;     // 0..24 dB

    // Spatial audio: 0 = headphone virtualization, 1 = room reverb (spatializer)
    bool spatialEnabled = false;
    int spatialMode = 0;
    float reverbSize = 8.0f;  // 0..10 (maps to 0..1.1)
    float reverbWidth = 10.f; // 0..10
    float reverbWet = 4.0f;   // 0..10
    float reverbDry = 5.0f;   // 0..10
    float reverbDamp = 5.0f;  // 0..10

    // Stereo widening
    bool widenEnabled = false;
    float widenDelay = 20.0f;   // 1..100 ms
    float widenFeedback = 0.3f; // 0..0.9
    float widenCrossfeed = 0.3f;// 0..0.8
    float widenDry = 0.8f;      // 0..1

    bool normalize = false;

    // Image adjust
    float hue = 0.0f;        // -180..180
    float brightness = 1.0f; // 0..2
    float contrast = 1.0f;   // 0..2
    float saturation = 1.0f; // 0..3
    float gamma = 1.0f;      // 0.01..10
    float sharpen = 0.0f;    // 0..2, 0 = off
    int debandRadius = 0;    // 0 = off, 4..32
    float grain = 0.0f;      // 0..10, 0 = off

    // Colour effects
    bool extractEnabled = false;
    QColor extractColor = QColor(0xff, 0, 0);
    bool thresholdEnabled = false;
    QColor thresholdColor = QColor(0xff, 0, 0);
    int thresholdSaturation = 20; // 0..255
    int thresholdSimilarity = 15; // 0..255
    bool negate = false;
    bool posterize = false;
    bool gradient = false;
    int gradientMode = 0; // 0 gradient, 1 edge, 2 hough
    bool gradientColor = false;
    bool gradientCartoon = false;
    int sepia = 0; // 0..255, 0 = off

    // Crop in source pixels
    int cropTop = 0, cropBottom = 0, cropLeft = 0, cropRight = 0;
    bool cropLinkVertical = false, cropLinkHorizontal = false;

    // Subtitle appearance (applied to the text renderer)
    QString subFont = "Inter";
    QColor subColor = QColor(0xf2, 0xf2, 0xf2);
    QColor subOutline = QColor(0x0c, 0x0c, 0x0c);
    int subSize = 16;              // relative: freetype-rel-fontsize, 20 smaller .. 6 larger
    int subSizePixels = 0;         // exact size in pixels; 0 = use the relative size
    int subOutlineThickness = 4;   // 0 none, 2 thin, 4 normal, 6 thick
    int subBackgroundOpacity = 0;  // 0..255 behind the text
    int subMargin = 0;             // pixels up from the bottom of the picture
    bool subEmbeddedStyle = true;  // use the fonts/colours/positions the subtitles bring

    bool adjustActive() const;
    bool videoAdjustDefault() const;
};

class Effects : public QObject {
    Q_OBJECT
public:
    enum Group { Equalizer, AudioFilters, VideoAdjust, VideoFilters, Crop, SubtitleStyle };
    Q_ENUM(Group)

    static Effects &instance();

    EffectsData &d() { return m_data; }
    const EffectsData &data() const { return m_data; }

    // Call after editing d() so the player picks up the change. live = false:
    // just save and refresh the UI (the next item opened picks it up).
    void commit(Group group, bool live = true);
    void resetGroup(Group group);
    void load();
    void save() const;

    static QStringList eqPresetNames();
    static QVector<float> eqPresetBands(int index); // gain per band in dB
    void applyEqPreset(int index);

signals:
    void changed(Effects::Group group, bool live = true);

private:
    Effects() = default;
    EffectsData m_data;
};

inline Effects &fx() { return Effects::instance(); }
