#!/bin/bash
# Builds the Arch package and publishes it as a GitHub release on the public
# releases repository, which doubles as the [vmp] pacman repository:
#
#   [vmp]
#   SigLevel = Optional TrustAll
#   Server = https://github.com/TheDoodAbides87/vmp-releases/releases/latest/download
#
# Each release carries the package plus a fresh repository database listing
# only that package, so "latest/download" always serves a consistent repo.
# It also carries the AppImage for other distributions, which the app
# downloads to update itself.
#
#   packaging/release.sh               build, then publish (asks first)
#   packaging/release.sh --build-only  build the package and AppImage into dist/ only
#   NO_APPIMAGE=1 packaging/release.sh  leave the AppImage out
#
# Needs: base-devel (makepkg), pacman (repo-add) and, to publish, gh logged in.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH_DIR="$ROOT/packaging/arch"
DIST="$ROOT/dist"
VERSION="$(sed -n 's/^#define VMP_VERSION "\(.*\)"/\1/p' "$ROOT/src/version.h")"
CHANNEL="$(sed -n 's/^#define VMP_CHANNEL "\(.*\)"/\1/p' "$ROOT/src/version.h")"
REPO="$(sed -n 's/^#define VMP_RELEASES_REPO "\(.*\)"/\1/p' "$ROOT/src/version.h")"
PKGVER="$VERSION${CHANNEL}"   # 0.4.0beta: pacman sorts it before 0.4.0
TAG="v$VERSION${CHANNEL:+-$CHANNEL}"
BUILD_ONLY=0
[ "${1:-}" = "--build-only" ] && BUILD_ONLY=1

cd "$ROOT"
if [ "$(sed -n 's/^pkgver=//p' "$ARCH_DIR/PKGBUILD")" != "$PKGVER" ]; then
    echo "packaging/arch/PKGBUILD has pkgver=$(sed -n 's/^pkgver=//p' "$ARCH_DIR/PKGBUILD"); src/version.h says $PKGVER." >&2
    echo "Set pkgver=$PKGVER (and pkgrel=1) in the PKGBUILD and commit it." >&2
    exit 1
fi
if [ $BUILD_ONLY = 0 ] && [ -n "$(git status --porcelain)" ]; then
    echo "Commit or stash your changes first: the package is built from HEAD." >&2
    exit 1
fi

if [ $BUILD_ONLY = 1 ] && [ -n "$(git status --porcelain)" ]; then
    # A test build of uncommitted work: the files git knows about or would add.
    echo ">> source archive of the working tree (uncommitted changes included)"
    git ls-files -z --cached --others --exclude-standard | grep -zv '^VLC Redesign (Community)/' |
        tar --null -T - --transform "s,^,vmp-$PKGVER/," -czf "$ARCH_DIR/vmp-$PKGVER.tar.gz"
else
    echo ">> source archive of HEAD"
    git archive --format=tar.gz --prefix="vmp-$PKGVER/" -o "$ARCH_DIR/vmp-$PKGVER.tar.gz" HEAD
fi

echo ">> makepkg"
mkdir -p "$DIST"
find "$DIST" -maxdepth 1 -type f \( -name 'vmp*' -o -name 'VMP-*.AppImage' \) -delete
(cd "$ARCH_DIR" && PKGDEST="$DIST" makepkg --force --cleanbuild --clean)
PKG="$(find "$DIST" -maxdepth 1 -name "vmp-$PKGVER-*.pkg.tar.zst" | head -n1)"

echo ">> repository database"
repo-add "$DIST/vmp.db.tar.gz" "$PKG"
# repo-add makes vmp.db / vmp.files symlinks; release assets must be real files.
for f in vmp.db vmp.files; do
    cp --remove-destination "$(readlink -f "$DIST/$f")" "$DIST/$f"
done
echo ">> built: $PKG"
APPIMAGE=""
if [ -z "${NO_APPIMAGE:-}" ]; then
    echo ">> AppImage"
    "$ROOT/packaging/build-appimage.sh"
    APPIMAGE="$(find "$DIST" -maxdepth 1 -name "VMP-*-x86_64.AppImage" | head -n1)"
fi
[ $BUILD_ONLY = 1 ] && exit 0

# Release notes: this version's section of the changelog.
NOTES="$DIST/notes.md"
awk -v v="## $VERSION " 'index($0, v) == 1 { on = 1; next } on && /^## / { exit } on' CHANGELOG.md > "$NOTES"
cat >> "$NOTES" <<'CREDITS'

---

VMP is an overhaul of VLC: VLC's engine (libVLC, by VideoLAN) with a new
interface. The design is by [Juxtopposed](https://www.youtube.com/@juxtopposed),
from [*I Redesigned VLC After 25 Years*](https://youtu.be/ERFeOgqk324).
VMP is independent and not affiliated with VideoLAN or Juxtopposed.

VMP is in early development: it changes often and things may break between
releases.
CREDITS

echo
echo "About to publish $TAG to https://github.com/$REPO (public) with:"
ls -1 "$DIST" | grep -v notes.md | sed 's/^/  /'
read -r -p "Publish? [y/N] " answer
[ "$answer" = "y" ] || [ "$answer" = "Y" ] || { echo "Not published."; exit 0; }

# Not a pre-release: releases/latest (which pacman and the update check read)
# skips pre-releases.
gh release create "$TAG" --repo "$REPO" --title "VMP $VERSION${CHANNEL:+ $CHANNEL}" \
    --notes-file "$NOTES" --latest \
    "$PKG" "$DIST/vmp.db" "$DIST/vmp.db.tar.gz" "$DIST/vmp.files" "$DIST/vmp.files.tar.gz" ${APPIMAGE:+"$APPIMAGE"}
echo ">> published $TAG"
