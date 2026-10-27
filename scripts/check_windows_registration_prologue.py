#!/usr/bin/env python3
"""Check shared PE32 C++ frame calls and physical effects in safe fixtures."""
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
    guarded_checks = """
  call _guarded_state_parent
  cmp eax, 0x11223344
  jne state_failed
  cmp esp, dword ptr [main_stack]
  jne state_failed
  mov eax, dword ptr fs:[0]
  cmp eax, dword ptr [old_chain]
  jne state_failed
  mov ebp, 0x12345678
  mov ebx, 0x23456789
  mov esi, 0x3456789a
  mov edi, 0x456789ab
  call _guarded_parent
  pushfd
  pop dword ptr [actual_flags]
  cmp eax, 0x11223344
  jne state_failed
  cmp edx, 0x22334455
  jne state_failed
  cmp ecx, offset guarded_return
  jne state_failed
  cmp ebp, 0x12345678
  jne state_failed
  cmp ebx, 0x23456789
  jne state_failed
  cmp esi, 0x3456789a
  jne state_failed
  cmp edi, 0x456789ab
  jne state_failed
  cmp esp, dword ptr [main_stack]
  jne state_failed
  mov eax, dword ptr fs:[0]
  cmp eax, dword ptr [old_chain]
  jne state_failed
  mov eax, dword ptr [actual_flags]
  and eax, 0x8d5
  cmp eax, 0x44
  jne state_failed
""" if variant == 2 and allocation >= 4 else ""
    return f"""
.intel_syntax noprefix
.text
.globl _mainCRTStartup
_mainCRTStartup:
  mov dword ptr [main_stack], esp
  mov eax, dword ptr fs:[0]
  mov dword ptr [old_chain], eax
  call _prologue_state_parent
  test eax, eax
  jne state_failed
  cmp esp, dword ptr [main_stack]
  jne state_failed
  mov eax, dword ptr fs:[0]
  cmp eax, dword ptr [old_chain]
  jne state_failed
  call _epilogue_state_parent
  cmp eax, 0x11223344
  jne state_failed
  cmp esp, dword ptr [main_stack]
  jne state_failed
  mov eax, dword ptr fs:[0]
  cmp eax, dword ptr [old_chain]
  jne state_failed
  mov ebp, 0x12345678
  mov ebx, 0x23456789
  mov esi, 0x3456789a
  mov edi, 0x456789ab
  call _epilogue_parent
  pushfd
  pop dword ptr [actual_flags]
  cmp eax, 0x11223344
  jne state_failed
  cmp edx, 0x22334455
  jne state_failed
  cmp ecx, offset epilogue_return
  jne state_failed
  cmp ebp, 0x12345678
  jne state_failed
  cmp ebx, dword ptr [epilogue_ebx]
  jne state_failed
  cmp esi, dword ptr [epilogue_esi]
  jne state_failed
  cmp edi, dword ptr [epilogue_edi]
  jne state_failed
  cmp esp, dword ptr [main_stack]
  jne state_failed
  mov eax, dword ptr fs:[0]
  cmp eax, dword ptr [old_chain]
  jne state_failed
  mov eax, dword ptr [actual_flags]
  xor eax, dword ptr [epilogue_flags]
  test eax, 0x8d5
  jne state_failed
  {guarded_checks}
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
  and dword ptr [ebp-4], 0
  setnz al
  movzx eax, al
  or dword ptr [ebp-4], -1
  setns cl
  movzx ecx, cl
  or eax, ecx
  mov ecx, dword ptr [ebp-12]
  mov dword ptr fs:[0], ecx
  mov esp, ebp
  pop ebp
  ret
.p2align 4, 0x90
.globl _epilogue_state_parent
_epilogue_state_parent:
  {push}
  mov eax, offset _prologue_handler
  call _prologue_helper
  and dword ptr [ebp-4], 0
  or dword ptr [ebp-4], -1
  xor ebx, ebx
  xor esi, esi
  xor edi, edi
  mov eax, 0x11223344
  call _epilogue_helper
  ret
.p2align 4, 0x90
.globl _epilogue_parent
_epilogue_parent:
  {push}
  mov eax, offset _prologue_handler
  call _prologue_helper
  mov eax, dword ptr [esp+12]
  mov dword ptr [epilogue_ebx], eax
  mov eax, dword ptr [esp+8]
  mov dword ptr [epilogue_esi], eax
  mov eax, dword ptr [esp+4]
  mov dword ptr [epilogue_edi], eax
  xor ebx, ebx
  xor esi, esi
  xor edi, edi
  mov eax, 0x11223344
  mov edx, 0x22334455
  cmp edx, eax
  pushfd
  pop dword ptr [epilogue_flags]
  call _epilogue_helper
epilogue_return:
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
.globl _epilogue_helper
_epilogue_helper:
  mov ecx, dword ptr [ebp-12]
  mov dword ptr fs:[0], ecx
  pop ecx
  pop edi
  pop edi
  pop esi
  pop ebx
  .byte 0x8b, 0xe5
  pop ebp
  push ecx
  ret
.p2align 4, 0x90
.globl _guarded_state_parent
_guarded_state_parent:
  {push}
  mov eax, offset _guarded_handler
  call _prologue_helper
  and dword ptr [ebp-4], 0
  or dword ptr [ebp-4], -1
  xor ebx, ebx
  xor esi, esi
  xor edi, edi
  mov eax, 0x11223344
  call _guarded_helper
  ret
.p2align 4, 0x90
.globl _guarded_parent
_guarded_parent:
  {push}
  mov eax, offset _guarded_handler
  call _prologue_helper
  mov eax, dword ptr [_guarded_cookie_mask]
  xor dword ptr [ebp-16], eax
  xor ebx, ebx
  xor esi, esi
  xor edi, edi
  mov eax, 0x11223344
  mov edx, 0x22334455
  cmp eax, edx
  call _guarded_helper
guarded_return:
  ret
.p2align 4, 0x90
.globl _guarded_helper
_guarded_helper:
  mov ecx, dword ptr [ebp-16]
  .byte 0x33, 0xcd
  call _guarded_checker
  .byte 0xe9
  .long _epilogue_helper - . - 4
.p2align 4, 0x90
_guarded_checker:
  .byte 0x3b, 0x0d
  .long ___security_cookie
  jne guarded_failed
  .byte 0xf3, 0xc3
guarded_failed:
  push 3
  call dword ptr ["__imp__ExitProcess@4"]
  int3
.p2align 4, 0x90
.globl _guarded_handler
_guarded_handler:
  mov edx, dword ptr [esp+8]
  lea eax, [edx+12]
  mov ecx, dword ptr [edx{stack + 12:+}]
  .byte 0x33, 0xc8
  call _guarded_checker
  mov ecx, dword ptr [edx-4]
  .byte 0x33, 0xc8
  call _guarded_checker
  mov eax, offset func_info
  jmp ___CxxFrameHandler3
.safeseh _guarded_handler
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
epilogue_flags: .long 0
epilogue_ebx: .long 0
epilogue_esi: .long 0
epilogue_edi: .long 0
.globl _prologue_allocation, _prologue_variant, _prologue_wide, _prologue_seed
_prologue_allocation: .long {allocation}
_prologue_variant: .long {variant}
_prologue_wide: .long {int(wide)}
_prologue_seed: .long 0x23456789
.globl _guarded_cookie_mask
_guarded_cookie_mask: .long 0
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
                    exports += ["epilogue_parent", "epilogue_state_parent",
                                "epilogue_helper"]
                    exports += ["guarded_parent", "guarded_state_parent",
                                "guarded_helper", "guarded_handler",
                                "guarded_cookie_mask"]
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
                        wrong_state = case / "wrong-state.exe"
                        changed = bytearray(pe.data)
                        update = pe.raw(pe.entry(b"prologue_state_parent") +
                                        (15 if wide else 12))
                        if changed[update:update + 4] != b"\x83\x65\xfc\x00":
                            raise ValueError("independent AND-state control has changed")
                        changed[update + 3] = 1
                        wrong_state.write_bytes(changed)
                        wrong_exit = case / "wrong-epilogue.exe"
                        changed = bytearray(pe.data)
                        restore = pe.raw(pe.entry(b"epilogue_helper")) + 13
                        if changed[restore] != 0x5e:
                            raise ValueError("independent epilogue restore has changed")
                        changed[restore] = 0x5f
                        wrong_exit.write_bytes(changed)
                        images = [(image, 0), (wrong, 1), (wrong_state, 1),
                                  (wrong_exit, 1)]
                        if variant == 2 and allocation >= 4:
                            wrong_cookie = case / "wrong-cookie.exe"
                            changed = bytearray(pe.data)
                            struct.pack_into("<I", changed,
                                             pe.raw(pe.entry(b"guarded_cookie_mask")), 1)
                            wrong_cookie.write_bytes(changed)
                            # The actual checker mismatch branch exits with
                            # its own code, distinct from a harness failure.
                            images.append((wrong_cookie, 3))
                        record["runtime"] = []
                        for original, expected in images:
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
