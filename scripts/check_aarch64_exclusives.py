#!/usr/bin/env python3
"""Build and verify original exclusive instruction loops on native Windows ARM64."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unittests/emulation/fixtures"
DEFINITION = FIXTURES / "AArch64ExclusiveOracle.def"
CASES = FIXTURES.parent / "AArch64ExclusiveCases.def"


def validate_observations(data: bytes, cases: list[str]) -> list[dict]:
    if not cases or len(set(cases)) != len(cases) or len(data) != len(cases) * 4:
        raise ValueError("incomplete original ARM64 exclusive observations")
    values = struct.unpack("<" + "I" * len(cases), data)
    if any(value != 1 for value in values):
        raise ValueError("an original ARM64 exclusive instruction check failed")
    return [{"case": case, "passed": True} for case in cases]


def validate_alignment_observations(data: bytes, cases: list[tuple[str, int]],
                                    fields: list[str], initial: int, status: int) -> list[dict]:
    record = struct.Struct("<" + "Q" * len(fields))
    required = [(index, name) for index, (name, width) in enumerate(cases) if width > 1]
    if not fields or len(set(fields)) != len(fields) or len(data) != len(required) * record.size:
        raise ValueError("incomplete original ARM64 alignment observations")
    observations = []
    for (index, name), words in zip(required, record.iter_unpack(data), strict=True):
        row = dict(zip(fields, words))
        if (row["CaseIndex"] != index or not row["Code"] or not row["FaultPC"]
                or row["FaultPC"] != row["ExceptionPC"] or row["FaultPC"] != row["ContextPC"]
                or row["ContextLow"] != initial or row["ContextStatus"] != status
                or row["ParameterCount"] > 15
                or any(row[f"Parameter{i}"] for i in range(row["ParameterCount"], 15))):
            raise ValueError("inconsistent original ARM64 alignment context")
        observations.append({"case": name, "record": row})
    return observations


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    definition = DEFINITION.read_text(encoding="utf-8")
    settings = dict(re.findall(r'NEVERD_EXCLUSIVE_ORACLE_TEXT\((\w+), "([^"]+)"\)', definition))
    values = dict(re.findall(r"NEVERD_EXCLUSIVE_ORACLE_VALUE\((\w+), (\w+)\)", definition))
    apis = re.findall(r"NEVERD_EXCLUSIVE_ORACLE_API\((\w+)\)", definition)
    case_text = CASES.read_text(encoding="utf-8")
    cases = re.findall(r"^NEVERD_EXCLUSIVE_CASE\((\w+),[^\n]+, (\d+), \d+\)$", case_text, re.M)
    cases = [(name, int(width)) for name, width in cases]
    seeds = dict(re.findall(r"NEVERD_EXCLUSIVE_VALUE\((\w+), (\w+)\)", case_text))
    fields = re.findall(r"NEVERD_EXCLUSIVE_ORACLE_FIELD\((\w+)\)", definition)
    sources = [DEFINITION, CASES, FIXTURES / "aarch64_exclusive.inc",
               FIXTURES / "aarch64_exclusive_oracle.c", Path(__file__).resolve()]
    report = {"native_observed": False, "status": "failed", "commands": [],
              "host": {"system": platform.system(), "machine": platform.machine()},
              "sources": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in sources}}

    def tool(name: str) -> str:
        found = shutil.which(name)
        if found:
            return found
        candidate = Path(os.environ.get("ProgramFiles", "")) / "LLVM/bin" / (name + ".exe")
        if candidate.is_file():
            return str(candidate)
        raise RuntimeError(f"required fixture tool unavailable: {name}")

    def run(command: list[str], name: str, timeout: int = 60) -> subprocess.CompletedProcess:
        report["commands"].append(command)
        result = subprocess.run(command, cwd=output, capture_output=True, timeout=timeout, check=False)
        (output / (name + ".stdout")).write_bytes(result.stdout)
        (output / (name + ".stderr")).write_bytes(result.stderr)
        report[name + "_returncode"] = result.returncode
        result.check_returncode()
        return result

    try:
        report["commit"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        report["source_dirty"] = bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True).strip())
        clang, link = tool("clang"), tool("lld-link")
        provider = output / "provider.def"
        provider.write_text("LIBRARY " + settings["Provider"] + "\nEXPORTS\n" +
                            "".join("  " + api + "\n" for api in apis), encoding="utf-8")
        library, obj = output / "provider.lib", output / "exclusive.obj"
        run([link, "/lib", "/machine:" + settings["Machine"], "/def:" + str(provider),
             "/out:" + str(library)], "imports")
        run([clang, "--target=" + settings["Target"], "-std=c11", "-O1", "-ffreestanding",
             "-fno-builtin", "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
             "-c", str(FIXTURES / "aarch64_exclusive_oracle.c"), "-o", str(obj)], "compile")
        program = output / settings["Program"]
        run([link, "/nodefaultlib", "/subsystem:console", "/machine:" + settings["Machine"],
             "/timestamp:0", "/entry:" + settings["Entry"], str(obj), str(library),
             "/out:" + str(program)], "link")
        report["binary_sha256"] = hashlib.sha256(program.read_bytes()).hexdigest()
        if args.build_only:
            report["status"] = "built_only"
            return
        if report["source_dirty"]:
            raise RuntimeError("native evidence requires committed clean sources")
        if platform.system() != "Windows" or platform.machine().lower() not in ("arm64", "aarch64"):
            raise RuntimeError("native exclusive evidence requires a Windows ARM64 host")
        result = run([str(program)], "native", int(values["NativeTimeoutSeconds"]))
        if result.stderr:
            raise ValueError("unexpected native exclusive diagnostic output")
        size = len(cases) * 4
        report["observations"] = validate_observations(result.stdout[:size], [name for name, _ in cases])
        report["alignment_observations"] = validate_alignment_observations(
            result.stdout[size:], cases, fields, int(seeds["Initial"].removesuffix("ULL"), 0),
            int(seeds["Updated"].removesuffix("ULL"), 0))
        report["native_observed"] = True
        report["status"] = "passed"
    finally:
        (output / "observations.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: report[key] for key in ("status", "native_observed", "host")}))


if __name__ == "__main__":
    main()
