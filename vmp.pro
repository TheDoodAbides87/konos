# VMP (Vibes Media Player) — a media player built on libVLC.
TEMPLATE = app
TARGET = vmp
QT += widgets svg openglwidgets dbus network
CONFIG += c++17 release
CONFIG -= debug_and_release

# libVLC (public API) + libvlccore (object variables for audio/video filters)
INCLUDEPATH += /usr/include/vlc/plugins
LIBS += -lvlc -lvlccore -ltag

# Developer-only UI scripting for screenshots: qmake CONFIG+=devhooks
devhooks: DEFINES += VMP_DEVHOOKS
devhooks: QT += testlib

# OpenSubtitles API key baked into this build (else users add one in Settings):
#   OPENSUBTITLES_API_KEY=... qmake6
OS_KEY = $$(OPENSUBTITLES_API_KEY)
!isEmpty(OS_KEY): DEFINES += VMP_OPENSUBTITLES_KEY=\\\"$$OS_KEY\\\"

# Omarchy build: also follows the Omarchy desktop theme (qmake CONFIG+=omarchy).
# The Arch package is built this way.
omarchy: DEFINES += VMP_OMARCHY

SOURCES += \
    src/main.cpp \
    src/updatecheck.cpp \
    src/showpresets.cpp \
    src/subtitlecues.cpp \
    src/subtitletimeline.cpp \
    src/mediainfocard.cpp \
    src/appstyle.cpp \
    src/artistimages.cpp \
    src/chrome.cpp \
    src/disc.cpp \
    src/effects.cpp \
    src/filedialogs.cpp \
    src/homepage.cpp \
    src/fxpanels.cpp \
    src/library.cpp \
    src/libraryview.cpp \
    src/mainwindow.cpp \
    src/musiceditors.cpp \
    src/playerpanel.cpp \
    src/durations.cpp \
    src/musicindex.cpp \
    src/opensubtitles.cpp \
    src/player.cpp \
    src/prefs.cpp \
    src/quicksettings.cpp \
    src/seekpreview.cpp \
    src/settingsdialog.cpp \
    src/subtitleoverlay.cpp \
    src/theme.cpp \
    src/videowidget.cpp \
    src/waveform.cpp \
    src/vlcvars.cpp \
    src/widgets.cpp

HEADERS += \
    src/appstyle.h \
    src/updatecheck.h \
    src/showpresets.h \
    src/subtitlecues.h \
    src/subtitletimeline.h \
    src/mediainfocard.h \
    src/artistimages.h \
    src/chrome.h \
    src/disc.h \
    src/effects.h \
    src/filedialogs.h \
    src/homepage.h \
    src/fxpanels.h \
    src/library.h \
    src/libraryview.h \
    src/mainwindow.h \
    src/musiceditors.h \
    src/playerpanel.h \
    src/durations.h \
    src/musicindex.h \
    src/opensubtitles.h \
    src/player.h \
    src/prefs.h \
    src/quicksettings.h \
    src/seekpreview.h \
    src/settingsdialog.h \
    src/subtitleoverlay.h \
    src/theme.h \
    src/version.h \
    src/videowidget.h \
    src/waveform.h \
    src/vlcvars.h \
    src/widgets.h

RESOURCES += resources/resources.qrc

target.path = /usr/bin
desktop.path = /usr/share/applications
desktop.files = packaging/vmp.desktop
icon.path = /usr/share/icons/hicolor/scalable/apps
icon.files = resources/vmp.svg
license.path = /usr/share/licenses/vmp
license.files = LICENSE resources/fonts/Inter-LICENSE.txt
INSTALLS += target desktop icon license
