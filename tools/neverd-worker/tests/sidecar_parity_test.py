#!/usr/bin/env python3
"""The CLI and the workbench write a project's edits as identical bytes, so a
project kept under version control diffs only where its edits differ."""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from transport_test import Client

FIXTURES = Path(__file__).resolve().parents[3] / "unittests/types/fixtures"

# Escapes both JSON writers must spell alike: a tab, a line break, a
# backspace and a form feed, beside text outside ASCII.
TEXT = "CLI note\twith a tab\r\nand a line\bwith a backspace\fform feed é 中文"


def run(worker, cli):
    with tempfile.TemporaryDirectory(prefix="neverd-sidecar-parity-") as directory:
        binary = Path(directory) / "arithmetic.evm"
        binary.write_text("600160020100", encoding="ascii")
        annotations = Path(str(binary) + ".neverd/annotations.json")
        renames = Path(str(binary) + ".neverd/renames.json")

        def execute(args):
            return subprocess.run(args, capture_output=True, timeout=30)

        for command in ([cli, "annotate", str(binary), "--add=0x0", "--text=" + TEXT],
                        [cli, "rename", str(binary), "--func=evm_entry", "--to=reviewed_entry"]):
            result = execute(command)
            assert result.returncode == 0, (command, result.stdout, result.stderr)
        written = {path: path.read_bytes() for path in (annotations, renames)}
        for text in written.values():
            assert text.startswith(b"[\n  {\n    \"addr\": ") and text.endswith(b"\n]\n"), text

        for path in Path(directory).glob("arithmetic.evm.neverd-*.json"):
            path.unlink()
        client = Client(worker)
        try:
            def ok(operation, payload=None):
                reply = client.call(operation, payload or {})
                assert reply["status"] == "ok", (operation, reply)
                return reply["payload"]

            ok("open", {"path": str(binary)})
            ok("annotation_set", {"address": "0x0", "text": TEXT})
            ok("save")
            ok("rename", {"address": "0x0", "name": "reviewed_entry"})
        finally:
            client.close()
        for path, text in written.items():
            assert path.read_bytes() == text, (path.name, path.read_bytes(), text)
        print("CLI and workbench wrote identical annotation and rename sidecars")


def run_libraries(worker, cli):
    """The type libraries chosen, as `neverd types --remove-library` and
    `--add-library` and the workbench's Type libraries window choose them."""
    with tempfile.TemporaryDirectory(prefix="neverd-libraries-parity-") as directory:
        binary = Path(directory) / "local_types.exe"
        shutil.copyfile(FIXTURES / "local_types.exe", binary)
        shutil.copyfile(FIXTURES / "local_types.pdb", Path(directory) / "local_types.pdb")
        kept = Path(str(binary) + ".neverd/libraries.json")
        command = [cli, "types", str(binary), "--remove-library", "windows-kernel-x64",
                   "--add-library", "uefi-x64", "--json"]
        result = subprocess.run(command, capture_output=True, timeout=60)
        assert result.returncode == 0, (command, result.stdout, result.stderr)
        listed = json.loads(result.stdout)
        joined = [row["name"] for row in listed["libraries"] if row["joined"]]
        assert joined == ["windows-user-x64", "uefi-x64"], listed
        written = kept.read_bytes()
        assert json.loads(written) == {"added": ["uefi-x64"], "removed": ["windows-kernel-x64"]}
        # Taking the kernel's back is no removal of UEFI's.
        command = [cli, "types", str(binary), "--add-library", "windows-kernel-x64"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, (command, result.stdout, result.stderr)
        assert "windows-kernel-x64   program" in result.stdout, result.stdout
        assert json.loads(kept.read_bytes()) == {"added": ["uefi-x64"], "removed": []}
        refused = subprocess.run([cli, "types", str(binary), "--add-library", "linux-x64"],
                                 capture_output=True, text=True, timeout=60)
        assert refused.returncode != 0 and "lays its records out otherwise" in refused.stderr, refused

        kept.unlink()
        client = Client(worker)
        try:
            reply = client.call("open", {"path": str(binary), "analysis": False})
            assert reply["status"] == "ok", reply
            reply = client.call("type_libraries_set", {"added": ["uefi-x64"],
                                                       "removed": ["windows-kernel-x64"]})
            assert reply["status"] == "ok", reply
        finally:
            client.close()
        assert kept.read_bytes() == written, (kept.read_bytes(), written)
        print("CLI and workbench wrote identical type library choices")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
    run_libraries(sys.argv[1], sys.argv[2])
