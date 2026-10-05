#!/usr/bin/env python3
"""Run the pinned feature producer/validator without duplicating its schema.

Arguments following COMMAND are passed to that submodule tool unchanged.
Use `COMMAND --help` for its compiler, source-manifest and output options.
"""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    commands = {
        "probe": "build_library_feature_probes.py",
        "libcxx": "build_libcxx_features.py",
        "msvc": "build_msvc_features.py",
        "musl": "build_musl_features.py",
        "repeat": "verify_library_feature_repeat.py",
        "validate": "validate_library_features.py",
    }
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--signatures", type=Path,
                        default=Path(__file__).resolve().parents[2] / "signatures")
    parser.add_argument("command", choices=commands)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    script = args.signatures / "scripts" / commands[args.command]
    if not script.is_file():
        parser.error(f"missing pinned feature tool: {script}; initialize the signatures submodule")
    return subprocess.run([sys.executable, str(script), *args.arguments]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
