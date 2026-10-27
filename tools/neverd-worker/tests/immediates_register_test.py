#!/usr/bin/env python3
"""Real SDK/worker/CLI: decoded immediates and original register dataflow."""
from contextlib import ExitStack
import argparse
import json
from pathlib import Path
import subprocess
import struct
import tempfile
from transport_test import Client
from source_synthesis_test import elf, ENTRY
from source_variable_types_test import ok, source


def page(client, pattern, **options):
    for _ in range(10000):
        result = ok(client, "search", {"kind": "immediate", "pattern": pattern, **options})
        if not result["building"]:
            return result
    raise AssertionError("bounded immediate index failed to finish")


def cli(exe, *args):
    process = subprocess.run([exe, *args, "--json"], capture_output=True, text=True, timeout=60)
    assert process.returncode == 0, process.stdout + process.stderr
    return json.loads(process.stdout)


def run(worker, executable=None):
    with tempfile.TemporaryDirectory(prefix="neverd-register-flow-") as directory, ExitStack() as stack:
        root = Path(directory)
        path = root / "flow.elf"
        path.write_bytes(elf(bytes.fromhex("b82a00000089c183c007c3")))
        client = stack.enter_context(Client(worker))
        ok(client, "open", {"path": str(path), "analysis": False})
        before = source(client, "low")["text"]

        def query(pc, name="eax", kind="value"):
            return ok(client, "register_flow", {"address": hex(ENTRY), "instruction": hex(ENTRY+pc), "register": name, "kind": kind})

        assert query(5)["value"] == "0x2A", query(5)
        assert query(7)["value"] == "0x2A", query(7)
        assert query(0)["unknown"]
        definitions = query(7, kind="definitions")
        assert [int(row["address"], 16) for row in definitions["items"]] == [ENTRY], definitions
        uses = query(0, kind="uses")
        assert {int(row["address"], 16) for row in uses["items"]} == {ENTRY+5, ENTRY+7}, uses
        inv = query(5, kind="registers")
        assert {r["name"] for r in inv["registers"]} >= {"eax", "ecx"}, inv
        assert client.call("register_flow", {"address": hex(ENTRY), "instruction": hex(ENTRY+5), "register": "bogus", "kind": "value"})["status"] == "error"
        forward = page(client, "42")
        assert {int(h["address"], 16) for h in forward["items"]} == {ENTRY}, forward
        assert page(client, "42", **{"from": hex(ENTRY)})["items"] == []
        assert int(page(client, "42", backward=True)["items"][0]["address"],16) == ENTRY
        assert source(client, "low")["text"] == before
        if executable:
            result = cli(executable, "disasm", str(path), "--func", hex(ENTRY), "--instruction", hex(ENTRY+5), "--register-flow", "value", "--register", "eax")
            assert result["value"] == "0x2A", result
            result = cli(executable, "search", str(path), "--immediate", "42")
            assert [int(row["addr"],16) for row in result["items"]] == [ENTRY], result
        # ENTER has two immediates: one instruction, one pagination cursor.
        other = root / "immediates.elf"
        other.write_bytes(elf(bytes.fromhex("b8ffffffff c8070007 b807000000 b807000000 c3")))
        ok(client, "open", {"path": str(other), "analysis": False})
        assert int(page(client, "-1")["items"][0]["address"],16) == ENTRY
        assert page(client, "0xffffffffffffffff")["items"] == []
        one = page(client, "7", limit=1)
        assert int(one["items"][0]["address"],16) == ENTRY+5, one
        assert len(one["items"][0]["operands"]) == 2, one
        two = page(client, "7", **{"from": one["next_address"], "limit": 1})
        assert int(two["items"][0]["address"],16) == ENTRY+9, two
        reverse = page(client, "7", backward=True, limit=2)
        assert [int(h["address"],16) for h in reverse["items"]] == [ENTRY+14, ENTRY+9], reverse
        call = root / "call.elf"
        call.write_bytes(elf(bytes.fromhex("b82a000000e80600000089c1c3909090c3")))
        ok(client, "open", {"path": str(call), "analysis": False})
        assert query(10)["unknown"], query(10)
        # BSF(0)'s chosen LowIR value is not an ISA guarantee.
        undefined = root / "undefined.elf"
        undefined.write_bytes(elf(bytes.fromhex("b8000000000fbcc089c1c3")))
        ok(client, "open", {"path": str(undefined), "analysis": False})
        assert query(8)["unknown"], query(8)
        large = root / "large.elf"
        large.write_bytes(elf(bytes.fromhex("90")*10000 + bytes.fromhex("b82a000000c3")))
        ok(client, "open", {"path": str(large), "analysis": False})
        assert page(client, "42", backward=True, **{"from": "0x1"})["items"] == []
        ok(client, "function_create", {"address": hex(ENTRY)})
        partial = ok(client, "search", {"kind": "immediate", "pattern": "42", "backward": True})
        assert partial["building"]
        ok(client, "rename", {"address": hex(ENTRY), "name": "renamed_entry"})
        progress = ok(client, "search", {"kind": "immediate", "pattern": "42", "backward": True})
        assert progress["decoded_bytes"] > partial["decoded_bytes"], (partial, progress)
        result = page(client, "42", backward=True)
        assert int(result["items"][0]["address"],16) == ENTRY+10000, result
        # Decode/search and reaching definitions share the architecture owner,
        # including when the frontend host is Windows x64.
        a64 = root / "aarch64.elf"
        image = bytearray(elf(bytes.fromhex("40058052e103002ac0035fd6")))
        struct.pack_into("<H", image, 18, 183)
        a64.write_bytes(image)
        ok(client, "open", {"path": str(a64), "analysis": False})
        assert int(page(client, "42")["items"][0]["address"],16) == ENTRY
        result = query(4, "w0", "definitions")
        assert [int(site["address"],16) for site in result["items"]] == [ENTRY], result
        # A search may finish before navigation discovers an entry beyond an
        # undecodable gap. The next search must use that new boundary too.
        discovered = root / "discovered.elf"
        discovered.write_bytes(elf(
            bytes.fromhex("e80b000000c3") + b"\x06" * 10 +
            bytes.fromhex("b82a000000c3")))
        ok(client, "open", {"path": str(discovered), "analysis": False})
        assert page(client, "42")["items"] == []
        source(client, "low", ENTRY + 16)
        resumed = page(client, "42")
        assert [int(h["address"], 16) for h in resumed["items"]] == [ENTRY + 16], resumed
        assert int(page(client, "42", backward=True)["items"][0]["address"], 16) == ENTRY + 16
        # Decimal input keeps its meaning with leading zeros, including 8/9;
        # explicit hexadecimal keeps its existing logical-width matching.
        decimal = root / "decimal.elf"
        decimal.write_bytes(elf(bytes.fromhex("b80a000000b908000000baf6ffffffc3")))
        ok(client, "open", {"path": str(decimal), "analysis": False})
        assert int(page(client, "010")["items"][0]["address"], 16) == ENTRY
        assert int(page(client, "008")["items"][0]["address"], 16) == ENTRY + 5
        assert int(page(client, "0x0a")["items"][0]["address"], 16) == ENTRY
        assert int(page(client, "-010")["items"][0]["address"], 16) == ENTRY + 10
        if executable:
            result = cli(executable, "search", str(decimal), "--immediate", "010")
            assert [int(row["addr"], 16) for row in result["items"]] == [ENTRY], result
        wide = root / "wide-immediates.elf"
        wide.write_bytes(elf(bytes.fromhex(
            "48b80000000000000080 48b9ffffffffffffffff c3")))
        ok(client, "open", {"path": str(wide), "analysis": False})
        for pattern in ("-9223372036854775808", "9223372036854775808",
                        "-0x8000000000000000", "0X8000000000000000"):
            result = page(client, pattern)
            assert [int(h["address"], 16) for h in result["items"]] == [ENTRY], result
        for pattern in ("18446744073709551615", "-1", "0xffffffffffffffff"):
            result = page(client, pattern)
            assert [int(h["address"], 16) for h in result["items"]] == [ENTRY + 10], result
        for pattern in ("18446744073709551616", "-9223372036854775809",
                        "0x", "0b10", "10oops"):
            reply = client.call("search", {"kind": "immediate", "pattern": pattern})
            assert reply["status"] == "error", (pattern, reply)
        from data_presentation_test import fixture as data_fixture
        data = root / "data-only.elf"
        image, entry = data_fixture()
        data.write_bytes(image)
        ok(client, "open", {"path": str(data), "analysis": False})
        assert page(client, "42", **{"from": hex(entry-1)})["items"] == []
        print("decoded immediates, register flow, undefined/call boundaries and CLI parity passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--cli")
    args = parser.parse_args()
    run(args.worker, args.cli)
