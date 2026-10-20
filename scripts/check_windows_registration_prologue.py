#!/usr/bin/env python3
"""Check shared PE32 C++ prologue receipts and physical effects in safe fixtures."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_realigned import require_test_result
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_libraries import load_libraries
else:
    from check_windows_registration_eh import run_image
    from check_windows_registration_realigned import require_test_result
    from check_windows_registration_rewrite import PE32
    from windows_registration_libraries import load_libraries


def assembly(allocation: int, variant: int, wide: bool) -> str:
    """Compare independent machine execution with each live byte's last writer."""
    stack = -28 - allocation
    cookie = ["mov eax, dword ptr [___security_cookie]", "xor eax, ebp"]
    values = {
        "ebp": ["mov eax, 0x12345678"],
        "handler": ["mov eax, offset _prologue_handler"],
        "chain": ["mov eax, dword ptr [old_chain]"],
        "ebx": ["mov eax, dword ptr [_prologue_seed]"],
        "esi": ["mov eax, 0x3456789a"],
        "edi": ["mov eax, 0x456789ab"],
        "cookie": cookie,
        "stack": [f"lea eax, [ebp{stack:+}]"],
        "state": ["mov eax, -1"],
    }
    writes = [(-8, "handler"), (-12, "chain"),
              (-16 - allocation, "ebx"), (-20 - allocation, "esi"),
              (-24 - allocation, "edi"), (0, "ebp"), (stack, "cookie")]
    if variant:
        writes.append((-16, "stack" if variant == 1 else "cookie"))
    writes.append((-4, "state"))
    last_writer = {}
    for offset, value in writes:
        for byte in range(4):
            last_writer[offset + byte] = (value, byte)
    checks = []
    for offset, (value, byte) in sorted(last_writer.items()):
        checks.extend(values[value])
        if byte:
            checks.append(f"shr eax, {byte * 8}")
        checks.extend([f"cmp byte ptr [ebp{offset:+}], al", "jne failed"])
    push = f".byte 0x68\n.long {allocation}" if wide else f"push {allocation}"
    extra = ("mov dword ptr [ebp-16], esp" if variant == 1 else
             "mov dword ptr [ebp-16], eax" if variant == 2 else "")
    return f"""
.intel_syntax noprefix
.text
.globl _mainCRTStartup
_mainCRTStartup:
  mov dword ptr [main_stack], esp
  mov eax, dword ptr fs:[0]
  mov dword ptr [old_chain], eax
  call _prologue_state_parent
  cmp esp, dword ptr [main_stack]
  jne state_failed
  mov eax, dword ptr fs:[0]
  cmp eax, dword ptr [old_chain]
  jne state_failed
  mov ebp, 0x12345678
  mov ebx, 0x23456789
  mov esi, 0x3456789a
  mov edi, 0x456789ab
  mov ecx, 0x56789abc
  mov edx, 0x6789abcd
  call _prologue_parent
parent_return:
  push eax
  call dword ptr ["__imp__ExitProcess@4"]
  int3
state_failed:
  push 1
  call dword ptr ["__imp__ExitProcess@4"]
  int3
.p2align 4, 0x90
.globl _prologue_state_parent
_prologue_state_parent:
  {push}
  mov eax, offset _prologue_handler
  call _prologue_helper
  mov dword ptr [ebp-4], 0
  mov dword ptr [ebp-4], -1
  mov ecx, dword ptr [ebp-12]
  mov dword ptr fs:[0], ecx
  mov esp, ebp
  pop ebp
  xor eax, eax
  ret
.p2align 4, 0x90
.globl _prologue_parent
_prologue_parent:
  {push}
  mov eax, offset _prologue_handler
  call _prologue_helper
  pushfd
  pop dword ptr [actual_flags]
  cmp ecx, 0x56789abc
  jne failed
  cmp edx, 0x6789abcd
  jne failed
  cmp ebx, dword ptr [_prologue_seed]
  jne failed
  cmp esi, 0x3456789a
  jne failed
  cmp edi, 0x456789ab
  jne failed
  lea ecx, [ebp-12]
  cmp eax, ecx
  jne failed
  cmp dword ptr fs:[0], ecx
  jne failed
  lea ecx, [ebp{stack:+}]
  cmp esp, ecx
  jne failed
  mov eax, dword ptr [main_stack]
  sub eax, 8
  cmp ebp, eax
  jne failed
  cmp dword ptr [ebp+4], offset parent_return
  jne failed
  mov eax, dword ptr [___security_cookie]
  xor eax, ebp
  pushfd
  pop ecx
  xor ecx, dword ptr [actual_flags]
  test ecx, 0x8c5
  jne failed
  {chr(10).join(checks)}
  xor eax, eax
  jmp finish
failed:
  mov eax, 1
finish:
  mov ecx, dword ptr [old_chain]
  mov dword ptr fs:[0], ecx
  lea esp, [ebp+4]
  mov ebp, dword ptr [ebp]
  ret
.p2align 4, 0x90
.globl _prologue_helper
_prologue_helper:
  push eax
  push dword ptr fs:[0]
  lea eax, [esp+12]
  sub esp, dword ptr [esp+12]
  push ebx
  push esi
  push edi
  mov dword ptr [eax], ebp
  .byte 0x8b, 0xe8
  mov eax, dword ptr [___security_cookie]
  .byte 0x33, 0xc5
  push eax
  {extra}
  push dword ptr [ebp-4]
  mov dword ptr [ebp-4], -1
  lea eax, [ebp-12]
  mov dword ptr fs:[0], eax
  ret
.p2align 4, 0x90
.globl _prologue_handler
_prologue_handler:
  mov eax, offset func_info
  jmp ___CxxFrameHandler3
.safeseh _prologue_handler
.section .rdata,"dr"
.p2align 2
func_info:
  .long 0x19930522, 1, unwind_map, 0, 0, 0, 0, 0, 1
unwind_map:
  .long -1, 0
.globl __load_config_used
__load_config_used:
  .long 72
  .zero 56
  .long ___security_cookie
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
.def @feat.00; .scl 3; .type 0; .endef
.set @feat.00,1
.data
.p2align 2
___security_cookie: .long 0xbb40e64e
main_stack: .long 0
old_chain: .long 0
actual_flags: .long 0
.globl _prologue_allocation, _prologue_variant, _prologue_wide, _prologue_seed
_prologue_allocation: .long {allocation}
_prologue_variant: .long {variant}
_prologue_wide: .long {int(wide)}
_prologue_seed: .long 0x23456789
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--runtime-libs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine", help="execute only the generated safe fixtures")
    parser.add_argument("--wine-prefix", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "passed": False, "commands": [], "cases": [],
              "evidence": "physical-prologue-source-and-wine" if args.wine else
                          "static-physical-prologue-source"}

    def run(command, environment=None):
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, text=True,
                                errors="replace", timeout=120, cwd=output,
                                env=os.environ | (environment or {}))
        report["commands"].append({"command": command,
                                   "exit_code": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("physical prologue build or source proof failed")

    try:
        libraries, report["runtime_libraries"] = load_libraries(
            args.runtime_libs.resolve())
        environment = os.environ | {
            "WINEPREFIX": str((args.wine_prefix or output / "wine-prefix").resolve()),
            "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "mscoree,mshtml=d"}
        for wide in (False, True):
            for variant in range(3):
                for allocation in (0, 1, 4, 32):
                    case = output / f"{'wide' if wide else 'short'}-{variant}-{allocation}"
                    case.mkdir(exist_ok=True)
                    source, obj, image, xml = (case / name for name in
                                              ("source.s", "source.obj",
                                               "original.exe", "proof.xml"))
                    source.write_text(assembly(allocation, variant, wide))
                    record = {"wide": wide, "variant": variant,
                              "allocation": allocation, "passed": False}
                    report["cases"].append(record)
                    run([args.compiler, "--target=i686-pc-windows-msvc",
                         "-c", source, "-o", obj])
                    exports = ["prologue_" + name for name in
                               ("parent", "state_parent", "helper", "handler",
                                "allocation", "variant", "wide", "seed")]
                    run([args.linker, "/entry:mainCRTStartup", "/nodefaultlib",
                         "/machine:x86", "/subsystem:console", "/fixed:no",
                         "/dynamicbase:no", "/out:" + str(image), obj,
                         *("/export:" + name for name in exports), *libraries])
                    record["image_sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
                    run([args.test_binary.resolve(),
                         "--gtest_filter=RegistrationTryLevel.CxxEAXHelperInputPE32*",
                         "--gtest_output=xml:" + str(xml)],
                        {"NEVERD_REGISTRATION_PROLOGUE_INPUT_PE32": str(image)})
                    if require_test_result(xml) != 1:
                        raise ValueError("physical prologue source test missing or skipped")
                    if args.wine:
                        pe = PE32(image.read_bytes())
                        wrong = case / "wrong-seed.exe"
                        changed = bytearray(pe.data)
                        struct.pack_into("<I", changed, pe.raw(pe.entry(b"prologue_seed")), 0)
                        wrong.write_bytes(changed)
                        record["runtime"] = []
                        for original, expected in ((image, 0), (wrong, 1)):
                            rebased = original.with_stem(original.stem + "-rebased")
                            rebased.write_bytes(PE32(original.read_bytes()).rebase(0x500000))
                            for executable in (original, rebased):
                                result = run_image(executable, [args.wine], environment, 40)
                                result.update(expected_exit=expected,
                                              image_sha256=hashlib.sha256(
                                                  executable.read_bytes()).hexdigest())
                                record["runtime"].append(result)
                                if result.get("exit_code") != expected:
                                    raise ValueError("physical prologue machine state differs")
                    record["passed"] = True
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("physical prologue proof:", "PASS" if report["passed"] else "FAIL")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
