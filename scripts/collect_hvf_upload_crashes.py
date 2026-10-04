"""Collect bounded macOS crash reports for recorded artifact-uploader children."""

import argparse
from datetime import datetime, timedelta
import json
import os
from pathlib import Path
import re
import stat
import time

MAX_REPORT_BYTES = 2 * 1024 * 1024


def timestamp(value):
    # IPS captureTime separates its UTC offset with a space; Node's process
    # records use ISO 8601 with Z. Normalize both without dropping the offset.
    return datetime.fromisoformat(re.sub(r"\s+(?=[+-][0-9]{4}$)", "", value).replace("Z", "+00:00"))


def read_report(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_REPORT_BYTES:
            raise ValueError("report is not a bounded regular file")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            payload = stream.read(MAX_REPORT_BYTES + 1)
        if len(payload) > MAX_REPORT_BYTES:
            raise ValueError("report exceeded its size bound")
    finally:
        os.close(fd)
    # Apple IPS reports contain a metadata object followed by a report object.
    # https://developer.apple.com/documentation/xcode/interpreting-the-json-format-of-a-crash-report
    decoder = json.JSONDecoder()
    text = payload.decode()
    _, end = decoder.raw_decode(text.lstrip())
    report = json.loads(text.lstrip()[end:].lstrip())
    if not isinstance(report, dict):
        raise ValueError("crash report must be an object")
    return payload, report


def matches(report, upload):
    if (report.get("pid") != upload["pid"]
            or report.get("parentPid") != upload["parent_pid"]
            or report.get("procName") != Path(upload["executable"]).name):
        return False
    try:
        captured = timestamp(report["captureTime"])
        started = timestamp(upload["started_at"])
        completed = timestamp(upload["completed_at"])
        return started - timedelta(seconds=1) <= captured <= completed + timedelta(seconds=30)
    except (KeyError, TypeError, ValueError):
        return False


def collect(evidence, directories, wait_seconds):
    uploads = evidence / "uploads"
    targets = {}
    for path in sorted(uploads.glob("*.json")):
        record = json.loads(path.read_text())
        if record.get("signal") and isinstance(record.get("pid"), int):
            targets[path.stem] = record
    if not targets:
        return {"kind": "hvf-uploader-crash-collection", "targets": [], "captured": []}
    destination = uploads / "crashes"
    destination.mkdir()
    result = {"kind": "hvf-uploader-crash-collection", "targets": list(targets),
              "captured": [], "errors": {}}
    deadline = time.monotonic() + wait_seconds
    remaining = set(targets)
    while remaining:
        for directory in directories:
            # A fresh CI runner should have few reports; cap work on reused hosts.
            candidates = sorted(directory.glob("node*.ips"), reverse=True)[:64]
            for path in candidates:
                try:
                    payload, report = read_report(path)
                    for name in sorted(remaining):
                        if matches(report, targets[name]):
                            output = destination / (name + ".ips")
                            with output.open("xb") as stream:
                                stream.write(payload)
                            result["captured"].append({"upload": name, "pid": report["pid"],
                                                       "report": output.name})
                            remaining.remove(name)
                except (OSError, UnicodeError, ValueError) as error:
                    result["errors"][path.name] = type(error).__name__
        if not remaining or time.monotonic() >= deadline:
            break
        time.sleep(min(1, max(0, deadline - time.monotonic())))
    result["missing"] = sorted(remaining)
    with (destination / "collection.json").open("x") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--wait-seconds", type=int, choices=range(31), default=20)
    args = parser.parse_args()
    result = collect(args.evidence, [Path.home() / "Library/Logs/DiagnosticReports",
                                    Path("/Library/Logs/DiagnosticReports")], args.wait_seconds)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
