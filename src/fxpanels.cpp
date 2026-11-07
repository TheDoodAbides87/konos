#include "fxpanels.h"
#include "effects.h"
#include "theme.h"
#include "widgets.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <vlc/vlc.h>

namespace {

// Keeps a panel's controls in sync with the Effects state.
class Binder : public QObject {
public:
    explicit Binder(QWidget *owner) : QObject(owner)
    {
        connect(&fx(), &Effects::changed, this, [this] {
            if (m_updating)
                return;
            for (auto &fn : m_refresh)
                fn();
        });
    }
    void add(std::function<void()> fn) { m_refresh.push_back(std::move(fn)); }

    // Edit the state without re-running our own refreshers.
    void edit(Effects::Group group, const std::function<void(EffectsData &)> &fn)
    {
        m_updating = true;
        fn(fx().d());
        fx().commit(group);
        m_updating = false;
    }

    ValueSlider *slider(const QString &label, double min, double max, float EffectsData::*field,
                        Effects::Group group, std::function<QString(double)> fmt, int ticks = 0,
                        std::function<void(EffectsData &)> extra = {})
    {
        auto *s = new ValueSlider(label, min, max, fx().data().*field, std::move(fmt));
        s->setTicks(ticks);
        connect(s, &ValueSlider::valueChanged, this, [this, field, group, extra](double v) {
            edit(group, [&](EffectsData &d) {
                d.*field = float(v);
                if (extra)
                    extra(d);
            });
        });
        add([s, field] { s->setValue(fx().data().*field); });
        return s;
    }

    ValueSlider *intSlider(const QString &label, int min, int max, int EffectsData::*field, Effects::Group group,
                           std::function<QString(double)> fmt, int ticks = 0)
    {
        auto *s = new ValueSlider(label, min, max, fx().data().*field, std::move(fmt));
        s->slider()->setStep(1);
        s->setTicks(ticks);
        connect(s, &ValueSlider::valueChanged, this,
                [this, field, group](double v) { edit(group, [&](EffectsData &d) { d.*field = int(qRound(v)); }); });
        add([s, field] { s->setValue(fx().data().*field); });
        return s;
    }

    ToggleSwitch *toggle(bool EffectsData::*field, Effects::Group group)
    {
        auto *t = new ToggleSwitch;
        t->setChecked(fx().data().*field);
        connect(t, &ToggleSwitch::toggled, this,
                [this, field, group](bool on) { edit(group, [&](EffectsData &d) { d.*field = on; }); });
        add([t, field] {
            const QSignalBlocker block(t);
            t->setChecked(fx().data().*field);
            t->update();
        });
        return t;
    }

    ColorField *color(QColor EffectsData::*field, Effects::Group group)
    {
        auto *c = new ColorField(fx().data().*field);
        connect(c, &ColorField::colorChanged, this,
                [this, field, group](const QColor &col) { edit(group, [&](EffectsData &d) { d.*field = col; }); });
        add([c, field] { c->setColor(fx().data().*field); });
        return c;
    }

private:
    std::vector<std::function<void()>> m_refresh;
    bool m_updating = false;
};

QString bandName(int i)
{
    if (i < 0)
        return QObject::tr("Preamp");
    // We run the equalizer on VLC's band set (equalizer-vlcfreqs), which is what
    // the design shows; libVLC's API reports the ISO set, so label explicitly.
    static const float vlcBands[] = {60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000};
    const float f = vlcBands[qBound(0, i, 9)];
    return f >= 1000 ? QString("%1 KHz").arg(f / 1000.0, 0, 'g', 3) : QString("%1 Hz").arg(int(f));
}

QString dB(double v) { return QString("%1 dB").arg(v, 0, 'f', 1); }
QString one(double v) { return QString::number(v, 'f', 1); }

QLabel *valueBox(QWidget *parent)
{
    auto *l = new QLabel(parent);
    l->setAlignment(Qt::AlignCenter);
    l->setMinimumWidth(80);
    l->setFixedHeight(38);
    auto style = [l] {
        l->setStyleSheet(QString("background: %1; border-radius: 6px; color: %2; font-size: 13px; padding: 0 10px;")
                             .arg(theme().pal().input.name(), theme().pal().text.name()));
    };
    style();
    QObject::connect(&theme(), &Theme::changed, l, style);
    return l;
}

QVBoxLayout *panelBody(CollapsiblePanel *panel) { return panel->body(); }

} // namespace

namespace FxPanels {

QWidget *equalizerVertical(QWidget *parent)
{
    auto *panel = new CollapsiblePanel(parent);
    panel->setTexts(QObject::tr("Close Equalizer"), QObject::tr("Open Equalizer"));
    panel->setHeaderCentered(true);
    auto *binder = new Binder(panel);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(14);
    const int bands = int(libvlc_audio_equalizer_get_band_count());
    for (int i = -1; i < bands; ++i) {
        const int col = i + 1;
        auto *slider = new SliderBar(Qt::Vertical);
        slider->setThickness(28);
        slider->setHandleOvershoot(8);
        slider->setRadius(8);
        slider->setRange(-20, 20);
        slider->setStep(0.5);
        slider->setMinimumHeight(260);
        auto *label = new QLabel(bandName(i));
        label->setProperty("role", "muted");
        label->setStyleSheet("font-size: 13px;");
        label->setAlignment(Qt::AlignCenter);
        auto *value = valueBox(panel);
        value->setMinimumWidth(0);
        value->setFixedWidth(66);
        value->setStyleSheet(value->styleSheet() + "font-size: 12px; padding: 0;");
        grid->addWidget(slider, 0, col, Qt::AlignHCenter);
        grid->addWidget(label, 1, col, Qt::AlignHCenter);
        grid->addWidget(value, 2, col, Qt::AlignHCenter);

        auto read = [i] { return i < 0 ? fx().data().eqPreamp : fx().data().eqBands.value(i); };
        slider->setValue(read());
        value->setText(dB(read()));
        QObject::connect(slider, &SliderBar::valueChanged, binder, [binder, value, i](double v) {
            value->setText(dB(v));
            binder->edit(Effects::Equalizer, [&](EffectsData &d) {
                if (i < 0)
                    d.eqPreamp = float(v);
                else
                    d.eqBands[i] = float(v);
                d.eqPreset = -1;
                d.eqEnabled = true;
            });
        });
        binder->add([slider, value, read] {
            slider->setValue(read());
            value->setText(dB(read()));
        });
    }
    panelBody(panel)->addLayout(grid);
    return panel;
}

QWidget *equalizerHorizontal(QWidget *parent)
{
    auto *panel = new CollapsiblePanel(parent);
    panel->setTexts(QObject::tr("Collapse Settings"), QObject::tr("Equalizer Bands"));
    auto *binder = new Binder(panel);
    const int bands = int(libvlc_audio_equalizer_get_band_count());
    for (int i = -1; i < bands; ++i) {
        auto *row = new QWidget;
        auto *v = new QVBoxLayout(row);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(10);
        auto *top = new QHBoxLayout;
        auto *label = new QLabel(bandName(i));
        label->setProperty("role", "dim");
        label->setStyleSheet("font-size: 15px;");
        auto *value = valueBox(row);
        value->setMinimumWidth(130);
        value->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        top->addWidget(label);
        top->addStretch();
        top->addWidget(value);
        auto *slider = new SliderBar;
        slider->setRange(-20, 20);
        slider->setStep(0.5);
        slider->setThickness(16);
        slider->setHandleOvershoot(3);
        v->addLayout(top);
        v->addWidget(slider);
        panelBody(panel)->addWidget(row);

        auto read = [i] { return i < 0 ? fx().data().eqPreamp : fx().data().eqBands.value(i); };
        slider->setValue(read());
        value->setText(dB(read()));
        QObject::connect(slider, &SliderBar::valueChanged, binder, [binder, value, i](double v) {
            value->setText(dB(v));
            binder->edit(Effects::Equalizer, [&](EffectsData &d) {
                if (i < 0)
                    d.eqPreamp = float(v);
                else
                    d.eqBands[i] = float(v);
                d.eqPreset = -1;
                d.eqEnabled = true;
            });
        });
        binder->add([slider, value, read] {
            slider->setValue(read());
            value->setText(dB(read()));
        });
    }
    panel->setExpanded(false);
    return panel;
}

QWidget *compressor(QWidget *parent)
{
    auto *panel = new CollapsiblePanel(parent);
    panel->setTexts(QObject::tr("Collapse Settings"), QObject::tr("Compressor Settings"));
    auto *b = new Binder(panel);
    auto *body = panelBody(panel);
    const auto g = Effects::AudioFilters;
    auto enable = [](EffectsData &d) { d.compEnabled = true; };
    body->addWidget(b->slider(QObject::tr("RMS / Peak"), 0, 1, &EffectsData::compRmsPeak, g, one, 0, enable));
    body->addWidget(b->slider(QObject::tr("Attack"), 1.5, 400, &EffectsData::compAttack, g,
                              [](double v) { return QString("%1 ms").arg(v, 0, 'f', 1); }, 0, enable));
    body->addWidget(b->slider(QObject::tr("Release"), 2, 800, &EffectsData::compRelease, g,
                              [](double v) { return QString("%1 ms").arg(v, 0, 'f', 1); }, 0, enable));
    body->addWidget(b->slider(QObject::tr("Threshold"), -30, 0, &EffectsData::compThreshold, g, dB, 0, enable));
    body->addWidget(b->slider(QObject::tr("Ratio"), 1, 20, &EffectsData::compRatio, g,
                              [](double v) { return QString("%1 : 1").arg(v, 0, 'f', 1); }, 0, enable));
    body->addWidget(b->slider(QObject::tr("Knee Radius"), 1, 10, &EffectsData::compKnee, g, dB, 0, enable));
    body->addWidget(b->slider(QObject::tr("Makeup Gain"), 0, 24, &EffectsData::compMakeup, g, dB, 0, enable));
    panel->setExpanded(false);
    return panel;
}

QWidget *reverb(QWidget *parent)
{
    auto *panel = new CollapsiblePanel(parent);
    panel->setTexts(QObject::tr("Collapse Settings"), QObject::tr("Room Settings"));
    auto *b = new Binder(panel);
    auto *body = panelBody(panel);
    const auto g = Effects::AudioFilters;
    body->addWidget(b->slider(QObject::tr("Size"), 0, 10, &EffectsData::reverbSize, g, one, 10));
    body->addWidget(b->slider(QObject::tr("Width"), 0, 10, &EffectsData::reverbWidth, g, one, 10));
    body->addWidget(b->slider(QObject::tr("Wet"), 0, 10, &EffectsData::reverbWet, g, one, 10));
    body->addWidget(b->slider(QObject::tr("Dry"), 0, 10, &EffectsData::reverbDry, g, one, 10));
    body->addWidget(b->slider(QObject::tr("Damp"), 0, 10, &EffectsData::reverbDamp, g, one, 10));
    panel->setExpanded(false);
    return panel;
}

QWidget *stereoWiden(QWidget *parent)
{
    auto *panel = new CollapsiblePanel(parent);
    panel->setTexts(QObject::tr("Collapse Settings"), QObject::tr("Widening Settings"));
    auto *b = new Binder(panel);
    auto *body = panelBody(panel);
    const auto g = Effects::AudioFilters;
    auto pct = [](double v) { return QString("%1 %").arg(v, 0, 'f', 1); };
    body->addWidget(b->slider(QObject::tr("Delay Time"), 1, 100, &EffectsData::widenDelay, g,
                              [](double v) { return QString("%1 ms").arg(v, 0, 'f', 1); }));
    body->addWidget(b->slider(QObject::tr("Feedback Gain"), 0, 0.9, &EffectsData::widenFeedback, g, pct, 9));
    body->addWidget(b->slider(QObject::tr("Crossfeed"), 0, 0.8, &EffectsData::widenCrossfeed, g, pct, 8));
    body->addWidget(b->slider(QObject::tr("Dry Mix"), 0, 1, &EffectsData::widenDry, g, pct, 10));
    panel->setExpanded(false);
    return panel;
}

QWidget *imageAdjust(QWidget *parent)
{
    auto *panel = new CollapsiblePanel(parent);
    panel->setTexts(QObject::tr("Collapse Settings"), QObject::tr("Image Adjustments"));
    auto *b = new Binder(panel);
    auto *body = panelBody(panel);
    const auto a = Effects::VideoAdjust;
    body->addWidget(b->slider(QObject::tr("Hue"), -180, 180, &EffectsData::hue, a,
                              [](double v) { return QString("%1°").arg(int(v)); }));
    body->addWidget(b->slider(QObject::tr("Brightness"), 0, 2, &EffectsData::brightness, a,
                              [](double v) { return QString::number(v, 'f', 2); }));
    body->addWidget(b->slider(QObject::tr("Contrast"), 0, 2, &EffectsData::contrast, a,
                              [](double v) { return QString::number(v, 'f', 2); }));
    auto *sat = b->slider(QObject::tr("Saturation"), 0, 3, &EffectsData::saturation, a,
                          [](double v) { return QString::number(v, 'f', 2); }, 2);
    body->addWidget(sat);
    body->addWidget(b->slider(QObject::tr("Gamma"), 0.01, 10, &EffectsData::gamma, a,
                              [](double v) { return QString::number(v, 'f', 2); }, 10));
    body->addWidget(b->slider(QObject::tr("Sharpen Sigma"), 0, 2, &EffectsData::sharpen, a,
                              [](double v) { return v <= 0 ? QObject::tr("Off") : QString::number(v, 'f', 2); }, 4));
    body->addWidget(b->intSlider(QObject::tr("Banding Removal Radius"), 0, 30, &EffectsData::debandRadius, a,
                                 [](double v) { return v < 4 ? QObject::tr("Off") : QString::number(int(v)); }, 6));
    body->addWidget(b->slider(QObject::tr("Film Grain Variance"), 0, 10, &EffectsData::grain, a,
                              [](double v) { return v <= 0 ? QObject::tr("Off") : QString::number(v, 'f', 1); }, 10));
    panel->setExpanded(false);
    return panel;
}

namespace {
// "Top   0 px" box from the crop design.
QWidget *pxBox(const QString &label, int EffectsData::*field, int EffectsData::*partner, bool EffectsData::*link,
               Binder *b)
{
    auto *box = new QWidget;
    box->setFixedHeight(46);
    box->setAttribute(Qt::WA_StyledBackground);
    auto style = [box] {
        box->setStyleSheet(QString("QWidget#pxbox { background: %1; border-radius: 8px; } "
                                   "QSpinBox { background: transparent; border: none; font-size: 16px; color: %2; }")
                               .arg(theme().pal().input.name(), theme().pal().text.name()));
    };
    box->setObjectName("pxbox");
    style();
    QObject::connect(&theme(), &Theme::changed, box, style);
    auto *h = new QHBoxLayout(box);
    h->setContentsMargins(16, 0, 16, 0);
    auto *name = new QLabel(label, box);
    name->setProperty("role", "dim");
    name->setStyleSheet("font-size: 16px;");
    name->setMinimumWidth(56);
    auto *spin = new QSpinBox(box);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setRange(0, 4000);
    spin->setValue(fx().data().*field);
    spin->setFixedWidth(60);
    auto *unit = new QLabel("px", box);
    unit->setProperty("role", "muted");
    unit->setStyleSheet("font-size: 16px;");
    h->addWidget(name);
    h->addWidget(spin);
    h->addWidget(unit);
    h->addStretch();
    QObject::connect(spin, &QSpinBox::valueChanged, b, [b, field, partner, link](int v) {
        b->edit(Effects::Crop, [&](EffectsData &d) {
            d.*field = v;
            if (d.*link)
                d.*partner = v;
        });
        // The partner box needs a refresh when linked.
        emit fx().changed(Effects::Crop);
    });
    b->add([spin, field] {
        const QSignalBlocker block(spin);
        spin->setValue(fx().data().*field);
    });
    return box;
}
} // namespace

QWidget *crop(QWidget *parent)
{
    auto *panel = new CollapsiblePanel(parent);
    panel->setTexts(QObject::tr("Collapse Settings"), QObject::tr("Crop"));
    auto *b = new Binder(panel);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);
    grid->addWidget(pxBox(QObject::tr("Top"), &EffectsData::cropTop, &EffectsData::cropBottom,
                          &EffectsData::cropLinkVertical, b), 0, 0);
    grid->addWidget(pxBox(QObject::tr("Bottom"), &EffectsData::cropBottom, &EffectsData::cropTop,
                          &EffectsData::cropLinkVertical, b), 0, 1);
    grid->addWidget(pxBox(QObject::tr("Left"), &EffectsData::cropLeft, &EffectsData::cropRight,
                          &EffectsData::cropLinkHorizontal, b), 1, 0);
    grid->addWidget(pxBox(QObject::tr("Right"), &EffectsData::cropRight, &EffectsData::cropLeft,
                          &EffectsData::cropLinkHorizontal, b), 1, 1);
    auto linkButton = [b](bool EffectsData::*field) {
        auto *btn = new IconButton(fx().data().*field ? "link-on" : "link");
        btn->setIconPixelSize(24);
        btn->setToolTip(QObject::tr("Link opposite sides"));
        btn->setActive(fx().data().*field);
        QObject::connect(btn, &IconButton::clicked, b, [b, field] {
            b->edit(Effects::Crop, [&](EffectsData &d) { d.*field = !(d.*field); });
            emit fx().changed(Effects::Crop);
        });
        b->add([btn, field] {
            btn->setIconName(fx().data().*field ? "link-on" : "link");
            btn->setActive(fx().data().*field);
        });
        return btn;
    };
    grid->addWidget(linkButton(&EffectsData::cropLinkVertical), 0, 2);
    grid->addWidget(linkButton(&EffectsData::cropLinkHorizontal), 1, 2);
    panelBody(panel)->addLayout(grid);
    panel->setExpanded(false);
    return panel;
}

QWidget *colorEffects(QWidget *parent)
{
    auto *panel = new CollapsiblePanel(parent);
    panel->setTexts(QObject::tr("Collapse Settings"), QObject::tr("Color Effects"));
    auto *b = new Binder(panel);
    auto *body = panelBody(panel);
    const auto g = Effects::VideoFilters;

    auto colorRow = [&](const QString &label, QColor EffectsData::*color, bool EffectsData::*enabled) {
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);
        auto *l = new QLabel(label);
        l->setProperty("role", "dim");
        l->setStyleSheet("font-size: 15px;");
        h->addWidget(l);
        h->addStretch();
        auto *field = b->color(color, g);
        field->setFixedWidth(160);
        h->addWidget(field);
        h->addWidget(b->toggle(enabled, g));
        return row;
    };
    auto toggleRow = [&](const QString &label, bool EffectsData::*field) {
        return settingRow(label, b->toggle(field, g));
    };

    body->addWidget(colorRow(QObject::tr("Color Extraction"), &EffectsData::extractColor, &EffectsData::extractEnabled));
    body->addWidget(hline());
    body->addWidget(colorRow(QObject::tr("Color Threshold"), &EffectsData::thresholdColor, &EffectsData::thresholdEnabled));
    auto *satS = b->intSlider(QObject::tr("Saturation"), 0, 255, &EffectsData::thresholdSaturation, g,
                              [](double v) { return QString::number(int(v)); });
    auto *simS = b->intSlider(QObject::tr("Similarity"), 0, 255, &EffectsData::thresholdSimilarity, g,
                              [](double v) { return QString::number(int(v)); });
    body->addWidget(satS);
    body->addWidget(simS);
    auto neutral = [satS, simS] {
        satS->slider()->setNeutral(!fx().data().thresholdEnabled);
        simS->slider()->setNeutral(!fx().data().thresholdEnabled);
    };
    neutral();
    b->add(neutral);
    body->addWidget(hline());
    body->addWidget(toggleRow(QObject::tr("Negate Colors"), &EffectsData::negate));
    body->addWidget(hline());
    body->addWidget(toggleRow(QObject::tr("Posterize"), &EffectsData::posterize));
    body->addWidget(hline());
    body->addWidget(toggleRow(QObject::tr("Gradient"), &EffectsData::gradient));
    auto *mode = new Dropdown;
    mode->addItems({QObject::tr("Gradient"), QObject::tr("Edge"), QObject::tr("Hough")});
    mode->setCurrentIndex(fx().data().gradientMode);
    QObject::connect(mode, &QComboBox::currentIndexChanged, b,
                     [b](int i) { b->edit(Effects::VideoFilters, [&](EffectsData &d) { d.gradientMode = i; }); });
    b->add([mode] {
        const QSignalBlocker block(mode);
        mode->setCurrentIndex(fx().data().gradientMode);
    });
    body->addWidget(mode);
    body->addWidget(toggleRow(QObject::tr("Color"), &EffectsData::gradientColor));
    body->addWidget(toggleRow(QObject::tr("Cartoon"), &EffectsData::gradientCartoon));
    body->addWidget(hline());
    body->addWidget(b->intSlider(QObject::tr("Sepia Intensity"), 0, 255, &EffectsData::sepia, g,
                                 [](double v) { return v < 1 ? QObject::tr("Off") : QString::number(int(v)); }, 2));
    panel->setExpanded(false);
    return panel;
}

} // namespace FxPanels
