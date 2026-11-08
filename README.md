# Konos Media Player

<img src="packaging/icons/256x256/konos.png" width="96" align="right" alt="Konos icon">

A desktop media player for videos, music, discs and streams.

> **Early development.** Konos is a beta that changes often: features move,
> settings may reset, and things can break between releases. Keep that in mind
> before relying on it.

**Konos is an overhaul of VLC.** It is VLC media player with a new interface:
playback, codecs, subtitles, discs, streaming and all audio/video effects come
from **libVLC 3**, the engine inside VLC, made by the
[VideoLAN](https://www.videolan.org/) project. What Konos replaces is the
interface around it, written from scratch in C++/Qt 6. Konos is an independent
project, not affiliated with or endorsed by VideoLAN.

**The design is by [Juxtopposed](https://www.youtube.com/@juxtopposed)**,
from the video [*I Redesigned VLC After 25 Years*](https://youtu.be/ERFeOgqk324).
Konos builds that redesign as a working player; each release works through a
part of the video. Juxtopposed isn't involved in Konos, and any rough edges are
Konos's own.

The icon is a play button inside a K-shaped ring. `packaging/icon-source.png`
is the artwork; `packaging/make-icon.py` makes the icon sizes from it, and the
mark without its tile that the app draws in its own window.

Licensed under the [MIT licence](LICENSE).

**Version 0.7.10 (beta).** See [CHANGELOG.md](CHANGELOG.md).

## What's implemented

| Mockup | Where it lives |
| --- | --- |
| Library grid with breadcrumbs, thumbnails, watched progress (`Player-1`) | `src/libraryview.cpp` |
| Player with icon rail + side panel (`Player`) and minimal player (`Player-2/3/4`) | `src/mainwindow.cpp`, `src/chrome.cpp` |
| Accent colours (orange, red, olive, green, teal, blue, purple, pink, gray) and Dark / Light / System themes | `src/theme.cpp` |
| Quick settings popup with Playback / Video / Audio / Subtitles tabs (`Subtitles`) | `src/quicksettings.cpp` |
| Settings window: Global, Audio + 11 more pages (`Settings - global/audio`) | `src/settingsdialog.cpp` |
| Equalizer, compressor, room reverb, stereo widening, image adjust, crop, colour effects (`Property 1=expanded*`) | `src/fxpanels.cpp` |
| Right-click menu (`right-click menu`) | `MainWindow::buildMainMenu`, `src/appstyle.cpp` |
| Theme cards / radio dots (`Frame 151`, `Frame 159`) | `src/widgets.cpp` |
| Home: Continue Watching, Listen Again, Frequently Visited Folders | `src/homepage.cpp` |
| Seek bar thumbnail previews | `src/seekpreview.cpp` |
| Music: Songs / Albums / Artists / Genres (tag index) | `src/musicindex.cpp` |
| Waveform seek bar (music, and videos when turned on) | `src/waveform.cpp` |
| Artist pictures (local / Deezer) | `src/artistimages.cpp` |
| Album editor (MusicBrainz, TagLib) and artist picture chooser | `src/musiceditors.cpp` |
| Right-hand player panel: queue, bookmarks, chapters | `src/playerpanel.cpp` |
| Media lengths for the queue header | `src/durations.cpp` |
| Subtitle timeline (drag to sync) and subtitle timing from files or ffmpeg | `src/subtitletimeline.cpp`, `src/subtitlecues.cpp` |
| Per-show presets and default subtitle/audio tracks | `src/showpresets.cpp` |
| Metadata card over the video | `src/mediainfocard.cpp` |
| Add Subtitles: computer / OpenSubtitles / link, per file, folder or playlist (`Add Subtitles` mockups) | `src/subtitleoverlay.cpp`, `src/opensubtitles.cpp` |
| Main menu, Capture Device, Convert / Save, Stream, About | `MainWindow::buildMainMenu`, `src/mediadialogs.cpp` |
| Keyboard Shortcuts dialog (changeable keys) | `src/shortcuts.cpp` |
| Customize Player (drag items into and out of the playback bar) | `src/customizeplayer.cpp`, `ControlBar::rebuild` |
| Profiles (Movies, Shows, Music, Work, your own) | `src/profiles.cpp` |
| Welcome on first launch: folder, theme, keys | `src/onboarding.cpp` |
| Plugins list (read-only) | `PluginsOverlay` in `src/mediadialogs.cpp` |

All settings are functional: effects apply live, the play queue supports
shuffle/repeat, playback positions are remembered (and shown as progress bars),
and recording, snapshots, sleep timer, subtitle sync and styling all work.
**Play Disc** (Ctrl+D) plays DVDs (with menus), Blu-rays, audio CDs and video CDs.

## Keyboard

Space play/pause · S stop · P/N previous/next · ←/→ seek · Ctrl+←/→ jump 1 min ·
↑/↓ volume · M mute · F / F11 fullscreen · Ctrl+\\ hide UI · Ctrl+B sidebar ·
Ctrl+L library/player · R record · Shift+S snapshot · [ ] = speed ·
Ctrl+O open file · Ctrl+Shift+O open files · Ctrl+F open directory · Ctrl+D disc ·
Ctrl+N network stream · Ctrl+C capture device · Ctrl+V clipboard ·
Ctrl+Y save playlist · Ctrl+R convert · Ctrl+S stream · Ctrl+P settings ·
Shift+F1 about · Ctrl+Q quit

Every key can be changed in the main menu → Keyboard Shortcuts.

In a DVD menu, the arrow keys and Enter (or the mouse) pick menu buttons.

## Installing

### Arch Linux (pacman)

Releases are published as a small pacman repository. Add it to
`/etc/pacman.conf` once:

```ini
[konos]
SigLevel = Optional TrustAll
Server = https://github.com/TheDoodAbides87/konos-releases/releases/latest/download
```

then `sudo pacman -Sy konos`. From then on `sudo pacman -Syu` updates Konos with
the rest of the system, and Konos tells you when a new version is out
(Settings → General → Updates).

### Other Linux distributions (AppImage)

Download `Konos-<version>-x86_64.AppImage` from the
[releases](https://github.com/TheDoodAbides87/konos-releases/releases/latest),
make it executable (`chmod +x`) and run it. It has libVLC and Qt inside. When a
new version is out, Konos offers **Update Now**: it downloads the new AppImage
over the old one, and the update starts the next time you open Konos.

### macOS and Windows

Planned: a `.dmg` and a Windows installer (`.exe`) that update themselves from
the same releases.

## Building

Dependencies (Arch): `qt6-base qt6-svg qt6-wayland taglib libvlc
vlc-plugins-base vlc-plugin-ffmpeg vlc-plugin-ass vlc-plugin-freetype` and a
C++17 compiler. Optional: `ffmpegthumbnailer` or `ffmpeg` for library
thumbnails and the subtitle timeline.

```sh
mkdir build && cd build
qmake6 ../konos.pro            # add CONFIG+=omarchy to follow the Omarchy theme
make -j$(nproc)
./konos [files or URLs…]
```

### Arch package and releases

```sh
./packaging/release.sh --build-only   # → dist/: the pacman package + repo database, and the AppImage
./packaging/release.sh                # build, then publish to the releases repo (asks first)
./packaging/build-appimage.sh         # just the AppImage → dist/Konos-<version>-x86_64.AppImage
```

The package is built from the last commit (`git archive HEAD`; with
`--build-only` and uncommitted changes, from the working tree) by
`packaging/arch/PKGBUILD`, with `CONFIG+=omarchy`. Publishing needs `gh`
logged in; the release is not marked as a pre-release, because
`releases/latest` (which pacman and the update check read) skips those. The
version comes from `src/version.h`; keep `pkgver` in the PKGBUILD in step
(`0.5.0` + `beta` → `0.5.0beta`, which pacman sorts before `0.5.0`).

The source repository is private; built packages go to the public
`TheDoodAbides87/konos-releases` repository, which holds only releases, so the
app and pacman can download them without a login.

### OpenSubtitles

Subtitle downloads use the OpenSubtitles.com REST API, which needs an API key:
sign up at opensubtitles.com, then create one under Profile → API consumers.
Paste it into Settings → Subtitles, or build it in:

```sh
OPENSUBTITLES_API_KEY=yourkey qmake6 ../konos.pro
```

A built-in key can be read out of the binary, so only do that for builds you
keep to yourself.

### Omarchy theme

`qmake6 CONFIG+=omarchy` (the Arch package is built this way) makes Konos take
its colours from the current Omarchy theme
(`~/.local/state/omarchy/current/theme/colors.toml`) and follow theme changes
while running. Settings → Interface gains an **Omarchy** theme and an
**Omarchy theme** accent swatch; Dark, Light and System are still there. Each
colour role keeps the stock palette's contrast, measured along the theme's
background-to-foreground range, and the volume bar uses the theme's green.
Outside Omarchy it falls back to the stock dark look.

## Notes

- Video is rendered through libVLC's memory callbacks as planar YUV and converted
  and scaled by an OpenGL shader, so the same code path works on X11 and Wayland.
- Hardware decoding uses VA-API (`vaapi_drm`; frames are copied back from the
  GPU). In this mode VLC 3 can't draw subtitles or DVD menu highlights onto GPU
  frames or run its video filters on them, so discs, media with a subtitle track
  showing and media with video effects on decode in software. The switch happens
  automatically (a brief reload when subtitles or an effect are turned on).
  Systems without VA-API decode in software.
- The public libVLC 3 API only exposes the equalizer. The other effects are
  driven through libvlccore object variables (`src/vlcvars.cpp`); this ties the
  build to VLC 3.0.x.
- Developer screenshots: build with `qmake6 CONFIG+=devhooks` to enable the
  `KONOS_SCRIPT` UI automation hook (not present in normal builds).
- Settings and the library carry over from OmaVideo (and "VLC Redesign") on
  first launch.

## Credits

- **Design:** [Juxtopposed](https://www.youtube.com/@juxtopposed), in
  [*I Redesigned VLC After 25 Years*](https://youtu.be/ERFeOgqk324). The
  mockups themselves are not included in this repository.
- **Playback engine:** [VLC / libVLC](https://www.videolan.org/vlc/) by
  VideoLAN and the VLC contributors (LGPL 2.1 or later). The AppImage bundles
  libVLC and its plugins unchanged.
- **Interface toolkit:** [Qt 6](https://www.qt.io/) (LGPL 3); tags are read and
  written with [TagLib](https://taglib.org/).
- "VLC" and the VLC cone are trademarks of VideoLAN. Konos is an independent
  project and uses neither as its name or icon.
