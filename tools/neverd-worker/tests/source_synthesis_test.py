#!/usr/bin/env python3
"""GUI source requests and CLI flags reach the real semantic synthesis engine."""
import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from transport_test import Client


ENTRY = 0x400078
WEIGHTED_MASKS = (0x7A6D35C1, 0xB53C678A, 0x4EF0A903, 0xD2195A6C)
WEIGHTED_UNION = 0
for _mask in WEIGHTED_MASKS:
    WEIGHTED_UNION |= _mask
VIEW_MASKS = tuple(0x10001 << bit for bit in range(16))
VIEW_KEYS = tuple((0x13579BDF * (bit + 1)) & 0xFFFFFFFF for bit in range(16))
VIEW_KEY = 0
for _mask, _key in zip(VIEW_MASKS, VIEW_KEYS):
    VIEW_KEY |= _mask & _key


def affine_elf():
    # System V x64: 182*x + 73*y + 211*z + 37
    #                + (x >> 4) - ((x >> 2) >> 2), all in 32-bit registers.
    # Keep the original shifts as distinct instructions, independent of a
    # fixture compiler's optimization and instruction-selection policies.
    code = bytes.fromhex(
        "69c7b6000000 6bce49 01c8 69cad3000000 01c8 83c025 "
        "89f9 c1e904 89fa c1ea02 c1ea02 29d1 01c8 c3")
    return elf(code)


def quadratic_elf():
    # System V x64: 11*x*x + 73*x*y + 182*y + 211*z + 37, plus the
    # same right-shift residual as the affine fixture.
    code = bytes.fromhex(
        "89f8 0fafc7 6bc00b 89f9 0fafce 6bc949 01c8 "
        "69ceb6000000 01c8 69cad3000000 01c8 83c025 "
        "89f9 c1e904 89fa c1ea02 c1ea02 29d1 01c8 c3")
    return elf(code)


def masked_elf():
    # System V x64: 37 * (((x ^ 0x39) | (y ^ 0x6d)) +
    #                    (x ^ ~0x39)) + 37, then OR 0x55 and add z.
    # The equivalent two-XOR form is (37*((x ^ ~0x39)&(y ^ 0x6d))|0x55)+z.
    code = bytes.fromhex(
        "89f8 3539000000 89f1 83f16d 09c8 83f7c6 01f8 "
        "6bc025 83c025 83c855 01d0 c3")
    return elf(code)


def frame_elf():
    # Store all three incoming registers. The unused middle store leaves an
    # empty source block; the two surviving homes feed (z|~x)+z+1+(z&~x).
    code = bytes.fromhex(
        "55 4889e5 897dfc 8975f8 8955f4 8b4dfc f7d1 "
        "8b45f4 09c8 0345f4 ffc0 234df4 01c8 5d c3")
    return elf(code)


def weighted_elf():
    # Fifteen overlapping masks form an inclusion-exclusion union over x^y.
    # Keep every AND/add/sub in machine code, then scale the sum and add z.
    code = bytearray.fromhex("89f9 31f1 31c0")
    for subset in range(1, 1 << len(WEIGHTED_MASKS)):
        mask = 0xFFFFFFFF
        for bit, value in enumerate(WEIGHTED_MASKS):
            if subset & (1 << bit):
                mask &= value
        code += bytes.fromhex("4189c8 4181e0") + struct.pack("<I", mask)
        code += bytes.fromhex("4401c0" if subset.bit_count() % 2 else "4429c0")
    code += bytes.fromhex("6bc025 01d0 c3")
    return elf(code)


def bitwise_elf():
    # Independently flipped, disjoint pieces of x^y recombine into one XOR.
    # Encode every piece directly so a fixture compiler cannot simplify it.
    code = bytearray.fromhex("89f9 31f1 31c0")
    for mask, key in zip(VIEW_MASKS, VIEW_KEYS):
        code += bytes.fromhex("4189c8 4181f0") + struct.pack("<I", key)
        code += bytes.fromhex("4181e0") + struct.pack("<I", mask)
        code += bytes.fromhex("4401c0")
    code += bytes.fromhex("6bc025 01d0 c3")
    return elf(code)


def elf(code):
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, ENTRY,
                         64, 0, 0, 64, 56, 1, 64, 0, 0)
    segment = struct.pack("<IIQQQQQQ", 1, 5, 0, 0x400000, 0x400000,
                          120 + len(code), 120 + len(code), 4096)
    return header + segment + code


def ok(client, operation, payload):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def source(client, representation, symbolic):
    return ok(client, "decompile", {
        "address": hex(ENTRY), "representation": representation, "limit": 2048,
        "source_options": {"symbolic": True} if symbolic else {},
    })["text"]


def function_name(text):
    # The worker recognizes the ELF entry as start; CLI's --no-debug mode may
    # retain its address-derived name. Both must identify this exact function.
    match = re.search(r"^\s*u?int(?:32|64)_t\s+(start|sub_400078)\s*\(", text, re.M)
    assert match, text
    return match.group(1)


HARNESS = r"""
#include <stdint.h>
int main(void) {
  static const uint32_t edges[] = {
    0, 1, 15, 16, 127, 128, 255, 256, 0x7fffffffU, 0x80000000U, 0xffffffffU
  };
  for (unsigned i = 0; i < sizeof(edges) / sizeof(*edges); ++i)
    for (unsigned j = 0; j < sizeof(edges) / sizeof(*edges); ++j)
      for (unsigned k = 0; k < sizeof(edges) / sizeof(*edges); ++k) {
        uint32_t x = edges[i], y = edges[j], z = edges[k];
        uint32_t want = EXPECTED_RESULT;
        if ((uint32_t)neverd_synthesis_subject(x, y, z) != want)
          return 1;
      }
  uint32_t state = 0x12345678U;
  for (unsigned i = 0; i < 4096; ++i) {
    uint32_t x = (state = 1664525U*state + 1013904223U);
    uint32_t y = (state = 1664525U*state + 1013904223U);
    uint32_t z = (state = 1664525U*state + 1013904223U);
    if ((uint32_t)neverd_synthesis_subject(x, y, z) != EXPECTED_RESULT)
      return 2;
  }
  return 0;
}
"""


def execute_source(root, name, text, compiler, expected):
    path = root / (name + ".c")
    alias = f"\n#define neverd_synthesis_subject {function_name(text)}\n"
    harness = HARNESS.replace("EXPECTED_RESULT", expected)
    path.write_text("#include <stdint.h>\n" + text + alias + harness,
                    encoding="utf-8")
    for level in ("-O0", "-O2"):
        binary = root / (name + level + ".exe")
        subprocess.run([compiler, "-std=c11", level, "-Werror=return-type",
                        "-Werror=implicit-function-declaration", str(path),
                        "-o", str(binary)], check=True, capture_output=True,
                       text=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)


def run_fixture(worker, cli, compiler, kind, image, expected):
    def enhanced_shape(text):
        if kind == "masked":
            assert text.count("^") <= 2, text
        elif kind == "frame":
            assert all(op not in text for op in ("|", "^", "&")), text
        elif kind == "weighted":
            assert text.count("&") <= 1 and text.count("^") <= 1, text
        elif kind == "bitwise":
            assert "&" not in text and text.count("^") <= 2, text
        else:
            assert ">>" not in text, text

    with tempfile.TemporaryDirectory(prefix="neverd-source-synthesis-") as directory:
        root = Path(directory)
        fixture = root / (kind + ".elf")
        fixture.write_bytes(image)
        outputs = {}
        with Client(worker) as client:
            ok(client, "open", {"path": str(fixture), "read_only": True})
            for representation in ("source", "llvmc"):
                ordinary = source(client, representation, False)
                enhanced = source(client, representation, True)
                function_name(enhanced)
                enhanced_shape(enhanced)
                if representation == "source":
                    if kind == "masked":
                        assert ordinary.count("^") >= 3, ordinary
                    elif kind == "frame":
                        assert "|" in ordinary, ordinary
                    elif kind in ("weighted", "bitwise"):
                        assert ordinary.count("&") >= 7, ordinary
                    else:
                        assert ">>" in ordinary, ordinary
                    assert enhanced != ordinary
                # Turning the menu off restores its independent ordinary page.
                assert source(client, representation, False) == ordinary
                assert source(client, representation, True) == enhanced
                outputs["worker-" + representation] = enhanced

        if cli:
            for llvm in (False, True):
                for single in (False, True):
                    name = f"cli-{llvm}-{single}"
                    output = root / (name + ".c")
                    args = [cli, "decompile", "--no-debug", "--symbolic-simplify"]
                    if llvm:
                        args.append("--llvm")
                    if single:
                        args.append("--func=" + hex(ENTRY))
                    subprocess.run(args + ["-o", str(output), str(fixture)],
                                   check=True, capture_output=True, text=True, timeout=90)
                    text = output.read_text(encoding="utf-8")
                    enhanced_shape(text)
                    outputs[name] = text

        if compiler:
            for name, text in outputs.items():
                execute_source(root, name, text, compiler, expected)
            print(f"{kind}: {len(outputs)} source outputs executed at O0/O2; "
                  "1,331 boundary triples and 4,096 random triples per run")
        else:
            print("SKIP generated-C execution: clang is unavailable")
        print("Real worker symbolic menu requests, toggle/cache isolation and CLI synthesis passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("worker")
    parser.add_argument("--cli")
    parser.add_argument("--compiler", default=shutil.which("clang"))
    options = parser.parse_args()
    try:
        for kind, image, expected in (
            ("affine", affine_elf(), "182U*x + 73U*y + 211U*z + 37U"),
            ("quadratic", quadratic_elf(), "11U*x*x + 73U*x*y + 182U*y + 211U*z + 37U"),
            ("masked", masked_elf(), "(37U*((x ^ ~0x39U)&(y ^ 0x6dU))|0x55U)+z"),
            ("frame", frame_elf(), "2U*z-x"),
            ("weighted", weighted_elf(), f"37U*((x^y)&{WEIGHTED_UNION}U)+z"),
            ("bitwise", bitwise_elf(), f"37U*(x^y^{VIEW_KEY}U)+z"),
        ):
            run_fixture(options.worker, options.cli, options.compiler, kind,
                        image, expected)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            sys.stderr.write(error.stdout)
        if error.stderr:
            sys.stderr.write(error.stderr)
        raise
