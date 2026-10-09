#!/usr/bin/env python3
"""Revalidate and execute the exact source C++ PE32 files on native Windows."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess

if __package__:
    from .check_windows_registration_cxx import SOURCE, parent_code_end
    from .check_windows_registration_cxx_rewrite import (
        BASES, image_name, observe, safe_handlers)
    from .check_windows_registration_rewrite import PE32
else:
    from check_windows_registration_cxx import SOURCE, parent_code_end
    from check_windows_registration_cxx_rewrite import (
        BASES, image_name, observe, safe_handlers)
    from check_windows_registration_rewrite import PE32


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    if os.name != "nt" and not args.wine:
        parser.error("native Windows is required unless --wine is explicit")
    root = args.evidence_root.resolve()
    report = {"schema": 1, "platform": platform.platform(),
              "evidence": "wine-cxx-replay" if args.wine else "native-cxx-replay",
              "passed": False, "cases": []}
    try:
        source = json.loads((root / "registration-cxx-rewrite.json").read_text())
        if source.get("schema") != 1 or \
                source.get("evidence") != "source-msvc-cxx-reconstruction" or \
                source.get("installation") != "manual-checked-transaction" or \
                source.get("source_sha256") != hashlib.sha256(SOURCE.read_bytes()).hexdigest() or \
                not source.get("passed") or len(source["cases"]) != 2 or \
                {case["case"] for case in source["cases"]} != {"value", "reference"}:
            raise ValueError("source C++ reconstruction evidence is incomplete")
        environment = os.environ.copy()
        if args.wine_prefix:
            environment.update(WINEARCH="win32", WINEDEBUG="-all",
                               WINEPREFIX=str(args.wine_prefix.resolve()))
        launcher = [args.wine] if args.wine else []
        for case in source["cases"]:
            name = case["case"]
            reference = name == "reference"
            parent = root / name
            contract = json.loads((parent / "compiled-contract.json").read_text())
            original = PE32((parent / "original.exe").read_bytes())
            entry = original.entry(b"registration_cxx_probe")
            end = parent_code_end(parent / "original.map", original, entry)
            if original.base != BASES[0] or case.get("reference") != reference or \
                    case["original_code_end_rva"] != end or \
                    contract["source_image_sha256"] != hashlib.sha256(original.data).hexdigest():
                raise ValueError("source C++ original owner or identity changed")
            patched = PE32((parent / "patched.exe").read_bytes())
            if contract["image_sha256"] != hashlib.sha256(patched.data).hexdigest():
                raise ValueError("source C++ installed image changed its compiler identity")
            _, old_handlers = safe_handlers(original)
            _, new_handlers = safe_handlers(patched)
            if new_handlers != sorted(set(old_handlers + [contract["registration_handler_rva"]])):
                raise ValueError("source C++ SafeSEH closure changed")
            records = case["observations"]
            expected = {(label + suffix + ".exe", label != "original", base)
                        for label in ("original", "patched")
                        for suffix, base in (("", BASES[0]), ("-rebased", BASES[1]))}
            if len(records) != 4 or \
                    {(image_name(r["image"]), r["generated"], r["runtime_base"])
                     for r in records} != expected:
                raise ValueError("source C++ preferred/rebased files are incomplete")
            replay = {"case": name, "observations": []}
            report["cases"].append(replay)
            for record in records:
                path = parent / image_name(record["image"])
                if hashlib.sha256(path.read_bytes()).hexdigest() != record["sha256"]:
                    raise ValueError("source C++ replay file is missing or changed")
                observation = observe(path, record["generated"], reference, end,
                                      contract, launcher, environment, args.timeout)
                if observation["runtime_base"] != record["runtime_base"]:
                    raise ValueError("source C++ loader changed its forced base")
                replay["observations"].append(observation)
            print(f"PASS native source C++ {name}: four identical-file executions", flush=True)
        report["passed"] = True
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'} source C++ replay: {args.output}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
