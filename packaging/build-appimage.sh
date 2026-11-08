#!/bin/bash
# Builds dist/Konos-<version>-<channel>-x86_64.AppImage from this source tree:
# Konos for other Linux distributions, with libVLC and Qt inside. (On Arch the
# pacman package is the way to install it; see packaging/arch.)
#
# Needs: qt6-base qt6-svg (qmake6), vlc (libvlc + plugins + headers), taglib, g++, curl.
# The linuxdeploy / appimagetool AppImages are downloaded to ~/.cache/konos-tools.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP=konos
BUILD="$ROOT/build-appimage"
QMAKE_ARGS=()
APPDIR="$BUILD/AppDir"
TOOLS="${TOOLS:-$HOME/.cache/konos-tools}"
VLC_LIBDIR="${VLC_LIBDIR:-/usr/lib/vlc}"
VERSION="$(sed -n 's/^#define KONOS_VERSION "\(.*\)"/\1/p' "$ROOT/src/version.h")"
CHANNEL="$(sed -n 's/^#define KONOS_CHANNEL "\(.*\)"/\1/p' "$ROOT/src/version.h")"
mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/Konos-$VERSION${CHANNEL:+-$CHANNEL}-x86_64.AppImage"

fetch() {
    mkdir -p "$TOOLS"
    [ -x "$TOOLS/$1" ] && return
    echo ">> downloading $1"
    curl -fsSL -o "$TOOLS/$1" "$2"
    chmod +x "$TOOLS/$1"
}
fetch linuxdeploy-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch linuxdeploy-plugin-qt-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
fetch appimagetool-x86_64.AppImage https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
export PATH="$TOOLS:$PATH"

echo ">> building"
rm -rf "$APPDIR"
mkdir -p "$BUILD"
(cd "$BUILD" && qmake6 "$ROOT/konos.pro" "${QMAKE_ARGS[@]}" && make -j"$(nproc)" && make install INSTALL_ROOT="$APPDIR")

echo ">> bundling libVLC plugins"
mkdir -p "$APPDIR/usr/lib/vlc"
cp -a "$VLC_LIBDIR/plugins" "$APPDIR/usr/lib/vlc/"
cp -a "$VLC_LIBDIR"/libvlc_*.so* "$APPDIR/usr/lib/vlc/" 2>/dev/null || true
[ -d "$VLC_LIBDIR/lua" ] && cp -a "$VLC_LIBDIR/lua" "$APPDIR/usr/lib/vlc/"
# Plugins we don't use and that would drag in big or conflicting stacks:
#   gui/*        VLC's own Qt5 interface
#   gstdecode    GStreamer (needs host GStreamer plugins at matching versions)
#   control/*    remote-control interfaces (libVLC never starts them), except hotkeys
#   notify/*     desktop notifications (drags in GTK 3)
#   video_splitter   no UI for it here
#   visualization/glspectrum, projectm   open their own OpenGL windows (the audio
#                visualizer uses "visual" and "goom", which stay)
#   video_output/*   video is drawn by the app through vmem; VLC's own windows aren't used
#   services_discovery/*   UPnP/SAP/etc. browsing isn't exposed in the UI, except
#                microdns, which finds Chromecasts for Stream → Chromecast
#   fluidsynth (MIDI, drags in SDL 3)
P="$APPDIR/usr/lib/vlc/plugins"
rm -rf "$P/gui" "$P/notify" "$P/video_splitter"
rm -f "$P/visualization/libglspectrum_plugin.so" "$P/visualization/libprojectm_plugin.so"
find "$P/services_discovery" -name '*.so' ! -name libmicrodns_plugin.so -delete
find "$P/control" -name '*.so' ! -name libhotkeys_plugin.so ! -name libdummy_plugin.so -delete
find "$P/video_output" -name '*.so' ! -name libvmem_plugin.so ! -name libvdummy_plugin.so -delete
rm -f "$P/codec/libgstdecode_plugin.so" "$P/codec/libfluidsynth_plugin.so" "$P/plugins.dat"

# Qt's Wayland EGL client integration (needed for the OpenGL video surface on
# Wayland); linuxdeploy-plugin-qt doesn't pick it up on its own.
QT_PLUGIN_DIR="$(qmake6 -query QT_INSTALL_PLUGINS)"
mkdir -p "$APPDIR/usr/plugins/wayland-graphics-integration-client"
cp -a "$QT_PLUGIN_DIR/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so" \
      "$APPDIR/usr/plugins/wayland-graphics-integration-client/"

echo ">> deploying dependencies"
DEPLOY_ARGS=(--deploy-deps-only "$APPDIR/usr/plugins/wayland-graphics-integration-client")
while IFS= read -r dir; do
    DEPLOY_ARGS+=(--deploy-deps-only "$dir")
done < <(find "$APPDIR/usr/lib/vlc" -type d)

# Disc decryption libraries are dlopen()ed by libdvdread/libbluray, so nothing
# links to them; add them explicitly when the host has them.
LDCACHE="$(ldconfig -p)"
for lib in libdvdcss.so.2 libaacs.so.0 libbdplus.so.0; do
    path="$(awk -v l="$lib" '$1 == l && /x86-64/ { print $NF }' <<<"$LDCACHE" | head -n1)"
    [ -n "$path" ] && DEPLOY_ARGS+=(--library "$path")
done

export QMAKE=qmake6
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
export EXTRA_QT_MODULES="waylandclient;svg"
export NO_STRIP=1
export APPIMAGE_EXTRACT_AND_RUN=1
linuxdeploy-x86_64.AppImage \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/$APP" \
    --desktop-file "$ROOT/packaging/$APP.desktop" \
    --icon-file "$ROOT/packaging/icons/256x256/konos.png" \
    --custom-apprun "$ROOT/packaging/AppRun" \
    "${DEPLOY_ARGS[@]}" \
    --plugin qt

# Qt's PDF image plugin (pulls in QtPdf) has no use in a media player.
rm -f "$APPDIR/usr/plugins/imageformats/libqpdf.so" "$APPDIR"/usr/lib/libQt6Pdf.so*

echo ">> generating plugin cache"
if [ -x "$VLC_LIBDIR/vlc-cache-gen" ]; then
    LD_LIBRARY_PATH="$APPDIR/usr/lib" "$VLC_LIBDIR/vlc-cache-gen" "$APPDIR/usr/lib/vlc/plugins" || true
fi

echo ">> packing"
rm -f "$OUT"
ARCH=x86_64 VERSION="$VERSION${CHANNEL:+-$CHANNEL}" appimagetool-x86_64.AppImage --no-appstream "$APPDIR" "$OUT"
echo ">> done: $OUT ($(du -h "$OUT" | cut -f1))"
