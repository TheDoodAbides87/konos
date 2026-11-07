#pragma once

#include <QProxyStyle>

// Fusion-based style that draws menus like the redesign's right-click menu.
class AppStyle : public QProxyStyle {
public:
    AppStyle();

    void polish(QWidget *widget) override;
    using QProxyStyle::polish;

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size,
                           const QWidget *widget) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override;
    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                  QStyleHintReturn *returnData) const override;
    QIcon standardIcon(StandardPixmap icon, const QStyleOption *option, const QWidget *widget) const override;
};
