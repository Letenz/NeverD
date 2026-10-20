#!/usr/bin/env python3
"""CLI definitions reopen in worker listing with identical spans and enum bits."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from data_presentation_test import fixture, listing, DATA
from source_variable_types_test import ok, source
from transport_test import Client


def run(worker, cli):
    with tempfile.TemporaryDirectory(prefix="neverd-data-cli-") as directory:
        root = Path(directory)
        path = root / "data.elf"
        image, entry = fixture()
        path.write_bytes(image)

        def invoke(command, *arguments, success=True, json_output=True):
            result = subprocess.run([cli, command, str(path), *arguments,
                *(["--json"] if json_output else [])], capture_output=True, text=True, timeout=60)
            assert (result.returncode == 0) == success, (arguments, result.stdout, result.stderr)
            return json.loads(result.stdout) if success and json_output else result

        header = root / "types.h"
        header.write_text("struct CLI_ROW { unsigned int A, B, C; }; enum CLI_FLAGS { CLI_READ=1, CLI_WRITE=2 };", encoding="utf-8")
        invoke("types", "--declare", str(header), "--save-declarations")
        rows = invoke("items", "--array", hex(DATA), "--element-size", "12", "--count", "4", "--type", "CLI_ROW")
        row = next(r for r in rows if int(r["addr"],16) == DATA)
        assert row["size"] == 48 and row["count"] == 4 and row["type"] == "CLI_ROW", row
        rows = invoke("items", "--alignment", hex(DATA+67), "--boundary", "16")
        row = next(r for r in rows if r["kind"] == "alignment")
        assert row["size"] == 13 and row["alignment"] == 16, row
        formats = invoke("operands", "--addr", hex(entry), "--operand", "1", "--enum", "CLI_FLAGS", "--flags")
        assert "CLI_FLAGS" in str(formats), formats
        # The raw decode remains machine-readable; the display field and text
        # use the same SDK enum formatter as the worker's listing.
        with Client(worker) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            ok(client, "function_create", {"address": hex(entry)})
            source(client, "low", entry)
            assert "CLI_ROW" in str(listing(client))
            assert "CLI_READ | CLI_WRITE | 0x8" in str(listing(client, entry))

        decoded = invoke("disasm", "--func", hex(entry))
        assert any("CLI_READ | CLI_WRITE | 0x8" in row.get("display_op_str", "") for row in decoded), decoded
        text = invoke("disasm", "--func", hex(entry), json_output=False).stdout
        assert "CLI_READ | CLI_WRITE | 0x8" in text, text
        baseline = invoke("items")
        for arguments in (("--array", hex(DATA), "--count", "0"),
                          ("--array", hex(DATA), "--element-size", "8", "--count", "4", "--type", "CLI_ROW"),
                          ("--alignment", hex(DATA+255), "--boundary", "1024"),
                          ("--alignment", hex(DATA+67), "--boundary", "3"),
                          ("--array", hex(DATA), "--alignment", hex(DATA+67)),
                          ("--count", "4")):
            invoke("items", *arguments, success=False)
            assert invoke("items") == baseline
        invoke("operands", "--addr", hex(entry), "--operand", "1", "--enum", "MISSING", success=False)
        invoke("operands", "--addr", hex(entry), "--operand", "0", "--enum", "CLI_FLAGS", success=False)
        assert invoke("operands") == formats
        # An exact boundary has a full boundary unit, consistently with GUI.
        rows = invoke("items", "--alignment", hex(DATA+96), "--boundary", "16")
        assert any(int(row["addr"],16) == DATA+96 and row["size"] == 16 for row in rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("cli")
    args = parser.parse_args()
    run(args.worker, args.cli)
