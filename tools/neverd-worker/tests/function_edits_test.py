#!/usr/bin/env python3
"""Function edits through worker IPC and the real engine: create, delete,
history, undo/redo and their sidecar.  Exits 77 (skip) with an engine that
keeps no function edits."""
from pathlib import Path
import struct
import sys
import tempfile
import time
from transport_test import Client

BASE = 0x400000


def two_routine_elf():
    """An x86-64 executable whose entry routine is followed by a second one
    that nothing reaches: `mov eax, 7; ret` and `mov eax, 9; ret`."""
    code = bytes.fromhex("b807000000c3" "b809000000c3")
    entry = BASE + 120
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1,
                         entry, 64, 0, 0, 64, 56, 1, 64, 0, 0)
    segment = struct.pack("<IIQQQQQQ", 1, 5, 0, BASE, BASE,
                          120 + len(code), 120 + len(code), 4096)
    return header + segment + code, entry


def addresses(client):
    page = client.call("functions", {"limit": 512})
    assert page["status"] == "ok", page
    return {int(item["address"], 16) for item in page["payload"]["items"]}


def wait_listed(client, address):
    """Function discovery runs while the worker is idle, as in the GUI."""
    deadline = time.monotonic() + 20
    while address not in addresses(client):
        assert time.monotonic() < deadline, "the entry function was never listed"
        time.sleep(0.3)


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-function-edits-") as directory:
        data, entry = two_routine_elf()
        second = entry + 6
        binary = Path(directory) / "edits.elf"
        binary.write_bytes(data)
        client = Client(executable)
        try:
            hello = client.hello
            if "function_create" not in hello["capabilities"]:
                print("SKIP: this engine keeps no function edits")
                return 77
            opened = client.call("open", {"path": str(binary)})
            assert opened["status"] == "ok", opened
            wait_listed(client, entry)
            assert second not in addresses(client)

            # Code nothing reaches becomes a function.
            created = client.call("function_create", {"address": hex(second)})
            assert created["status"] == "ok", created
            assert second in addresses(client)
            code = client.call("decompile", {"address": hex(second), "representation": "c", "limit": 64})
            assert code["status"] == "ok" and "9" in code["payload"]["text"], code
            again = client.call("function_create", {"address": hex(second)})
            assert again["status"] == "error", again

            # The edit is history: undone and redone like a rename.
            history = client.call("history")["payload"]
            assert history["items"][-1]["kind"] == "function", history
            assert client.call("undo")["status"] == "ok"
            assert second not in addresses(client)
            assert client.call("redo")["status"] == "ok"
            assert second in addresses(client)

            # The image's own function can go too.
            deleted = client.call("function_delete", {"address": hex(entry)})
            assert deleted["status"] == "ok", deleted
            assert entry not in addresses(client)
        finally:
            client.close()
        assert (Path(directory) / "edits.elf.neverd-functions.json").exists()

        # A later session reads the edits back.
        client = Client(executable)
        try:
            assert client.call("open", {"path": str(binary)})["status"] == "ok"
            time.sleep(1)
            listed = addresses(client)
            assert second in listed and entry not in listed, listed
        finally:
            client.close()
    print("function edits: create, history, undo/redo, delete and sidecar round trip passed")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1]))
