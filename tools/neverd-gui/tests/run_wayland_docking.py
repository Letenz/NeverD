#!/usr/bin/env python3
"""Run docking mouse regressions inside a disposable headless GNOME desktop.

Requires gnome-shell, dbus-run-session and gdbus. This never injects input into
an existing desktop: the unsafe Shell Eval API and virtual pointer live only
in the private compositor and D-Bus session created here.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale", default="1")
    parser.add_argument("--inside-session", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("binary", type=Path)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    for tool in ("gnome-shell", "gdbus", "dbus-run-session"):
        if not shutil.which(tool):
            parser.error(f"missing test dependency: {tool}")
    if not args.inside_session:
        return subprocess.call([
            "dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
            "--inside-session", "--scale", args.scale,
            str(args.binary.resolve()), *args.arguments,
        ])
    # Runtime sockets include the application's own subdirectory and name.
    # Keep this prefix short when the caller uses a long temporary root.
    with tempfile.TemporaryDirectory(prefix="nd-") as directory:
        root = Path(directory)
        env = dict(os.environ)
        for key, folder in (("XDG_RUNTIME_DIR", "runtime"),
                            ("XDG_CONFIG_HOME", "config"),
                            ("XDG_CACHE_HOME", "cache"),
                            ("XDG_DATA_HOME", "data")):
            path = root / folder
            path.mkdir(mode=0o700)
            env[key] = str(path)
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        env.update(XDG_SESSION_TYPE="wayland", LIBGL_ALWAYS_SOFTWARE="1")
        with (root / "compositor.log").open("w+") as log:
            compositor = subprocess.Popen([
                "gnome-shell", "--unsafe-mode", "--wayland", "--no-x11",
                "--headless", "--virtual-monitor=2560x1600",
                "--wayland-display=neverd-docking",
            ], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                socket = root / "runtime" / "neverd-docking"
                deadline = time.monotonic() + 15
                while not socket.exists():
                    if compositor.poll() is not None or time.monotonic() > deadline:
                        log.seek(0)
                        print(log.read(), file=sys.stderr)
                        return 1
                    time.sleep(0.1)
                env.update(WAYLAND_DISPLAY="neverd-docking", QT_QPA_PLATFORM="wayland",
                           QT_SCALE_FACTOR=args.scale, NEVERD_TEST_GNOME_POINTER="1")
                return subprocess.call([str(args.binary.resolve()), *args.arguments], env=env)
            finally:
                compositor.terminate()
                try:
                    compositor.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    compositor.kill()
                    compositor.wait()


if __name__ == "__main__":
    raise SystemExit(main())
