#!/usr/bin/env python3
"""Collect original Windows SSE exception records without assuming their status."""

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
DEFINITION = FIXTURES / "WindowsAlignmentCases.def"


def definitions() -> tuple[dict, dict, list, list, list]:
    text = DEFINITION.read_text(encoding="utf-8")
    values = {name: int(value.removesuffix("ULL"), 0) for name, value in re.findall(
        r"^NEVERD_WINDOWS_ALIGNMENT_(?:VALUE|WIDE)\((\w+), (\w+)\)$", text, re.M)}
    settings = dict(re.findall(r'^NEVERD_WINDOWS_ALIGNMENT_TEXT\((\w+), "([^"]*)"\)$',
                               text, re.M))
    operations = re.findall(r"^NEVERD_WINDOWS_ALIGNMENT_OPERATION\((\w+),", text, re.M)
    scenarios = re.findall(r"^NEVERD_WINDOWS_ALIGNMENT_SCENARIO\((\w+)\)$", text, re.M)
    fields = re.findall(r"^NEVERD_WINDOWS_ALIGNMENT_FIELD\((\w+)\)$", text, re.M)
    if not values or not settings or any(not v or len(set(v)) != len(v)
                                         for v in (operations, scenarios, fields)):
        raise ValueError("invalid Windows alignment observation inventory")
    return values, settings, operations, scenarios, fields


def parse_observations(data: bytes) -> list[dict]:
    values, _, operations, scenarios, fields = definitions()
    record = struct.Struct("<" + "Q" * len(fields))
    if len(data) != record.size * len(operations) * len(scenarios):
        raise ValueError("missing or extra Windows exception observations")
    records = []
    parameters = [field for field in fields if re.fullmatch(r"Parameter\d+", field)]
    for index, words in enumerate(record.iter_unpack(data)):
        row = dict(zip(fields, words))
        op, case = divmod(index, len(scenarios))
        if (row["Operation"], row["Scenario"]) != (op, case):
            raise ValueError("Windows exception observation order changed")
        if not row["Code"] or row["ParameterCount"] > len(parameters):
            raise ValueError("invalid Windows exception record")
        if not row["FaultPC"] or not row["FaultPC"] == row["ExceptionPC"] == row["ContextPC"]:
            raise ValueError("Windows exception PC differs from its original fault site")
        if (row["Operand"] != row["ContextOperand"]
                or row["VectorLow"] != values["VectorLow"]
                or row["VectorHigh"] != values["VectorHigh"]):
            raise ValueError("Windows exception changed the original input state")
        if any(row[key] for key in parameters[row["ParameterCount"]:]):
            raise ValueError("undefined exception parameters were not normalized")
        records.append({"operation": operations[op], "scenario": scenarios[case],
                        "record": row})
    return records


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    values, settings, _, _, _ = definitions()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"status": "failed", "native_windows_observed": False,
              "host": {"system": platform.system(), "machine": platform.machine(),
                       "version": platform.version()}, "commands": []}

    def git(*arguments: str) -> str:
        return subprocess.check_output(["git", *arguments], cwd=ROOT, text=True).strip()

    def run(command: list[str], prefix: str, timeout: int) -> subprocess.CompletedProcess:
        report["commands"].append(command)
        try:
            result = subprocess.run(command, cwd=output, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            (output / (prefix + ".stdout")).write_bytes(error.stdout or b"")
            (output / (prefix + ".stderr")).write_bytes(error.stderr or b"")
            raise
        (output / (prefix + ".stdout")).write_bytes(result.stdout)
        (output / (prefix + ".stderr")).write_bytes(result.stderr)
        report[prefix + "_returncode"] = result.returncode
        result.check_returncode()
        return result

    try:
        report["commit"] = git("rev-parse", "HEAD")
        report["source_dirty"] = bool(git("status", "--porcelain"))
        if report["source_dirty"]:
            raise ValueError("native observations require committed clean sources")
        native = platform.system() == "Windows"
        if not args.build_only and (not native or platform.machine().lower() not in {"amd64", "x86_64"}):
            raise ValueError("native observations require an x64 Windows host")
        paths = [str(Path(os.environ.get("ProgramFiles", "")) / "LLVM/bin"), os.environ.get("PATH", "")]

        def tool(name: str) -> str:
            found = shutil.which(name, path=os.pathsep.join(paths))
            if not found:
                raise ValueError(f"required fixture tool unavailable: {name}")
            return found

        clang, link = tool("clang"), tool("lld-link")
        sources = [DEFINITION, FIXTURES / "windows_alignment.c", FIXTURES / "windows_alignment.S",
                   Path(__file__).resolve()]
        report["source_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in sources}
        apis = re.findall(r"^NEVERD_WINDOWS_ALIGNMENT_API\((\w+)\)$", DEFINITION.read_text(), re.M)
        provider = output / "provider.def"
        provider.write_text("LIBRARY " + settings["Provider"] + "\nEXPORTS\n" +
                            "".join("  " + api + "\n" for api in apis), encoding="utf-8")
        timeout = values["BuildTimeoutSeconds"]
        run([link, "/lib", "/machine:x64", "/def:" + str(provider),
             "/out:" + str(output / "provider.lib")], "imports", timeout)
        objects = []
        for suffix in ("c", "S"):
            obj = output / ("alignment-" + suffix + ".obj")
            flags = ["-std=c11", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
                     "-fno-vectorize", "-fno-slp-vectorize", "-O1", "-Wall", "-Wextra"] if suffix == "c" else []
            run([clang, "--target=" + settings["WindowsTarget" if native else "CrossTarget"],
                 *flags, "-c", str(FIXTURES / ("windows_alignment." + suffix)), "-o", str(obj)],
                "compile-" + suffix, timeout)
            objects.append(str(obj))
        program = output / settings["Program"]
        run([link, "/nodefaultlib", "/subsystem:console", "/machine:x64", "/timestamp:0",
             "/entry:" + settings["Entry"], *objects, str(output / "provider.lib"),
             "/out:" + str(program)], "link", timeout)
        report["program_sha256"] = hashlib.sha256(program.read_bytes()).hexdigest()
        if args.build_only:
            report["status"] = "built_only"
        else:
            result = run([str(program)], "observe", values["TimeoutSeconds"])
            if result.stderr:
                raise ValueError("native observation emitted unexpected stderr")
            report["observations"] = parse_observations(result.stdout)
            report["native_windows_observed"] = True
            report["status"] = "observed"
        report["final_commit"] = git("rev-parse", "HEAD")
        report["final_source_dirty"] = bool(git("status", "--porcelain"))
        if report["final_commit"] != report["commit"] or report["final_source_dirty"]:
            raise ValueError("source changed during native observation")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["native_windows_observed"] = False
        report["error"] = str(error)
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if report["status"] == "failed":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
