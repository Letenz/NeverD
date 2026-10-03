#!/usr/bin/env python3
"""Build and run original freestanding Windows caller-context observations."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unittests/emulation/fixtures"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("X64", "AArch64"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--probe", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    definition = (FIXTURES / "WindowsContextCases.def").read_text()
    settings = dict(re.findall(r'NEVERD_CAPTURE_(?:TEXT|BUILD)\((\w+), "([^"]*)"\)', definition))
    values = dict(re.findall(r'NEVERD_CAPTURE_VALUE\((\w+), ([^\n]+)\)', definition))
    apis = re.findall(r"^NEVERD_CAPTURE_API\((\w+)\)", definition, re.M)
    cases = re.findall(r'^NEVERD_CAPTURE_CASE\((\w+), "([^"]*)"\)', definition, re.M)
    assert apis and cases
    tool_paths = [Path(os.environ.get("ProgramFiles", "")) / "LLVM/bin"]

    def tool(name: str) -> str:
        found = shutil.which(name)
        if found:
            return found
        for directory in tool_paths:
            candidate = directory / (name + ".exe")
            if candidate.is_file():
                return str(candidate)
        raise RuntimeError(f"required fixture tool unavailable: {name}")

    clang, link = tool("clang"), tool("lld-link")
    commands = []

    def run(command: list[str]) -> None:
        commands.append(command)
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        with (output / "build.log").open("a", encoding="utf-8") as log:
            log.write(result.stdout + result.stderr)
        result.check_returncode()

    provider = output / "provider.def"
    provider.write_text("LIBRARY " + settings["Kernel"] + "\nEXPORTS\n" +
                        "".join("  " + api + "\n" for api in apis), encoding="utf-8")
    machine = "/machine:" + settings[args.arch + "Machine"]
    run([link, "/lib", machine, "/def:" + str(provider), "/out:" + str(output / "provider.lib")])
    objects = []
    for suffix in ("c", "S"):
        obj = output / ("context-" + suffix + ".obj")
        flags = ["-std=c11", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
                 "-fno-vectorize", "-fno-slp-vectorize", "-O1"] if suffix == "c" else []
        run([clang, "--target=" + settings[args.arch + "Target"], *flags, "-c",
             str(FIXTURES / ("windows_context." + suffix)), "-o", str(obj)])
        objects.append(str(obj))
    program = output / settings["ProgramFile"]
    run([link, "/nodefaultlib", "/subsystem:console", machine, "/timestamp:0",
         "/entry:" + settings["Entry"], *objects, str(output / "provider.lib"), "/out:" + str(program)])
    if args.build_only:
        print(program)
        return
    if platform.system() != "Windows":
        raise RuntimeError("native context observations require Windows")
    host = platform.machine().lower()
    if args.arch == "AArch64" and "arm64" not in host:
        raise RuntimeError(f"ARM64 native oracle cannot run on {host}")
    observations = []
    if args.probe:
        cases = [("Probe", "!P")]
    success = True
    for name, argument in cases:
        result = subprocess.run([str(program), argument], capture_output=True, check=False,
                                timeout=int(values["NativeTimeoutSeconds"]))
        (output / (name + ".stdout")).write_bytes(result.stdout)
        (output / (name + ".stderr")).write_bytes(result.stderr)
        expected = ord(argument[1]).to_bytes(4, "little")
        passed = (result.returncode == int(values["CompletionStatus"]) and
                  not result.stderr and (args.probe or result.stdout == expected))
        success &= passed
        observations.append({"name": name, "argument": argument, "status": result.returncode,
                             "stdout": result.stdout.hex(), "stderr": result.stderr.hex(), "passed": passed})
    report = {"commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "architecture": args.arch, "host": host, "commands": commands,
              "observations": observations, "passed": success, "probe": args.probe}
    (output / "observations.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if not success:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
