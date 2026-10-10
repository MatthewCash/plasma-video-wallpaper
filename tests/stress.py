#!/usr/bin/env python3
"""Exercise the real Qt/EGL/mpv lifecycle outside plasmashell.

Run inside a graphical session with a known VA-API-decodable local video.
No desktop configuration or existing process is modified.
"""
import argparse
import os
import re
import selectors
import signal
import subprocess
import time


def run(binary, source, name, duration=1500, extra=(), env_extra=None,
        terminate_at=None, require_video=False, require_error=False):
    env = os.environ | {
        "LC_NUMERIC": "C",
        "QT_QPA_PLATFORM": "wayland",
        "QSG_RHI_BACKEND": "opengl",
        "QT_FORCE_STDERR_LOGGING": "1",
        "QT_LOGGING_RULES": "*.debug=false;*.info=true",
    } | (env_extra or {})
    process = subprocess.Popen(
        [binary, source, str(duration), *extra], env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    output = bytearray()
    sent = False
    deadline = time.monotonic() + 25
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    try:
        while True:
            if time.monotonic() > deadline:
                process.kill()
                raise AssertionError(f"{name}: shutdown hung\n{output.decode(errors='replace')}")
            ready = selector.select(0.1)
            if not ready:
                if process.poll() is not None:
                    output.extend(process.stdout.read())
                    break
                continue
            data = os.read(process.stdout.fileno(), 65536)
            if not data:
                break
            output.extend(data)
            if terminate_at and not sent and terminate_at.encode() in output:
                process.send_signal(signal.SIGTERM)
                sent = True
        code = process.wait(timeout=10)
    finally:
        selector.close()
        if process.poll() is None:
            process.kill()
            process.wait()
    text = output.decode(errors="replace")
    try:
        assert code == 0, f"exit {code}"
        assert not terminate_at or sent, f"never reached {terminate_at}"
        assert "video wallpaper runtime drained" in text, "missing final drain"
        assert "quarantined" not in text, "lost render context"
        assert "render context/thread changed" not in text
        assert "runtime error:" not in text
        assert "ERROR: AddressSanitizer" not in text
        started = re.findall(r"video wallpaper session (\d+) worker started", text)
        destroyed = re.findall(r"video wallpaper session (\d+) client destroyed", text)
        assert sorted(started) == sorted(destroyed), f"unretired workers: {started} vs {destroyed}"
        if require_video:
            assert "decoder: vaapi" in text, "no direct hardware decoding"
            assert "Using EGL dmabuf interop" in text, "no EGL DMA-BUF interop"
        if require_error:
            assert 'test status "error"' in text, "error was not exposed to QML"
    except AssertionError as error:
        raise AssertionError(f"{name}: {error}\n{text}") from error
    print(f"PASS {name}: {len(started)} sessions retired", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("source")
    parser.add_argument("--iterations", type=int, default=5)
    args = parser.parse_args()
    run(args.binary, args.source, "two-window hardware playback", 3000, require_video=True)
    run(args.binary, args.source, "source changes, removal, hide and recreate", 6000,
        extra=("--churn",), require_video=True)
    run(args.binary, args.source, "late application-owned windows", 2000,
        extra=("--late-window",), require_video=True)
    run(args.binary, args.source, "hide and resume with persistent scene graph", 4000,
        extra=("--churn", "--persistent"), require_video=True)
    run(args.binary, args.source, "never-rendered windows", 100,
        extra=("--never-shown",))
    run(args.binary, args.source, "invalid render-node configuration", 500,
        env_extra={"PLASMA_VIDEO_WALLPAPER_RENDER_NODE": "/nonexistent/render"}, require_error=True)
    for i in range(args.iterations):
        run(args.binary, args.source, f"quit before initialization {i}",
            env_extra={"WALLPAPER_TEST_INIT_DELAY_MS": "250"},
            terminate_at="worker started")
        run(args.binary, args.source, f"quit during render creation {i}",
            env_extra={"WALLPAPER_TEST_RENDER_DELAY_MS": "250"},
            terminate_at="render creating")
        run(args.binary, args.source, f"quit immediately after render ready {i}",
            terminate_at="render ready")


if __name__ == "__main__":
    main()
