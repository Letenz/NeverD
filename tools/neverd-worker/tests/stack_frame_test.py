#!/usr/bin/env python3
"""Real stack assertions: both C routes, history, replicas, native isolation."""
import argparse
from contextlib import ExitStack
import copy
from pathlib import Path
import tempfile
import subprocess
import re
from transport_test import Client
from source_variable_types_test import ok, source
from source_variable_edits_test import fixture
from source_synthesis_test import ENTRY


def run(executable, compiler=None):
    with tempfile.TemporaryDirectory(prefix="neverd-stack-frame-") as directory, ExitStack() as stack:
        binary = Path(directory) / "frame.elf"
        binary.write_bytes(fixture())
        client = stack.enter_context(Client(executable))
        ok(client, "open", {"path": str(binary), "analysis": False})
        baseline = {rep: source(client, rep)["text"] for rep in ("low", "med", "llvm")}
        flow = ok(client, "stack_frame", {"address": hex(ENTRY)})
        assert any(a["offset"] == -16 and a["bytes"] == 8 for a in flow["accesses"]), flow
        frame = {"snapshot": flow["snapshot"], "points": [], "variables": []}
        request = {"address": hex(ENTRY), "representation": "c", "kind": "stack_frame"}
        state = ok(client, "analysis_snapshot")
        for points in ([{"address": hex(ENTRY+1), "after": -40}],
                       [{"address": hex(ENTRY), "after": 1 << 30}]):
            reply = client.call("code_edit", {**request, "stack_frame": {**frame, "points": points}})
            assert reply["status"] == "error", reply
            assert ok(client, "analysis_snapshot") == state
        frame["variables"] = [{"offset": -16, "bytes": 8, "name": "saved_context", "type": "uint64_t"}]
        ok(client, "code_edit", {**request, "stack_frame": frame})
        for rep in ("c", "llvmc"):
            view = source(client, rep)
            assert "saved_context" in view["text"], view
            assert any(v.get("stack_offset") == -16 and v["name"] == "saved_context" for v in view["source_variables"]), view
        page = ok(client, "listing", {"address": hex(ENTRY+4), "before": 8, "after": 16})
        assert "saved_context" in str(page), page
        ok(client, "undo")
        assert "saved_context" not in source(client)["text"]
        ok(client, "redo")
        assert "saved_context" in source(client)["text"]
        frame["points"] = [{"address": hex(ENTRY), "after": -40}]
        ok(client, "code_edit", {**request, "stack_frame": frame})
        for rep in ("c", "llvmc"):
            assert "saved_context" in source(client, rep)["text"]
            if compiler:
                text = source(client, rep)["text"]
                result_type = re.search(r"(u?int64_t) sub_500000\(", text).group(1)
                harness = f"\nstatic int calls; {result_type} sub_500000() {{ return ++calls; }}\nint main(void) {{ return start(17) != 2 || calls != 3; }}\n"
                c_file, program = Path(directory) / f"{rep}.c", Path(directory) / f"{rep}.exe"
                c_file.write_text(text + harness, encoding="utf-8")
                if Path(compiler).stem.lower() in ("cl", "clang-cl"):
                    command = [compiler, "/nologo", "/TC", "/std:c11", str(c_file), "/Fe:" + str(program)]
                else:
                    command = [compiler, "-std=c11", "-fno-strict-aliasing", str(c_file), "-o", str(program)]
                built = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
                assert built.returncode == 0, built.stdout + built.stderr
                subprocess.run([str(program)], cwd=directory, check=True, timeout=10)
        for rep, text in baseline.items():
            assert source(client, rep)["text"] == text, rep
        # Invalid snapshots and overlaps cannot publish half a correction.
        state = ok(client, "analysis_snapshot")
        bad = copy.deepcopy(frame)
        bad["snapshot"] = "obsolete"
        assert client.call("code_edit", {**request, "stack_frame": bad})["status"] == "error"
        bad = copy.deepcopy(frame)
        bad["variables"].append({"offset": -12, "bytes": 4, "name": "overlap"})
        assert client.call("code_edit", {**request, "stack_frame": bad})["status"] == "error"
        assert ok(client, "analysis_snapshot") == state
        replica = stack.enter_context(Client(executable))
        ok(replica, "analysis_restore", state)
        assert "saved_context" in source(replica)["text"]
        assert replica.call("code_edit", {**request, "reset": True})["status"] == "error"
        replica.close()
        client.close()
        client = stack.enter_context(Client(executable))
        ok(client, "open", {"path": str(binary), "analysis": False})
        assert "saved_context" in source(client)["text"]
        ok(client, "code_edit", {**request, "reset": True})
        assert "saved_context" not in source(client)["text"]
        for rep, text in baseline.items(): assert source(client, rep)["text"] == text


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    parser.add_argument("--cc")
    args = parser.parse_args()
    run(args.executable, args.cc)
