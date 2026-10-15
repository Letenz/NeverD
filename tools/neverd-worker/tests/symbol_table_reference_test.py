#!/usr/bin/env python3
"""An image's static symbol table, found before it opens, and both answers."""
from contextlib import ExitStack
from pathlib import Path
import shutil
import struct
import sys
import tempfile

from transport_test import Client


FIXTURES = Path(__file__).resolve().parents[3] / "unittests/safety/fixtures/binaries"
# ELF64: e_shoff at 0x28, e_shentsize and e_shnum at 0x3A; sh_type at 4.
SHT_PROGBITS, SHT_SYMTAB = 1, 2


def ok(client, operation, payload=None):
    result = client.call(operation, payload or {})
    assert result["status"] == "ok", result
    return result["payload"]


def function_names(client):
    return {item["name"] for item in ok(client, "functions", {"offset": 0, "limit": 500})["items"]}


def names(client):
    """Every function, data and string label, as the Names window lists them."""
    return {(item["name"], item["kind"]) for item in ok(client, "names", {"offset": 0, "limit": 500})["items"]}


def without_symbol_table(source, target):
    """A copy of an ELF64 image whose .symtab no loader reads: its section
    becomes plain bytes, as if strip had taken the table out."""
    data = bytearray(source.read_bytes())
    section_table, = struct.unpack_from("<Q", data, 0x28)
    entry_size, count = struct.unpack_from("<HH", data, 0x3A)
    patched = 0
    for index in range(count):
        at = section_table + index * entry_size + 4
        if struct.unpack_from("<I", data, at)[0] == SHT_SYMTAB:
            struct.pack_into("<I", data, at, SHT_PROGBITS)
            patched += 1
    assert patched == 1, patched
    target.write_bytes(bytes(data))


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-symbol-table-") as directory, ExitStack() as clients:
        root = Path(directory)
        binary = root / "input"
        shutil.copyfile(FIXTURES / "safety_cases_elf_x64", binary)
        stripped = root / "stripped"
        without_symbol_table(binary, stripped)

        owner = clients.enter_context(Client(executable))
        assert "symbol_table_reference" in owner.hello["capabilities"]
        table = ok(owner, "symbol_table_reference", {"path": str(binary)})["symbols"]
        assert table["table"] == ".symtab" and table["count"] > 0 and table["names"], table
        # A PE without a COFF symbol table, the copy without one, and no file.
        for other in (FIXTURES / "safety_cases_pe_x64.exe", stripped):
            assert ok(owner, "symbol_table_reference", {"path": str(other)})["symbols"] is None, other
        missing = owner.call("symbol_table_reference", {"path": str(root / "missing")})
        assert missing["status"] != "ok", missing
        # An answer other than none is refused rather than guessed at.
        refused = owner.call("open", {"path": str(binary), "analysis": False, "symbols": {"mode": "verified"}})
        assert refused["status"] != "ok", refused

        # Yes: the table's names load.
        reader = clients.enter_context(Client(executable))
        opened = ok(reader, "open", {"path": str(binary), "read_only": True, "analysis": False})
        assert opened["symbols"] is None, opened["symbols"]
        assert set(table["names"]) <= function_names(reader), table["names"]
        named = names(reader)

        # No: the image reads exactly as the copy without the table does, which
        # lacks names the table held.
        copy = clients.enter_context(Client(executable))
        ok(copy, "open", {"path": str(stripped), "read_only": True, "analysis": False})
        unnamed = names(copy)
        opened = ok(owner, "open", {"path": str(binary), "analysis": False, "symbols": {"mode": "none"}})
        assert opened["symbols"] == {"mode": "none"}, opened["symbols"]
        declined = names(owner)
        assert declined == unnamed, sorted(declined ^ unnamed)
        assert declined < named, sorted(named - declined)

        # Its read-only replica keeps the answer.
        snapshot = ok(owner, "analysis_snapshot")
        assert snapshot["symbols"] == {"mode": "none"}, snapshot
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", snapshot)
        assert ok(replica, "metadata")["symbols"] == {"mode": "none"}
        assert names(replica) == declined


if __name__ == "__main__":
    run(sys.argv[1])
