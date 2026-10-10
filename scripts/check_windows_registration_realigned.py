#!/usr/bin/env python3
"""Execute and relift a compiler-generated PE32 realigned catch callback.

This is a generated frame/analysis probe, not source-rewrite evidence. Both
preferred and forced bases must pass; a mismatched-result control must fail.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_eh import image_digest, run_image
    from .check_windows_registration_frame import KERNEL32, undecorate_kernel32_imports
    from .check_windows_registration_rewrite import PE32
else:
    from check_windows_registration_eh import image_digest, run_image
    from check_windows_registration_frame import KERNEL32, undecorate_kernel32_imports
    from check_windows_registration_rewrite import PE32

SOURCE = (Path(__file__).resolve().parents[1] / "unittests/lift/eh/fixtures/"
          "registration_realigned_driver.cpp")
RUNTIME = '''LIBRARY vcruntime140.dll
EXPORTS
__CxxThrowException@8
__CxxFrameHandler3
"??_7type_info@@6B@"
'''


def undecorate_throw_import(library: Path) -> None:
    """Keep the exact stdcall symbol and import the CRT's undecorated name."""
    data = bytearray(library.read_bytes())
    if data[:8] != b"!<arch>\n":
        raise ValueError("invalid CRT import archive")
    offset, seen = 8, 0
    while offset < len(data):
        if offset + 60 > len(data) or data[offset + 58:offset + 60] != b"`\n":
            raise ValueError("truncated CRT import member")
        size = int(data[offset + 48:offset + 58])
        begin, end = offset + 60, offset + 60 + size
        if size < 0 or end > len(data):
            raise ValueError("invalid CRT import member extent")
        if size >= 20 and data[begin:begin + 4] == b"\x00\x00\xff\xff":
            names = bytes(data[begin + 20:end]).split(b"\0")
            if names[0] == b"__CxxThrowException@8":
                _, _, version, machine, _, payload, _, kind = struct.unpack_from(
                    "<HHHHIIHH", data, begin)
                if version != 0 or machine != 0x14c or payload != size - 20 or \
                        names != [b"__CxxThrowException@8", b"vcruntime140.dll", b""] or \
                        kind & 3 or kind >> 2 not in (1, 3):
                    raise ValueError("unrecognized CRT stdcall import")
                struct.pack_into("<H", data, begin + 18, 3 << 2)
                seen += 1
        offset = end + (size & 1)
    if offset != len(data) or seen != 1:
        raise ValueError("CRT stdcall import is missing or duplicated")
    library.write_bytes(data)


def require_test_result(path: Path) -> int:
    root = ET.parse(path).getroot()
    count = int(root.get("tests", "0"))
    if count <= 0 or any(int(root.get(key, "0"))
                        for key in ("failures", "errors", "disabled")) or \
            root.findall(".//skipped"):
        raise ValueError("realigned callback checks failed or skipped")
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine", default="wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["WINEDEBUG"] = "-all"
    if args.wine_prefix:
        env["WINEARCH"] = "win32"
        env["WINEPREFIX"] = str(args.wine_prefix.resolve())
    report = {"schema": 1, "evidence": "generated-realigned-callback-analysis",
              "passed": False, "commands": [], "images": []}

    def run(command: list[str], extra: dict | None = None) -> None:
        result = subprocess.run(command, env=env | (extra or {}), cwd=out,
                                capture_output=True, text=True, errors="replace",
                                timeout=args.timeout, check=False)
        report["commands"].append({"command": command, "exit_code": result.returncode,
                                    "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("realigned probe build or analysis failed")

    try:
        compiler, linker = shutil.which(args.compiler), shutil.which(args.linker)
        wine = shutil.which(args.wine) if os.name != "nt" else None
        if not compiler or not linker or (os.name != "nt" and not wine):
            raise ValueError("required compiler, linker or Wine is unavailable")
        binary = str(args.test_binary.resolve())
        run([binary, "--gtest_filter=WindowsRegistrationRealigned.EmitsIndependentCallbackFrame",
             "--gtest_output=xml:" + str(out / "emit.xml")],
            {"NEVERD_REGISTRATION_REALIGNED_OBJECT": str(out / "frame.obj")})
        require_test_result(out / "emit.xml")
        report["source_sha256"] = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
        report["object_sha256"] = hashlib.sha256((out / "frame.obj").read_bytes()).hexdigest()
        for name, definition in (("kernel32", KERNEL32), ("runtime", RUNTIME)):
            (out / (name + ".def")).write_text(definition)
            run([linker, "/lib", "/machine:x86", "/def:" + str(out / (name + ".def")),
                 "/out:" + str(out / (name + ".lib"))])
        undecorate_kernel32_imports(out / "kernel32.lib")
        undecorate_throw_import(out / "runtime.lib")
        for control in (False, True):
            name = "wrong-result" if control else "probe"
            run([compiler, "--target=i686-pc-windows-msvc", "-fms-extensions",
                 "-fexceptions", "-fcxx-exceptions", "-fno-omit-frame-pointer", "-O1",
                 "-DEXPECTED_RESULT=" + ("8" if control else "7"), "-c", str(SOURCE),
                 "-o", str(out / (name + ".obj"))])
            image = out / (name + ".exe")
            run([linker, "/entry:mainCRTStartup", "/nodefaultlib", "/machine:x86",
                 "/subsystem:console", "/fixed:no", "/dynamicbase:no", "/out:" + str(image),
                 str(out / (name + ".obj")), str(out / "frame.obj"),
                 str(out / "runtime.lib"), str(out / "kernel32.lib")])
            rebased = out / (name + "-rebased.exe")
            rebased.write_bytes(PE32(image.read_bytes()).rebase(0x18000000))
            for path in (image, rebased):
                runtime = run_image(path, [wine] if wine else [], env, args.timeout)
                record = {"image": path.name, "sha256": image_digest(path),
                          "expected_exit": int(control), "runtime": runtime}
                report["images"].append(record)
                if runtime.get("exit_code") != int(control):
                    raise ValueError("callback frame runtime/control mismatch")
                if not control:
                    xml = out / (path.stem + ".xml")
                    run([binary, "--gtest_filter=WindowsRegistrationRealigned.InputPE32RecoversTheCallbackContract",
                         "--gtest_output=xml:" + str(xml)],
                        {"NEVERD_REGISTRATION_REALIGNED_PE32": str(path)})
                    record["analysis_tests"] = require_test_result(xml)
        report["passed"] = True
    except (OSError, ValueError, struct.error, subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    (out / "realigned-callback.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(out / "realigned-callback.json"),
                      "error": report.get("error")}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
