#!/usr/bin/env python3
"""Test a fixed native x64 continuation bridge; grant no reconstruction authority."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess

if __package__:
    from .check_windows_native_seh_resume import compile_step
    from .check_windows_nonlocal_finally import require_architecture
    from .check_windows_registration_eh import run_image
else:
    from check_windows_native_seh_resume import compile_step
    from check_windows_nonlocal_finally import require_architecture
    from check_windows_registration_eh import run_image

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unittests/lift/eh/fixtures"
SOURCES = ("native_seh_bridge.ll", "native_seh_bridge.s",
           "native_seh_bridge_runtime.c", "native_seh_resume_probe.asm")
EXPECTED = (1234782349, 123782349, 136782349, 13782349,
            123478239, 12378239, 13678239, 1378239,
            1, 1, 1, 1, 1, 1, 256)
OBSERVATION = re.compile(
    r"NATIVE_SEH_BRIDGE " + r"(\d+) " * 8 +
    r"registers=(\d+) stack=(\d+) guards=(\d+) callbacks=(\d+) "
    r"codes=(\d+) consistent=(\d+) iterations=(\d+) "
    r"runtime=([^\r\n]+?) unwind=([^\r\n]+?) handler=([^\r\n]+?)\s*")


def observe(result: dict) -> tuple[int, ...]:
    match = OBSERVATION.fullmatch(result.get("stdout", ""))
    if result.get("exit_code") != 0 or not match:
        raise ValueError("native bridge execution did not complete")
    values = tuple(map(int, match.groups()[:15]))
    if values != EXPECTED:
        raise ValueError("native bridge continuation, callbacks, exceptions or ABI differ")
    return values


def replace_once(source: str, before: str, after: str) -> str:
    if source.count(before) != 1:
        raise ValueError("negative control no longer matches the fixed fixture")
    return source.replace(before, after)


def negative_variants(body: str, bridge: str) -> dict[str, tuple[str, str]]:
    # These are separately compiled corrupt fixtures, never production rewrites.
    return {
        "excluded-call-scope-end": (body, replace_once(
            bridge, "  call QWORD PTR [rip+__imp_RaiseException]\n  nop\n.Ltry_end:",
            ".Ltry_end:\n  call QWORD PTR [rip+__imp_RaiseException]\n  nop")),
        "missing-unwind-flag": (body, replace_once(
            bridge, "@except, @unwind", "@except")),
        "repeat-ordinary-suffix": (body, replace_once(
            bridge, "lea rax, [rip+.Lresume_repeat]",
            "lea rax, [rip+.Lrepeat_normal_suffix]")),
        "abnormal-byte-alias": (replace_once(
            body, "  %actual = zext i8 %abnormal to i32",
            "  %first.call = icmp eq i32 %count, 0\n"
            "  %actual = select i1 %first.call, i32 0, i32 2"), bridge),
        "unexpected-before-correct-code": (body, replace_once(
            bridge, ".Lsecond_begin:",
            "  sub rsp, 48\n"
            "  mov DWORD PTR [rsp+32], 0xe0424444\n"
            "  lea rax, [rsp+32]\n"
            "  mov QWORD PTR [rsp+40], rax\n"
            "  lea rcx, [rsp+40]\n"
            "  mov rdx, rbp\n"
            "  call native_resume_filter\n"
            "  add rsp, 48\n.Lsecond_begin:")),
        "shifted-target-frame": (body, replace_once(
            bridge, "  mov QWORD PTR [rsp+56], rsp",
            "  lea rax, [rsp+8]\n  mov QWORD PTR [rsp+56], rax")),
    }


def observe_negative(name: str, result: dict) -> None:
    if name == "shifted-target-frame":
        # A crash/timeout is supporting corruption evidence, never an explicit
        # production guard or reconstruction refusal certificate.
        if result.get("error") or "NATIVE_SEH_BRIDGE_STARTED\n" not in result.get("stderr", ""):
            raise ValueError("shifted frame process did not start and terminate normally")
        if result.get("exit_code") in (None, 0, 2):
            raise ValueError("shifted frame has no observed corruption termination")
        return
    match = OBSERVATION.fullmatch(result.get("stdout", ""))
    if result.get("exit_code") not in (0, 1) or not match:
        raise ValueError("negative control did not reach its intended observer")
    values = tuple(map(int, match.groups()[:15]))
    expected = list(EXPECTED)
    if name in ("excluded-call-scope-end", "missing-unwind-flag"):
        expected[2:4] = (16782349, 16782349)
        expected[6:8] = (1678239, 1678239)
        expected[11] = 0
    elif name == "repeat-ordinary-suffix":
        expected[4:8] = EXPECTED[:4]
    elif name == "abnormal-byte-alias":
        expected[11] = 0
    elif name == "unexpected-before-correct-code":
        expected[12] = 0
    else:
        raise ValueError("unknown native bridge negative control")
    if values != tuple(expected):
        raise ValueError("negative control failed outside its intended observer")
    if result["exit_code"] != (0 if name == "repeat-ordinary-suffix" else 1):
        raise ValueError("negative control has an unrelated process exit")


def capture_providers(stdout: str, output: Path) -> list[dict]:
    match = OBSERVATION.fullmatch(stdout)
    if not match:
        raise ValueError("native bridge has no provider observation")
    providers = []
    for role, value in zip(("vcruntime", "local-unwind", "specific-handler"),
                           match.groups()[15:]):
        path = Path(value.strip())
        if not path.is_absolute():
            raise ValueError("native bridge provider is not an absolute path")
        if role == "vcruntime" and path.name.lower() != "vcruntime140.dll":
            raise ValueError("native bridge has no VCRUNTIME140 identity")
        data = path.read_bytes()
        require_architecture(data, "x64")
        digest = hashlib.sha256(data).hexdigest()
        captured = output / "runtime-providers" / digest / path.name
        captured.parent.mkdir(parents=True, exist_ok=True)
        captured.write_bytes(data)
        providers.append({"role": role, "path": str(path), "sha256": digest,
                          "captured": str(captured)})
    return providers


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler", default="cl")
    parser.add_argument("--llvm-compiler", default="clang")
    parser.add_argument("--assembler", default="ml64")
    parser.add_argument("--llvm-readobj", default="llvm-readobj")
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("this experiment requires native Windows x64")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "experimental-fixed-native-x64-bridge",
              "platform": platform.platform(), "architecture": "x64",
              "toolset": os.environ.get("VCToolsVersion", ""),
              "source_sha256": {name: hashlib.sha256(
                  (FIXTURES / name).read_bytes()).hexdigest() for name in SOURCES},
              "passed": False, "reconstruction_authority": False, "cases": [],
              "limitations": ["fixed trusted frame and descriptors only",
                              "no arbitrary source writes, nesting or concurrency proof",
                              "EHCONT and CET enforcement not tested"]}
    try:
        if os.environ.get("VSCMD_ARG_TGT_ARCH") != "x64" or not report["toolset"]:
            raise ValueError("select the native x64 MSVC environment first")
        version = subprocess.run([args.llvm_compiler, "--version"],
                                 capture_output=True, text=True, errors="replace",
                                 timeout=30)
        report["llvm_toolchain"] = {"exit_code": version.returncode,
                                    "stdout": version.stdout, "stderr": version.stderr}
        if version.returncode:
            raise ValueError("LLVM bridge compiler is unavailable")
        variants = negative_variants((FIXTURES / SOURCES[0]).read_text(),
                                     (FIXTURES / SOURCES[1]).read_text())
        profiles = [(f"O{opt}", opt, None) for opt in (0, 2)]
        profiles += [(name, 2, variant) for name, variant in variants.items()]
        for name, optimization, variant in profiles:
            case = output / name
            case.mkdir(exist_ok=True)
            record = {"case": name, "passed": False,
                      "negative_control": variant is not None}
            report["cases"].append(record)
            body, bridge = FIXTURES / SOURCES[0], FIXTURES / SOURCES[1]
            if variant is not None:
                body, bridge = case / "body.ll", case / "bridge.s"
                body.write_text(variant[0])
                bridge.write_text(variant[1])
            inputs = (body, bridge, FIXTURES / SOURCES[2], FIXTURES / SOURCES[3])
            record["compiled_sources"] = {str(path): hashlib.sha256(
                path.read_bytes()).hexdigest() for path in inputs}
            llvm_object, bridge_object = case / "body.obj", case / "bridge.obj"
            wrapper, image = case / "probe.obj", case / "bridge.exe"
            common = [args.llvm_compiler, "-target", "x86_64-pc-windows-msvc",
                      "-Werror", "-Wno-override-module"]
            compile_step([*common, f"-O{optimization}", "-x", "ir", "-c",
                          str(body), "-o", str(llvm_object)], case, record)
            compile_step([*common, "-c", str(bridge), "-o", str(bridge_object)],
                         case, record)
            compile_step([args.assembler, "/nologo", "/c", f"/Fo{wrapper}",
                          str(inputs[3])], case, record)
            compile_step([args.compiler, "/nologo", "/W4", "/WX", "/TC",
                          "/Od" if optimization == 0 else "/O2", "/GS", "/MD", "/Z7",
                          f"/Fo{case / 'runtime.obj'}", f"/Fe{image}",
                          f"/DBRIDGE_CORRUPTION_PROBE={int(name == 'shifted-target-frame')}",
                          str(inputs[2]), str(llvm_object), str(bridge_object), str(wrapper),
                          "/link", "/debug", "/incremental:no", "/fixed:no",
                          f"/pdb:{case / 'bridge.pdb'}", f"/map:{case / 'bridge.map'}"],
                         case, record)
            data = image.read_bytes()
            require_architecture(data, "x64")
            record["sha256"] = hashlib.sha256(data).hexdigest()
            # Preserve both assembler-owned and final linked unwind/configuration data.
            compile_step([args.llvm_readobj, "--unwind", "--coff-load-config",
                          str(bridge_object), str(image)], case, record)
            record["runtime"] = run_image(image, [], os.environ.copy(), 30)
            if variant is None:
                record["values"] = observe(record["runtime"])
                record["loaded_providers"] = capture_providers(
                    record["runtime"]["stdout"], output)
            else:
                observe_negative(name, record["runtime"])
                record["observed_rejection"] = "expected corrupt-control observation"
            record["passed"] = True
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    destination = output / "native-seh-bridge.json"
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "report": str(destination),
                      "error": report.get("error"), "reconstruction_authority": False}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
