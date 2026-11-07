#pragma once

#include <QWidget>

// Effect editors bound to the shared Effects state. Each panel refreshes itself
// when the state is changed elsewhere (e.g. quick settings vs. settings window).
namespace FxPanels {

// Settings-window equalizer: vertical sliders with dB boxes underneath.
QWidget *equalizerVertical(QWidget *parent);
// Quick-settings equalizer: one horizontal slider per band.
QWidget *equalizerHorizontal(QWidget *parent);
QWidget *compressor(QWidget *parent);
QWidget *reverb(QWidget *parent);
QWidget *stereoWiden(QWidget *parent);
QWidget *imageAdjust(QWidget *parent);
QWidget *crop(QWidget *parent);
QWidget *colorEffects(QWidget *parent);

} // namespace FxPanels
