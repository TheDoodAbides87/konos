# Changelog

## 0.7.2 (beta) — 2026-11-08

### Fixed
- **AppImage: Stream → Chromecast works.** The AppImage left out the
  Chromecast plugins and the one that finds Chromecasts on the network; only
  the Arch package had them.
- **AppImage: the audio visualizer works.** Its plugins were left out too.
- **The playback bar fits narrow windows.** When the items chosen in
  Customize Player don't fit, the least important ones are hidden (names,
  thumbnail and dividers first; Play always stays) so the seek bar keeps
  room.
- Messages say "1 item" / "2 items" rather than "item(s)".

### Changed
- Playback positions of files that were deleted are forgotten (a file on a
  drive that isn't plugged in keeps its position).
- Reading songs' tags and files' lengths shares one background engine
  instead of two.
- Tidier code: shared dialog parts, test-only commands in their own file, no
  compiler warnings.

## 0.7.1 (beta) — 2026-11-08

Fixes and speed-ups from an audit of the code.

### Fixed
- **Saved data can't be lost to an interrupted save.** The library (playlists,
  pins, playback positions, podcasts, saved streams), show presets, bookmarks,
  downloaded subtitles, album covers and caches are written to a temporary
  file that replaces the old one only when it's complete. Before, a crash, a
  full disk or a power cut mid-save could leave the file cut short, and Konos
  would then start empty and save over it. A file that still can't be read is
  now kept as `<name>.broken-<date>`, and Konos says so when it starts.
- **Feeds that failed to load stayed on "Loading…" for good.** A channel
  list, podcast or radio list that couldn't be reached now shows why, and is
  tried again next time.
- **The AppImage's Update Now checks the download** before installing it: its
  size and SHA-256 must match what GitHub lists for the release, and it must
  be a program. A damaged download is refused and the installed Konos is left
  as it was.

### Changed
- **Less work while something plays.** Playback positions are saved only when
  they've moved (not every 5 seconds while paused), and Home rebuilds only
  when a video joins or leaves Continue Watching.
- **Smoother scrolling in big libraries.** Covers and thumbnails are read and
  decoded in the background instead of while the list is drawn, and each is
  scaled once to the size it's shown.
- **Memory stays bounded.** Thumbnails are kept up to 96 MB and radio and
  podcast artwork up to 48 MB, rather than without limit. Feeds over 24 MB and
  artwork over 8 MB aren't downloaded ("This link is too big to be a channel
  list or feed").

## 0.7.0 (beta) — 2026-11-07

Group E, the last group of changes from the redesign video: customizing
(18:24–19:52).

### Added
- **Customize Player** (18:56): main menu → Customize Player shows every item
  the playback bar can have, in a box. Drag them into the bar (left or right
  of the seek bar), along it to re-order them, and back into the box (or
  right-click) to take them off. Besides the usual buttons there are Stop,
  Playback Speed, Forward / Backward 10s and 15s, Repeat, Shuffle, Record,
  Snapshot, Picture in Picture, Fullscreen, Aspect Ratio, Go Home, Replay,
  Chapters, Bookmark, Bookmarks, Playlists, Properties, Favorite (pins what's
  playing to the sidebar), Settings and Quit, the file, folder and artist
  names, a thumbnail, spacers and dividers. Reset to Default puts it back.
- **Profiles** (19:24): main menu → Profile (Movies, Shows, Music, Work, or
  New Profile…). Each profile keeps its own settings, effects, keys, playback
  bar and theme; switching saves the one in use and loads the other. Library
  folders, the window, sign-ins and update checks are shared. A new profile
  starts as a copy of the settings in use.
- **Welcome** (19:44): the first time Konos opens, three steps: a media folder
  to start with, the theme and accent colour (the app behind changes as you
  pick), and the basic keyboard shortcuts. Skip leaves everything as it is.
  It doesn't appear for anyone updating.
- **Plugins** (18:24): main menu → Plugins & Extensions → Plugins (and
  Settings → Extensions → Show All Plugins) lists every VLC plugin with its
  capability and score, searchable and filtered by capability. Read-only:
  libVLC can't install VLC add-ons, skins or Lua extensions.

## 0.6.0 (beta) — 2026-11-07

Group D of the changes from the redesign video: menus and dialogs
(12:56–13:04, 15:56–17:32).

**VMP is now Konos** (Konos Media Player), after the Greek word for cone. The
package, app, settings folder and GitHub repositories have the new name; your
settings and library carry over on first launch. On Arch, `sudo pacman -Syu`
swaps `vmp` for `konos`. Rename `[vmp]` to `[konos]` in `/etc/pacman.conf` and
point it at `konos-releases` (see the README) when convenient: the old entry
keeps working for now.

### Added
- **The main menu from the video** (12:56): the title bar's menu now lists
  Open File, Open Multiple Files, Open Directory, Open Disc, Open Network
  Stream, Open Capture Device, Open Location from Clipboard, Open Recent,
  Save Playlist to File, Convert / Save, Stream, Keyboard Shortcuts, Plugins
  & Extensions, Settings and About, with the video's keys. Right-clicking the
  video still gives the playback menu.
- **Open Capture Device**: a camera (with sound from any input), the screen
  (X11 sessions only: VLC 3 can't capture a Wayland screen), or a sound input.
- **Open Location from Clipboard**: copied files play at once; copied
  addresses open in the network stream dialog to check first.
- **Save Playlist to File**: the queue as `.m3u8` or `.xspf`.
- **Convert / Save**: re-encode files to H.264 or H.265 MP4, Theora OGG, MP3,
  FLAC, Ogg Vorbis or WAV, optionally scaled down; several files in a row,
  with progress. The originals stay as they are.
- **Stream**: send what's playing (or a file) over HTTP to other devices on
  the network (re-encoded so anything can play it, or as it is), or cast it
  to a Chromecast.
- **Keyboard Shortcuts** (15:56): every shortcut in one searchable dialog,
  each with a main and a second key that can be changed; a key taken from one
  action is removed from the other. Settings → Controls lists the current keys.
- **About Konos**: version, credits, licences.
- **Advanced Options on every settings page** (16:16, 18:48):
  - Interface: mini player and queue beside the library.
  - Privacy: radio and podcast directories on or off.
  - Controls: what double-clicking the video does; sideways scrolling.
  - Playback: go back a few seconds when resuming; skip remembering
    positions in short items.
  - Video: drop late frames.
  - Audio: keep the pitch at other speeds, ReplayGain, audio system.
  - Subtitles: text encoding for old files; extra subtitle folders.
  - Library: show hidden files.
  - Network: HTTP proxy; reconnect dropped streams.
  - Performance: decoding threads; faster, rougher decoding.
  - Recording: snapshot size and names.
  - Extensions: VLC's plugin folder.

### Changed
- **Subtitle file matching in words** (17:28): "Load subtitle files
  automatically" is now Off / Any title / Contains title / Title with extra
  characters / Exact title match, with an example of each. "Off" carries over
  from the old switch.
- Keyboard shortcuts in the redesign's places: Ctrl+F opens a directory,
  Ctrl+C a capture device, Ctrl+V the clipboard, Ctrl+Y saves the playlist,
  Ctrl+R converts, Ctrl+S streams, Shift+F1 is About.

## 0.5.0 (beta) — 2026-11-07

Group C of the changes from the redesign video: the library (10:27–13:44),
and the move from OmaVideo to VMP.

### Renamed
- **OmaVideo is now VMP (Vibes Media Player)**: it plays music as much as
  video. The program is `vmp`; settings, library and presets move over from
  OmaVideo on first launch. New icon: a blocky traffic cone stood on its tip
  with a V cut into it.

### Added
- **Sorting like the video** (10:42): Videos and other lists sort by
  Unwatched First, Watched First, Newest First, Oldest First, Title A–Z or
  Z–A (also Largest and Longest First). Every list remembers its order
  between runs.
- **Details view** (10:27): the view button now goes grid → list → details.
  Details shows columns (length, size, modified, progress; for music artist,
  album, year and length), and clicking a column title sorts by it.
- **More in the ⋯ menu** (10:48): Add Files (copied into the open folder, or
  added to the open playlist), Add Folder, **Edit Subtitle Tracks** and **Edit
  Audio Tracks** for a show's folder, **Resync** and **Info**. The track
  editors set the folder's own default track and attach a subtitle or audio
  file to each episode; **Find Automatically** matches files by episode
  number (S01E05, 1x05…) in the folder and the folders inside it, so a
  `Subs/` folder just works.
- **Info** on files and folders (right-click → Info…): size, length,
  progress, tags, attached files; for folders, what's inside and how much is
  watched.
- **Mini player** (11:45): browsing the library while something plays shows
  the video itself (or the cover) in a corner card. Click it to go back;
  drag it to another corner; ✕ hides it until the next item. The **queue**
  button now opens the queue beside the library instead of leaving it.
- **New Playlist dialog** (13:19): a name and its files, dragged in from the
  library or the file manager, or picked with Add Files / Add Folder.
- **Sidebar pins** (13:35): right-click a folder, playlist, album, artist,
  file, channel list or podcast → Pin to Sidebar; pins show under the
  sections (and as icons in the player's rail). Right-click a pin to unpin
  or move it.
- **Network** (12:24, 13:04): **Open Network Stream** takes several addresses
  at once and can save them as a playlist. A link to an M3U channel list
  opens its channels (next/previous zaps through them); a podcast feed opens
  its episodes. **Radio** lists popular internet radio stations from the
  radio-browser.info community directory, with search; **Podcasts** searches
  podcasts by name, lists your subscriptions and their episodes (which
  resume where you left off).
- **Update check**: once a day (Settings → General → Updates, on by default)
  VMP asks GitHub for the latest release and shows a notice when a newer one
  is out; **Check Now** checks straight away. The AppImage updates itself
  (**Update Now**); the Arch package updates with `pacman -Syu`.
- **MIT licence.**

### Changed
- **Packaging**: the Omarchy build is now an Arch package
  (`packaging/arch/PKGBUILD`) published as a pacman repository on the public
  releases repo, so `pacman -Syu` keeps VMP up to date. Other Linux
  distributions get the AppImage (`packaging/build-appimage.sh`), attached
  to the same releases. `packaging/release.sh` builds and publishes both. A
  macOS `.dmg` and a Windows installer are planned.

## 0.4.0 (beta) — 2026-11-03

Group B of the changes from the redesign video: subtitles, audio and video
settings while watching (6:54–9:11).

### Added
- **Subtitle timeline** (6:54): a strip above the controls showing when each
  subtitle line appears around the current moment, over the sound's waveform.
  Drag it sideways to move the subtitles earlier or later; scroll to zoom;
  Reset clears the delay. Works for subtitle files (.srt, .vtt, .ass/.ssa)
  and, with ffmpeg installed, text tracks inside the video. Turn it on in
  Settings → Playback → Show, or in the Subtitles menu.
- **Subtitle appearance** (7:24–7:40): text size as **Relative** (VLC's named
  sizes) or **Exact** pixels; outline thickness; background opacity;
  position (raise the subtitles from the bottom, applies live); and **Use the
  Subtitles' Own Styling** — off, styled .ass tracks (anime) are swapped for
  a plain copy that takes your look instead.
- **Show presets** (7:40–7:55): save the subtitle look and timing, the default
  tracks and the audio delay under a name ("Save as New Preset…" in the
  Subtitles or Audio menu). The preset is linked to the show's folder and
  comes back for every episode in it (or in folders below it); other folders
  get your normal settings back.
- **Default tracks** (7:55–8:41): Subtitles → Defaults → *Turn On
  Automatically* and Audio → Defaults → *Pick Automatically*: a language,
  the Nth track, or None, picked for every episode. Saved in presets too.
- **Equalizer preset curves** (8:41): each preset in the list shows its curve.
- **Video transforms** (8:56): rotate (0/90/180/270°), flip horizontally or
  vertically, zoom (125–300%), done while drawing so they apply instantly.
  **Stream Resolution** picks the quality of online streams. **HDR to SDR**
  fixes HDR videos that look washed out and grey.
- **Show options** (9:11), in Settings → Playback: Subtitle Timeline,
  **Waveform on Seek Bar** for videos (music always had it), **Metadata** — a
  card with the title and picture/sound details (size, codecs, frame rate,
  language, file size) — and **Audio Visualizer** for music (Spectrum,
  Oscilloscope, Spectrometer, VU Meter, Goom) in place of the cover.
- **Play and ⋯ buttons on album tiles**: hovering an album, artist, genre or
  playlist tile in the grid shows a play button (bottom left), which adds it to
  the end of the queue and starts playing it, and a ⋯ button (bottom right)
  with Add to Playlist, Play, Shuffle, Play Next, Play Later, Edit Album Info,
  Go to Artist (from Albums) and Show in Folder. Clicking the cover itself
  still opens the album's tracks without playing. The right-click menu has the
  same items; Play Next is also on playlists.

### Changed
- Settings → Subtitles: "Text background" became "Use the subtitles' own
  styling"; the background is now an opacity in the Subtitles menu.
- Dropdown lists can show wider pictures (the equalizer curves).
- The volume slider is a rounded wedge with a dark outline, filled green →
  orange → red as it gets louder (red is the boost past 100 %); grey when
  muted.

### Fixed
- The title and artist under album art (and other text drawn on the video
  area) came out scrambled after a video had played: the video upload left
  an OpenGL setting changed that Qt's text drawing relies on.

### Not done
- Reverse playback (9:11): libVLC 3 can't play backwards.

## 0.3.0 (beta) — 2026-11-03

Group A of the changes from the redesign video ("I Redesigned VLC After 25
Years"): the player itself.

### Added
- **Fullscreen bars on hover** (video 3:36–4:00): in fullscreen the title bar
  (title, picture in picture, exit fullscreen, close) and the controls only
  appear when the pointer comes near the top or bottom edge, and go again soon
  after it moves away. Both show for a moment when fullscreen starts.
- **Picture in picture** (3:51): a new title bar button turns the window into
  a small 480-pixel-wide video with no other UI. On Hyprland it's also
  floated, pinned to every workspace and placed in the bottom-right corner
  (both the new Lua dispatchers and the classic ones are supported). The
  title shows on hover; double-click, Esc or the button go back.
- **"Pressed …" in on-screen messages** (15:16–15:24): messages triggered by a
  keyboard shortcut name the key underneath (e.g. "Aspect ratio: 4:3 /
  Pressed A"). Settings → Subtitles → On-Screen Messages turns it off.
  New shortcut messages: seeking (Forward 10 s / Back 1 min), mute, and VLC's
  **A** (cycle aspect ratio), **V** (cycle subtitles) and **B** (cycle audio
  track) keys, which are new.
- **Queue header** (2:48–2:52, 20:20): "3 items · 1h 12m" with the total
  length, plus repeat (off → all → one), shuffle, remove selected and add
  files buttons. Lengths come from the music index, playback history, or a
  quick background read of the file (cached).
- **Drag and drop into the queue** (2:50, 11:36–11:52): drag files from the
  library or a file manager onto the queue to add them where they're dropped;
  drag queue items to reorder them. The current item keeps playing.
- **Track and device lists** (4:32–4:40): the Audio menu starts with its
  tracks and an **Output Device** list (with refresh) to switch between
  speakers, headphones and so on; the choice is remembered. The Subtitles menu
  lists **Embedded Tracks** and **External Tracks** (subtitle files, found
  next to the video or added with +) separately.

### Changed
- Clicking a file in the library now selects it instead of playing it;
  double-click or Enter plays it. A double-clicked video plays on its own
  (music tracks still queue the rest of their album). Folders, albums and
  playlists still open with one click.
- Several files can be selected at once (Ctrl-click, Shift-click, Ctrl+A).
  Right-click the selection for **Replace Queue**, **Add to Queue** and
  **Add to playlist**, or drag it onto the queue. In grid view, selected
  tiles get a check mark.
- Adding to an empty queue starts playing and switches to the player, as
  Replace Queue does. Adding while something plays still just adds.
- In the queue, the same selection removes several items at once (− button
  or right-click) and drags them to a new spot together.
- The resume prompt ("Resumed at 12:34 · Start over", "Continue from…?") sits
  just above the playback controls instead of at the top (13:56–14:04).

### Fixed
- Playing a video with a subtitle file next to it could freeze the app when
  it switched to software decoding (needed to draw subtitles): libVLC's video
  setup asked for the current track while playback was being restarted.

## 0.2.0 (beta) — 2026-10-30

First beta, and the first release as **OmaVideo** (previously "VLC Redesign").

### Renamed
- The app is now **OmaVideo**: binaries `omavideo` / `omavideo-omarchy`,
  AppImages `OmaVideo-…` / `OmaVideo-Omarchy-…`, window titles, About and the
  user agent. Settings, library and history from VLC Redesign are copied over
  on first launch (the old ones are left in place).
- New icon and in-app mark: the Omarchy "O" built as a traffic cone, drawn in
  the accent colour inside the app.
- Default snapshot and recording folders are now "OmaVideo Snapshots" and
  "OmaVideo Recordings" (folders you picked yourself are kept).

### Added
- **Black borders in fullscreen**: the bars around the picture are pure black
  in fullscreen, whatever the theme (Omarchy themes used their own dark grey).
- **Audio and Subtitles menus of their own**: the control bar's audio and
  subtitle buttons open a menu with just those settings and a title (sized to
  fit, with "Open Audio/Subtitle Settings" at the bottom). The settings button's
  panel now has only the Playback and Video tabs, plus the effects presets.
- **Player panel on the right** with three tabs: **Queue**, **Bookmarks** and
  **Chapters**. The queue button in the control bar opens it on the Queue tab
  (again closes it); it no longer takes over the left side panel. The open tab
  is remembered.
- **Bookmarks**: + marks the current moment ("Title 1", "Title 2", …, shown as
  00:00:42:023), − removes the selected one; click one to jump there,
  right-click or F2 to rename, Delete to remove. Also Playback → Add Bookmark
  (Ctrl+Shift+B). Kept per file in `bookmarks.json`.
- **Chapters** tab: the media's chapters with their start times, the current
  one in the accent colour; click to jump.
- **Play Disc** (Open Media menu, library "+" menu, Ctrl+D): DVDs with working
  menus (arrow keys/Enter or mouse), Blu-rays, audio CDs (one queue entry per
  track) and video CDs. With several drives it becomes a submenu.
- Playback menu: **Disc Menu**, **Title** and **Chapter** submenus (chapters also
  work for chaptered files such as MKV). Next/Previous step through chapters on
  discs.
- Errors that VLC modules report as dialogs (for example a Blu-ray needing AACS
  keys) now show in the app.
- Settings → Extensions shows the app version.

- **Music menu**: Music now opens on Add, Songs, Albums, Artists, Genres and
  Folders. Songs are listed as "title - artist" with their covers; albums,
  artists and genres open to their tracks (albums in track order). Tags and
  embedded covers are read in the background with libVLC and kept in
  `music.json`, so only new or changed files are read again. Recent, the play
  queue and playlists show songs the same way.
- **Artists → albums → songs**: opening an artist shows their albums; opening
  an album shows its songs. The Sort menu on Artists has two parts: **Group
  by** Artist or Album Artist (folds "feat." credits and compilations into the
  album artist), and **Album Year** Ascending or Descending for the albums on
  each artist's page.
- **Artist pictures**: a dropdown on Artists switches between **Album cover**
  (default) and **Artist image**. Artist images come from an `artist.jpg`
  (or .png/.webp) in the artist's folder, otherwise from Deezer's public
  catalogue, cached on disk. Only artist names are sent, and only after you
  choose "Artist image"; artists without a picture keep their album cover.
- **Choose Artist Image…** (right-click an artist while showing artist
  images): pick the right picture from Deezer's matches for the name (or a
  different spelling), or use a file. Your choice wins over artist.jpg and the
  automatic one; **Automatic** in the same window undoes it.
- **Edit Album Info…** (right-click an album): edit album, album artist,
  year, genre and each song's number, title and artist. **Fill in from
  MusicBrainz** searches releases (year, country, format, track count); picking
  one fills everything in, using the original release year, and can save its
  front cover as cover.jpg. **Save to Files** writes the tags with TagLib
  (MP3, FLAC, Ogg, Opus, M4A, …) and the library re-reads them.
- Dropdown lists no longer cut off their items (e.g. "Album cover").
- Music views use **square tiles**, so album art and artist pictures keep
  their shape, with the year and song count under each album.
- **Music sorting**: each Music view has its own sort options, remembered
  between runs. Albums sort by name, album artist or year (oldest or newest
  first); Songs by title, artist, album, year or date. The index now stores
  years, so existing music is read once more after updating.
- **Waveform seek bar** for music: while a song plays, the seek bar shows its
  waveform, with the played part in the accent colour. It takes a fraction of
  a second per song and is cached, so each file is decoded once.
- **New Home page**: **Continue Watching** (unfinished videos with progress
  bars), **Listen Again** (recent tracks with cover art, title and artist,
  played as a list) and **Frequently Visited Folders** (ranked by how often you
  play from or open them, topped up with your library folders). Each row shows
  as many cards as fit; its "›" header opens the full section, the search box
  filters the cards, and right-click has the usual actions. Titles, artists and
  covers are remembered as tracks play; clearing history clears them too.
- **Add Subtitles** (subtitle panel → Find Online…, or right-click → Subtitles →
  Download Subtitles…), after the redesign's mockups: pair every video in
  **this file / its folder / the play queue** with a subtitle **from the
  computer, OpenSubtitles or a link**, shown as video ‑ ‑ ‑ subtitle rows.
  - OpenSubtitles searches by file hash plus a title, season and episode taken
    from the file name; the best match is pre-picked (click a row for other
    releases). Subtitles are saved next to the video as
    `<video>.<lang>.srt`, so they load automatically next time, and the one
    for the playing video is added at once.
  - Uses the OpenSubtitles.com REST API (opensubtitles.org's own API is closed
    to third-party apps; the subtitles are the same). Needs a free API key in
    Settings → Subtitles, or one built in with `OPENSUBTITLES_API_KEY`.
    Signing in is optional and only the session token is stored.
- **Seek previews**: hovering or dragging on the seek bar shows a thumbnail of
  that moment above the bar, with the time. Frames come from a hidden,
  paused libVLC player that seeks exactly (about 70 ms per frame at 1080p,
  300 ms at 4K; requests made meanwhile collapse into the latest one) and
  closes 15 s after the pointer leaves. Local video files only; audio, discs
  and streams show just the time.
- File pickers (Open File, Open Folder, subtitles, library folders, settings
  paths) match the app's theme, including Omarchy themes: app icons, Home /
  Videos / Music / Downloads / Computer places, accent Open button, readable
  columns. The whole app also gets a matching palette, so nothing falls back
  to Qt's light defaults.
- **Omarchy edition** (`build-appimage.sh --omarchy`): a separate AppImage that
  takes its colours and accent from the current Omarchy theme and updates live
  when the theme changes. Light themes such as Catppuccin Latte work too.

### Performance
- Hardware decoding now actually works (VA-API). libVLC 3's automatic choice
  refused it with memory output, so all video was decoded in software.
- Frames are passed as planar YUV and converted/scaled on the GPU instead of
  being converted to RGB and scaled on the CPU. Textures are only uploaded when
  a new picture arrives.
- Playback CPU (Ryzen 9 5900X, RX 6700 XT, H.264): 1080p 18% → 6%,
  4K 50% → 15%. Memory: 342 → 238 MB (1080p), 636 → 328 MB (4K).
- Icons are rendered once per colour/size instead of on every repaint.
- Album art is downscaled while decoding instead of after.
- AppImage: 109 → 98 MB and 408 → 315 bundled libraries, by leaving out VLC
  plugins the app can't use (and what they drag in, such as GTK 3 and SDL 3).

### Known limitations
- Media with a subtitle track showing, with video effects on, and discs decode
  in software (VLC 3 can't draw overlays or run filters on GPU frames when the
  app renders the video itself).
- Commercial Blu-rays need `libaacs` and a key database on the host.
- Audio CD and Blu-ray playback haven't been tested on real discs yet.
- English only. Thumbnails need `ffmpegthumbnailer` or `ffmpeg` on the host.

## Initial build — 2026-10-20

The redesign on libVLC 3 (labelled 1.0.0 in the binary; not a release).
