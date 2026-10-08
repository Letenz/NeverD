#!/usr/bin/env python3
"""Replay the exact reconstructed PE32 evidence on the native Windows loader.

Linux may use --wine to check this replay harness locally. Every input hash,
case, code owner, relocation base and expected negative cookie result is
revalidated rather than inheriting a prior runner's success flag.
"""
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
    from .check_windows_registration_cookie import require_cookie_outcome
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_rewrite import CASES, PE32, observe
else:
    from check_windows_registration_cookie import require_cookie_outcome
    from check_windows_registration_eh import run_image
    from check_windows_registration_rewrite import CASES, PE32, observe


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
    launcher = [args.wine] if args.wine else []
    runtime_name = "Wine" if launcher else "Windows"
    environment = os.environ.copy()
    if args.wine_prefix:
        environment.update(WINEPREFIX=str(args.wine_prefix.resolve()), WINEDEBUG="-all")
    root = args.evidence_root.resolve()
    report = {"schema": 1, "platform": platform.platform(),
              "evidence": "wine-replay" if launcher else "native-windows-replay",
              "passed": False, "source_cases": [], "cookies": []}

    def checked_image(parent: Path, record: dict) -> Path:
        name = Path(record["image"]).name
        image = parent / name
        if image.parent != parent or not image.is_file() or \
                hashlib.sha256(image.read_bytes()).hexdigest() != record["sha256"]:
            raise ValueError("runtime evidence image is missing or changed")
        return image

    try:
        for case, (_, trace) in CASES.items():
            parent = root / ("source-" + case)
            source = json.loads((parent / "registration-rewrite.json").read_text())
            records = source["observations"]
            expected_images = {(label + suffix + ".exe", label != "original", base)
                               for label in ("original", "patched", "product-patched",
                                             "collision-patched", "cli-section", "cli-inplace")
                               for suffix, base in (("", 0x400000), ("-rebased", 0x18000000))}
            if source.get("case") != case or not source.get("passed") or \
                    source.get("expected_trace") != trace or len(records) != 12 or \
                    {(Path(r["image"]).name, r["generated"], r["runtime_base"])
                     for r in records} != expected_images:
                raise ValueError("source reconstruction evidence is incomplete")
            observations = []
            for record in records:
                observation = observe(checked_image(parent, record), record["generated"],
                                      launcher, environment, args.timeout, trace)
                if observation["runtime_base"] != record["runtime_base"]:
                    raise ValueError("native loader changed the forced image base")
                observations.append(observation)
            report["source_cases"].append({"case": case, "observations": observations})
            print(f"PASS {runtime_name} source {case}: {len(observations)} executions", flush=True)
        for name, gs in (("eh4-cookie-runtime", False), ("eh4-gs-cookie-runtime", True)):
            parent = root / name
            source = json.loads((parent / "cookie-runtime.json").read_text())
            records = source["observations"]
            expected = {(base, label) for base in (0x400000, 0x18000000)
                        for label in (("valid", "corrupt-eh", "corrupt-gs") if gs
                                      else ("valid", "corrupt-eh"))}
            if not source.get("passed") or source.get("gs_required") != gs or \
                    len(records) != len(expected) or \
                    {(r["image_base"], r["cookie_corrupted"]) for r in records} != expected:
                raise ValueError("EH4 cookie runtime evidence is incomplete")
            for record in records:
                image = checked_image(parent, record)
                pe = PE32(image.read_bytes())
                if pe.base != record["image_base"] or \
                        pe.u16(pe.optional + 70) & 0x40:
                    raise ValueError("cookie probe lost its forced relocation base")
                result = run_image(image, launcher, environment, args.timeout)
                require_cookie_outcome(result, record["cookie_corrupted"] != "valid")
                report["cookies"].append({"image": str(image), "runtime": result})
            print(f"PASS {runtime_name} {name}: {len(records)} executions", flush=True)
        report["passed"] = True
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'} PE32 replay: {args.output}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
