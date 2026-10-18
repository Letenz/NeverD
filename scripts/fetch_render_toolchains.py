#!/usr/bin/env python3
"""Fetch the toolchains check_type_renders.py checks 32-bit and cross
layouts with.

A program's types render for its own target, which is often not the host's:
a 32-bit Linux library's Rust and Python, Windows x86's Python, an ARM
program's C against its C library's headers.  This fetches what checks
them, without root, into one directory (default
~/.cache/neverd-render-toolchains), and writes toolchains.json there, which
check_type_renders.py --toolchains reads:

- rustup with the standard library of each cross target (`rustc --target`
  needs no linker for `--emit=metadata`);
- Python for i386 Linux, from Debian's i386 packages, which runs on an
  x86-64 host with its 32-bit C library (/lib/ld-linux.so.2);
- Python for 32-bit ARM, from python-build-standalone, run by qemu-arm
  against the armhf C library of Debian's cross packages;
- Python for 32-bit Windows, from python-build-standalone, run by Wine
  (wine64 runs 32-bit programs from Wine 9 on) in a prefix of its own;
- the C library headers of armhf and arm64 Linux (Debian's cross packages)
  and of Android (bionic, at the commit the Android libraries were
  generated from).

It needs an x86-64 Linux host, dpkg and apt-get (Debian or Ubuntu) for
the cross packages, and Wine for Windows' Python;
each part that cannot be fetched is reported and left out, and the checker
skips what it would have checked.  Run it again to fetch what is missing;
--force fetches everything again.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

REPOSITORY = Path(__file__).resolve().parents[1]
DEFAULT = Path.home() / ".cache" / "neverd-render-toolchains"

RUSTUP = ("https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/"
          "rustup-init")
# The targets check_type_renders.py compiles Rust for besides the host's.
RUST_TARGETS = ("i686-unknown-linux-gnu", "armv7-unknown-linux-gnueabihf",
                "i686-linux-android", "i686-pc-windows-msvc",
                "x86_64-pc-windows-msvc", "aarch64-apple-darwin")

STANDALONE = ("https://github.com/astral-sh/python-build-standalone/releases/"
              "download/20261013/")
# python-build-standalone's builds, with the SHA-256 its release lists.
PYTHONS = {
    "armv7": ("cpython-3.13.16%2B20261013-armv7-unknown-linux-gnueabihf-"
              "install_only.tar.gz",
              "f7882410b109c150cf455d659680d1aec831a35af127c90c2ce985c07a1a724b"),
    "win32": ("cpython-3.13.16%2B20261013-i686-pc-windows-msvc-"
              "install_only.tar.gz",
              "9426fda0e3534d5ef2d4597316d67ebf95168b0e79eafe2d8d00e3febb375d1f"),
}

DEBIAN = ("https://deb.debian.org/debian", "http://deb.debian.org/debian")
DEBIAN_SUITE = "trixie"
# What Debian's i386 Python needs to run ctypes: the interpreter, its
# standard library, and the libraries those link.
DEBIAN_I386 = ("python3.13-minimal", "libpython3.13-minimal",
               "libpython3.13-stdlib", "libffi8", "zlib1g", "libexpat1")

# The host's cross packages: qemu's user emulators (qemu-user holds the
# static ones on Ubuntu from 24.10, qemu-user-static before and on Debian),
# and the armhf and arm64 C libraries with their headers.
QEMU = ("qemu-user-static", "qemu-user")
CROSS = {
    "armhf": ("libc6-armhf-cross", "libc6-dev-armhf-cross",
              "linux-libc-dev-armhf-cross", "libgcc-s1-armhf-cross"),
    "arm64": ("libc6-arm64-cross", "libc6-dev-arm64-cross",
              "linux-libc-dev-arm64-cross"),
}

BIONIC = "https://android.googlesource.com/platform/bionic/+archive/"


def say(text):
    print(text, flush=True)


def download(url, path, attempts=6):
    """Fetches url into path, again after a broken connection, which some
    networks make of a long TLS transfer."""
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url, timeout=120) as response, \
                    open(path, "wb") as out:
                shutil.copyfileobj(response, out, 1 << 20)
            return
        except OSError as error:
            if attempt == attempts:
                raise
            say(f"     {url}: {error}; again in {attempt * 5} s")
            time.sleep(attempt * 5)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as data:
        for chunk in iter(lambda: data.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command, **options):
    result = subprocess.run(command, capture_output=True, text=True,
                            **options)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, command))} failed:\n"
                           f"{(result.stdout + result.stderr).strip()}")
    return result.stdout


def fetch_rust(root, force):
    rustup_home, cargo_home = root / "rustup", root / "cargo"
    rustc = cargo_home / "bin" / "rustc"
    environment = dict(os.environ, RUSTUP_HOME=str(rustup_home),
                       CARGO_HOME=str(cargo_home))
    if force or not rustc.exists():
        with tempfile.TemporaryDirectory() as scratch:
            init = Path(scratch) / "rustup-init"
            download(RUSTUP, init)
            listed = Path(scratch) / "rustup-init.sha256"
            download(RUSTUP + ".sha256", listed)
            if listed.read_text().split()[0] != sha256(init):
                raise RuntimeError("rustup-init does not match its SHA-256")
            init.chmod(0o755)
            run([init, "-y", "--no-modify-path", "--profile", "minimal",
                 "--default-toolchain", "stable"], env=environment)
    run([cargo_home / "bin" / "rustup", "target", "add", *RUST_TARGETS],
        env=environment)
    return {"rustc": str(rustc), "env": {"RUSTUP_HOME": str(rustup_home),
                                         "CARGO_HOME": str(cargo_home)}}


def fetch_standalone(root, name, force):
    """A python-build-standalone build, unpacked into python-<name>."""
    asset, digest = PYTHONS[name]
    directory = root / f"python-{name}"
    if force or not directory.exists():
        with tempfile.TemporaryDirectory() as scratch:
            archive = Path(scratch) / "python.tar.gz"
            download(STANDALONE + asset, archive)
            if sha256(archive) != digest:
                raise RuntimeError(f"{asset} does not match its SHA-256")
            shutil.rmtree(directory, ignore_errors=True)
            directory.mkdir(parents=True)
            with tarfile.open(archive) as tar:
                tar.extractall(directory, filter="data")
    return directory / "python"


def debian_index(scratch):
    """Debian's i386 package index, and the archive it came from: over
    HTTPS where the network lets it, else HTTP, the packages then checked
    against the index's SHA-256 alone."""
    for archive in DEBIAN:
        url = f"{archive}/dists/{DEBIAN_SUITE}/main/binary-i386/Packages.xz"
        try:
            path = Path(scratch) / "Packages.xz"
            download(url, path, attempts=3)
            return archive, lzma.decompress(path.read_bytes()).decode()
        except OSError as error:
            say(f"     {url}: {error}")
    raise RuntimeError("no Debian archive answered")


def unpack_deb(deb, into):
    run(["dpkg-deb", "-x", deb, into])


def fetch_python_i386(root, force):
    directory = root / "python-i386"
    python = directory / "usr" / "bin" / "python3.13"
    if force or not python.exists():
        with tempfile.TemporaryDirectory() as scratch:
            archive, index = debian_index(scratch)
            shutil.rmtree(directory, ignore_errors=True)
            directory.mkdir(parents=True)
            entries = {}
            for block in index.split("\n\n"):
                fields = dict(re.findall(r"^(\w[\w-]*): (.*)$", block, re.M))
                if fields.get("Package") in DEBIAN_I386:
                    entries[fields["Package"]] = fields
            for package in DEBIAN_I386:
                fields = entries.get(package)
                if not fields:
                    raise RuntimeError(f"Debian {DEBIAN_SUITE} lists no "
                                       f"i386 {package}")
                deb = Path(scratch) / Path(fields["Filename"]).name
                download(f"{archive}/{fields['Filename']}", deb)
                if sha256(deb) != fields["SHA256"]:
                    raise RuntimeError(f"{deb.name} does not match its index")
                unpack_deb(deb, directory)
    return python


def apt_download(packages, scratch):
    """The host's packages, which apt checks against its signed lists."""
    run(["apt-get", "download", *packages], cwd=scratch)
    return sorted(Path(scratch).glob("*.deb"))


def fetch_cross(root, force):
    out = {}
    qemu = root / "qemu" / "usr" / "bin" / "qemu-arm"
    if force or not qemu.exists():
        with tempfile.TemporaryDirectory() as scratch:
            for package in QEMU:
                try:
                    debs = apt_download([package], scratch)
                    break
                except RuntimeError:
                    continue
            else:
                raise RuntimeError("apt has neither qemu-user-static nor "
                                   "qemu-user")
            for deb in debs:
                unpack_deb(deb, root / "qemu")
    out["qemu-arm"] = str(qemu)
    for arch, packages in CROSS.items():
        triple = "arm-linux-gnueabihf" if arch == "armhf" else \
            "aarch64-linux-gnu"
        sysroot = root / arch / "usr" / triple
        if force or not (sysroot / "include").exists():
            with tempfile.TemporaryDirectory() as scratch:
                for deb in apt_download(packages, scratch):
                    unpack_deb(deb, root / arch)
        out[arch] = str(sysroot)
    return out


def bionic_commit():
    """The bionic commit the Android libraries were generated from, which
    their header names."""
    header = REPOSITORY / "lib" / "types" / "headers" / "AndroidX86.h"
    match = re.search(r"bionic at commit\s*(?://\s*)?([0-9a-f]{40})",
                      header.read_text(encoding="utf-8"))
    if not match:
        raise RuntimeError(f"{header} names no bionic commit")
    return match.group(1)


def fetch_bionic(root, force):
    directory = root / "bionic" / "libc"
    commit = bionic_commit()
    marker = root / "bionic" / "commit"
    if force or not marker.exists() or marker.read_text().strip() != commit:
        with tempfile.TemporaryDirectory() as scratch:
            archive = Path(scratch) / "libc.tar.gz"
            download(f"{BIONIC}{commit}/libc.tar.gz", archive)
            shutil.rmtree(directory, ignore_errors=True)
            directory.mkdir(parents=True)
            with tarfile.open(archive) as tar:
                tar.extractall(directory, filter="data")
        marker.write_text(commit + "\n")
    return str(directory)


def prepare_wine(root, force):
    wine = shutil.which("wine64") or shutil.which("wine")
    if not wine:
        raise RuntimeError("no wine64 or wine")
    prefix = root / "wineprefix"
    environment = wine_environment(prefix)
    if force or not (prefix / "system.reg").exists():
        prefix.mkdir(parents=True, exist_ok=True)
        run([wine, "wineboot", "-i"], env=environment)
        run(["wineserver", "-w"], env=environment)
    return wine, prefix


def wine_environment(prefix):
    environment = {key: value for key, value in os.environ.items()
                   if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
    environment.update(WINEPREFIX=str(prefix), WINEDEBUG="-all",
                       WINEDLLOVERRIDES="mscoree,mshtml=")
    return environment


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.
                                     RawDescriptionHelpFormatter)
    parser.add_argument("--into", type=Path, default=DEFAULT,
                        help=f"the directory to fetch into (default {DEFAULT})")
    parser.add_argument("--force", action="store_true",
                        help="fetch everything again")
    arguments = parser.parse_args()
    root = arguments.into.expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest = {"python": {}, "sysroots": {}}
    failed = []

    def step(name, fetch):
        say(f"== {name}")
        try:
            fetch()
            say(f"ok   {name}")
        except (OSError, RuntimeError) as error:
            failed.append(name)
            say(f"FAIL {name}: {error}")

    def rust():
        manifest["rust"] = fetch_rust(root, arguments.force)

    def python_i386():
        python = fetch_python_i386(root, arguments.force)
        # Its libraries, zlib's and libffi's among them, beside it.
        manifest["python"]["i386"] = {
            "command": [str(python)],
            "env": {"LD_LIBRARY_PATH": str(root / "python-i386" / "usr" /
                                           "lib" / "i386-linux-gnu")}}

    def cross():
        found = fetch_cross(root, arguments.force)
        manifest["sysroots"]["armhf"] = found["armhf"]
        manifest["sysroots"]["arm64"] = found["arm64"]
        python = fetch_standalone(root, "armv7", arguments.force)
        manifest["python"]["armv7"] = {
            "command": [found["qemu-arm"], "-L", found["armhf"],
                        str(python / "bin" / "python3")]}

    def win32():
        wine, prefix = prepare_wine(root, arguments.force)
        python = fetch_standalone(root, "win32", arguments.force)
        manifest["python"]["win32"] = {
            "command": [wine, str(python / "python.exe")],
            "env": {"WINEPREFIX": str(prefix), "WINEDEBUG": "-all",
                    "WINEDLLOVERRIDES": "mscoree,mshtml="},
            "unset": ["DISPLAY", "WAYLAND_DISPLAY"],
            "windows_paths": True}

    def bionic():
        manifest["sysroots"]["bionic"] = fetch_bionic(root, arguments.force)

    step("Rust and its cross targets", rust)
    step("Python for i386 Linux", python_i386)
    step("qemu, the armhf and arm64 C libraries, Python for 32-bit ARM", cross)
    step("Wine and Python for 32-bit Windows", win32)
    step("bionic's headers", bionic)
    (root / "toolchains.json").write_text(json.dumps(manifest, indent=2) +
                                          "\n", encoding="utf-8")
    say(f"wrote {root / 'toolchains.json'}")
    if failed:
        sys.exit(f"left out: {', '.join(failed)}")


if __name__ == "__main__":
    main()
