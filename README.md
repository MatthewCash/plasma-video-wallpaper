# Plasma Video Wallpaper

A small Plasma 6 wallpaper plugin that plays a video with mpv.

It uses libmpv directly instead of Qt Multimedia, so playback goes through mpv's normal GPU/video stack.

## Build

With Nix:

```sh
nix build path:.
```

Without Nix, build it like a normal CMake/KDE package. You need Qt 6 (including
OpenGL, QML and Quick), Plasma 6 headers, ECM, EGL, and libmpv. The implementation
targets Linux, Qt's OpenGL/EGL scene graph, and direct VA-API decoding.

When used as an input of a NixOS flake, make this flake's `nixpkgs` follow the
host's nixpkgs input. The standalone lock file is independent of the host; a
standalone build against it is not a test against the host's Qt/mpv versions.

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

The host must start with `LC_NUMERIC=C`. This plugin does not change the locale
of an already multithreaded Plasma process. Use the same setting for
plasmashell, the lockscreen, and the login manager if using this wallpaper there.

## Hardware decoding

The renderer identifies the DRM render node belonging to Qt's current EGL
display, opens its own FD, and supplies it to libmpv with
`MPV_RENDER_PARAM_DRM_DISPLAY_V2`. It does not borrow Qt's Wayland connection.
Direct `hwdec=vaapi` and EGL DMA-BUF import keep decoded frames on the GPU;
there is no `vaapi-copy`, QImage conversion, or CPU frame readback. GPU conversion
and composition into the wallpaper FBO are still necessary.

If EGL device discovery is unavailable, explicitly set
`PLASMA_VIDEO_WALLPAPER_RENDER_NODE` to the correct `/dev/dri/...` render node.
When EGL identifies the device, an override pointing to another GPU is rejected.
Do not assume that `renderD128` is the right device on every machine.

The item exposes `status`, `errorString`, `hardwareDecoder`, and `retry()` to QML.
An unsupported decode/interop path is reported rather than silently accepted as
software decoding. Runtime hardware-decode fallback is disabled and the actual
decoder is checked after playback starts. Logs identify sessions and the selected
interop, for example:

```text
video wallpaper session 1 libmpv_render/vaapi Using EGL dmabuf interop via GL_EXT_EGL_image_storage
video wallpaper session 1 decoder: vaapi
```

## Lifecycle contract

- Each Qt renderer generation has a fresh mpv client and private render context.
  Recreated items/windows never reuse another renderer's GL state.
- A control worker owns client initialization, commands, events and destruction.
  The Qt render thread exclusively creates/renders/frees the render context
  using its original current GL context.
- Client destruction is forbidden until the renderer has released its lease.
  Cancellation before initialization or during render-context creation is
  handled explicitly, including a renderer that never reaches its first frame.
- Callbacks only set atomic notification state and write to nonblocking eventfds.
  Their userdata and FDs outlive their producers. They never dereference QObjects.
- `aboutToQuit` closes admission and requests retirement, without blocking.
  Qt scene-graph destruction frees render contexts; workers then destroy clients
  without requiring the GUI event loop. An application-owned service joins them
  after Qt's normal render-loop cleanup. No detached threads or queued-to-GUI
  finalizers are used.
- Visibility/exposure changes pause playback. Resource loss starts a fresh
  renderer generation. Source changes and retries are serialized through command
  replies rather than treating a submitted load as successful playback.
- Advanced render control is deliberately disabled: direct VA-API does not
  require it, and a hidden Qt window cannot guarantee continuous render servicing.
- Qt signal connections are disconnected in the derived item destructor, before
  QQuickItem can emit signals during its own destruction.

This covers the normal Qt-owned-window path used by Plasma. In particular,
plasma-manager may apply this wallpaper and immediately restart plasmashell;
the implementation does not assume a minimum startup or playback duration.
Custom `QQuickRenderControl` hosts must invalidate their renderers before final
application cleanup.

An irretrievably lost GL context cannot legally be replaced just to free mpv's
old render context. In that exceptional case the client/render resources are
quarantined until process exit, notifications are disabled, and an error is
logged. Restart the host to reclaim them. This avoids unsafe destruction but
does not make an in-process plugin immune to bugs or GPU resets in the driver.

## Tests

Build the standalone harnesses without replacing or restarting your desktop:

```sh
nix develop path:.
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
LC_NUMERIC=C ctest --test-dir build --output-on-failure
python3 tests/stress.py build/tests/wallpaper-lifecycle /path/to/video.mp4
```

The graphical suite opens two small Wayland windows. It verifies direct VA-API
and DMA-BUF interop, checks all workers retire, exercises source errors, item
removal, hiding and scene-graph recreation, and sends SIGTERM before initialization,
during render attachment, and immediately after render attachment. A late-window
test leaves window destruction until application cleanup, after `app.exec()`.
Injection delays and software test mode are compiled only into the test harness,
not the installed plugin.

To exercise the installed QML module and QML-engine replacement:

```sh
LC_NUMERIC=C QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 \
  build/tests/qml-lifecycle /path/to/package/lib/qt-6/qml /path/to/video.mp4
```

Use `-fsanitize=address,undefined -fno-omit-frame-pointer` in
`CMAKE_CXX_FLAGS` for ASan/UBSan, or a separate build with `-fsanitize=thread`
for the native session tests. Run UBSan with `UBSAN_OPTIONS=halt_on_error=1`.
The graphical churn test on Qt 6.11.2 has also produced a 96-byte LeakSanitizer
report allocated in `QWaylandDisplay::createSurface`; this is separate from
session ownership accounting. Leak checking remains enabled for native session
tests; graphical memory-safety runs can use `ASAN_OPTIONS=detect_leaks=0` to
test ASan/UBSan independently of that report. Do not interpret that run as a
clean LeakSanitizer result.

For development inside another flake, override this flake's nixpkgs input with
the host's pinned nixpkgs when running these tests. To deploy changed local-path
sources, update the parent flake's `plasma-video-wallpaper` lock entry, then use
your normal rebuild/activation procedure. Merely rebuilding an unchanged locked
input can keep using the old plugin.
