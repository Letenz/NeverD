#!/usr/bin/env python3
"""Compile real template projections and compare CLI identities/source pages."""
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile


def run(executable, fixture, pack, compiler, mode="compile"):
    def cli(command, *args):
        result = subprocess.run(
            [executable, command, fixture, *args],
            text=True, capture_output=True, timeout=60,
        )
        assert result.returncode == 0, result.stderr
        return result.stdout

    functions = json.loads(cli("funcs", "--sig-file", pack, "--json"))
    supported = [f for f in functions if any(
        a["scope"] == "whole-function" for a in f["library_annotations"]
    )]
    assert len(supported) == 16, supported
    assert len({f["display_name"] for f in supported}) == 16
    assert all(f["name"] == f["linkage_name"] for f in supported)
    inline = next(f for f in functions if "nd_vector_u32_data_inline" in f["name"])
    with tempfile.TemporaryDirectory(prefix="neverd-library-cli-") as directory:
        root = Path(directory)
        for route in ([], ["--llvm"]):
            ordinary = root / "ordinary.c"
            annotated = root / "annotated.c"
            cli("decompile", *route, "-o", str(ordinary))
            cli("decompile", *route, "--sig-file", pack, "-o", str(annotated))
            assert ordinary.read_bytes() == annotated.read_bytes()
            source = annotated.read_text()
            identifiers = re.findall(r"^[^\n;{}]*?\b([A-Za-z_][A-Za-z_0-9]*)\([^;\n]*\) \{", source, re.M)
            assert len(identifiers) == len(set(identifiers)), identifiers
            if mode == "msvc-names":
                # PDB projections retain the existing external type contract.
                # These four template instances share each operation's stem.
                for stem in ("data", "size", "empty", "capacity"):
                    assert {stem, *(f"{stem}_{n}" for n in range(2, 5))} <= set(identifiers)
            else:
                result = subprocess.run(
                    [compiler, "-fsyntax-only", "-std=c11", str(annotated)],
                    capture_output=True, text=True, timeout=60,
                )
                assert result.returncode == 0, result.stderr
            cli("decompile", *route, "--func", inline["addr"], "-o", str(ordinary))
            view = json.loads(cli("decompile", *route, "--func", inline["addr"],
                                  "--sig-file", pack, "--json"))
            assert view["schema_version"] == 1
            assert "".join(p["text"] for p in view["pages"]).encode() == ordinary.read_bytes()
            for page in view["pages"]:
                assert page["function_identity"]["display_name"] == inline["display_name"]
                region = page["library_regions"][0]
                assert region["rule_id"].endswith(".vector-u32.data")
                assert region["foldable"] and region["occurrences"] and region["spans"]
    print(f"16 distinct templates: unchanged HighC/LLVMC, stable C identifiers, paged evidence ({mode}) passed")


if __name__ == "__main__":
    run(*sys.argv[1:])
