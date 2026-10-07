#!/usr/bin/env python3
"""Observe original cross-page process writes on matching Windows hosts."""

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
DEFINITION = FIXTURES / "WindowsMemoryWriteCases.def"


def definitions() -> tuple[dict, dict, list, list]:
    text = DEFINITION.read_text(encoding="utf-8")
    values = {name: int(value, 0) for name, value in re.findall(
        r"^NEVERD_MEMORY_WRITE_VALUE\((\w+), (\w+)\)$", text, re.M)}
    settings = dict(re.findall(r'NEVERD_MEMORY_WRITE_TEXT\(\s*(\w+),\s*"([^"]*)"\s*\)', text))
    protections = [(name, int(value, 0)) for name, value in re.findall(
        r"^NEVERD_MEMORY_WRITE_PROTECTION\((\w+), (\w+)\)$", text, re.M)]
    fields = re.findall(r"^NEVERD_MEMORY_WRITE_FIELD\((\w+)\)$", text, re.M)
    if (not values or not settings or not protections or not fields
            or len(set(fields)) != len(fields)
            or len({name for name, _ in protections}) != len(protections)
            or len({value for _, value in protections}) != len(protections)
            or struct.calcsize("<" + "Q" * len(fields)) != values["RecordSize"]):
        raise ValueError(settings.get("InvalidInventory"))
    return values, settings, protections, fields


def parse_observations(data: bytes) -> list[dict]:
    values, settings, protections, fields = definitions()
    record = struct.Struct("<" + "Q" * len(fields))
    count = len(protections)
    if len(data) != record.size * count * count:
        raise ValueError(settings["InvalidSize"])
    records = []
    rights = {value for _, value in protections}
    for index, words in enumerate(record.iter_unpack(data)):
        row = dict(zip(fields, words))
        if (row["First"], row["Second"]) != divmod(index, count):
            raise ValueError(settings["InvalidIdentity"])
        if (row["Result"] not in (0, 1) or row["Error"] > 0xffffffff
                or row["AfterFirst"] not in rights or row["AfterSecond"] not in rights
                or max(row["ByteFirst"], row["ByteSecond"]) > 0xff):
            raise ValueError(settings["InvalidResult"])
        records.append(row)
    return records


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("X64", "AArch64"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    values, settings, _, _ = definitions()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"status": "failed", "native_windows_observed": False,
              "contract_verified": False, "architecture": args.arch,
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
        if report["source_dirty"] and not args.build_only:
            raise ValueError(settings["DirtySource"])
        host = {"amd64": "X64", "x86_64": "X64", "arm64": "AArch64", "aarch64": "AArch64"}.get(
            platform.machine().lower())
        if not args.build_only and (platform.system() != "Windows" or host != args.arch):
            raise ValueError(settings["HostMismatch"])
        paths = [str(Path(os.environ.get("ProgramFiles", "")) / "LLVM/bin"), os.environ.get("PATH", "")]

        def tool(name: str) -> str:
            found = shutil.which(name, path=os.pathsep.join(paths))
            if not found:
                raise ValueError(settings["MissingTool"] + ": " + name)
            return found

        clang, link = tool("clang"), tool("lld-link")
        source = FIXTURES / settings["SourceFile"]
        report["source_sha256"] = {
            path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (DEFINITION, source, Path(__file__).resolve())}
        apis = re.findall(r"^NEVERD_MEMORY_WRITE_API\((\w+)\)$", DEFINITION.read_text(), re.M)
        provider = output / "provider.def"
        provider.write_text("LIBRARY " + settings["ProviderFile"] + "\nEXPORTS\n" +
                            "".join("  " + api + "\n" for api in apis), encoding="utf-8")
        timeout = values["BuildTimeoutSeconds"]
        run([clang, "--version"], "compiler", timeout)
        run([link, "--version"], "linker", timeout)
        machine = "/machine:" + settings[args.arch + "Machine"]
        run([link, "/lib", machine, "/def:" + str(provider),
             "/out:" + str(output / "provider.lib")], "imports", timeout)
        obj = output / "program.obj"
        run([clang, "--target=" + settings[args.arch + "Target"], "-std=c11", "-ffreestanding",
             "-fno-builtin", "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
             "-O1", "-Wall", "-Wextra", "-Werror", "-c", str(source), "-o", str(obj)], "compile", timeout)
        program = output / settings["ProgramFile"]
        run([link, "/nodefaultlib", "/subsystem:console", machine, "/timestamp:0",
             "/entry:" + settings["ProgramEntry"], "/base:" + settings["ProgramBase"],
             str(obj), str(output / "provider.lib"), "/out:" + str(program)], "link", timeout)
        report["program_sha256"] = hashlib.sha256(program.read_bytes()).hexdigest()
        if args.build_only:
            report["status"] = "built_only"
        else:
            result = run([str(program)], "observe", values["NativeTimeoutSeconds"])
            if result.stderr:
                raise ValueError(settings["UnexpectedStderr"])
            observations = parse_observations(result.stdout)
            (output / "observations.json").write_text(json.dumps(observations, indent=2) + "\n", encoding="utf-8")
            report["observations"] = len(observations)
            report["native_windows_observed"] = True
            report["status"] = "observed"
        report["final_commit"] = git("rev-parse", "HEAD")
        report["final_source_dirty"] = bool(git("status", "--porcelain"))
        if not args.build_only and (report["final_commit"] != report["commit"] or report["final_source_dirty"]):
            raise ValueError(settings["ChangedSource"])
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["native_windows_observed"] = False
        report["error"] = str(error)
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("status", "architecture", "native_windows_observed", "contract_verified")}))
    if report["status"] == "failed":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
