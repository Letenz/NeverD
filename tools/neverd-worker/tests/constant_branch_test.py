#!/usr/bin/env python3
"""Recovered branch targets, skipped bytes and editable function instructions."""
import json
from pathlib import Path
import struct
import sys
import tempfile

from transport_test import Client


def ok(client, operation, payload=None):
    result = client.call(operation, payload)
    assert result["status"] == "ok", result
    return result["payload"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-constant-branch-") as directory:
        binary = Path(directory) / "branches.elf"
        base, entry = 0x400000, 0x400078
        code = bytes.fromhex("b87800400083c00bffe0e9c390909090")
        header = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + bytes(9),
                             2, 62, 1, entry, 64, 0, 0, 64, 56, 1, 64, 0, 0)
        segment = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base,
                              120 + len(code), 120 + len(code), 4096)
        binary.write_bytes(header + segment + code)
        sidecar = Path(str(binary) + ".neverd/items.json")
        rows = lambda: json.loads(sidecar.read_text()) if sidecar.exists() else []

        def listing(client, address):
            return ok(client, "listing", {"address": hex(address),
                                          "before": 0, "after": 12})["lines"]

        with Client(executable) as client:
            ok(client, "open", {"path": str(binary)})
            ok(client, "analyze")
            initial = listing(client, entry + 10)
            assert any(row["address"] == hex(entry + 10) and "db" in row["text"]
                       and "0E9h" in row["text"] for row in initial), initial
            assert any(row["address"] == hex(entry + 11) and row["kind"] == "insn"
                       and "ret" in row["text"] for row in initial), initial
            outgoing = ok(client, "xrefs", {"address": hex(entry + 8), "direction": "from"})
            assert [(row["to"], row["kind"]) for row in outgoing["items"]] == [
                (hex(entry + 11), "jump")], outgoing
            incoming = ok(client, "xrefs", {"address": hex(entry + 11), "direction": "to"})
            assert any(row["from"] == hex(entry + 8) for row in incoming["items"]), incoming
            count = ok(client, "functions", {"offset": 0, "limit": 1})["total"]
            # U must work on an automatic instruction without deleting its function.
            changed = ok(client, "item_define", {"address": hex(entry), "action": "undefine"})
            assert changed["saved"] and changed["size"] == 5, changed
            assert rows()[0]["kind"] == "undefined", rows()
            assert not any(row["address"] == hex(entry) and row["kind"] == "insn"
                           for row in listing(client, entry)), listing(client, entry)
            assert ok(client, "functions", {"offset": 0, "limit": 1})["total"] == count
            ok(client, "undo")
            assert rows() == []
            ok(client, "redo")
            saved = rows()
        with Client(executable) as client:
            ok(client, "open", {"path": str(binary)})
            ok(client, "analyze")
            assert rows() == saved
            assert not any(row["address"] == hex(entry) and row["kind"] == "insn"
                           for row in listing(client, entry)), listing(client, entry)
            ok(client, "item_define", {"address": hex(entry), "action": "code"})
            assert rows()[0]["kind"] == "code", rows()
            assert any(row["address"] == hex(entry) and row["kind"] == "insn"
                       for row in listing(client, entry)), listing(client, entry)

        # A rejected lift must leave the native bytes inspectable, with an
        # explicit linear-view warning rather than a fabricated recovered CFG.
        broken = Path(directory) / "incomplete.elf"
        broken.write_bytes(header + segment + bytes.fromhex("b80100000060") + bytes(10))
        with Client(executable) as client:
            ok(client, "open", {"path": str(broken)})
            ok(client, "analyze")
            ok(client, "function_create", {"address": hex(entry)})
            lines = ok(client, "listing", {"address": hex(entry), "before": 0,
                                           "after": 60})["lines"]
            assert any("control-flow recovery failed" in row["text"] for row in lines), lines
            assert any(row["address"] == hex(entry) and row["kind"] == "insn"
                       for row in lines), lines


if __name__ == "__main__":
    run(sys.argv[1])
