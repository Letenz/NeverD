#!/usr/bin/env python3
"""The program's main: a stripped program the host compiler builds names no
function, and its start-up code hands __libc_start_main its main.  The engine
names that function main and declares it as the C runtime calls it, so its C
reads argc and argv where the code reads them."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from transport_test import Client
from source_variable_types_test import ok

PROGRAM = r"""
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  printf("%s\n", argv[1]);
  return atoi(argv[argc - 1]);
}
"""


def run(executable, compiler):
    strip = shutil.which("strip")
    if not strip or not sys.platform.startswith("linux"):
        print("SKIP: needs strip and a host that builds glibc programs")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-program-main-") as directory:
        root = Path(directory)
        (root / "program.c").write_text(PROGRAM)
        built = subprocess.run([compiler, "-O1", "program.c", "-o", "program"],
                               cwd=root, capture_output=True, text=True, timeout=120)
        if built.returncode or subprocess.run([strip, str(root / "program")]).returncode:
            print("SKIP: the host compiler builds no stripped program:\n" + built.stderr)
            return 77
        with Client(executable) as client:
            ok(client, "open", {"path": str(root / "program"), "read_only": True})
            main = ok(client, "resolve", {"query": "main"})
            assert main["function_address"] == main["address"], main
            # HighC and LLVM-C declare it alike.
            for representation in ("c", "llvmc"):
                text = ok(client, "decompile", {"address": main["address"],
                                                "representation": representation,
                                                "limit": 2048})["text"]
                definition = next((line for line in text.splitlines()
                                   if line.startswith("int32_t main(")), None)
                assert definition is not None, (representation, text)
                assert definition == ("int32_t main(int32_t argc, char** argv, "
                                      "char** envp) {"), (representation, definition)
                assert "argv" in text.split(definition, 1)[1], (representation, text)
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--cc", required=True)
    arguments = parser.parse_args()
    sys.exit(run(arguments.worker, arguments.cc))
