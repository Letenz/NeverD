#!/usr/bin/env python3
"""Select exact, same-run qualified bytes for the product Release."""

import argparse
import json
import os
from pathlib import Path
import shutil

from web_product_receipt import (
    ADAPTER_REQUIRED, NATIVE_REQUIRED, audit_installed, audit_junit, digest,
)


def prepare(directory, output, commit, run):
    receipts = list(directory.rglob("windows-product-receipt.json"))
    if len(receipts) != 1:
        raise ValueError("Require exactly one Windows product receipt")
    receipt_path = receipts[0]
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    expected_run = (f"{os.environ.get('GITHUB_SERVER_URL', 'https://github.com')}/"
                    f"{os.environ.get('GITHUB_REPOSITORY', 'NeverSight/NeverD')}/actions/runs/{run}")
    if (receipt.get("schema_version") != 1 or receipt.get("status") != "qualified"
            or receipt.get("source_commit") != commit
            or receipt.get("qualification_url") != expected_run
            or receipt.get("adapters", {}).get("passed") != len(ADAPTER_REQUIRED)):
        raise ValueError("Require complete qualification at this exact source and run")
    parent = receipt_path.parent
    inventory = parent / "windows-native-inventory.json"
    discovered = [case["name"] for case in json.loads(inventory.read_text(encoding="utf-8-sig"))["tests"]]
    if len(set(discovered)) != len(discovered):
        raise ValueError("Duplicate discovered cases")
    native = audit_junit(parent / "windows-native-results.xml", set(discovered), NATIVE_REQUIRED)
    native["inventory_sha256"] = digest(inventory)
    adapters = audit_junit(parent / "windows-adapter-results.xml", ADAPTER_REQUIRED, ADAPTER_REQUIRED)
    installed = audit_installed(parent / "windows-installed-results.json")
    if any(receipt.get(key) != value for key, value in (
        ("native", native), ("adapters", adapters), ("installed", installed)
    )):
        raise ValueError("Qualification reports changed")
    name = receipt["archive"]["name"]
    if name != f"neverd-web-windows-x64-{commit[:12]}.zip":
        raise ValueError("Unexpected product filename")
    archive = receipt_path.parent / name
    if (archive.stat().st_size != receipt["archive"]["bytes"]
            or digest(archive) != receipt["archive"]["sha256"]):
        raise ValueError("Qualified product bytes changed")
    output.mkdir(exist_ok=False)
    for path in (archive, receipt_path):
        shutil.copy2(path, output / path.name)
    for name in ("windows-native-results.xml", "windows-adapter-results.xml",
                 "windows-native-inventory.json", "windows-installed-results.json"):
        shutil.copy2(receipt_path.parent / name, output / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.directory, args.output, os.environ["GITHUB_SHA"], os.environ["GITHUB_RUN_ID"])


if __name__ == "__main__":
    main()
