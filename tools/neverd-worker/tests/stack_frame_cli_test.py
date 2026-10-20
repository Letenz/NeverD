#!/usr/bin/env python3
"""CLI/worker stack policy parity, refusal, and executable C semantics."""

import argparse
import copy
import json
from pathlib import Path
import re
import subprocess
import tempfile

from source_synthesis_test import ENTRY, elf
from source_variable_edits_test import fixture
from source_variable_types_test import ok, source
from transport_test import Client


def run(worker, cli, compiler):
    with tempfile.TemporaryDirectory(prefix="neverd-stack-cli-") as directory:
        root = Path(directory)
        binary = root / "frame.elf"
        binary.write_bytes(fixture())
        policy_file = root / "frame.json"

        def invoke(*arguments, path=binary, success=True):
            result = subprocess.run(
                [cli, "decompile", str(path), *arguments],
                capture_output=True, text=True, timeout=60)
            assert (result.returncode == 0) == success, (
                arguments, result.returncode, result.stdout, result.stderr)
            return result

        def inspect(path=binary, *arguments):
            return json.loads(invoke(
                "--func", hex(ENTRY), "--stack-frame-info", "--json",
                *arguments, path=path).stdout)

        def write_policy(policy):
            policy_file.write_text(json.dumps(policy), encoding="utf-8")

        inspected = inspect()
        policy = inspected["policy"]
        assert policy["snapshot"] == inspected["snapshot"]
        assert len(policy["input_sha256"]) == 64
        assert any(a["offset"] == -16 and a["bytes"] == 8
                   for a in inspected["accesses"]), inspected
        # A complete inspection result and a bare policy are both accepted.
        invoke("--func", hex(ENTRY), "--stack-frame-info", "--json",
               "-o", str(policy_file))
        assert json.loads(policy_file.read_text()) == inspected
        assert inspect(binary, "--stack-frame", str(policy_file)) == inspected
        policy["variables"] = [
            {"offset": -16, "bytes": 8, "name": "saved_context",
             "type": "uint64_t"}]
        policy["points"] = [{"address": hex(ENTRY), "after": -40}]
        write_policy(policy)
        with Client(worker) as client:
            ok(client, "open", {"path": str(binary), "analysis": False})
            worker_info = ok(client, "stack_frame", {"address": hex(ENTRY)})
            assert worker_info["snapshot"] == inspected["snapshot"]
            def normalized_accesses(info):
                return [{**a, "address": int(a["address"], 16)}
                        for a in info["accesses"]]
            assert normalized_accesses(worker_info) == normalized_accesses(inspected)
            native = {rep: source(client, rep)["text"]
                      for rep in ("low", "med", "llvm")}
            ok(client, "code_edit", {
                "address": hex(ENTRY), "representation": "c",
                "kind": "stack_frame", "stack_frame": policy})
            for route in ("c", "llvmc"):
                flags = ["--llvm"] if route == "llvmc" else []
                output = json.loads(invoke(
                    "--func", hex(ENTRY), "--stack-frame", str(policy_file),
                    "--json", *flags).stdout)
                text = "".join(page["text"] for page in output["pages"])
                # The workbench gives the symbol-less ELF entry its display
                # name, while the CLI keeps the engine's address-based name.
                worker_text = re.sub(r"\bstart\b", "sub_400078",
                                     source(client, route)["text"])
                assert text == worker_text, route
                variables = output["pages"][0]["source_variables"]
                assert any(v["name"] == "saved_context" and
                           v.get("stack_offset") == -16 for v in variables)
                result_type = re.search(r"(u?int64_t) sub_500000\(", text).group(1)
                harness = (
                    f"\nstatic int calls; {result_type} sub_500000() "
                    "{ return ++calls; }\n"
                    "int main(void) { return sub_400078(17) != 2 || calls != 3; }\n")
                c_file, program = root / f"{route}.c", root / f"{route}.exe"
                c_file.write_text(text + harness, encoding="utf-8")
                if Path(compiler).stem.lower() in ("cl", "clang-cl"):
                    command = [compiler, "/nologo", "/TC", "/std:c11",
                               str(c_file), "/Fe:" + str(program)]
                else:
                    command = [compiler, "-std=c11", "-fno-strict-aliasing",
                               str(c_file), "-o", str(program)]
                built = subprocess.run(command, cwd=root, capture_output=True,
                                       text=True, timeout=60)
                assert built.returncode == 0, built.stdout + built.stderr
                subprocess.run([str(program)], cwd=root, check=True, timeout=10)
            for route, text in native.items():
                assert source(client, route)["text"] == text
        # Source optimization options must not be forwarded to the other
        # representation during the cross-route storage check.
        invoke("--func", hex(ENTRY), "--stack-frame", str(policy_file),
               "--llvm", "--llvm-opt=O1", "--json")

        # Query, invalid options and failed policies never truncate an output.
        output_file = root / "keep.c"
        output_file.write_text("keep", encoding="utf-8")
        for field, value in (("snapshot", "obsolete"),
                             ("input_sha256", "0" * 64),
                             ("points", [{"address": hex(ENTRY + 1), "after": -40}]),
                             ("points", [{"address": hex(ENTRY), "after": 1 << 30}]),
                             ("variables", [{"offset": -16, "bytes": 4, "name": "x"}]),
                             ("variables", [{"offset": -16, "bytes": 8, "name": "int"}]),
                             ("variables", [{"offset": -16, "bytes": 8, "name": "x", "type": 42}]),
                             ("corrections", [])):
            bad = copy.deepcopy(policy)
            bad[field] = value
            write_policy(bad)
            invoke("--func", hex(ENTRY), "--stack-frame", str(policy_file),
                   "-o", str(output_file), success=False)
            assert output_file.read_text() == "keep"
        write_policy(policy)
        for flags in ([], ["--func", hex(ENTRY), "--no-opt"],
                      ["--func", hex(ENTRY), "--devirtualize"],
                      ["--func", hex(ENTRY), "--language=rust"]):
            invoke(*flags, "--stack-frame", str(policy_file), success=False)
        for malformed in ("[]", "{", '{"policy":[]}', " " * (128 * 1024 + 1)):
            policy_file.write_text(malformed, encoding="utf-8")
            invoke("--func", hex(ENTRY), "--stack-frame", str(policy_file),
                   success=False)

        dynamic = root / "dynamic.elf"
        data = bytearray(fixture())
        data[120:124] = bytes.fromhex("4883e4f0")  # and rsp, -16
        dynamic.write_bytes(data)
        unknown = inspect(dynamic)
        assert next(i for i in unknown["instructions"]
                    if int(i["address"], 16) == ENTRY)["after"] is None
        correction = unknown["policy"]
        correction["points"] = [{"address": hex(ENTRY), "after": -40}]
        write_policy(correction)
        repaired = inspect(dynamic, "--stack-frame", str(policy_file))
        assert next(i for i in repaired["instructions"]
                    if int(i["address"], 16) == ENTRY)["after"] == -40
        assert any(a["offset"] == -16 for a in repaired["accesses"])

        # An observed slot optimized out of HighC must not silently lose its
        # requested name when accessed directly through the C API/CLI.
        folded = root / "folded.elf"
        folded.write_bytes(elf(bytes.fromhex(
            "4883ec10 48c7042407000000 488b0424 4883c410 c3")))
        folded_policy = inspect(folded)["policy"]
        folded_policy["variables"] = [
            {"offset": -16, "bytes": 8, "name": "must_survive"}]
        write_policy(folded_policy)
        refused = invoke("--func", hex(ENTRY), "--stack-frame", str(policy_file),
                         path=folded, success=False)
        assert "independent named storage" in refused.stderr, refused.stderr
        # Explicit empty policy returns the ordinary automatic source.
        write_policy({})
        baseline = invoke("--func", hex(ENTRY), "--json", path=folded).stdout
        reset = invoke("--func", hex(ENTRY), "--json", "--stack-frame",
                       str(policy_file), path=folded).stdout
        assert json.loads(baseline) == json.loads(reset)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("cli")
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    run(args.worker, args.cli, args.cc)
