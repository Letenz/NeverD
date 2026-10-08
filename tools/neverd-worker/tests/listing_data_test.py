#!/usr/bin/env python3
"""Unwritten data is laid out by its symbols and by the code's accesses.

Builds a benign glibc program with the host C compiler and strips it, as
distributions ship programs; it is analyzed, never executed. Exits 77 (skip)
without a compiler and strip that build one.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from transport_test import Client

# As xxd reports an error: the program's name is static data no symbol names
# once the program is stripped.
SOURCE = r"""#include <stdio.h>
#include <stdlib.h>

static const char *pname;

static void error_exit(int ret, const char *msg) {
  fprintf(stderr, "%s: %s\n", pname, msg);
  exit(ret);
}

int main(int argc, char **argv) {
  pname = argv[0];
  if (argc > 2)
    error_exit(2, argv[2]);
  puts(argv[argc - 1]);
  return 0;
}
"""

DATA_LINE = re.compile(r"^(\w+)\s+(db|dw|dd|dq)\s+(.*?)\s*(;.*)?$")


def data_lines(client, address, after):
    lines = client.call("listing", {"address": address, "before": 0, "after": after})
    assert lines["status"] == "ok", lines
    return [line["text"] for line in lines["payload"]["lines"]]


def run(executable):
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    strip = shutil.which("strip")
    if not compiler or not strip or not sys.platform.startswith("linux"):
        print("SKIP: needs a host C compiler and strip that build glibc programs")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-listing-data-") as directory:
        source = Path(directory) / "report.c"
        source.write_text(SOURCE, encoding="ascii")
        binary = Path(directory) / "report"
        built = subprocess.run([compiler, "-O1", "-fPIE", "-pie", str(source), "-o", str(binary)],
                               capture_output=True, text=True)
        if built.returncode or subprocess.run([strip, str(binary)]).returncode:
            print("SKIP: the host compiler cannot build the program:", built.stderr.strip())
            return 77
        client = Client(executable)
        try:
            opened = client.call("open", {"path": str(binary), "read_only": True})
            assert opened["status"] == "ok", opened
            stderr = client.call("resolve", {"query": "stderr"})
            if stderr["status"] != "ok":
                print("SKIP: the program reaches stderr through the GOT, not a copy in .bss")
                return 77
            address = stderr["payload"]["address"]
            # The reference index names data by its accesses.
            assert client.call("xrefs", {"address": address})["status"] == "ok"
            lines = data_lines(client, address, 40)
            items = [match.groups()[:3] for match in map(DATA_LINE.match, lines) if match]
            # The copied FILE pointer is as large as its symbol says, not a
            # run to the next name or the end of .bss.
            assert ("stderr", "dq", "?") in items, lines
            assert not any(name == "stderr" and "dup" in value for name, _, value in items), lines
            # The program's name has its own item, named by the width of
            # the code's accesses.
            named = [name for name, directive, value in items
                     if name.startswith("qword_") and directive == "dq" and value == "?"]
            assert named, lines
            # Its operands take the same name.
            main = client.call("resolve", {"query": "main"})
            assert main["status"] == "ok", main
            code = data_lines(client, main["payload"]["address"], 60)
            assert any(name in text for name in named for text in code), (named, code)
        finally:
            client.close()
    print("listing data: stderr is one qword; the program's name is qword data its operands name")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
