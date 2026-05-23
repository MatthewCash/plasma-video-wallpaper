# Plasma Video Wallpaper

A small Plasma 6 wallpaper plugin that plays a video with mpv.

It uses libmpv directly instead of Qt Multimedia, so playback goes through mpv's normal GPU/video stack.

## Build

With Nix:

```sh
nix build
```

Without Nix, build it like a normal CMake/KDE package. You need Qt 6, Plasma 6 headers, ECM, and libmpv.

## Configure

The wallpaper plugin id is:

```text
simplevideowallpaper
```

Set the video path in the wallpaper config under `General.source`.

Example value:

```text
file:///path/to/video.mp4
```

Local `file://` URLs are converted to normal paths before being passed to mpv.

## Notes

Each screen gets its own wallpaper instance. On a multi-monitor setup that means each screen runs its own mpv player.

Audio is disabled. The video loops forever.

