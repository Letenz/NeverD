#!/usr/bin/env python3
"""Import names lead to their thunk or slot through the real engine.

Builds a benign glibc program with the host C compiler; it is analyzed, never
executed. Exits 77 (skip) without a compiler that builds one.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from transport_test import Client

SOURCE = '#include <stdio.h>\nint main(void) { puts("neverd"); return 0; }\n'


def run(executable):
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not compiler or not sys.platform.startswith("linux"):
        print("SKIP: needs a host C compiler that builds glibc programs")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-import-names-") as directory:
        source = Path(directory) / "imports.c"
        source.write_text(SOURCE, encoding="ascii")
        binary = Path(directory) / "imports"
        built = subprocess.run([compiler, "-O1", "-fPIE", "-pie", "-fplt", str(source), "-o", str(binary)],
                               capture_output=True, text=True)
        if built.returncode:
            print("SKIP: the host compiler cannot build the program:", built.stderr.strip())
            return 77
        client = Client(executable)
        try:
            opened = client.call("open", {"path": str(binary), "read_only": True})
            assert opened["status"] == "ok", opened
            # __libc_start_main is bound to data (GLOB_DAT): it has only its slot.
            start = client.call("resolve", {"query": "__libc_start_main"})
            assert start["status"] == "ok", start
            assert start["payload"]["import"] is True, start
            assert start["payload"]["function_address"] is None, start
            # puts is called through its PLT entry, which the name leads to.
            puts = client.call("resolve", {"query": "puts"})
            assert puts["status"] == "ok", puts
            assert puts["payload"]["import"] is True, puts
            assert puts["payload"]["function_address"] == puts["payload"]["address"], puts
            assert puts["payload"]["address"] != start["payload"]["address"]
            # The thunk's display name leads there too; main is no import.
            thunk = client.call("resolve", {"query": "_puts"})
            assert thunk["payload"]["address"] == puts["payload"]["address"], thunk
            main = client.call("resolve", {"query": "main"})
            assert main["status"] == "ok" and main["payload"]["import"] is False, main
        finally:
            client.close()
    print("import names: data-bound slot, PLT thunk and its display name resolve; main is no import")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
