#!/usr/bin/env python3
"""Audit Windows product coverage and bind the shipped archive to its receipts."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET
import zipfile


INSTALLED_CHECKS = {
    "public_c_abi", "immutable_source_export", "asar_icu_77_1",
    "cli", "worker", "mcp",
}
HOST_CASES = {
    "RootAndAncestorJunctionsCannotEscape",
    "HardLinksAndAlternateStreamsAreRefused",
    "NamespaceAndInvalidUTF8CannotSelectDevicesOrRemoteInputs",
    "InputHandlesBlockWritersAndReplacement",
    "EnumeratedIdentityDetectsReplacementBeforeOpen",
    "UTF8NamesAndRelocationPreserveSnapshotIdentity",
    "SpoolIsUnlinkedPrivateNonInheritedAndReadOnlyAfterSeal",
    "CapturedBlobsSurviveDeletedInputsAndConcurrentRangeReads",
    "StoreRequiresExactLengthSealAndCumulativeBudget",
    "ExportIsVerifiedPrivateAndManifestDoesNotClobber",
    "ExportRefusesReadMismatchAndCorruptHash",
    "CapabilitiesAdvertiseTheNativeOwnersAndExports",
    "RewriteCommitKeepsExactBytesAndRefusesExistingTargets",
    "RewriteFailureRemovesOnlyOwnedFilesAndKeepsForeignData",
    "RewriteIncompleteAndBudgetFailuresRemoveOwnedDirectory",
}
NATIVE_REQUIRED = {"WebWindowsHost." + name for name in HOST_CASES} | {
    "WebAsar.PrivatePathValidationCoversUnicodeAliasesDevicesAndEscapes",
    "WebSDK.SourceQueriesUsePinnedBytesAndDoNotExposeNamesOrText",
    "WebRewriteSDK.MetadataKeepsRecoveredValuesPrivateAndExportBindsPass",
    "WebReadableExportSDK.ExactOriginalAndSameTreeSurviveInputDeletion",
    "WebDataPackSDK.MetadataAliasPagingAndExplicitSourceSelection",
}
ADAPTER_REQUIRED = {"NeverDWorkerWeb", "NeverDWebMCPWeb"}


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def audit_junit(path, expected, required):
    if not expected or not required <= expected:
        raise ValueError(f"Missing required discovery: {sorted(required - expected)}")
    root = ET.parse(path).getroot()
    seen = set()
    for case in root.iter("testcase"):
        name = case.get("name", "")
        if name in seen or name not in expected:
            raise ValueError(f"Unexpected or duplicate test: {name}")
        if case.get("status", "run") not in {"run", "passed"} or any(
            case.find(tag) is not None for tag in ("failure", "error", "skipped")
        ):
            raise ValueError(f"Required test did not pass: {name}")
        seen.add(name)
    if seen != expected:
        raise ValueError(f"Incomplete test receipt: {sorted(expected - seen)}")
    for suite in root.iter("testsuite"):
        for field in ("failures", "errors", "skipped", "disabled"):
            if int(suite.get(field, "0")) != 0:
                raise ValueError(f"Nonzero {field} in {path}")
    return {"passed": len(seen), "failed": 0, "skipped": 0,
            "sha256": digest(path)}


def audit_installed(path):
    value = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if (value.get("schema_version") != 1 or value.get("status") != "ok"
            or set(value.get("checks", [])) != INSTALLED_CHECKS
            or value.get("input_reader") != "windows-ntfs-handle-v1"
            or value.get("artifact_storage") != "windows-private-unlinked-spool-v1"):
        raise ValueError("Installed product checks are incomplete")
    return value | {"sha256": digest(path)}


def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("archive", "native-inventory", "native-junit", "adapter-junit",
                   "installed-result", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    ref = git("rev-parse", "HEAD")
    if ref != os.environ.get("GITHUB_SHA") or not re.fullmatch(r"[a-f0-9]{40}", ref):
        raise ValueError("Product source must equal the qualification commit")
    inventory = json.loads(args.native_inventory.read_text(encoding="utf-8-sig"))
    names = [test["name"] for test in inventory["tests"]]
    if len(set(names)) != len(names):
        raise ValueError("Duplicate discovered tests")
    native = audit_junit(args.native_junit, set(names), NATIVE_REQUIRED)
    native["inventory_sha256"] = digest(args.native_inventory)
    adapters = audit_junit(args.adapter_junit, ADAPTER_REQUIRED, ADAPTER_REQUIRED)
    installed = audit_installed(args.installed_result)
    files = []
    with zipfile.ZipFile(args.archive) as archive:
        for member in archive.infolist():
            if not member.is_dir():
                with archive.open(member) as stream:
                    value = hashlib.sha256()
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        value.update(chunk)
                files.append({"path": member.filename, "bytes": member.file_size,
                              "sha256": value.hexdigest()})
    members = {row["path"] for row in files}
    if len(members) != len(files) or not {
        "bin/neverd.exe", "bin/neverd.dll", "bin/neverd-worker.exe",
        "bin/neverd-web-mcp.exe", "bin/icuuc77.dll", "bin/icudt77.dll",
        "bin/msvcp140.dll", "bin/vcruntime140.dll", "LICENSE", "NOTICE",
        "web-windows-product.md", "web-product-dependencies.json",
        "lib/neverd.lib", "sdk/neverd/sdk/NeverDCAPIWeb.h",
        "share/neverd/licenses/LLVM-LICENSE.TXT",
        "share/neverd/licenses/icu/ICU-77.1-LICENSE",
    } <= members:
        raise ValueError("Incomplete or duplicate product members")
    repository = os.environ.get("GITHUB_REPOSITORY", "NeverSight/NeverD")
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    run = os.environ["GITHUB_RUN_ID"]
    submodules = []
    for line in git("ls-tree", "-r", "HEAD").splitlines():
        mode, kind, tail = line.split(" ", 2)
        if mode == "160000" and kind == "commit":
            commit, name = tail.split("\t", 1)
            submodules.append({"path": name, "commit": commit})
    receipt = {
        "schema_version": 1, "status": "qualified",
        "source_commit": ref, "source_url": f"{server}/{repository}/tree/{ref}",
        "source_archive_url": f"{server}/{repository}/archive/{ref}.tar.gz",
        "submodules": submodules,
        "qualification_url": f"{server}/{repository}/actions/runs/{run}",
        "run_attempt": int(os.environ.get("GITHUB_RUN_ATTEMPT", "1")),
        "archive": {"name": args.archive.name, "bytes": args.archive.stat().st_size,
                    "sha256": digest(args.archive)},
        "dependencies": json.loads(Path("cmake/web-product-dependencies.json").read_text()),
        "msvc_toolset": os.environ.get("VCToolsVersion"),
        "windows_sdk": os.environ.get("WindowsSDKVersion"),
        "native": native, "adapters": adapters, "installed": installed,
        "files": files,
    }
    args.output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    args.archive.with_suffix(args.archive.suffix + ".sha256").write_text(
        f"{receipt['archive']['sha256']}  {args.archive.name}\n", encoding="ascii")
    print(f"Qualified {native['passed']} native tests, two adapters and six installed checks")


if __name__ == "__main__":
    main()
