#!/usr/bin/env python3
"""Arrays/alignment/enum rows: pagination, real bits, replay and atomic refusal."""
import argparse
from contextlib import ExitStack
from pathlib import Path
import struct
import tempfile
from transport_test import Client
from source_variable_types_test import ok, global_elf, source
from source_synthesis_test import ENTRY, elf

DATA = 0x401000


def fixture():
    image, entry = global_elf()
    image = bytearray(image)
    struct.pack_into("<QQ", image, 152, 256, 256)
    image += bytes(224)
    # mov eax, 0xB; ret. Bytes after it are not an immediate match.
    image[176:182] = bytes.fromhex("b80b000000c3")
    for n in range(64):
        struct.pack_into("<I", image, 4096+4*n, n+1)
    return image, entry


def listing(client, address=DATA, **options):
    return ok(client, "listing", {"address": hex(address), "before": 0, "after": 48, **options})


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-data-presentation-") as directory, ExitStack() as stack:
        path = Path(directory) / "data.elf"
        image, entry = fixture()
        path.write_bytes(image)
        client = stack.enter_context(Client(executable))
        ok(client, "open", {"path": str(path), "analysis": False})
        native = {r: source(client, r, entry)["text"] for r in ("low", "med", "llvm")}
        declarations = "struct QA_ROW { unsigned int A; unsigned int B; unsigned int C; }; enum QA_FLAGS { QA_READ = 1, QA_WRITE = 2, QA_READ_ALIAS = 1 }; enum QA_ALIAS { QA_ELEVEN = 11, QA_ELEVEN_ALIAS = 11 };"
        ok(client, "local_types_declare", {"source": declarations})
        ok(client, "item_define", {"address": hex(DATA), "action": "array", "element_size": 4, "count": 16})
        page = listing(client)
        assert "[0]" in str(page) and "[15]" in str(page), page
        assert "[7]" in str(listing(client, DATA+28)), page
        state = ok(client, "analysis_snapshot")
        for change in ({"count": 0}, {"count": 1048576}, {"element_size": 3}, {"element_size": 8, "type": "QA_ROW"}):
            reply = client.call("item_define", {"address": hex(DATA), "action": "array", "element_size": 4, "count": 16, **change})
            assert reply["status"] == "error", reply
            assert ok(client, "analysis_snapshot") == state
        ok(client, "undo")
        assert "array element" not in str(listing(client))
        ok(client, "redo")
        assert "[15]" in str(listing(client))
        ok(client, "item_define", {"address": hex(DATA), "action": "array", "element_size": 12, "count": 4, "type": "QA_ROW"})
        assert "QA_ROW" in str(listing(client)) and "[3]" in str(listing(client))
        ok(client, "item_define", {"address": hex(DATA+67), "action": "alignment", "alignment": 16})
        assert "align" in str(listing(client, DATA+67))
        state = ok(client, "analysis_snapshot")
        assert client.call("item_define", {"address": hex(DATA+255), "action": "alignment", "alignment": 1024})["status"] == "error"
        assert client.call("item_define", {"address": hex(entry), "action": "array", "element_size": 1, "count": 6})["status"] == "error"
        assert ok(client, "analysis_snapshot") == state
        # Exact decoded immediate, flags and residual unknown bits survive.
        ok(client, "operand_format", {"address": hex(entry), "operand": 1, "action": "enum", "type": "QA_FLAGS", "flags": True})
        shown = str(listing(client, entry))
        assert "QA_READ | QA_WRITE | 0x8" in shown, shown
        ok(client, "operand_format", {"address": hex(entry), "operand": 1, "action": "enum", "type": "QA_ALIAS"})
        assert "QA_ELEVEN" in str(listing(client, entry)) and "QA_ELEVEN_ALIAS" not in str(listing(client, entry))
        ok(client, "undo")
        state = ok(client, "analysis_snapshot")
        for request in ({"address": hex(entry+5)}, {"type": "QA_ROW"}, {"type": "MISSING"}):
            reply = client.call("operand_format", {"address": hex(entry), "operand": 1, "action": "enum", "type": "QA_FLAGS", **request})
            assert reply["status"] == "error", reply
            assert ok(client, "analysis_snapshot") == state
        ok(client, "undo")
        assert "QA_READ |" not in str(listing(client, entry))
        ok(client, "redo")
        # A changed type cannot resize the durable array span or keep a cached
        # enum spelling. Numeric storage remains available until the type returns.
        ok(client, "local_types_forget", {"names": ["QA_ROW", "QA_FLAGS"]})
        assert "stale element type" in str(listing(client))
        assert "QA_READ |" not in str(listing(client, entry))
        ok(client, "undo")
        assert "QA_READ | QA_WRITE | 0x8" in str(listing(client, entry))
        for rep, text in native.items(): assert source(client, rep, entry)["text"] == text
        replica = stack.enter_context(Client(executable))
        ok(replica, "analysis_restore", ok(client, "analysis_snapshot"))
        source(replica, "low", entry)
        assert "QA_ROW" in str(listing(replica))
        replica_page = listing(replica, entry)
        assert "QA_READ | QA_WRITE | 0x8" in str(replica_page), (replica_page, ok(client, "analysis_snapshot"))
        assert replica.call("item_define", {"address": hex(DATA), "action": "undefine"})["status"] == "error"
        replica.close(); client.close()
        client = stack.enter_context(Client(executable))
        ok(client, "open", {"path": str(path), "analysis": False})
        source(client, "low", entry)
        assert "QA_ROW" in str(listing(client))
        assert "QA_READ | QA_WRITE | 0x8" in str(listing(client, entry))
        ok(client, "item_define", {"address": hex(DATA+24), "action": "undefine"})
        assert "array element" not in str(listing(client))
        negative = Path(directory) / "negative.elf"
        image[177:181] = bytes.fromhex("ffffffff")
        negative.write_bytes(image)
        ok(client, "open", {"path": str(negative), "analysis": False})
        source(client, "low", entry)
        ok(client, "local_types_declare", {"source": "enum QA_SIGNED { QA_NEGATIVE = -1 };"})
        ok(client, "operand_format", {"address": hex(entry), "operand": 1, "action": "enum", "type": "QA_SIGNED"})
        assert "QA_NEGATIVE" in str(listing(client, entry))
        # A shift's byte immediate has its own logical width, independently
        # of its 32-bit destination. Folding must retain decoded indices too.
        for name, code, operand, declaration, expected in (
                ("shift", "c1e0ffc3", 1,
                 "enum QA_BYTE : signed char { QA_BYTE_NEGATIVE = -1 };",
                 "QA_BYTE_NEGATIVE"),
                ("imul", "6bc00bc3", 2,
                 "enum QA_IMUL { QA_FACTOR = 11 };", "QA_FACTOR")):
            small = Path(directory) / (name + ".elf")
            small.write_bytes(elf(bytes.fromhex(code)))
            ok(client, "open", {"path": str(small), "analysis": False})
            source(client, "low", ENTRY)
            ok(client, "local_types_declare",
               {"source": declaration, "language": "cpp"})
            enum_type = "QA_BYTE" if name == "shift" else "QA_IMUL"
            ok(client, "operand_format",
               {"address": hex(ENTRY), "operand": operand,
                "action": "enum", "type": enum_type})
            assert expected in str(listing(client, ENTRY))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(); parser.add_argument("worker")
    run(parser.parse_args().worker)
