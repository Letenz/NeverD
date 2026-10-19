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


def affine_elf():
    # System V x64: 182*x + 73*y + 211*z + 37
    #                + (x >> 4) - ((x >> 2) >> 2), all in 32-bit registers.
    # Keep the original shifts as distinct instructions, independent of a
    # fixture compiler's optimization and instruction-selection policies.
    code = bytes.fromhex(
        "69c7b6000000 6bce49 01c8 69cad3000000 01c8 83c025 "
        "89f9 c1e904 89fa c1ea02 c1ea02 29d1 01c8 c3")
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
        uint32_t want = 182U*x + 73U*y + 211U*z + 37U;
        if ((uint32_t)neverd_affine_subject(x, y, z) != want)
          return 1;
      }
  uint32_t state = 0x12345678U;
  for (unsigned i = 0; i < 4096; ++i) {
    uint32_t x = (state = 1664525U*state + 1013904223U);
    uint32_t y = (state = 1664525U*state + 1013904223U);
    uint32_t z = (state = 1664525U*state + 1013904223U);
    if ((uint32_t)neverd_affine_subject(x, y, z) != 182U*x + 73U*y + 211U*z + 37U)
      return 2;
  }
  return 0;
}
"""


def execute_source(root, name, text, compiler):
    path = root / (name + ".c")
    alias = f"\n#define neverd_affine_subject {function_name(text)}\n"
    path.write_text("#include <stdint.h>\n" + text + alias + HARNESS, encoding="utf-8")
    for level in ("-O0", "-O2"):
        binary = root / (name + level + ".exe")
        subprocess.run([compiler, "-std=c11", level, "-Werror=return-type",
                        "-Werror=implicit-function-declaration", str(path),
                        "-o", str(binary)], check=True, capture_output=True,
                       text=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)


def run(worker, cli, compiler):
    with tempfile.TemporaryDirectory(prefix="neverd-source-synthesis-") as directory:
        root = Path(directory)
        fixture = root / "affine.elf"
        fixture.write_bytes(affine_elf())
        outputs = {}
        with Client(worker) as client:
            ok(client, "open", {"path": str(fixture), "read_only": True})
            for representation in ("source", "llvmc"):
                ordinary = source(client, representation, False)
                enhanced = source(client, representation, True)
                function_name(enhanced)
                assert ">>" not in enhanced, enhanced
                if representation == "source":
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
                    assert ">>" not in text, text
                    outputs[name] = text

        if compiler:
            for name, text in outputs.items():
                execute_source(root, name, text, compiler)
            print(f"{len(outputs)} source outputs executed at O0/O2; "
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
        run(options.worker, options.cli, options.compiler)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            sys.stderr.write(error.stdout)
        if error.stderr:
            sys.stderr.write(error.stderr)
        raise
