#!/usr/bin/env python3
"""Build and execute genuine MSVC PE32 C++ ABI baselines, not rewrite evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

from check_windows_registration_eh import run_image
from check_windows_registration_rewrite import PE32

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_cxx_runtime.cpp"
OBSERVATION = re.compile(
    r"neverd-registration-cxx: value=(\d+) caller=([0-9a-fA-F]{8}) "
    r"entry=([0-9a-fA-F]{8}) chain=(\d+) iterations=(\d+) trace=(\d+) "
    r"caught=(\d+)\s*")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler", default="cl")
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("building this oracle requires native Windows MSVC")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "original-msvc-cxx-runtime",
              "platform": platform.platform(), "passed": False,
              "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
              "cases": []}
    try:
        for reference in (False, True):
            name = "reference" if reference else "value"
            case = output / name
            case.mkdir(exist_ok=True)
            image = case / "original.exe"
            record = {"case": name, "passed": False, "observations": []}
            report["cases"].append(record)
            command = [args.compiler, "/nologo", "/std:c++17", "/EHsc",
                       "/GS-", "/Od", "/Oy-", "/Z7", "/MD",
                       "/D_CRT_SECURE_NO_WARNINGS",
                       f"/DREGISTRATION_CXX_REFERENCE={int(reference)}",
                       f"/Fo{case / 'original.obj'}", f"/Fe{image}", str(SOURCE),
                       "/link", "/debug", "/incremental:no", "/dynamicbase:no",
                       "/fixed:no", f"/pdb:{case / 'original.pdb'}",
                       f"/map:{case / 'original.map'}"]
            compiled = subprocess.run(command, cwd=case, capture_output=True,
                                      text=True, errors="replace", timeout=120)
            record["compiler"] = {"command": command,
                                  "exit_code": compiled.returncode,
                                  "stdout": compiled.stdout,
                                  "stderr": compiled.stderr}
            if compiled.returncode:
                raise ValueError(f"MSVC compilation failed for {name}")
            pe = PE32(image.read_bytes())
            entry = pe.entry(b"registration_cxx_probe")
            rebased = case / "original-rebased.exe"
            rebased.write_bytes(pe.rebase(0x530000))
            for path in (image, rebased):
                data = PE32(path.read_bytes())
                result = run_image(path, [], os.environ.copy(), 60)
                observation = {"image": str(path),
                               "sha256": hashlib.sha256(data.data).hexdigest(),
                               "runtime": result, "passed": False}
                record["observations"].append(observation)
                match = OBSERVATION.fullmatch(result.get("stdout", ""))
                if result.get("exit_code") != 0 or not match:
                    raise ValueError(f"C++ exception execution failed for {path}")
                value, caller, actual_entry, chain, iterations, trace, caught = (
                    int(v, 16 if i in (1, 2) else 10)
                    for i, v in enumerate(match.groups()))
                if ((value, chain, iterations, trace, caught) !=
                        (7, 1, 4, 213, 18 if reference else 7) or
                        actual_entry - entry != data.base):
                    raise ValueError("C++ cleanup, catch object, chain or base differs")
                owners = [n for n, rva, size, _, _ in data.sections
                          if rva <= caller - data.base < rva + size]
                if owners != [".text"]:
                    raise ValueError("C++ caller PC has no original text owner")
                observation.update(passed=True, value=value, trace=trace,
                                   caught=caught, caller_rva=caller - data.base,
                                   runtime_base=data.base, chain_restored=True,
                                   iterations=iterations)
            record["passed"] = True
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    finally:
        destination = output / "cxx-runtime.json"
        destination.write_text(json.dumps(report, indent=2))
    print(("PASS" if report["passed"] else "FAIL") +
          " original MSVC C++ runtime: " + str(destination))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
