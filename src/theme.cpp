#include "theme.h"
#include "prefs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QHash>
#include <QRegularExpression>
#include <QPainter>
#include <QPalette>
#include <QPainterPath>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QTimer>

#ifdef VMP_OMARCHY
static constexpr const char *kModePref = "omarchy/theme";
static constexpr const char *kAccentPref = "omarchy/accent";
#else
static constexpr const char *kModePref = "ui/theme";
static constexpr const char *kAccentPref = "ui/accent";
#endif

QColor mixColor(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

// 24x24 line icons. "currentColor" is replaced with the requested colour.
static const QHash<QString, QString> &iconSources()
{
    static const QHash<QString, QString> icons = {
        {"home", R"(<path d="M3.5 10.2 12 3.5l8.5 6.7V19a1.5 1.5 0 0 1-1.5 1.5h-4.2v-6h-5.6v6H5A1.5 1.5 0 0 1 3.5 19z"/>)"},
        {"videos", R"(<rect x="3.5" y="3.5" width="17" height="17" rx="3"/><path d="M10 8.5v7l5.5-3.5z" fill="currentColor"/>)"},
        {"music", R"(<rect x="3.5" y="3.5" width="17" height="17" rx="3"/><circle cx="10" cy="15.5" r="2"/><path d="M12 15.5V8l3.5 1.2"/>)"},
        {"network", R"(<path d="M4.9 19.1a10 10 0 0 1 0-14.2"/><path d="M7.8 16.2a6 6 0 0 1 0-8.4"/><circle cx="12" cy="12" r="1.6" fill="currentColor"/><path d="M16.2 7.8a6 6 0 0 1 0 8.4"/><path d="M19.1 4.9a10 10 0 0 1 0 14.2"/>)"},
        {"playlist", R"(<path d="M3.5 6h15M3.5 11h11M3.5 16h8"/><path d="M15.5 13.5v6l5-3z" fill="currentColor"/>)"},
        {"recent", R"(<path d="M3.5 12a8.5 8.5 0 1 0 2.6-6.1L3.5 8.5"/><path d="M3.5 3.8v4.7h4.7"/><path d="M12 7.5V12l3 2"/>)"},
        {"plus", R"(<path d="M12 5v14M5 12h14"/>)"},
        {"minus", R"(<path d="M5 12h14"/>)"},
        {"pip", R"(<rect x="3" y="5" width="18" height="14" rx="2"/><rect x="12" y="11.5" width="6.5" height="5" rx="1" fill="currentColor"/>)"},
        {"bookmark", R"(<path d="M6.5 3.5h11a1 1 0 0 1 1 1v16l-6.5-4.2-6.5 4.2v-16a1 1 0 0 1 1-1z"/><path d="M9.5 3.5v8l2.5-1.6 2.5 1.6v-8"/>)"},
        {"edit", R"(<path d="M15.5 4.5l4 4L9 19H5v-4z"/><path d="M13 7l4 4"/>)"},
        {"more", R"(<circle cx="5.5" cy="12" r="1.3" fill="currentColor" stroke="none"/><circle cx="12" cy="12" r="1.3" fill="currentColor" stroke="none"/><circle cx="18.5" cy="12" r="1.3" fill="currentColor" stroke="none"/>)"},
        {"sidebar", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2.5"/><rect x="5.5" y="6.5" width="4" height="11" rx="1" fill="currentColor" stroke="none"/>)"},
        {"folder", R"(<path d="M3.5 7A2 2 0 0 1 5.5 5h3.7l2 2h7.3a2 2 0 0 1 2 2v8.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/>)"},
        {"folder-up", R"(<path d="M3.5 7A2 2 0 0 1 5.5 5h3.7l2 2h7.3a2 2 0 0 1 2 2v8.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/><path d="M12 16.5v-5.5M9.5 13.3 12 10.8l2.5 2.5"/>)"},
        {"folder-plus", R"(<path d="M3.5 7A2 2 0 0 1 5.5 5h3.7l2 2h7.3a2 2 0 0 1 2 2v8.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/><path d="M12 10.5v6M9 13.5h6"/>)"},
        {"folder-fill", R"(<path d="M3.5 7A2 2 0 0 1 5.5 5h3.7l2 2h7.3a2 2 0 0 1 2 2v8.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z" fill="currentColor"/>)"},
        {"minimize", R"(<path d="M6 12h12"/>)"},
        {"maximize", R"(<rect x="6" y="6" width="12" height="12" rx="1.5"/>)"},
        {"restore", R"(<rect x="5.5" y="8.5" width="10" height="10" rx="1.5"/><path d="M8.5 5.5h8.5a1.5 1.5 0 0 1 1.5 1.5v8.5"/>)"},
        {"fullscreen", R"(<path d="M4.5 9V4.5H9M15 4.5h4.5V9M19.5 15v4.5H15M9 19.5H4.5V15"/>)"},
        {"fullscreen-exit", R"(<path d="M9 4.5V9H4.5M19.5 9H15V4.5M15 19.5V15h4.5M4.5 15H9v4.5"/>)"},
        {"close", R"(<path d="M6 6l12 12M18 6 6 18"/>)"},
        {"arrow-left", R"(<path d="M19 12H5M11 6l-6 6 6 6"/>)"},
        {"arrow-right", R"(<path d="M5 12h14M13 6l6 6-6 6"/>)"},
        {"sort", R"(<path d="M7.5 4.5v15M4 16l3.5 3.5L11 16"/><path d="M16.5 19.5v-15M13 8l3.5-3.5L20 8"/>)"},
        {"grid", R"(<rect x="4" y="4" width="6.5" height="6.5" rx="1.5"/><rect x="13.5" y="4" width="6.5" height="6.5" rx="1.5"/><rect x="4" y="13.5" width="6.5" height="6.5" rx="1.5"/><rect x="13.5" y="13.5" width="6.5" height="6.5" rx="1.5"/>)"},
        {"details", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M3.5 9.5h17M3.5 14.5h17M10 9.5v10"/>)"},
        {"radio", R"(<rect x="3.5" y="8.5" width="17" height="11" rx="2"/><path d="m7 8.5 9-5"/><circle cx="15.5" cy="14" r="2.4"/><path d="M6.5 12.5h3.5M6.5 15.5h3.5"/>)"},
        {"podcast", R"(<circle cx="12" cy="10" r="2.2"/><path d="M12 13.5v6.5"/><path d="M7.4 14.2a6 6 0 1 1 9.2 0"/><path d="M5.1 17a9 9 0 1 1 13.8 0"/>)"},
        {"pin", R"(<path d="M9 3.5h6l-1 5.5 3 3H7l3-3z"/><path d="M12 12v8.5"/>)"},
        {"listview", R"(<rect x="3.5" y="4.5" width="5" height="5" rx="1"/><path d="M11.5 7h9"/><rect x="3.5" y="14.5" width="5" height="5" rx="1"/><path d="M11.5 17h9"/>)"},
        {"search", R"(<circle cx="11" cy="11" r="6.5"/><path d="m20 20-4.2-4.2"/>)"},
        {"external", R"(<path d="M14 4.5h5.5V10M19.5 4.5 11 13"/><path d="M18 14v4.5a1.5 1.5 0 0 1-1.5 1.5h-11A1.5 1.5 0 0 1 4 18.5v-11A1.5 1.5 0 0 1 5.5 6H10"/>)"},
        {"play-fill", R"(<path d="M7.5 5.2v13.6a.8.8 0 0 0 1.2.7l11-6.8a.8.8 0 0 0 0-1.4l-11-6.8a.8.8 0 0 0-1.2.7z" fill="currentColor" stroke="none"/>)"},
        {"pause-fill", R"(<rect x="6" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none"/><rect x="14" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none"/>)"},
        {"prev-fill", R"(<path d="M18 6v12a.7.7 0 0 1-1.1.6L8.5 12.6a.7.7 0 0 1 0-1.2l8.4-6A.7.7 0 0 1 18 6z" fill="currentColor" stroke="none"/><rect x="5.5" y="5.5" width="2.4" height="13" rx="1" fill="currentColor" stroke="none"/>)"},
        {"next-fill", R"(<path d="M6 6v12a.7.7 0 0 0 1.1.6l8.4-6a.7.7 0 0 0 0-1.2l-8.4-6A.7.7 0 0 0 6 6z" fill="currentColor" stroke="none"/><rect x="16.1" y="5.5" width="2.4" height="13" rx="1" fill="currentColor" stroke="none"/>)"},
        {"play", R"(<path d="M7.5 5.2v13.6a.8.8 0 0 0 1.2.7l11-6.8a.8.8 0 0 0 0-1.4l-11-6.8a.8.8 0 0 0-1.2.7z"/>)"},
        {"pause", R"(<rect x="6.5" y="5" width="3.5" height="14" rx="1"/><rect x="14" y="5" width="3.5" height="14" rx="1"/>)"},
        {"stop", R"(<rect x="6.5" y="6.5" width="11" height="11" rx="1"/>)"},
        {"prev", R"(<path d="M6.5 5.5v13"/><path d="M18 6v12l-8.5-6z"/>)"},
        {"next", R"(<path d="M17.5 5.5v13"/><path d="M6 6v12l8.5-6z"/>)"},
        {"record", R"(<circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="3.5" fill="currentColor"/>)"},
        {"hide-ui", R"(<rect x="3.5" y="3.5" width="17" height="17" rx="3"/><path d="m8.5 8.5 7 7"/>)"},
        {"playback", R"(<path d="M6.5 4.8v14.4a1 1 0 0 0 1.5.9l11.3-7.2a1 1 0 0 0 0-1.8L8 3.9a1 1 0 0 0-1.5.9z"/><path d="M10 9.5v5l4-2.5z" fill="currentColor"/>)"},
        {"video", R"(<rect x="3.5" y="5.5" width="17" height="14" rx="2"/><path d="M3.5 9.5h17"/><path d="m8 5.5-1.2 4M13 5.5l-1.2 4M18 5.5l-1.2 4"/>)"},
        {"audio", R"(<path d="M3 11v2M6.5 8.5v7M10 4.5v15M13.5 7.5v9M17 9.5v5M20.5 11v2"/>)"},
        {"subtitles", R"(<rect x="2.5" y="5" width="19" height="14" rx="3"/><path d="M6 12.5h4.5M13 12.5h5M6 15.5h7M15.5 15.5H18"/>)"},
        {"settings", R"(<path d="M12.22 2.5h-.44a1.9 1.9 0 0 0-1.9 1.9v.17a1.9 1.9 0 0 1-.95 1.64l-.41.24a1.9 1.9 0 0 1-1.9 0l-.14-.08a1.9 1.9 0 0 0-2.6.7l-.22.38a1.9 1.9 0 0 0 .7 2.6l.14.09a1.9 1.9 0 0 1 .95 1.63v.48a1.9 1.9 0 0 1-.95 1.66l-.14.08a1.9 1.9 0 0 0-.7 2.6l.22.36a1.9 1.9 0 0 0 2.6.7l.14-.08a1.9 1.9 0 0 1 1.9 0l.41.24a1.9 1.9 0 0 1 .95 1.64v.17a1.9 1.9 0 0 0 1.9 1.9h.44a1.9 1.9 0 0 0 1.9-1.9v-.17a1.9 1.9 0 0 1 .95-1.64l.41-.24a1.9 1.9 0 0 1 1.9 0l.14.08a1.9 1.9 0 0 0 2.6-.7l.22-.37a1.9 1.9 0 0 0-.7-2.6l-.14-.08a1.9 1.9 0 0 1-.95-1.65v-.47a1.9 1.9 0 0 1 .95-1.65l.14-.08a1.9 1.9 0 0 0 .7-2.6l-.22-.37a1.9 1.9 0 0 0-2.6-.7l-.14.08a1.9 1.9 0 0 1-1.9 0l-.41-.24a1.9 1.9 0 0 1-.95-1.64v-.17a1.9 1.9 0 0 0-1.9-1.9z"/><circle cx="12" cy="12" r="2.8"/>)"},
        {"play-next", R"(<path d="M11 6.5h9.5M11 12h9.5M3.5 17.5h17"/><path d="M3.8 4.4v6.2l4.8-3.1z" fill="currentColor"/>)"},
        {"play-later", R"(<path d="M3.5 6.5h17M3.5 12h9.5M3.5 17.5h9.5"/><path d="M16 11.4v6.2l4.8-3.1z" fill="currentColor"/>)"},
        {"queue", R"(<path d="M8.5 6.5h12M8.5 12h12M8.5 17.5h12"/><circle cx="4.5" cy="6.5" r=".9" fill="currentColor"/><circle cx="4.5" cy="12" r=".9" fill="currentColor"/><circle cx="4.5" cy="17.5" r=".9" fill="currentColor"/>)"},
        {"volume", R"(<path d="M10.5 5.5 6.5 9H3.5v6h3l4 3.5z"/><path d="M14.5 9.2a4 4 0 0 1 0 5.6"/><path d="M17.3 6.5a8 8 0 0 1 0 11"/>)"},
        {"volume-low", R"(<path d="M10.5 5.5 6.5 9H3.5v6h3l4 3.5z"/><path d="M14.5 9.2a4 4 0 0 1 0 5.6"/>)"},
        {"volume-mute", R"(<path d="M10.5 5.5 6.5 9H3.5v6h3l4 3.5z"/><path d="m15 9.5 5 5M20 9.5l-5 5"/>)"},
        {"chevron-right", R"(<path d="m9.5 6 6 6-6 6"/>)"},
        {"chevron-left", R"(<path d="m14.5 6-6 6 6 6"/>)"},
        {"chevron-down", R"(<path d="m6 9.5 6 6 6-6"/>)"},
        {"chevron-up", R"(<path d="m6 14.5 6-6 6 6"/>)"},
        {"caret-right", R"(<path d="M10 7.5v9l5-4.5z" fill="currentColor" stroke="none"/>)"},
        {"info", R"(<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5.5"/><circle cx="12" cy="7.8" r=".6" fill="currentColor"/>)"},
        {"tune", R"(<path d="M5.5 3.5v17M12 3.5v17M18.5 3.5v17"/><path d="M3.5 14.5h4M10 8.5h4M16.5 12.5h4"/>)"},
        {"pointer", R"(<path d="M5.5 3.5v15.2l4.2-4 2.9 6.2 2.6-1.2-2.9-6.1h5.9z"/>)"},
        {"shield", R"(<path d="M12 3.2 19.5 6v5.6c0 4.8-3.3 8-7.5 9.2-4.2-1.2-7.5-4.4-7.5-9.2V6z"/>)"},
        {"files", R"(<rect x="7" y="6.5" width="12.5" height="14" rx="2.5"/><path d="M4.5 16.5V6a2.5 2.5 0 0 1 2.5-2.5h8.5"/>)"},
        {"capture", R"(<rect x="3.5" y="4.5" width="17" height="12" rx="2"/><path d="M10.5 8.2v4.6l3.8-2.3z" fill="currentColor"/><path d="M8 20h8M12 16.5V20"/>)"},
        {"save-down", R"(<path d="M12 4v12M6.5 10.5L12 16l5.5-5.5"/><path d="M5 20h14"/>)"},
        {"convert", R"(<path d="M4 8h14.5M15 4.5L18.5 8 15 11.5"/><path d="M20 16H5.5M9 12.5L5.5 16 9 19.5"/>)"},
        {"upload", R"(<path d="M12 15.5V4.5M6.5 9.5L12 4l5.5 5.5"/><path d="M4.5 15v3a2.5 2.5 0 0 0 2.5 2.5h10a2.5 2.5 0 0 0 2.5-2.5v-3"/>)"},
        {"keyboard", R"(<rect x="2.5" y="5.5" width="19" height="13" rx="2.5"/><path d="M6.5 9.5h.01M9.5 9.5h.01M12.5 9.5h.01M15.5 9.5h.01M18 9.5h.01M6.5 12.5h.01M9.5 12.5h.01M12.5 12.5h.01M15.5 12.5h.01M18 12.5h.01M7.5 15.5h9"/>)"},
        {"puzzle", R"(<path d="M5 6.5a1.5 1.5 0 0 1 1.5-1.5H10a2 2 0 1 1 4 0h3.5A1.5 1.5 0 0 1 19 6.5V10a2 2 0 1 1 0 4v3.5a1.5 1.5 0 0 1-1.5 1.5H14a2 2 0 1 0-4 0H6.5A1.5 1.5 0 0 1 5 17.5V14a2 2 0 1 0 0-4z"/>)"},
        {"gauge", R"(<path d="M4.2 18.5a9 9 0 1 1 15.6 0"/><path d="m12 14 4-4.5"/><circle cx="12" cy="14" r="1.2" fill="currentColor"/>)"},
        {"open-media", R"(<rect x="4.5" y="3.5" width="15" height="17" rx="3"/><path d="M5 9V6.5A2.5 2.5 0 0 1 7.5 4H10z" fill="currentColor" stroke="none"/>)"},
        {"quit", R"(<path d="M9.5 20.5H6.5a2 2 0 0 1-2-2v-13a2 2 0 0 1 2-2h3"/><path d="m15 16 4-4-4-4M19 12H9.5"/>)"},
        {"link", R"(<path d="M9 7.5H7.5a4.5 4.5 0 0 0 0 9H9M15 7.5h1.5a4.5 4.5 0 0 1 0 9H15"/>)"},
        {"link-on", R"(<path d="M9 7.5H7.5a4.5 4.5 0 0 0 0 9H9M15 7.5h1.5a4.5 4.5 0 0 1 0 9H15M8.5 12h7"/>)"},
        {"check", R"(<path d="m5 12.5 4.5 4.5L19 7.5"/>)"},
        {"camera", R"(<path d="M4.5 8h3l2-2.5h5l2 2.5h3a1 1 0 0 1 1 1v9.5a1 1 0 0 1-1 1h-15a1 1 0 0 1-1-1V9a1 1 0 0 1 1-1z"/><circle cx="12" cy="13.5" r="3.3"/>)"},
        {"trash", R"(<path d="M4.5 7h15M10 11v5.5M14 11v5.5M6.5 7l.9 12.1a1.5 1.5 0 0 0 1.5 1.4h6.2a1.5 1.5 0 0 0 1.5-1.4L17.5 7M9.5 7V4.5h5V7"/>)"},
        {"globe", R"(<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17"/><path d="M12 3.5a13 13 0 0 1 0 17 13 13 0 0 1 0-17z"/>)"},
        {"repeat", R"(<path d="m17 3 3.5 3.5L17 10"/><path d="M4 11.5v-1a4 4 0 0 1 4-4h12.5"/><path d="m7 21-3.5-3.5L7 14"/><path d="M20 12.5v1a4 4 0 0 1-4 4H3.5"/>)"},
        {"repeat-one", R"(<path d="m17 3 3.5 3.5L17 10"/><path d="M4 11.5v-1a4 4 0 0 1 4-4h12.5"/><path d="m7 21-3.5-3.5L7 14"/><path d="M20 12.5v1a4 4 0 0 1-4 4H3.5"/><path d="M11.5 10.5 12.5 10v4.5"/>)"},
        {"shuffle", R"(<path d="M3 17.5h1.6a4 4 0 0 0 3.3-1.8l5.6-7.9a4 4 0 0 1 3.3-1.8H21"/><path d="m18 3 3 3-3 3"/><path d="M3 6.5h1.8a4 4 0 0 1 3.2 1.6"/><path d="M21 17.5h-4.2a4 4 0 0 1-3.2-1.6"/><path d="m18 14.5 3 3-3 3"/>)"},
        {"note", R"(<circle cx="7.5" cy="17.5" r="2.5"/><circle cx="17" cy="15.5" r="2.5"/><path d="M10 17.5V5.5l9.5-2v12"/>)"},
        {"song", R"(<circle cx="8.5" cy="17" r="3"/><path d="M11.5 17V4.5c2.5 0 5.5 1.5 5.5 4.5"/>)"},
        {"album", R"(<rect x="4" y="8.5" width="16" height="12" rx="2"/><path d="M6 5.5h12M8 2.5h8"/><circle cx="10.5" cy="16.5" r="1.8"/><path d="M12.3 16.5v-5l2.5 1"/>)"},
        {"artist", R"(<circle cx="9" cy="7.5" r="3.5"/><path d="M2.5 20c0-3.6 2.9-6.5 6.5-6.5 1.5 0 2.9.5 4 1.4"/><circle cx="16.5" cy="17.5" r="2"/><path d="M18.5 17.5V10l2.5 1"/>)"},
        {"genre", R"(<rect x="5.5" y="2.5" width="13" height="19" rx="2"/><circle cx="12" cy="14.5" r="3.5"/><circle cx="12" cy="7" r="1.2"/>)"},
        {"disc", R"(<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="2.5"/><path d="M12 6.5a5.5 5.5 0 0 0-5.5 5.5"/>)"},
        {"chapters", R"(<path d="M5 6.5h14M5 12h14M5 17.5h9"/>)"},
        {"menu-disc", R"(<rect x="3.5" y="5" width="17" height="14" rx="2"/><path d="M7.5 9.5h9M7.5 12.5h9M7.5 15.5h5"/>)"},
        {"film", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M7.5 4.5v15M16.5 4.5v15M3.5 9h4M3.5 15h4M16.5 9h4M16.5 15h4"/>)"},
        {"file", R"(<path d="M13.5 3.5h-7a1.5 1.5 0 0 0-1.5 1.5v14a1.5 1.5 0 0 0 1.5 1.5h11a1.5 1.5 0 0 0 1.5-1.5V8.5z"/><path d="M13.5 3.5v5h5"/>)"},
        {"aspect", R"(<rect x="3" y="6" width="18" height="12" rx="2"/><path d="M7 10V9h2M17 14v1h-2"/>)"},
        {"speed", R"(<path d="M4.2 18.5a9 9 0 1 1 15.6 0"/><path d="m12 14 3.5-5"/>)"},
        {"eq", R"(<path d="M6 4v16M12 4v16M18 4v16"/><rect x="4" y="12" width="4" height="2.5" rx="1" fill="currentColor"/><rect x="10" y="7" width="4" height="2.5" rx="1" fill="currentColor"/><rect x="16" y="14" width="4" height="2.5" rx="1" fill="currentColor"/>)"},
        {"crop", R"(<path d="M6.5 2.5v13a2 2 0 0 0 2 2h13"/><path d="M2.5 6.5h13a2 2 0 0 1 2 2v13"/>)"},
        {"wand", R"(<path d="m4 20 11-11"/><path d="m13.5 7.5 3 3"/><path d="M18 3v3M16.5 4.5h3M20 9v2M19 10h2M10 3v2M9 4h2"/>)"},
        {"mute", R"(<path d="M10.5 5.5 6.5 9H3.5v6h3l4 3.5z"/><path d="m15 9.5 5 5M20 9.5l-5 5"/>)"},
        {"jump-fwd", R"(<path d="M20 12a8 8 0 1 1-2.4-5.7"/><path d="M20 4v4.5h-4.5"/><path d="M10 10v5M13 10h2.5v1.5H13V15h2.5"/>)"},
        {"jump-back", R"(<path d="M4 12a8 8 0 1 0 2.4-5.7"/><path d="M4 4v4.5h4.5"/><path d="M10 10v5M13 10h2.5v1.5H13V15h2.5"/>)"},
        {"stream", R"(<circle cx="12" cy="12" r="1.6" fill="currentColor"/><path d="M8.5 15.5a5 5 0 0 1 0-7M15.5 8.5a5 5 0 0 1 0 7"/>)"},
        {"refresh", R"(<path d="M20 12a8 8 0 0 1-14 5.3M4 12a8 8 0 0 1 14-5.3"/><path d="M18 3v3.7h-3.7M6 21v-3.7h3.7"/>)"},
        {"eye", R"(<path d="M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z"/><circle cx="12" cy="12" r="3"/>)"},
        {"eye-off", R"(<path d="M4 4l16 16"/><path d="M9.9 5.8A9.6 9.6 0 0 1 12 5.5c6 0 9.5 6.5 9.5 6.5a17 17 0 0 1-2.7 3.4M6.4 6.9C3.9 8.6 2.5 12 2.5 12S6 18.5 12 18.5c1.6 0 3-.4 4.2-1.1"/>)"},
    };
    return icons;
}

Theme &Theme::instance()
{
    static Theme t;
    return t;
}

// The Omarchy build adds a last swatch that follows the desktop theme's accent.
static constexpr int kStockAccents = 9;

QList<QColor> Theme::accentChoices()
{
    QList<QColor> c = {QColor(0xE8, 0x83, 0x1C), QColor(0xD0, 0x4E, 0x34), QColor(0xB8, 0x9B, 0x2A),
                       QColor(0x2A, 0xA1, 0x45), QColor(0x1F, 0x9D, 0xB8), QColor(0x33, 0x66, 0xD6),
                       QColor(0x8F, 0x45, 0xF2), QColor(0xEC, 0x50, 0xA8), QColor(0xA0, 0xA0, 0xA0)};
#ifdef VMP_OMARCHY
    const Theme &t = instance();
    c << (t.m_omLoaded ? t.m_omAccent : c.first());
#endif
    return c;
}

QStringList Theme::accentNames()
{
    QStringList n = {"Orange", "Red", "Olive", "Green", "Teal", "Blue", "Purple", "Pink", "Gray"};
#ifdef VMP_OMARCHY
    n << QCoreApplication::translate("Theme", "Omarchy theme");
#endif
    return n;
}

QStringList Theme::modeNames()
{
    QStringList n = {QCoreApplication::translate("Theme", "Dark"), QCoreApplication::translate("Theme", "Light"),
                     QCoreApplication::translate("Theme", "System")};
#ifdef VMP_OMARCHY
    n << QCoreApplication::translate("Theme", "Omarchy");
#endif
    return n;
}

void Theme::pick(Mode mode)
{
    instance().setMode(mode);
    prefs().set(kModePref, int(mode));
}

void Theme::pickAccent(int index)
{
    instance().setAccentIndex(index);
    prefs().set(kAccentPref, index);
}

void Theme::loadSaved()
{
    instance().setMode(Mode(prefs().integer(kModePref)));
    instance().setAccentIndex(prefs().integer(kAccentPref));
}

Theme::Theme()
{
    m_dark.window = QColor(0x12, 0x12, 0x12);
    m_dark.chrome = QColor(0x1c, 0x1c, 0x1c);
    m_dark.panel = QColor(0x17, 0x17, 0x17);
    m_dark.surface = QColor(0x24, 0x24, 0x24);
    m_dark.hover = QColor(0x2a, 0x2a, 0x2a);
    m_dark.selected = QColor(0x26, 0x26, 0x26);
    m_dark.input = QColor(0x0b, 0x0b, 0x0b);
    m_dark.border = QColor(0x2a, 0x2a, 0x2a);
    m_dark.text = QColor(0xd6, 0xd6, 0xd6);
    m_dark.textDim = QColor(0x9a, 0x9a, 0x9a);
    m_dark.textMuted = QColor(0x6c, 0x6c, 0x6c);
    m_dark.videoBg = QColor(0x0a, 0x0a, 0x0a);
    m_dark.knobOff = QColor(0x45, 0x45, 0x45);

    m_light.window = QColor(0xf4, 0xf4, 0xf4);
    m_light.chrome = QColor(0xe9, 0xe9, 0xe9);
    m_light.panel = QColor(0xfb, 0xfb, 0xfb);
    m_light.surface = QColor(0xe4, 0xe4, 0xe4);
    m_light.hover = QColor(0xdc, 0xdc, 0xdc);
    m_light.selected = QColor(0xe2, 0xe2, 0xe2);
    m_light.input = QColor(0xdd, 0xdd, 0xdd);
    m_light.border = QColor(0xd6, 0xd6, 0xd6);
    m_light.text = QColor(0x22, 0x22, 0x22);
    m_light.textDim = QColor(0x55, 0x55, 0x55);
    m_light.textMuted = QColor(0x8a, 0x8a, 0x8a);
    m_light.videoBg = QColor(0x0a, 0x0a, 0x0a);
    m_light.knobOff = QColor(0xb0, 0xb0, 0xb0);

    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_mode == System)
            emit changed();
    });

#ifdef VMP_OMARCHY
    // omarchy-theme-set replaces current/theme wholesale, so watch the parent
    // directory too; the file watch is lost with the old copy and re-added.
    m_omWatcher = new QFileSystemWatcher(this);
    m_omReload = new QTimer(this);
    m_omReload->setSingleShot(true);
    m_omReload->setInterval(250);
    connect(m_omReload, &QTimer::timeout, this, [this] {
        const bool affected = m_mode == Omarchy || m_accent == kStockAccents;
        loadOmarchy();
        if (affected)
            emit changed();
    });
    auto schedule = [this] { m_omReload->start(); };
    connect(m_omWatcher, &QFileSystemWatcher::directoryChanged, this, schedule);
    connect(m_omWatcher, &QFileSystemWatcher::fileChanged, this, schedule);
    loadOmarchy();
#endif
}

// Like mixColor, but t may fall outside 0..1 (shades beyond the background).
static QColor ramp(const QColor &a, const QColor &b, qreal t)
{
    auto ch = [t](qreal x, qreal y) { return qBound(0.0, x + (y - x) * t, 1.0); };
    return QColor::fromRgbF(ch(a.redF(), b.redF()), ch(a.greenF(), b.greenF()), ch(a.blueF(), b.blueF()));
}

static QString omarchyThemeDir()
{
    return QDir::homePath() + "/.local/state/omarchy/current/theme";
}

void Theme::loadOmarchy()
{
    const QString dir = omarchyThemeDir();
    const QString path = dir + "/colors.toml";
    for (const QString &watch : {QFileInfo(dir).path(), dir, path})
        if (QFileInfo::exists(watch) && !m_omWatcher->files().contains(watch) && !m_omWatcher->directories().contains(watch))
            m_omWatcher->addPath(watch);

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_omLoaded = false; // mid-switch, or not an Omarchy desktop: keep the stock look
        return;
    }
    // colors.toml is flat `key = "value"` pairs.
    QHash<QString, QString> v;
    static const QRegularExpression line(R"re(^\s*([A-Za-z_]+)\s*=\s*"([^"]*)")re");
    for (const QByteArray &raw : file.readAll().split('\n'))
        if (const auto m = line.match(QString::fromUtf8(raw)); m.hasMatch())
            v.insert(m.captured(1), m.captured(2));
    const QColor bg(v.value("background")), fg(v.value("foreground"));
    if (!bg.isValid() || !fg.isValid()) {
        m_omLoaded = false;
        return;
    }
    m_omDark = v.value("mode", "dark") != "light";
    m_omAccent = QColor(v.value("accent"));
    if (!m_omAccent.isValid())
        m_omAccent = QColor(v.value("blue"));
    if (!m_omAccent.isValid())
        m_omAccent = fg;
    m_omGreen = QColor(v.value("green"));
    if (!m_omGreen.isValid())
        m_omGreen = QColor(0x9c, 0xc4, 0x1e);

    // Every role keeps the stock palette's distance between background and
    // text, measured along the theme's own background-to-foreground ramp.
    m_omLoaded = true;
    const Palette &stock = m_omDark ? m_dark : m_light;
    m_omarchy.window = bg;
    m_omarchy.text = fg;
    auto map = [&](const QColor &c) {
        const qreal w = qGray(stock.window.rgb()), t = qGray(stock.text.rgb());
        return ramp(bg, fg, (qGray(c.rgb()) - w) / (t - w));
    };
    m_omarchy.chrome = map(stock.chrome);
    m_omarchy.panel = map(stock.panel);
    m_omarchy.surface = map(stock.surface);
    m_omarchy.hover = map(stock.hover);
    m_omarchy.selected = map(stock.selected);
    m_omarchy.input = map(stock.input);
    m_omarchy.border = map(stock.border);
    m_omarchy.textDim = map(stock.textDim);
    m_omarchy.textMuted = map(stock.textMuted);
    m_omarchy.knobOff = map(stock.knobOff);
    // Light themes keep a black backdrop for video, like the stock light theme.
    m_omarchy.videoBg = m_omDark ? map(stock.videoBg) : stock.videoBg;
}

bool Theme::omarchyActive() const
{
    return m_mode == Omarchy && m_omLoaded;
}

const Palette &Theme::pal() const
{
    if (omarchyActive())
        return m_omarchy;
    return isDark() ? m_dark : m_light;
}

QColor Theme::tone(const QColor &stock) const
{
    if (!omarchyActive())
        return stock;
    const Palette &ref = m_omDark ? m_dark : m_light;
    const qreal w = qGray(ref.window.rgb()), t = qGray(ref.text.rgb());
    QColor c = ramp(m_omarchy.window, m_omarchy.text, (qGray(stock.rgb()) - w) / (t - w));
    c.setAlpha(stock.alpha());
    return c;
}

QColor Theme::accent() const
{
    const auto choices = accentChoices();
    return choices.value(m_accent, choices.first());
}

QColor Theme::accentFill() const
{
    return isDark() ? mixColor(tone(QColor(0x0b, 0x0b, 0x0b)), accent(), 0.28)
                    : mixColor(tone(QColor(0xff, 0xff, 0xff)), accent(), 0.35);
}

QColor Theme::onAccent() const
{
    // Some Omarchy themes have dark accents; keep text on them readable.
    if (m_accent == kStockAccents && qGray(accent().rgb()) < 110)
        return QColor(0xf5, 0xf5, 0xf5);
    return QColor(0x16, 0x16, 0x16);
}

QColor Theme::volumeColor() const
{
    return omarchyActive() ? m_omGreen : QColor(0x9c, 0xc4, 0x1e);
}

void Theme::setAccentIndex(int index)
{
    if (index < 0 || index >= accentChoices().size() || index == m_accent)
        return;
    m_accent = index;
    emit changed();
}

void Theme::setMode(Mode mode)
{
    if (mode == m_mode || mode < Dark || mode >= modeNames().size())
        return;
    m_mode = mode;
    emit changed();
}

bool Theme::isDark() const
{
    if (m_mode == Omarchy)
        return !m_omLoaded || m_omDark;
    if (m_mode == System)
        return QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
    return m_mode == Dark;
}

QPixmap Theme::pixmap(const QString &name, const QColor &color, int size, qreal dpr) const
{
    // Painted from paint events (buttons, list rows, menus), so render each
    // icon/colour/size combination once. Icons are tiny; the cap is a backstop.
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(name).arg(color.rgba()).arg(size).arg(dpr);
    if (const auto it = cache.constFind(key); it != cache.cend())
        return *it;
    if (cache.size() > 1024)
        cache.clear();

    const QString body = iconSources().value(name);
    QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
                                 "stroke='currentColor' stroke-width='1.6' stroke-linecap='round' "
                                 "stroke-linejoin='round'>%1</svg>")
                      .arg(body);
    svg.replace("currentColor", color.name(QColor::HexRgb));
    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pm(QSize(size, size) * dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(color.alphaF());
    renderer.render(&p);
    p.end();
    pm.setDevicePixelRatio(dpr);
    cache.insert(key, pm);
    return pm;
}

QIcon Theme::icon(const QString &name, const QColor &color) const
{
    const QColor c = color.isValid() ? color : pal().textDim;
    QIcon icon;
    for (int s : {16, 20, 24, 32})
        icon.addPixmap(pixmap(name, c, s, 2.0));
    return icon;
}

void Theme::paintLogo(QPainter *p, const QRectF &r, const QColor &accent)
{
    // The VMP mark: a blocky traffic cone stood on its tip, with a V cut down
    // into it from the top. Same rows as packaging/make-icon.py, which draws
    // the app icon (keep the two in sync).
    struct Row {
        int left, right, slotLeft, slotRight;
    };
    static const Row rows[] = {
        {1, 15, 4, 12}, {1, 15, 4, 12}, {2, 14, 5, 11}, {2, 14, 5, 11}, {3, 13, 6, 10},
        {3, 13, 6, 10}, {4, 12, 7, 9},  {4, 12, 7, 9},  {5, 11, 0, 0},  {5, 11, 0, 0},
        {6, 10, 0, 0},  {6, 10, 0, 0},  {7, 9, 0, 0},
    };
    // 13 rows of 14 used columns (1..15) fill the square; blocks stay square.
    const qreal unit = qMin(r.width() / 14.0, r.height() / 13.0);
    const QPointF o(r.center().x() - 8 * unit, r.center().y() - 6.5 * unit);
    QPainterPath shape;
    for (int i = 0; i < int(std::size(rows)); ++i) {
        const Row &row = rows[i];
        const qreal y = o.y() + i * unit;
        auto block = [&](int a, int b) { shape.addRect(QRectF(o.x() + a * unit, y, (b - a) * unit, unit)); };
        if (row.slotRight > row.slotLeft) {
            block(row.left, row.slotLeft);
            block(row.slotRight, row.right);
        } else {
            block(row.left, row.right);
        }
    }
    p->save();
    p->setRenderHint(QPainter::Antialiasing, unit < 4); // crisp blocks, smoothed when tiny
    p->setPen(Qt::NoPen);
    p->setBrush(accent);
    p->drawPath(shape.simplified());
    p->restore();
}

QPalette Theme::palette() const
{
    const Palette &c = pal();
    QPalette p;
    p.setColor(QPalette::Window, c.chrome);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.input);
    p.setColor(QPalette::AlternateBase, c.panel);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.surface);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, c.text);
    p.setColor(QPalette::PlaceholderText, c.textMuted);
    p.setColor(QPalette::ToolTipBase, c.surface);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Highlight, accentFill());
    p.setColor(QPalette::HighlightedText, c.text);
    p.setColor(QPalette::Link, accent());
    p.setColor(QPalette::Light, c.hover);
    p.setColor(QPalette::Midlight, c.surface);
    p.setColor(QPalette::Mid, c.border);
    p.setColor(QPalette::Dark, c.input);
    p.setColor(QPalette::Shadow, Qt::black);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, c.textMuted);
    return p;
}

QString Theme::styleSheet() const
{
    const Palette &c = pal();
    const QString acc = accent().name();
    const QString accFill = accentFill().name();
    QString css = QStringLiteral(R"(
* { font-family: "Inter"; font-size: 13px; color: %text; outline: none; }
QWidget#Root, QWidget#Library, QStackedWidget#Content { background: %window; }
QWidget#Chrome { background: %chrome; }
QWidget#SidePanel { background: %input; }
QWidget#NavPanel { background: %panel; border-radius: 6px; }
QWidget#VideoArea { background: %video; }
QLabel { background: transparent; color: %text; }
QLabel[role="dim"] { color: %dim; }
QLabel[role="muted"] { color: %muted; }
QLabel[role="title"] { font-size: 15px; color: %text; }
QLabel[role="heading"] { font-size: 16px; font-weight: 500; color: %text; }
QLabel[role="section"] { color: %muted; font-size: 13px; }
QLabel[role="time"] { color: %dim; font-size: 13px; }
QLabel[role="crumb"] { color: %dim; font-size: 15px; }
QLabel[role="crumbCurrent"] { color: %text; font-size: 15px; font-weight: 600; }
QToolTip { background: %surface; color: %text; border: 1px solid %border; padding: 4px 8px; border-radius: 4px; }
QLineEdit { background: %input; border: 1px solid transparent; border-radius: 6px; padding: 7px 12px;
            color: %text; selection-background-color: %acc; selection-color: %onAcc; }
QLineEdit:focus { border: 1px solid %border; }
QLineEdit[role="search"] { padding-left: 38px; background: %surfaceAlt; }
QLineEdit[role="value"] { font-size: 14px; padding: 7px 14px; }
QPushButton { background: %surface; border: none; border-radius: 6px; padding: 9px 18px; color: %text; font-weight: 500; }
QPushButton:hover { background: %hover; }
QPushButton:pressed { background: %selected; }
QPushButton:disabled { color: %muted; }
QPushButton[role="wide"] { background: %surface; padding: 12px; font-weight: 400; color: %dim; font-size: 15px; }
QPushButton[role="wide"]:hover { background: %hover; color: %text; }
QPushButton[role="link"] { background: transparent; color: %dim; font-weight: 400; padding: 8px; }
QPushButton[role="link"]:hover { color: %text; }
QPushButton[role="accent"] { background: %acc; color: %onAcc; }
QPushButton[role="accent"]:hover { background: %accHover; }
QPushButton[role="homeHeader"] { background: transparent; padding: 0; font-size: 16px; font-weight: 400;
    color: %text; text-align: left; }
QPushButton[role="homeHeader"]:hover { background: transparent; color: %acc; }
QWidget#HomeContent { background: transparent; }
QPushButton[role="accent"]:disabled { background: %surface; color: %muted; }
QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: %scroll; border-radius: 3px; min-height: 30px; margin: 0 2px; }
QScrollBar::handle:vertical:hover { background: %muted; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: %scroll; border-radius: 3px; min-width: 30px; margin: 2px 0; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QListView, QListWidget, QTreeWidget { background: transparent; border: none; }
QListWidget::item, QTreeWidget::item { padding: 6px 4px; border-radius: 4px; color: %dim; }
QListWidget::item:hover, QTreeWidget::item:hover { background: %hover; }
QListWidget::item:selected, QTreeWidget::item:selected { background: %selected; color: %text; }
QHeaderView::section { background: transparent; color: %muted; border: none; padding: 6px 4px; }
QComboBox QAbstractItemView { background: %surface; border: 1px solid %border; border-radius: 6px; padding: 4px;
                              selection-background-color: %hover; selection-color: %text; color: %dim; }
QDialog { background: %chrome; }
QMessageBox QLabel { color: %text; }
QFrame[role="hline"] { background: %border; max-height: 1px; min-height: 1px; border: none; }
QFrame[role="card"] { background: %surfaceAlt; border-radius: 8px; }
QWidget#QuickSettings { background: %chrome; border-radius: 12px; border: 1px solid %border; }
QWidget#SettingsPanel { background: %window; border-radius: 14px; }
QWidget#SettingsNav { background: %panel; border-radius: 8px; }
QFileDialog { background: %chrome; }
QFileDialog QLabel { color: %dim; }
QFileDialog QTreeView, QFileDialog QListView#listView { background: %input; border: none; border-radius: 8px;
    padding: 4px; color: %text; selection-background-color: %accFill; selection-color: %text; }
QFileDialog QTreeView::item, QFileDialog QListView#listView::item { padding: 5px 2px; color: %text; border: none; }
QFileDialog QTreeView::item:hover, QFileDialog QListView#listView::item:hover { background: %hover; }
QFileDialog QTreeView::item:selected, QFileDialog QListView#listView::item:selected { background: %accFill; color: %text; }
QFileDialog QTreeView::branch { background: transparent; }
QFileDialog QListView#sidebar { background: %panel; border: none; border-radius: 8px; padding: 6px; }
QFileDialog QListView#sidebar::item { padding: 7px 6px; color: %dim; border-radius: 6px; }
QFileDialog QListView#sidebar::item:hover { background: %hover; color: %text; }
QFileDialog QListView#sidebar::item:selected { background: %selected; color: %text; }
QFileDialog QHeaderView { background: %input; border: none; border-top-left-radius: 8px; border-top-right-radius: 8px; }
QFileDialog QHeaderView::section { background: %input; color: %muted; border: none;
    border-bottom: 1px solid %border; padding: 8px 6px; }
QFileDialog QHeaderView::section:hover { color: %text; }
QFileDialog QComboBox { background: %input; border: 1px solid transparent; border-radius: 6px; padding: 7px 12px;
    color: %text; min-height: 18px; }
QFileDialog QComboBox:focus { border: 1px solid %border; }
QFileDialog QComboBox::drop-down { border: none; width: 28px; }
QFileDialog QToolButton { background: transparent; border: none; border-radius: 6px; padding: 6px; }
QFileDialog QToolButton:hover { background: %hover; }
QFileDialog QToolButton:checked, QFileDialog QToolButton:pressed { background: %selected; }
QFileDialog QToolButton:disabled { background: transparent; }
QFileDialog QDialogButtonBox QPushButton { min-width: 92px; padding: 8px 18px; }
QFileDialog QSplitter::handle { background: transparent; width: 8px; }
QFileDialog QLineEdit { padding: 8px 12px; }
QWidget#SubtitleWell { background: %input; border-radius: 8px; }
QWidget#SubtitleList { background: transparent; }
QWidget#Toast { background: rgba(20,20,20,225); border-radius: 8px; }
QWidget#Toast QLabel { color: #e6e6e6; }
)");
    css.replace("%window", c.window.name())
        .replace("%chrome", c.chrome.name())
        .replace("%panel", c.panel.name())
        .replace("%surfaceAlt", tone(isDark() ? QColor(0x1b, 0x1b, 0x1b) : QColor(0xec, 0xec, 0xec)).name())
        .replace("%surface", c.surface.name())
        .replace("%hover", c.hover.name())
        .replace("%selected", c.selected.name())
        .replace("%input", c.input.name())
        .replace("%border", c.border.name())
        .replace("%text", c.text.name())
        .replace("%dim", c.textDim.name())
        .replace("%muted", c.textMuted.name())
        .replace("%video", c.videoBg.name())
        .replace("%scroll", tone(isDark() ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xc4, 0xc4, 0xc4)).name())
        .replace("%menu", tone(isDark() ? QColor(0x23, 0x23, 0x23) : QColor(0xfa, 0xfa, 0xfa)).name())
        .replace("%onAcc", onAccent().name())
        .replace("%accHover", accent().lighter(112).name())
        .replace("%accFill", accFill)
        .replace("%acc", acc);
    return css;
}
