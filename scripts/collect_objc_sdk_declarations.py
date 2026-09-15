#!/usr/bin/env python3
"""Collect public Objective-C declaration facts from actual Apple SDKs.

Keep each architecture and platform separate. These development artifacts do
not activate call bindings or certify recovered application behavior. Catalog
generation must subsequently reconcile every applicable declaration profile.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

try:
    from .generate_objc_framework_declarations import framework_header
    from .generate_objc_receiver_declarations import ReceiverDeclarations
except ImportError:
    from generate_objc_framework_declarations import framework_header
    from generate_objc_receiver_declarations import ReceiverDeclarations


FRAMEWORKS = ("UIKit", "CoreImage", "WebKit", "MapKit", "PassKit",
              "SafariServices", "AVFoundation")
SDKS = ("macosx", "iphoneos", "iphonesimulator")
TARGET_PROFILES = (
    ("iphoneos", "device", "arm64-apple-ios18.0"),
    ("iphonesimulator", "simulator", "arm64-apple-ios18.0-simulator"),
    ("iphonesimulator", "simulator", "x86_64-apple-ios18.0-simulator"),
    ("macosx", "catalyst", "arm64-apple-ios18.0-macabi"),
    ("macosx", "catalyst", "x86_64-apple-ios18.0-macabi"),
    ("macosx", "desktop", "arm64-apple-macos15.0"),
    ("macosx", "desktop", "x86_64-apple-macos15.0"),
)


def profile_plan(frameworks, sdks):
    if not frameworks or len(set(frameworks)) != len(frameworks):
        raise ValueError("framework selection is empty or duplicated")
    if not sdks or len(set(sdks)) != len(sdks) or not set(sdks) <= set(SDKS):
        raise ValueError("SDK selection is empty, duplicated or unsupported")
    profiles = []
    for framework in sorted(frameworks):
        header = framework_header(framework)  # Validate before forming paths.
        for sdk, environment, target in TARGET_PROFILES:
            if sdk not in sdks or (framework == "UIKit" and environment == "desktop"):
                continue  # UIKit has no native macOS provider.
            profiles.append({"id": framework + "/" + target,
                             "framework": framework, "header": header,
                             "sdk": sdk, "environment": environment,
                             "target": target})
    return profiles


def framework_directory(sdk, framework, environment):
    framework_header(framework)
    root = sdk / "System/Library/Frameworks"
    support = sdk / "System/iOSSupport/System/Library/Frameworks"
    if environment == "catalyst" and (support / (framework + ".framework")).exists():
        root = support
    directory = (root / (framework + ".framework")).resolve(strict=True)
    if not directory.is_relative_to(sdk.resolve(strict=True)):
        raise ValueError("public framework resolves outside the selected SDK")
    return directory


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def collect_profile(output, sdk, library, profile):
    framework = profile["framework"]
    directory = framework_directory(sdk, framework, profile["environment"])
    headers = (directory / "Headers").resolve(strict=True)
    if not headers.is_relative_to(directory):
        raise ValueError("public header directory resolves outside its framework")
    stub = directory / (framework + ".tbd")
    work = output / framework / profile["target"]
    work.mkdir(parents=True)
    source = work / "declarations.m"
    source.write_text("#import <" + profile["header"] + ">\n")
    arguments = []
    if profile["environment"] == "catalyst":
        arguments.append("-F" + str(sdk / "System/iOSSupport/System/Library/Frameworks"))
    compiler = ReceiverDeclarations(library, headers)
    facts = compiler.extract_owned(source, sdk, profile["target"], arguments)
    if not facts["owners"] or not facts["methods"]:
        raise ValueError("framework has no complete owned declaration inventory")
    owned_headers = []
    for header in sorted(headers.rglob("*.h")):
        if not header.resolve(strict=True).is_relative_to(headers):
            raise ValueError("public header resolves outside its framework")
        owned_headers.append({"path": header.relative_to(headers).as_posix(),
                              "sha256": digest(header)})
    if not owned_headers:
        raise ValueError("framework has no public header evidence")
    write_json(work / "declarations.json", facts)
    write_json(work / "headers.json", owned_headers)
    # Retain the SDK's exact export document for later provider validation.
    # This collector neither interprets its reexports nor invents aliases.
    shutil.copyfile(stub, work / "framework.tbd")
    files = [{"path": file.relative_to(output).as_posix(),
              "sha256": digest(file)} for file in (
                  source, work / "declarations.json", work / "headers.json",
                  work / "framework.tbd")]
    return {**profile, "status": "success", "files": files,
            "compiler_arguments": arguments,
            "framework_path": directory.relative_to(sdk).as_posix(),
            "compiler_version": compiler.string(compiler.clang_getClangVersion()),
            "owner_count": len(facts["owners"]), "method_count": len(facts["methods"])}


def collect(output, sdks, library, frameworks=FRAMEWORKS, sdk_version=None):
    plan = profile_plan(frameworks, tuple(sdks))
    output.mkdir(parents=True, exist_ok=False)
    evidence = {"schema_version": 1, "scope": "public-sdk-declaration-facts",
                "status": "incomplete", "frameworks": sorted(frameworks),
                "requested_sdks": sorted(sdks), "expected_profiles": plan,
                "consumer_commit": os.environ.get("CONSUMER_COMMIT"),
                "sdks": {}, "profiles": []}
    write_json(output / "receipt.json", evidence)
    try:
        evidence["libclang_sha256"] = digest(library)
        for name, sdk in sorted(sdks.items()):
            settings = sdk / "SDKSettings.json"
            version = json.loads(settings.read_text())["Version"]
            if sdk_version is not None and version != sdk_version:
                raise ValueError(f"{name}: expected SDK {sdk_version}, found {version}")
            evidence["sdks"][name] = {"path": str(sdk), "version": version,
                                      "settings_sha256": digest(settings)}
        for profile in plan:
            result = collect_profile(output, sdks[profile["sdk"]], library, profile)
            evidence["profiles"].append(result)
            write_json(output / "receipt.json", evidence)
            print(profile["id"], result["method_count"], flush=True)
        evidence["status"] = "success"
    except Exception as error:
        evidence["error"] = str(error)
        raise
    finally:
        write_json(output / "receipt.json", evidence)
    return evidence


def xcrun(*arguments):
    return subprocess.check_output(["/usr/bin/xcrun", *arguments], text=True,
                                   timeout=30).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sdk", choices=SDKS, action="append")
    parser.add_argument("--framework", action="append")
    parser.add_argument("--sdk-version")
    args = parser.parse_args()
    names = args.sdk or SDKS
    frameworks = args.framework or FRAMEWORKS
    profile_plan(frameworks, names)
    sdks = {name: Path(xcrun("--sdk", name, "--show-sdk-path")).resolve(strict=True)
            for name in names}
    clang = Path(xcrun("--find", "clang")).resolve(strict=True)
    library = clang.parent.parent / "lib/libclang.dylib"
    collect(args.output, sdks, library, frameworks, args.sdk_version)


if __name__ == "__main__":
    main()
