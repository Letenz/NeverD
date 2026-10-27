#!/usr/bin/env python3
"""A PE's PDB outranks the MAP beside it; the MAP's answers and loads."""
from contextlib import ExitStack
from pathlib import Path
import shutil
import sys
import tempfile

from transport_test import Client


ROOT = Path(__file__).resolve().parents[3]
FIXTURES = ROOT / "unittests/safety/fixtures/binaries"
COMPANIONS = ROOT / "unittests/debug/fixtures"
MAIN = "0x140001250"


def ok(client, operation, payload=None):
    result = client.call(operation, payload or {})
    assert result["status"] == "ok", result
    return result["payload"]


def name_at(client, address):
    page = ok(client, "functions", {"offset": 0, "limit": 512})
    for item in page["items"]:
        if item.get("address") == address:
            return item.get("name")
    return None


def kinds(metadata):
    return [entry["kind"] for entry in metadata["debug_info"].get("files", [])]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-map-prompt-") as directory, ExitStack() as clients:
        root = Path(directory)
        binary = root / "safety_cases_pe_x64.exe"
        shutil.copyfile(FIXTURES / "safety_cases_pe_x64.exe", binary)
        pdb = root / "safety_cases_pe_x64.pdb"
        shutil.copyfile(FIXTURES / "safety_cases_pe_x64.pdb", pdb)
        text = (FIXTURES / "safety_cases_pe_x64.exe.map").read_text()
        assert "                 main" in text
        beside = root / "safety_cases_pe_x64.exe.map"
        beside.write_text(text.replace("                 main", "                 map_says_main", 1))

        owner = clients.enter_context(Client(executable))
        for capability in ("map_reference", "map_inspect", "map_load", "dwarf_load"):
            assert capability in owner.hello["capabilities"], capability
        reference = ok(owner, "map_reference", {"path": str(binary)})["map"]
        assert reference["path"] == str(beside), reference
        assert reference["dialect"] == "lld-link" and reference["match"] == "consistent", reference
        assert reference["functions"] > 0 and reference["outside"] == 0, reference
        assert ok(owner, "map_reference", {"path": str(FIXTURES / "safety_cases_elf_x64")})["map"] is not None
        missing = owner.call("map_reference", {"path": str(root / "missing")})
        assert missing["status"] != "ok", missing
        refused = owner.call("open", {"path": str(binary), "analysis": False, "map": {"mode": "maybe"}})
        assert refused["status"] != "ok", refused

        # The accepted PDB loads after the image, and the MAP beside it does
        # not come first: the PDB names main.
        opened = ok(owner, "open", {"path": str(binary), "analysis": False,
                                    "pdb": {"path": str(pdb), "mode": "verified"}})
        assert opened["debug_info"]["kind"] == "pdb", opened["debug_info"]
        assert kinds(opened) == ["pdb"], opened["debug_info"]
        assert name_at(owner, MAIN) == "main"

        # A MAP chosen after it fills only what the PDB leaves unnamed.
        inspected = ok(owner, "map_inspect", {"path": str(beside)})
        assert inspected["match"] == "consistent" and "stamp" in inspected, inspected
        loaded = ok(owner, "map_load", {"path": str(beside), "mode": "checked",
                                        "stamp": inspected["stamp"]})
        assert kinds(loaded) == ["pdb", "map"], loaded["debug_info"]
        assert name_at(owner, MAIN) == "main"
        assert ok(owner, "metadata")["map"]["mode"] == "checked"
        snapshot = ok(owner, "analysis_snapshot")
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", snapshot)
        assert kinds(ok(replica, "metadata")) == ["pdb", "map"]

        # Without the PDB the MAP names the image, unless it is declined too.
        reader = clients.enter_context(Client(executable))
        opened = ok(reader, "open", {"path": str(binary), "read_only": True, "analysis": False,
                                     "pdb": {"mode": "none"}})
        assert opened["debug_info"]["kind"] == "map", opened["debug_info"]
        assert name_at(reader, MAIN) == "map_says_main"
        opened = ok(reader, "open", {"path": str(binary), "read_only": True, "analysis": False,
                                     "pdb": {"mode": "none"}, "map": {"mode": "none"}})
        assert opened["debug_info"]["kind"] == "none" and opened["map"] == {"mode": "none"}, opened

        # A MAP of another build is refused checked and loads unchecked.
        other = root / "other.map"
        other.write_text(text.replace("00001000 000003d6  4096 .text", "00001000 000003d7  4096 .text"))
        assert ok(reader, "map_inspect", {"path": str(other)})["reason"] == "section"
        mismatch = reader.call("map_load", {"path": str(other), "mode": "checked"})
        assert mismatch["status"] != "ok" and mismatch["error"]["code"] == "map_mismatch", mismatch
        loaded = ok(reader, "map_load", {"path": str(other), "mode": "unchecked"})
        assert loaded["mode"] == "unchecked" and kinds(loaded) == ["map"], loaded

        # A DWARF file the user chose for a stripped image.
        image = root / "image.so"
        shutil.copyfile(COMPANIONS / "companion.so", image)
        companion = root / "elsewhere.debug"
        shutil.copyfile(COMPANIONS / "companion.so.debug", companion)
        dwarf = clients.enter_context(Client(executable))
        ok(dwarf, "open", {"path": str(image), "analysis": False})
        loaded = ok(dwarf, "dwarf_load", {"path": str(companion)})
        assert loaded["debug_info"]["kind"] == "dwarf", loaded["debug_info"]
        assert ok(dwarf, "metadata")["dwarf"]["path"] == str(companion)
        wrong = dwarf.call("dwarf_load", {"path": str(FIXTURES / "safety_cases_elf_arm64")})
        assert wrong["status"] != "ok", wrong


if __name__ == "__main__":
    run(sys.argv[1])
