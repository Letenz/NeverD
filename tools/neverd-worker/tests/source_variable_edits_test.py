#!/usr/bin/env python3
"""Proven source associations through the real worker and durable journal."""
import argparse
import json
import struct
import subprocess
import tempfile
from pathlib import Path
from transport_test import Client
from source_synthesis_test import elf, ENTRY
from source_variable_types_test import ok, source

def fixture():
    code = bytearray(bytes.fromhex(
        "4883ec28 48897c2418 e800000000 488b7c2418 4883c701 "
        "e800000000 4889442418 e800000000 488b442418 4883c428 c3"))
    for index in (9, 23, 33):
        code[index + 1:index + 5] = struct.pack("<i", 0x500000 - ENTRY - index - 5)
    return elf(code)

def edit(client, view, action, **arguments):
    return ok(client, "code_edit", {"address": hex(ENTRY), "representation": "c",
        "kind": "variables", "action": action, "snapshot": view["variable_snapshot"], **arguments})

def run(executable, compiler=None):
    with tempfile.TemporaryDirectory(prefix="neverd-variable-associations-") as directory:
        path = Path(directory) / "lifetimes.elf"
        path.write_bytes(fixture())
        with Client(executable) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            before = source(client)
            native = {rep: source(client, rep)["text"] for rep in ("low", "med", "llvm")}
            edit(client, before, "split", variable_id="stack:-16")
            after = source(client)
            result = after["variable_edit_results"][0]
            assert result["applied"] and len(result["outputs"]) == 2, after
            for identity in result["outputs"]:
                variable = next(v for v in after["source_variables"] if v["id"] == identity)
                assert variable["name"] in after["text"], after
                assert any(v.get("variable_id") == identity for v in after["code_names"]), after
            assert before["variable_snapshot"] != after["variable_snapshot"]
            # A real complete copy connects the first split slot to arg0.
            target = next(v for v in after["source_variables"] if v["name"] == "arg0")
            # Partitions are graph identities, not lexical lifetime numbers.
            attempts = [client.call("code_edit", {"address": hex(ENTRY), "representation": "c",
                "kind": "variables", "action": "map", "snapshot": after["variable_snapshot"],
                "variable_id": identity, "to": target["id"]}) for identity in result["outputs"]]
            assert any(r["status"] == "ok" for r in attempts), attempts
            mapped = source(client)
            assert all(r["applied"] for r in mapped["variable_edit_results"]), mapped
            assert mapped["text"] != after["text"]
            # Associations retain navigation for the unchanged return statement.
            returns = [row["addresses"] for row in before["rows"]
                       if "return" in row.get("code_anchor", "")]
            assert len(returns) == 1 and returns[0], before
            for view in (after, mapped):
                assert view["mapping_status"] == "instruction_anchors", view
                assert [row["addresses"] for row in view["rows"]
                        if "return" in row.get("code_anchor", "")] == returns, view
            if compiler:
                harness = "\nstatic int calls; int64_t sub_500000() { return ++calls; }\nint main(void) { return start(17) != 2 || calls != 3; }\n"
                for index, view in enumerate((before, after, mapped)):
                    c_file, program = Path(directory) / f"profile{index}.c", Path(directory) / f"profile{index}.exe"
                    c_file.write_text(view["text"] + harness)
                    if Path(compiler).stem.lower() in ("cl", "clang-cl"):
                        command = [compiler, "/nologo", "/TC", "/std:c11", str(c_file), "/Fe:" + str(program)]
                    else:
                        command = [compiler, "-std=c11", "-fno-strict-aliasing", str(c_file), "-o", str(program)]
                    built = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
                    assert built.returncode == 0, built.stdout + built.stderr
                    subprocess.run([str(program)], cwd=directory, check=True, timeout=10)
            edit(client, mapped, "unmap", association=1)
            assert source(client)["text"] == after["text"]
            edit(client, after, "unsplit", association=0)
            assert source(client)["text"] == before["text"]
            ok(client, "undo")
            assert source(client)["text"] == after["text"]
            ok(client, "redo")
            assert source(client)["text"] == before["text"]
            edit(client, before, "split", variable_id="stack:-16")
            saved = source(client)
            # Old snapshots and independently defined SSA values are refused
            # without changing committed policy, revisions or the journal.
            snapshot = ok(client, "analysis_snapshot")
            refused = client.call("code_edit", {"address": hex(ENTRY), "representation": "c",
                "kind": "variables", "action": "split", "snapshot": "obsolete", "variable_id": "stack:-16"})
            assert refused["status"] == "error" and refused["error"]["code"] == "stale_target", refused
            assert ok(client, "analysis_snapshot") == snapshot
            llvm = source(client, "llvmc")
            selected = next(v for v in llvm["source_variables"] if not v["parameter"])
            refused = client.call("code_edit", {"address": hex(ENTRY), "representation": "llvmc",
                "kind": "variables", "action": "split", "snapshot": llvm["variable_snapshot"], "variable_id": selected["id"]})
            assert refused["status"] == "error" and refused["error"]["code"] == "variable_edit_refused", refused
            assert ok(client, "analysis_snapshot") == snapshot
            for rep, text in native.items(): assert source(client, rep)["text"] == text
            with Client(executable) as replica:
                ok(replica, "analysis_restore", ok(client, "analysis_snapshot"))
                assert source(replica)["text"] == saved["text"]
                refused = replica.call("code_edit", {"address": hex(ENTRY), "representation": "c", "kind": "variables", "action": "clear", "snapshot": saved["variable_snapshot"]})
                assert refused["status"] == "error" and refused["error"]["code"] == "read_only", refused
        with Client(executable) as client:
            ok(client, "open", {"path": str(path), "analysis": False})
            reopened = source(client)
            assert reopened["text"] == saved["text"] and reopened["variable_snapshot"] == saved["variable_snapshot"]
            altered = ok(client, "decompile", {"address": hex(ENTRY), "representation": "c", "limit": 2048, "source_options": {"symbolic": True}})
            assert not altered["variable_edit_results"][0]["applied"]
            edit(client, reopened, "clear")
            assert source(client)["text"] == before["text"]

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--cc")
    arguments = parser.parse_args()
    run(arguments.worker, arguments.cc)
