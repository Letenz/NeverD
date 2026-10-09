#!/usr/bin/env python3
"""Edits that change names read them again without a full listing build.

A rename, a data item, an undo and a comment let the listing read names and
items again over its last string scan, or not rebuild at all.  After each
edit, every page must match what a full rebuild (reload) shows.

Reads a binary file first -- one segment, no symbols, a function only its
call names and a string only its reference does -- then builds a benign glibc
program with the host C compiler and strips it, as distributions ship
programs.  Both are analyzed, never executed.  Exits 77 (skip) after the
binary file without a compiler and strip that build the program.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from transport_test import Client

SOURCE = r"""#include <stdio.h>
#include <stdlib.h>

static const char *pname;
static int calls;

static void report(const char *what) {
  ++calls;
  fprintf(stderr, "%s: %s (%d)\n", pname, what, calls);
}

int main(int argc, char **argv) {
  pname = argv[0];
  if (argc > 2)
    report("too many arguments");
  puts(argc > 1 ? argv[1] : "no argument given");
  return calls;
}
"""


# x86-64 code a binary file holds at BINARY_BASE: start calls add_seven at
# +0x10 and points at the text at +0x18.
BINARY_BASE = 0x400000
BINARY_CODE = bytes.fromhex(
    "e80b000000"  # call add_seven
    "488d3d0c000000"  # lea rdi, [rip + text]
    "c3909090"  # ret, padding
    "8d4707c3"  # add_seven: lea eax, [rdi + 7]; ret
    "00000000") + b"binary file text\0"


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def snapshot(client, anchors):
    """Every page an edit can change: listing pages and the tables."""
    pages = {}
    for anchor in anchors:
        pages[anchor] = ok(client, "listing",
                           {"address": anchor, "before": 40, "after": 120})["lines"]
    for table in ("functions", "names", "strings"):
        pages[table] = ok(client, table, {"offset": 0, "limit": 512})["items"]
    return pages


def same_as_full_rebuild(client, anchors, step):
    light = snapshot(client, anchors)
    ok(client, "reload")
    full = snapshot(client, anchors)
    for key in light:
        if light[key] != full[key]:
            for index, (seen, rebuilt) in enumerate(zip(light[key], full[key])):
                if seen != rebuilt:
                    raise AssertionError(f"{step}: {key} differs at {index}: {seen} != {rebuilt}")
            raise AssertionError(f"{step}: {key} has {len(light[key])} rows, not {len(full[key])}")


def binary_file(executable, directory):
    blob = Path(directory) / "blob.bin"
    blob.write_bytes(BINARY_CODE)
    start, callee, text = (hex(BINARY_BASE + offset) for offset in (0, 0x10, 0x18))
    client = Client(executable)
    try:
        ok(client, "open", {"path": str(blob), "loader": "binary", "processor": "x86_64",
                            "base": hex(BINARY_BASE)})
        ok(client, "xrefs", {"address": text})
        anchors = [start, callee, text]
        same_as_full_rebuild(client, anchors, "binary file open")
        ok(client, "rename", {"address": callee, "name": "add_seven"})
        same_as_full_rebuild(client, anchors, "binary file function rename")
        ok(client, "rename", {"address": text, "name": "label_text"})
        same_as_full_rebuild(client, anchors, "binary file string rename")
        ok(client, "item_define", {"address": text, "action": "data", "size": 4})
        same_as_full_rebuild(client, anchors, "binary file string made data")
        ok(client, "undo")
        same_as_full_rebuild(client, anchors, "binary file undo")
    finally:
        client.close()


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-names-rebuild-") as directory:
        binary_file(executable, directory)
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    strip = shutil.which("strip")
    if not compiler or not strip or not sys.platform.startswith("linux"):
        print("SKIP: needs a host C compiler and strip that build glibc programs")
        return 77
    with tempfile.TemporaryDirectory(prefix="neverd-names-rebuild-") as directory:
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
            ok(client, "open", {"path": str(binary)})
            main = ok(client, "resolve", {"query": "main"})["address"]
            # The reference index names data by its accesses.
            ok(client, "xrefs", {"address": main})
            strings = ok(client, "strings", {"offset": 0, "limit": 512})["items"]
            text = next(row for row in strings if row["text"] == "no argument given")
            # The program's own data the code accesses, which only its
            # automatic name names: beside the copy of stderr in .bss.
            stderr = client.call("resolve", {"query": "stderr"})
            if stderr["status"] != "ok":
                print("SKIP: the program reaches stderr through the GOT, not a copy in .bss")
                return 77
            bss = ok(client, "listing", {"address": stderr["payload"]["address"], "before": 0,
                                          "after": 40})["lines"]
            data = next(line["address"] for line in bss
                        if line["text"].startswith(("qword_", "dword_")))
            anchors = [main, text["address"], data]
            same_as_full_rebuild(client, anchors, "open")
            ok(client, "rename", {"address": main, "name": "report_main"})
            same_as_full_rebuild(client, anchors, "function rename")
            ok(client, "rename", {"address": data, "name": "program_state"})
            same_as_full_rebuild(client, anchors, "data rename")
            ok(client, "rename", {"address": text["address"], "name": "usage_text"})
            same_as_full_rebuild(client, anchors, "string rename")
            ok(client, "item_define", {"address": text["address"], "action": "data", "size": 4})
            same_as_full_rebuild(client, anchors, "string made data")
            ok(client, "undo")
            same_as_full_rebuild(client, anchors, "undo")
            ok(client, "annotation_set", {"address": main, "text": "entry logic"})
            ok(client, "save")
            same_as_full_rebuild(client, anchors, "comment")
        finally:
            client.close()
    print("names rebuild: renames, a data item, undo and a comment show as a full rebuild does,"
          " in a binary file and a program")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
