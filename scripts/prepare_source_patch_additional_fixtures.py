#!/usr/bin/env python3
"""Publish reproducible macOS and 32-bit process fixtures using a built NeverD.

Run on macOS with Clang/LLD, an existing C-only NeverC, and matching ELF GCC
drivers. NeverC's repository is neither built nor modified by this script.
Outputs must be new directories; failed attempts remain available for diagnosis.
"""
from __future__ import annotations

import argparse
import base64
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scripts import check_source_patch_additional_runtime as runtime
from scripts import check_source_patch_cpp_runtime as cpp
from scripts import check_source_patch_pe_runtime as pe
from scripts import check_source_patch_snapshot_runtime as snapshot

START = {
    "x86": ".text\n.global _start\n.type _start,@function\n_start:\n"
           "and $-16,%esp\ncall entry\nmov %eax,%ebx\nmov $1,%eax\nint $0x80\n",
    "arm": ".syntax unified\n.arch armv7-a\n.arm\n.text\n.global _start\n"
           ".type _start,%function\n_start:\nbl entry\nmov r7,#1\nsvc #0\n",
    "thumb": ".syntax unified\n.arch armv7-a\n.thumb\n.text\n.global _start\n"
             ".type _start,%function\n.thumb_func\n_start:\nbl entry\nmov r7,#1\nsvc #0\n",
}
MACHO_WRITABLE_POINTER = r"""
extern int original_helper(int);
static volatile int counter = 40;
static volatile int *volatile pointer;
int replacement(int x) {
  if (!pointer) pointer = &counter;
  ++*pointer;
  return *pointer + original_helper(x);
}
"""


def portable_receipt(receipt: dict, repository: Path, work: Path, output: Path) -> bytes:
    receipt = dict(receipt)
    if receipt.get("compiler_version"):
        receipt["compiler_version"] = receipt["compiler_version"].splitlines()[0]
    if receipt.get("compiler_path"):
        receipt["compiler_path"] = Path(receipt["compiler_path"]).name
    if receipt.get("compiler_arguments"):
        receipt["compiler_arguments"] = list(receipt["compiler_arguments"])
        receipt["compiler_arguments"][0] = Path(receipt["compiler_arguments"][0]).name

    def convert(value):
        if isinstance(value, dict):
            return {key: convert(item) for key, item in value.items()}
        if isinstance(value, list):
            return [convert(item) for item in value]
        if not isinstance(value, str):
            return value
        for path, replacement in ((output, "<fixtures>"), (work, "<work>"),
                                  (repository, "<repository>")):
            value = value.replace(str(path) + "/", replacement + "/")
        return re.sub(r"/(?:private/)?var/folders/[^ \n]*?/neverd-source-compiler-[^/ \n]+/",
                      "<temporary>/", value)
    data = (json.dumps(convert(receipt), indent=2) + "\n").encode()
    if re.search(rb"/(?:Users|home)/|/(?:private/)?var/folders/", data):
        raise ValueError("publication receipt contains a private path")
    return data


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "library", "clang", "neverc", "linker", "elf_linker",
                 "nm", "gcc_x86", "gcc_arm32", "output", "work"):
        parser.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    args = parser.parse_args()
    for name, value in vars(args).items():
        # LLD selects its driver from argv[0]; retain the lld-link symlink.
        setattr(args, name, value.absolute())
    repository = Path(__file__).resolve().parents[1]
    output, work = args.output.resolve(), args.work.resolve()
    output.mkdir(parents=True, exist_ok=False)
    work.mkdir(parents=True, exist_ok=False)
    (output / "originals").mkdir()
    os.chdir(output)
    neverc_digest = pe.digest(args.neverc)
    manifest = {"schema": snapshot.SCHEMA, "producer_commit": "binary-sha256:" + neverc_digest,
                "engine_source_commit": subprocess.check_output(
                    ["git", "rev-parse", "HEAD"], cwd=repository, text=True).strip(),
                "engine_source_digest": snapshot.engine_digest(repository),
                "engine_sha256": pe.digest(args.library), "original_paths": "fixture",
                "receipt_paths": "portable", "producers": {}, "images": []}
    for name in ("clang", "neverc", "gcc_x86", "gcc_arm32"):
        executable = getattr(args, name).resolve()
        version = pe.run([str(executable), "--version"], work / (name + "-version.log"))
        manifest["producers"][name] = {"version": version.stdout.splitlines()[0],
                                       "executable_sha256": pe.digest(executable)}
    inputs = repository / "unittests/Inputs/source-patch/neverc-linux"
    for name in ("original-eh.cpp", "original-callback.cpp", "replacement.c", "hosted.c", "exceptions.cpp"):
        shutil.copyfile(inputs / name, output / name)
    abi_original = pe.ORIGINAL.replace("int _fltused = 0;", "").replace(
        "__declspec(dllexport)", "").replace("__declspec(noinline)", "__attribute__((noinline))")
    (output / "abi-original.c").write_text(abi_original, encoding="utf-8")
    linux_original = abi_original.replace(
        "int entry(void)", "int (*volatile original_call)(int) = original_helper;\nint entry(void)").replace(
        "original_helper(4)", "original_call(4)")
    (output / "abi-linux.c").write_text(linux_original, encoding="utf-8")
    (output / "abi-windows.c").write_text(pe.ORIGINAL, encoding="utf-8")
    sys.path.insert(0, str(repository / "pluginsdk/python"))
    from neverd_plugin.ffi import HostAPI
    host = HostAPI(ctypes.CDLL(str(args.library)))

    def symbols(path: Path, system: str) -> dict:
        # The loader owns function identity and Thumb-address normalization.
        # NM is used only for the independently bound writable data symbol.
        session = host.function("neverd_session_create")()
        if not session:
            raise RuntimeError("session creation failed")
        found = {}
        try:
            if host.function("neverd_session_load")(session, os.fsencode(path)) != 1:
                raise RuntimeError(host.owned_string("neverd_last_error", session))
            for index in range(host.function("neverd_func_count")(session)):
                name = host.owned_string("neverd_func_name", session, index).removeprefix("_")
                if name in ("target", "original_helper", "original_fp", "original_thrower"):
                    if name in found:
                        raise ValueError("ambiguous original function")
                    found[name] = host.function("neverd_func_entry")(session, index)
        finally:
            host.function("neverd_session_destroy")(session)
        if system == "windows":
            return found
        result = pe.run([str(args.nm), "--defined-only", "--format=posix", str(path)],
                        work / (path.name + "-symbols.log"))
        for line in result.stdout.splitlines():
            cells = line.split()
            if len(cells) >= 3 and cells[0] == ("_cleanup_count" if system == "macos" else "cleanup_count"):
                if "cleanup_count" in found:
                    raise ValueError("ambiguous original writable data")
                found["cleanup_count"] = int(cells[2], 16)
        return found

    def publish(original: Path, target: Path, entry: int, input_path: Path,
                kind: str, bindings: list[str] = (), language: str = "c",
                gcc: Path | None = None) -> dict:
        before = pe.digest(original)
        command = [str(args.neverd), "patch", str(original), "--entry=replacement",
                   f"--func={entry:#x}", "--patch-report", "-o", str(target)]
        command += ["--bind=" + binding for binding in bindings]
        if kind in ("ir", "object"):
            command += ["--from-source=" + str(input_path), "--artifact=" + kind,
                        "--language=" + language, "--compiler-path=" + str(gcc or args.clang)]
            if gcc:
                command += ["--compiler=gcc"]
            if language == "c++":
                command += ["--compiler-arg=-O0"]
        else:
            command += ["--from-" + kind.removeprefix("supplied-") + "=" + str(input_path)]
        result = pe.run(command, target.with_suffix(".patch.log"))
        reports = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        reports = [row for row in reports if row.get("schema") == "neverd.source-patch.v1"]
        if (len(reports) != 1 or reports[0].get("success") is not True or
            len(reports[0].get("installed", [])) != 1 or pe.digest(original) != before):
            raise ValueError("publication receipt or original identity mismatch")
        if kind == "supplied-object" and reports[0].get("object_sha256") != pe.digest(input_path):
            raise ValueError("published object identity mismatch")
        return reports[0]

    def record(system: str, arch: str, case: str, original: Path, image: Path,
               receipt: dict | None = None) -> None:
        before, after = original.read_bytes(), image.read_bytes()
        writes, index = [], 0
        while index < min(len(before), len(after)):
            if before[index] == after[index]:
                index += 1
                continue
            begin = index
            while index < min(len(before), len(after)) and before[index] != after[index]:
                index += 1
            writes.append({"offset": begin, "data": base64.b64encode(zlib.compress(after[begin:index])).decode()})
        if len(after) > len(before):
            writes.append({"offset": len(before), "data": base64.b64encode(zlib.compress(after[len(before):])).decode()})
        delta = {"original_sha256": pe.digest(original), "sha256": pe.digest(image),
                 "size": len(after), "writes": writes}
        if snapshot.apply_delta(before, delta) != after:
            raise ValueError("snapshot delta changed image bytes")
        name = system + "-" + arch + "-" + case + ".json"
        path = output / name
        path.write_text(json.dumps(delta, indent=2) + "\n", encoding="utf-8")
        receipts = []
        if receipt:
            data = portable_receipt(receipt, repository, work, output)
            receipts.append({"sha256": hashlib.sha256(data).hexdigest(),
                             "data": base64.b64encode(zlib.compress(data)).decode()})
        manifest["images"].append({"system": system, "architecture": arch, "case": case,
            "original": original.relative_to(output).as_posix(), "delta": name,
            "delta_sha256": pe.digest(path), "receipts": receipts})

    profiles = [("macos", "x64", "x86_64-apple-macos14.0", "abi-", ()),
                ("macos", "arm64", "arm64-apple-macos14.0", "abi-", ()),
                ("windows", "x86", pe.PROFILES["x86"][0], "abi-", ()),
                ("linux", "x86", "i686-unknown-linux-gnu", "abi-", ()),
                ("linux", "arm32", "armv7-unknown-linux-gnueabihf", "arm-", ("-marm", "-mfpu=vfpv3-d16")),
                ("linux", "arm32", "armv7-unknown-linux-gnueabihf", "thumb-", ("-mthumb", "-mfpu=vfpv3-d16"))]
    for system, arch, triple, prefix, flags in profiles:
        directory = work / (system + "-" + arch + "-" + prefix.rstrip("-"))
        directory.mkdir()
        original = output / "originals" / (directory.name + {"macos": ".macho", "windows": ".exe", "linux": ".elf"}[system])
        obj = directory / "original.o"
        source = output / ("abi-windows.c" if system == "windows" else
                           "abi-linux.c" if system == "linux" else "abi-original.c")
        pe.run([str(args.clang), "--target=" + triple, "-O0", "-funwind-tables",
                *flags, *(["-fPIE"] if system == "linux" else []),
                "-c", str(source), "-o", str(obj)], directory / "compile.log")
        if system == "macos":
            main = directory / "main.c"
            main.write_text("int entry(void); int main(void) { return entry(); }\n")
            pe.run([str(args.clang), "--target=" + triple, str(main), str(obj),
                    "-Wl,-headerpad,0x1000", "-Wl,-segalign,0x4000", "-Wl,-adhoc_codesign",
                    "-o", str(original)], directory / "link.log")
        elif system == "windows":
            pe.run([str(args.linker), str(obj), "/out:" + str(original), "/entry:entry",
                    "/implib:" + str(directory / "original.lib"),
                    "/subsystem:console", "/nodefaultlib", "/dynamicbase", "/debug:dwarf"], directory / "link.log")
        else:
            start = directory / "start.s"
            start.write_text(START["x86" if arch == "x86" else prefix.rstrip("-")])
            pe.run([str(args.clang), "--target=" + triple, *flags, "-c", str(start),
                    "-o", str(directory / "start.o")], directory / "start.log")
            interpreter = "/lib/ld-linux.so.2" if arch == "x86" else "/lib/ld-linux-armhf.so.3"
            pe.run([str(args.elf_linker), "--eh-frame-hdr", "-pie", "--dynamic-linker=" + interpreter,
                    "-e", "_start", str(obj),
                    str(directory / "start.o"), "-o", str(original)], directory / "link.log")
        original_symbols = symbols(original, system)
        record(system, arch, prefix + "original", original, original)
        native_inputs = output / "producers" / directory.name
        native_inputs.mkdir(parents=True)
        native_commands = []
        for case, (_, text) in pe.CASES.items():
            runtime_pointer = system == "macos" and case == "writable-pointer"
            if runtime_pointer:
                text = MACHO_WRITABLE_POINTER
            source = output / (case + ("-runtime" if runtime_pointer else "") + ".c")
            source.write_text(text, encoding="utf-8")
            binding = "original_fp" if case == "floating-stack" else "original_helper"
            bindings = [f"function:{binding}={original_symbols[binding]:#x}"]
            artifacts = pe.ARTIFACTS + (("gcc-source", "gcc-object") if system == "linux" else ())
            for artifact in artifacts:
                image = directory / (case + "-" + artifact)
                input_path, kind = source, artifact
                gcc = getattr(args, "gcc_" + arch) if artifact == "gcc-source" else None
                if gcc:
                    kind = "object"
                if artifact in ("supplied-object", "gcc-object"):
                    input_path, kind = native_inputs / (case + "-" + artifact + ".o"), "supplied-object"
                    compiler = args.clang if artifact == "supplied-object" else getattr(args, "gcc_" + arch)
                    target = ["--target=" + triple] if artifact == "supplied-object" else []
                    command = [str(compiler), *target, *flags, "-O2", "-fno-lto", "-funwind-tables",
                               "-ffunction-sections", *([] if system == "windows" else ["-fPIC"]),
                               "-c", str(source), "-o", str(input_path)]
                    pe.run(command, image.with_suffix(".compile.log"))
                    native_commands.append({"producer": "clang" if artifact == "supplied-object" else "gcc_" + arch,
                        "arguments": [Path(command[0]).name, *command[1:]],
                        "object": input_path.relative_to(output).as_posix(), "sha256": pe.digest(input_path)})
                receipt = publish(original, image, original_symbols["target"], input_path, kind, bindings, gcc=gcc)
                record(system, arch, prefix + case + "-" + artifact, original, image, receipt)
        (native_inputs / "commands.json").write_bytes(portable_receipt(
            {"language": "c", "commands": native_commands}, repository, work, output))
        print(system, arch, prefix, "C ABI fixtures published", flush=True)

    (output / "windows-original-eh.cpp").write_text(cpp.ORIGINAL, encoding="utf-8")
    (output / "windows-exceptions.cpp").write_text(cpp.REPLACEMENT, encoding="utf-8")
    for arch in cpp.ARCHES:
        original = output / "originals" / ("windows-" + arch + "-original-eh.exe")
        shutil.copyfile(repository / "unittests/Inputs/source-patch/native-processes" /
                        (arch + "-original.exe"), original)
        original_symbols = cpp.exports(args.library, original)
        directory = work / ("windows-" + arch + "-exceptions")
        directory.mkdir()
        image = directory / "exceptions.exe"
        receipt = publish(original, image, original_symbols["target"], output / "windows-exceptions.cpp", "ir",
            [f"data:cleanup_count={original_symbols['cleanup_count']:#x}:4",
             f"const:??_7type_info@@6B@={original_symbols['??_7type_info@@6B@']:#x}:8"], "c++")
        record("windows", arch, "original-eh", original, original)
        record("windows", arch, "exceptions", original, image, receipt)
        print("windows", arch, "Clang C++ FH3 fixtures published", flush=True)

    for arch, triple in (("x64", "x86_64-apple-macos14.0"), ("arm64", "arm64-apple-macos14.0")):
        directory = work / ("macos-" + arch + "-producers")
        directory.mkdir()
        originals = {}
        for case in ("original-eh", "original-callback"):
            original = output / "originals" / ("macos-" + arch + "-" + case + ".macho")
            pe.run([str(args.clang), "--driver-mode=g++", "--target=" + triple, "-O0",
                    str(output / (case + ".cpp")), "-Wl,-headerpad,0x1000",
                    "-Wl,-segalign,0x4000", "-Wl,-adhoc_codesign",
                    "-o", str(original)], directory / (case + "-link.log"))
            originals[case] = original
            record("macos", arch, case, original, original)
        producer = output / "producers" / arch
        producer.mkdir(parents=True)
        base = [str(args.neverc), "--no-default-config", "--target=" + triple, "-O2",
                "-funwind-tables", "-fno-discard-value-names", "-fno-lto", "-fPIC"]
        commands = []
        for case in ("c-ir", "c-object", "c-hosted", "hosted"):
            source = output / ("hosted.c" if case == "hosted" else "replacement.c")
            artifact = producer / (case + (".ll" if case == "c-ir" else ".o"))
            command = base + (["-S", "-emit-llvm", "-o", str(artifact)] if case == "c-ir" else
                             ["-c", "-o", str(artifact)] if case == "c-object" else
                             ["-fdyncode", "-fdyncode-hosted", "-fdyncode-entry=replacement",
                              "-fdyncode-keep-obj=" + str(artifact),
                              "-fdyncode-report=" + str(producer / (case + ".audit.json")),
                              "-o", str(producer / (case + ".bin"))]) + ["-x", "c", str(source)]
            # Relative source names keep LLVM source_filename and producer
            # audits portable without rewriting an authenticated artifact.
            pe.run([value.replace(str(output) + "/", "") for value in command],
                   directory / (case + "-producer.log"))
            commands.append([value.replace(str(output) + "/", "<fixtures>/")
                             if i else Path(value).name for i, value in enumerate(command)])
            original = originals["original-callback" if case == "hosted" else "original-eh"]
            entry = symbols(original, "macos")["target"]
            image = directory / (case + ".macho")
            receipt = publish(original, image, entry, artifact,
                              "supplied-ir" if case == "c-ir" else "supplied-object")
            if case in ("c-hosted", "hosted"):
                flat = (producer / (case + ".bin")).read_bytes()
                audit = json.loads((producer / (case + ".audit.json")).read_text())
                if (audit.get("execution_profile") != "hosted-v1" or
                    audit.get("target_triple") != triple or audit.get("keep_unwind") is not True or
                    audit.get("entry_offset") != 0 or audit.get("input_digest") != pe.digest(artifact) or
                    audit.get("output_digest") != hashlib.sha256(flat).hexdigest() or
                    runtime.macho_bytes(image, receipt["installed"][0]["replacement_va"], len(flat)) != flat):
                    raise ValueError("hosted producer audit or installed byte identity mismatch")
            record("macos", arch, case, original, image, receipt)
        (producer / "commands.json").write_text(json.dumps({
            **manifest["producers"]["neverc"], "language": "c", "commands": commands}, indent=2) + "\n")
        original = originals["original-eh"]
        original_symbols = symbols(original, "macos")
        image = directory / "exceptions.macho"
        receipt = publish(original, image, original_symbols["target"], output / "exceptions.cpp", "ir",
            [f"data:cleanup_count={original_symbols['cleanup_count']:#x}:4",
             f"function:original_thrower={original_symbols['original_thrower']:#x}"], "c++")
        record("macos", arch, "exceptions", original, image, receipt)
        print("macos", arch, "C producer/hosted and Clang C++ fixtures published", flush=True)
    if neverc_digest != pe.digest(args.neverc):
        raise ValueError("NeverC executable changed during qualification; retain and repeat this attempt")
    for system, arch in runtime.PROFILES:
        rows = [row for row in manifest["images"] if (row["system"], row["architecture"]) == (system, arch)]
        if len(rows) != len(runtime.cases(system, arch)) or {row["case"] for row in rows} != set(runtime.cases(system, arch)):
            raise ValueError("published fixture inventory is incomplete")
    manifest["source_sha256"] = {path.relative_to(output).as_posix(): pe.digest(path)
        for path in sorted(output.rglob("*")) if path.is_file() and
        path.name != ".neverc-output.lock" and
        (path.parent == output and path.suffix in (".c", ".cpp") or "producers" in path.relative_to(output).parts)}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("Published", len(manifest["images"]), "authenticated process fixtures")


if __name__ == "__main__":
    main()
