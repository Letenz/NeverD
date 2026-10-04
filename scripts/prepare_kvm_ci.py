#!/usr/bin/env python3
"""Give only the current hosted CI account access to an existing KVM device."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import stat
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def prepare(device: Path) -> dict:
    if (platform.system() != "Linux" or os.environ.get("GITHUB_ACTIONS") != "true"
            or os.environ.get("RUNNER_ENVIRONMENT") != "github-hosted"):
        raise ValueError("device access preparation is restricted to hosted Linux CI")
    report = {"device": str(device), "account": os.getuid(), "commands": []}

    def identity() -> dict:
        info = device.stat()
        if not stat.S_ISCHR(info.st_mode):
            raise ValueError("KVM path is not a character device")
        return {"uid": info.st_uid, "gid": info.st_gid,
                "mode": oct(stat.S_IMODE(info.st_mode)),
                "major": os.major(info.st_rdev), "minor": os.minor(info.st_rdev)}

    try:
        report["before"] = identity()
    except FileNotFoundError:
        return {**report, "status": "device_absent"}
    if os.access(device, os.R_OK | os.W_OK):
        return {**report, "status": "already_accessible", "after": report["before"]}
    # Hosted runners are disposable. Do not load modules, create device nodes,
    # grant access to every account or modify a local/self-hosted machine.
    for operation in (("chown", str(os.getuid())), ("chmod", "u+rw")):
        command = ["sudo", "-n", *operation, str(device)]
        report["commands"].append(command)
        subprocess.run(command, check=True, timeout=15)
    report["after"] = identity()
    if (report["before"]["major"], report["before"]["minor"]) != (
            report["after"]["major"], report["after"]["minor"]):
        raise ValueError("KVM device identity changed during preparation")
    if not os.access(device, os.R_OK | os.W_OK):
        raise ValueError("current CI account still cannot access KVM")
    return {**report, "status": "access_granted"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    definition = (ROOT / "lib/emulation/backends/kvm/KvmProtocol.def").read_text()
    paths = re.findall(r'^NEVERD_KVM_STRING\(Device, "([^"]+)"\)$', definition, re.M)
    if len(paths) != 1:
        raise ValueError("expected exactly one runtime KVM device path")
    report = prepare(Path(paths[0]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
