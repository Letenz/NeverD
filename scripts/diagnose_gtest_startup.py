#!/usr/bin/env python3
"""Retain bounded startup output and a debugger trace after a CI failure."""
import json
from pathlib import Path
import shutil
import subprocess
import sys


def capture(command, output, timeout):
    with output.open("w", encoding="utf-8") as stream:
        stream.write(json.dumps(command) + "\n")
        stream.flush()
        try:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=timeout, check=False)
            return {"command": command, "returncode": result.returncode}
        except (OSError, subprocess.TimeoutExpired) as error:
            stream.write(str(error) + "\n")
            return {"command": command, "error": str(error)}


def main():
    build = Path(sys.argv[1]).resolve()
    output = build / "startup-diagnostics"
    output.mkdir(parents=True, exist_ok=True)
    reports = {}
    debugger = shutil.which("lldb")
    for name in sys.argv[2:]:
        binary = build / "bin" / name
        if not binary.is_file():
            reports[name] = {"not_built": True}
            continue
        command = [str(binary), "--gtest_list_tests"]
        reports[name] = capture(command, output / (name + ".log"), 30)
        if reports[name].get("returncode") != 0 and debugger:
            reports[name]["debugger"] = capture(
                [debugger, "--batch", "-o", "run", "-k", "thread backtrace all",
                 "--", *command], output / (name + ".lldb.log"), 60)
    (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
    print(json.dumps(reports, indent=2))


if __name__ == "__main__":
    main()
