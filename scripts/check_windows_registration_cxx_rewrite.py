#!/usr/bin/env python3
"""Install genuine MSVC PE32 C++ source and execute its unchanged reconstruction.

The manual transaction remains separate from the public patch capability. Wine
and native replay must observe the same hashed files, exact generated caller
owner, ordered cleanups, catch-object effects, chain restoration and rebasing.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_cxx import OBSERVATION, SOURCE, parent_code_end
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_snapshot import saved_stack_probe
else:
    from check_windows_registration_cxx import OBSERVATION, SOURCE, parent_code_end
    from check_windows_registration_eh import run_image
    from check_windows_registration_rewrite import PE32
    from windows_registration_snapshot import saved_stack_probe

BASES = (0x400000, 0x18000000)
IMAGE_LABELS = ("original", "patched", "product-patched", "collision-patched",
                "cli-section", "cli-inplace")


def image_name(path: str) -> str:
    return Path(path.replace("\\", "/")).name


def require_image_matrix(records: list[dict], schema: int) -> None:
    if schema not in (1, 2):
        raise ValueError("source C++ execution matrix has an unsupported schema")
    labels = IMAGE_LABELS if schema == 2 else IMAGE_LABELS[:2]
    expected = {(label + suffix + ".exe", label != "original", base)
                for label in labels
                for suffix, base in (("", BASES[0]), ("-rebased", BASES[1]))}
    if len(records) != len(expected) or \
            {(image_name(r["image"]), r["generated"], r["runtime_base"])
             for r in records} != expected:
        raise ValueError("source C++ preferred/rebased route files are incomplete")


def require_installation_identity(image: PE32, manual: PE32) -> None:
    if manual.base != BASES[0] or image.base not in BASES:
        raise ValueError("source C++ installation has an unexpected base")
    expected = manual.data if image.base == manual.base else manual.rebase(image.base)
    if image.data != expected:
        raise ValueError("public C++ installation differs from its complete checked transaction")


def code_owner(image: PE32, begin: int, end: int, name: str) -> None:
    if not 0 <= begin < end <= 0xffffffff:
        raise ValueError("C++ code owner has an invalid range")
    owners = [section for section in image.sections
              if section[1] <= begin and end <= section[1] + section[2]]
    if len(owners) != 1 or owners[0][0] != name:
        raise ValueError("C++ code owner has no unique executable section")
    image.raw(begin, end - begin)
    section_table = image.optional + image.u16(image.optional - 4)
    index = image.sections.index(owners[0])
    flags = image.u32(section_table + 40 * index + 36)
    if flags & 0x60000000 != 0x60000000 or flags & 0x80000000:
        raise ValueError("C++ code section lost read/execute permissions")


def safe_handlers(image: PE32) -> tuple[int, list[int]]:
    rva, size = image.directory(10)
    if not rva or size < 4:
        raise ValueError("C++ input has no load configuration")
    config = image.raw(rva, size)
    declared = image.u32(config)
    if declared < 72:
        raise ValueError("C++ load configuration has no complete SafeSEH fields")
    config = image.raw(rva, declared)
    table, count = image.u32(config + 64), image.u32(config + 68)
    if not table or not 0 < count <= 65536 or table < image.base:
        raise ValueError("C++ SafeSEH table is incomplete")
    offset = image.raw(table - image.base, count * 4)
    handlers = [image.u32(offset + 4 * i) for i in range(count)]
    if handlers != sorted(set(handlers)):
        raise ValueError("C++ SafeSEH table is not strictly ordered")
    for handler in handlers:
        owners = [s for s in image.sections if s[1] <= handler < s[1] + s[2]]
        if len(owners) != 1 or owners[0][0] not in (".text", ".ndtext"):
            raise ValueError("C++ SafeSEH handler has no executable owner")
        code_owner(image, handler, handler + 1, owners[0][0])
    return rva + 64, handlers


def validate_compiled_image(image: PE32, contract: dict) -> None:
    if contract.get("schema") != 1 or \
            contract.get("evidence") != "checked-cxx-manual-installation" or \
            contract.get("image_base") != BASES[0]:
        raise ValueError("C++ compiler contract has an unsupported identity")
    begin, end = (contract["generated_code_begin_rva"],
                  contract["generated_code_end_rva"])
    code_owner(image, begin, end, ".ndtext")
    entry = image.entry(b"registration_cxx_probe")
    if entry != contract["source_entry_rva"]:
        raise ValueError("C++ export changed its source entry")
    offset = image.raw(entry, 5)
    delta = struct.unpack_from("<i", image.data, offset + 1)[0]
    if image.data[offset] != 0xe9 or entry + 5 + delta != begin:
        raise ValueError("C++ trampoline names a different generated owner")
    if image.u16(image.optional + 70) & 0x40:
        raise ValueError("C++ evidence must force its declared relocation base")
    relocations = image.relocation_fields()
    fields = contract["absolute_pointer_fields"]
    if len(fields) != 9 or len({field["rva"] for field in fields}) != 9:
        raise ValueError("C++ compiler contract lost dispatch pointer fields")
    for field in fields:
        rva = field["rva"]
        if rva not in relocations or \
                image.u32(image.raw(rva)) != \
                (field["value"] + image.base - contract["image_base"]) & 0xffffffff:
            raise ValueError("C++ dispatch pointer lost its HIGHLOW value")
    handler = contract["registration_handler_rva"]
    code_owner(image, handler, handler + 10, ".ndtext")
    offset = image.raw(handler, 10)
    if image.data[offset] != 0xb8 or image.data[offset + 5] != 0xe9 or \
            image.u32(offset + 1) != image.base + contract["func_info_rva"]:
        raise ValueError("C++ generated handler lost its FuncInfo dispatch")
    runtime = handler + 10 + struct.unpack_from("<i", image.data, offset + 6)[0]
    code_owner(image, runtime, runtime + 1, ".text")
    config_field, handlers = safe_handlers(image)
    if handler not in handlers or config_field not in relocations:
        raise ValueError("C++ generated handler lost SafeSEH or its pointer relocation")
    if image.u32(image.raw(contract["func_info_rva"], 36)) != 0x19930522:
        raise ValueError("C++ generated FuncInfo changed its FH3 ABI")


def require_cxx_outcome(result: dict, base: int, entry: int, begin: int,
                        end: int, reference: bool) -> dict:
    match = OBSERVATION.fullmatch(result.get("stdout", ""))
    if result.get("exit_code") != 0 or not match:
        raise ValueError("C++ source execution failed or produced no exact observation")
    value, caller, actual_entry, chain, iterations, trace, caught = (
        int(v, 16 if i in (1, 2) else 10)
        for i, v in enumerate(match.groups()))
    if (value, chain, iterations, trace, caught) != \
            (7, 1, 4, 213, 18 if reference else 7):
        raise ValueError("C++ cleanup, catch object or registration chain differs")
    if actual_entry != base + entry or not begin <= caller - base < end:
        raise ValueError("C++ caller PC or export lies outside its exact source owner")
    return {"value": value, "caught": caught, "trace": trace,
            "chain_restored": True, "iterations": iterations,
            "caller_rva": caller - base, "runtime_base": base,
            "caller_owner_begin_rva": begin, "caller_owner_end_rva": end}


def observe(image_path: Path, generated: bool, reference: bool,
            original_end: int, contract: dict, launcher: list[str],
            environment: dict[str, str], timeout: float) -> dict:
    image = PE32(image_path.read_bytes())
    if image.base not in BASES:
        raise ValueError("C++ runtime evidence has an unexpected image base")
    entry = image.entry(b"registration_cxx_probe")
    if generated:
        validate_compiled_image(image, contract)
        begin, end = (contract["generated_code_begin_rva"],
                      contract["generated_code_end_rva"])
    else:
        begin, end = entry, original_end
        code_owner(image, begin, end, ".text")
    result = run_image(image_path, launcher, environment, timeout)
    outcome = require_cxx_outcome(result, image.base, entry, begin, end, reference)
    return {"image": str(image_path), "sha256": hashlib.sha256(image.data).hexdigest(),
            "generated": generated, "reference": reference,
            **outcome, "runtime": result}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", required=True, type=Path)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--patch-binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--wine", default="wine" if os.name != "nt" else None)
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--saved-stack-probe", action="store_true",
                        help="derive a bounded catch SavedESP overwrite and post-catch read probe")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 2, "evidence": "source-msvc-cxx-reconstruction",
              "installation": "manual-public-cli-checked-transactions", "passed": False,
              "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
              "cases": [], "steps": []}
    if args.saved_stack_probe:
        report["source_probe"] = "derived-saved-esp-writeback"
    try:
        runtime = shutil.which(args.wine) if args.wine else None
        if args.wine and not runtime:
            raise ValueError("requested Wine runtime is unavailable")
        launcher = [runtime] if runtime else []
        environment = os.environ.copy()
        if args.wine_prefix:
            environment.update(WINEARCH="win32", WINEDEBUG="-all",
                               WINEPREFIX=str(args.wine_prefix.resolve()))
        original_root = args.input_root.resolve()
        baseline = json.loads((original_root / "cxx-runtime.json").read_text())
        if not baseline.get("passed") or \
                baseline.get("source_sha256") != report["source_sha256"]:
            raise ValueError("native MSVC baseline is incomplete or has different source")
        profiles = {case["case"]: case for case in baseline["cases"]
                    if case["case"] in ("value", "reference")}
        if set(profiles) != {"value", "reference"}:
            raise ValueError("native MSVC baseline lost a typed catch profile")
        for name, reference in (("value", False), ("reference", True)):
            parent = output / name
            parent.mkdir(exist_ok=True)
            original = original_root / name / "original.exe"
            native = profiles[name]
            native_image = [r for r in native["observations"]
                            if image_name(r["image"]) == "original.exe"]
            if not native.get("passed") or len(native_image) != 1 or \
                    not native_image[0].get("passed") or \
                    native_image[0]["sha256"] != hashlib.sha256(original.read_bytes()).hexdigest():
                raise ValueError("native MSVC image identity is missing or changed")
            native_digest = hashlib.sha256(original.read_bytes()).hexdigest()
            if args.saved_stack_probe:
                (parent / "original.exe").write_bytes(
                    saved_stack_probe(PE32(original.read_bytes()), reference))
            else:
                shutil.copyfile(original, parent / "original.exe")
            original = parent / "original.exe"
            shutil.copyfile(original_root / name / "original.map", parent / "original.map")
            pdb = original_root / name / "original.pdb"
            if pdb.is_file():
                shutil.copyfile(pdb, parent / "original.pdb")
            image = PE32(original.read_bytes())
            entry = image.entry(b"registration_cxx_probe")
            original_end = parent_code_end(original_root / name / "original.map", image, entry)
            if image.base != BASES[0]:
                raise ValueError("native MSVC baseline has a different preferred base")
            case = {"case": name, "reference": reference,
                    "original_code_end_rva": original_end, "observations": []}
            if args.saved_stack_probe:
                case["native_baseline_sha256"] = native_digest
            report["cases"].append(case)
            for label in IMAGE_LABELS[1:]:
                (parent / (label + ".exe")).unlink(missing_ok=True)
            test_environment = dict(environment,
                NEVERD_REGISTRATION_INPUT_CXX_PE32=str(parent / "original.exe"),
                NEVERD_REGISTRATION_OUTPUT_CXX_PE32=str(parent / "patched.exe"),
                NEVERD_REGISTRATION_OUTPUT_CXX_PRODUCT_PE32=str(parent / "product-patched.exe"),
                NEVERD_REGISTRATION_OUTPUT_CXX_COLLISION_PE32=str(parent / "collision-patched.exe"),
                NEVERD_REGISTRATION_OUTPUT_CXX_RECEIPT=str(parent / "compiled-contract.json"),
                NEVERD_REGISTRATION_OUTPUT_IR=str(parent / "source.ll"))
            command = [str(args.test_binary.resolve()), "--gtest_filter=WindowsRegistrationCxxSource.*",
                       f"--gtest_output=xml:{parent / 'rewrite.xml'}"]
            result = subprocess.run(command, env=test_environment, capture_output=True,
                                    text=True, errors="replace", timeout=args.timeout)
            report["steps"].append({"command": command, "exit_code": result.returncode,
                                    "stdout": result.stdout, "stderr": result.stderr})
            if result.returncode:
                raise ValueError(f"source C++ installation failed for {name}")
            xml = ET.parse(parent / "rewrite.xml").getroot()
            if int(xml.get("tests", "0")) != 1 or xml.findall(".//skipped") or \
                    int(xml.get("failures", "0")) or int(xml.get("errors", "0")):
                raise ValueError("source C++ checks did not execute successfully")
            contract = json.loads((parent / "compiled-contract.json").read_text())
            patched = parent / "patched.exe"
            if contract["source_image_sha256"] != hashlib.sha256(original.read_bytes()).hexdigest() or \
                    contract["image_sha256"] != hashlib.sha256(patched.read_bytes()).hexdigest():
                raise ValueError("source C++ installation changed its proved image identity")
            _, original_handlers = safe_handlers(image)
            _, generated_handlers = safe_handlers(PE32(patched.read_bytes()))
            if generated_handlers != sorted(set(original_handlers +
                    [contract["registration_handler_rva"]])):
                raise ValueError("source C++ installation changed its original SafeSEH closure")
            for mode in ("section", "inplace"):
                destination = parent / f"cli-{mode}.exe"
                command = [str(args.patch_binary.resolve()), "patch", str(parent / "original.exe"),
                           f"--from-ir={parent / 'source.ll'}", f"--mode={mode}",
                           "--no-opt", "-o", str(destination)]
                result = subprocess.run(command, env=test_environment, capture_output=True,
                                        text=True, errors="replace", timeout=args.timeout)
                report["steps"].append({"command": command, "exit_code": result.returncode,
                                        "stdout": result.stdout, "stderr": result.stderr})
                if result.returncode or not destination.is_file():
                    raise ValueError(f"CLI {mode} C++ reconstruction failed")
            manual = PE32(patched.read_bytes())
            for label in IMAGE_LABELS:
                source = parent / (label + ".exe")
                rebased = parent / (label + "-rebased.exe")
                rebased.write_bytes(PE32(source.read_bytes()).rebase(BASES[1]))
                for path in (source, rebased):
                    if label != "original":
                        require_installation_identity(PE32(path.read_bytes()), manual)
                    case["observations"].append(observe(
                        path, label != "original", reference, original_end,
                        contract, launcher, environment, args.timeout))
            require_image_matrix(case["observations"], report["schema"])
            print(f"PASS source C++ {name}: twelve preferred/rebased route executions", flush=True)
        report["passed"] = True
    except (OSError, ValueError, KeyError, ET.ParseError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    destination = output / "registration-cxx-rewrite.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'} source C++ reconstruction: {destination}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
