#!/usr/bin/env python3
"""Shared array/alignment/enum admission, CLI persistence and GUI rendering."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile

from data_presentation_test import DATA, fixture, listing
from source_variable_types_test import ok, source
from transport_test import Client


def check_sdk(engine, binary):
    # Exercise the public JSON boundary, including types CLI string options
    # cannot express. A rejected replacement must retain the existing array.
    dll_directory = (os.add_dll_directory(str(Path(engine).parent))
                     if os.name == "nt" else None)
    try:
        lib = ctypes.CDLL(engine)
        ptr, text = ctypes.c_void_p, ctypes.c_char_p
        signatures = {
            "neverd_session_create": (ptr, []),
            "neverd_session_destroy": (None, [ptr]),
            "neverd_session_load": (ctypes.c_int, [ptr, text]),
            "neverd_item_set": (ctypes.c_int, [ptr, ctypes.c_uint64, text]),
            "neverd_items_json": (ptr, [ptr]),
            "neverd_free_string": (None, [ptr]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(lib, name)
            function.restype, function.argtypes = result, arguments
        session = lib.neverd_session_create()
        assert session
        try:
            assert lib.neverd_session_load(session, str(binary).encode()) == 1

            def items():
                value = lib.neverd_items_json(session)
                assert value
                try:
                    return json.loads(ctypes.string_at(value))
                finally:
                    lib.neverd_free_string(value)

            valid = {"kind": "array", "element_size": 4, "count": 16}
            assert lib.neverd_item_set(
                session, DATA, json.dumps(valid).encode()) == 0
            before = items()
            for extra in ({"type": 42}, {"type": None}, {"size": "64"},
                          {"size": None}, {"size": True}):
                invalid = {**valid, **extra}
                assert lib.neverd_item_set(
                    session, DATA, json.dumps(invalid).encode()) != 0, invalid
                assert items() == before
        finally:
            lib.neverd_session_destroy(session)
    finally:
        if dll_directory:
            dll_directory.close()


def run(worker, cli, engine):
    with tempfile.TemporaryDirectory(prefix="neverd-data-cli-") as directory:
        root = Path(directory)
        binary, gui_binary = root / "cli.elf", root / "gui.elf"
        image, entry = fixture()
        binary.write_bytes(image)
        gui_binary.write_bytes(image)
        check_sdk(engine, binary)

        def invoke(command, *arguments, path=binary, success=True):
            result = subprocess.run(
                [cli, command, str(path), *arguments], capture_output=True,
                text=True, timeout=60)
            assert (result.returncode == 0) == success, (
                command, arguments, result.returncode, result.stdout, result.stderr)
            return result.stdout

        declarations = (
            "struct QA_ROW { unsigned int A; unsigned int B; unsigned int C; };"
            "enum QA_FLAGS { QA_READ = 1, QA_WRITE = 2, QA_READ_ALIAS = 1 };"
            "enum QA_ALIAS { QA_ELEVEN = 11, QA_ELEVEN_ALIAS = 11 };")
        header = root / "types.h"
        header.write_text(declarations, encoding="utf-8")
        cli_project = Path(str(binary) + ".neverd")
        # Existing one-shot declarations remain unsaved unless requested.
        invoke("types", "--declare", str(header), "--json")
        assert not (cli_project / "types.json").exists()
        invoke("types", "--declare", str(header), "--save", "--json")
        invoke("items", "--array", hex(DATA), "--size", "4", "--count", "16")
        invoke("items", "--array", hex(DATA + 64), "--size", "12",
               "--count", "4", "--type", "QA_ROW")
        invoke("items", "--alignment", hex(DATA + 117), "--boundary", "16")
        invoke("items", "--alignment", hex(DATA + 128), "--boundary", "16")
        invoke("operands", "--addr", hex(entry), "--operand", "1",
               "--enum", "QA_FLAGS", "--flags")
        rows = json.loads(invoke("items", "--json"))
        assert [r["size"] for r in rows] == [64, 48, 11, 16], rows
        assert "16 elements x 4 bytes" in invoke("items")
        assert "boundary 16" in invoke("items")
        assert "QA_FLAGS, flags" in invoke("operands")
        saved = {name: (cli_project / name).read_bytes()
                 for name in ("items.json", "operands.json", "types.json")}

        # The GUI's worker reads the CLI edits and writes byte-identical rows
        # for the same operations on another copy of the input.
        with Client(worker) as client:
            ok(client, "open", {"path": str(binary), "analysis": False})
            native = {r: source(client, r, entry)["text"]
                      for r in ("low", "med", "llvm")}
            assert "[15]" in str(listing(client))
            assert "QA_ROW" in str(listing(client, DATA + 64))
            assert "align" in str(listing(client, DATA + 117))
            assert "QA_READ | QA_WRITE | 0x8" in str(listing(client, entry))
            for command, arguments in (
                    ("items", ["--array", hex(DATA), "--size", "4", "--count", "8"]),
                    ("items", ["--alignment", hex(DATA + 117), "--boundary", "32"]),
                    ("operands", ["--addr", hex(entry), "--operand", "1",
                                  "--enum", "QA_ALIAS"]),
                    ("types", ["--declare", str(header), "--save"])):
                invoke(command, *arguments, success=False)
                for name, expected in saved.items():
                    assert (cli_project / name).read_bytes() == expected
            assert json.loads(invoke("items", "--json")) == rows
            ok(client, "open", {"path": str(gui_binary), "analysis": False})
            source(client, "low", entry)
            ok(client, "local_types_declare", {"source": declarations})
            for address, size, count, name in (
                    (DATA, 4, 16, ""), (DATA + 64, 12, 4, "QA_ROW")):
                ok(client, "item_define", {
                    "address": hex(address), "action": "array",
                    "element_size": size, "count": count, "type": name})
            for address in (DATA + 117, DATA + 128):
                ok(client, "item_define", {
                    "address": hex(address), "action": "alignment", "alignment": 16})
            ok(client, "operand_format", {
                "address": hex(entry), "operand": 1, "action": "enum",
                "type": "QA_FLAGS", "flags": True})
            for representation, expected in native.items():
                assert source(client, representation, entry)["text"] == expected
        gui_project = Path(str(gui_binary) + ".neverd")
        for name, expected in saved.items():
            assert (gui_project / name).read_bytes() == expected, name

        # Invalid options and unsupported storage must not save partial edits.
        failures = [
            ("types", ["--save"]),
            ("items", ["--array", hex(DATA)]),
            ("items", ["--array", hex(DATA), "--count", "0"]),
            ("items", ["--array", hex(DATA), "--count", "16", "--size", "3"]),
            ("items", ["--array", hex(DATA), "--count", "16", "--size", "8",
                       "--type", "QA_ROW"]),
            ("items", ["--array", hex(entry), "--count", "6"]),
            ("items", ["--array", hex(entry + 1), "--count", "1"]),
            ("items", ["--alignment", hex(entry + 1), "--boundary", "2"]),
            ("items", ["--array", hex(DATA + 250), "--count", "32"]),
            ("items", ["--array", hex(DATA), "--count", "16", "--alignment", hex(DATA)]),
            ("items", ["--alignment", hex(DATA), "--boundary", "3"]),
            ("items", ["--alignment", hex(DATA), "--size", "4"]),
            ("items", ["--count", "4"]),
            ("items", ["--type", "QA_ROW"]),
            ("items", ["--boundary", "16"]),
            ("items", ["--array="]),
            ("operands", ["--enum", "QA_FLAGS"]),
            ("operands", ["--flags"]),
            ("operands", ["--addr", hex(entry), "--operand", "0", "--enum", "QA_FLAGS"]),
            ("operands", ["--addr", hex(entry + 5), "--operand", "1", "--enum", "QA_FLAGS"]),
            ("operands", ["--addr", hex(entry), "--operand", "1", "--enum", "QA_ROW"]),
            ("operands", ["--addr", hex(entry), "--operand", "1", "--enum", "MISSING"]),
        ]
        for conflict in ("--clear", "--struct=QA_ROW", "--negate", "--invert",
                         "--delta=0", "--base=hex"):
            failures.append(("operands", [
                "--addr", hex(entry), "--operand", "1", "--enum", "QA_FLAGS", conflict]))
        broken = root / "broken.h"
        broken.write_text("struct {", encoding="utf-8")
        failures.append(("types", [
            "--declare", str(header), "--declare", str(broken), "--save"]))
        for command, arguments in failures:
            invoke(command, *arguments, success=False)
            for name, expected in saved.items():
                assert (cli_project / name).read_bytes() == expected, (arguments, name)
        assert binary.read_bytes() == image

        invoke("operands", "--addr", hex(entry), "--operand", "1", "--enum", "QA_ALIAS")
        with Client(worker) as client:
            ok(client, "open", {"path": str(binary), "analysis": False})
            source(client, "low", entry)
            shown = str(listing(client, entry))
            assert "QA_ELEVEN" in shown and "QA_ELEVEN_ALIAS" not in shown, shown
        invoke("operands", "--addr", hex(entry), "--operand", "1", "--clear")
        assert json.loads(invoke("operands", "--json")) == []
        invoke("items", "--clear", hex(DATA))
        assert len(json.loads(invoke("items", "--json"))) == 3


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("cli")
    parser.add_argument("--engine", required=True)
    args = parser.parse_args()
    run(args.worker, args.cli, args.engine)
