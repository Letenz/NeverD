#!/usr/bin/env python3
"""Manual PDB analysis, legacy display aliases and replica provenance."""
from contextlib import ExitStack
from pathlib import Path
import os
import shutil
import struct
import sys
import tempfile
import time

from transport_test import Client


FIXTURES = Path(__file__).resolve().parents[3] / "unittests/safety/fixtures/binaries"
FUNCTION = "tainted_stack_overflow"
ADDRESS = "0x140001000"


def ok(client, operation, payload=None):
    result = client.call(operation, payload or {})
    assert result["status"] == "ok", result
    return result["payload"]


def user_renames(client):
    # The engine rebuilds each rename's original spelling from the names the
    # image has now, as ProjectHistory::verifyLoadedState expects; the user
    # owns only the address and the new name.
    return [{key: value for key, value in row.items() if key != "original"}
            for row in ok(client, "analysis_snapshot")["state"]["renames"]]


def without_debug_directory(binary):
    image = bytearray(binary.read_bytes())
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", image, optional)[0]
    assert magic in (0x10B, 0x20B)
    directories = optional + (112 if magic == 0x20B else 96)
    image[directories + 6 * 8:directories + 7 * 8] = bytes(8)
    binary.write_bytes(image)


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-manual-pdb-") as directory, ExitStack() as clients:
        root = Path(directory)
        binary = root / "input.exe"
        stripped = root / "stripped.exe"
        pdb = root / "symbols elsewhere" / "matching.pdb"
        pdb.parent.mkdir()
        shutil.copyfile(FIXTURES / "safety_cases_pe_x64.exe", binary)
        shutil.copyfile(binary, stripped)
        shutil.copyfile(FIXTURES / "safety_cases_pe_x64.pdb", pdb)
        without_debug_directory(stripped)

        owner = clients.enter_context(Client(executable))
        assert "pdb_load" in owner.hello["capabilities"]
        ok(owner, "open", {"path": str(binary), "analysis": False, "debug_info": False})
        assert ok(owner, "functions", {"filter": FUNCTION})["total"] == 0
        ok(owner, "rename", {"address": ADDRESS, "name": "user_name"})
        ok(owner, "annotation_set", {"address": ADDRESS, "text": "keep this comment"})
        before = ok(owner, "functions", {"filter": "user_name"})
        rename_state = user_renames(owner)
        selected = ok(owner, "pdb_inspect", {"path": str(pdb)})
        assert selected["verified"]
        verified_selection = {"path": selected["path"], "mode": "verified",
                              "stamp": selected["stamp"]}
        assert ok(owner, "functions", {"filter": "user_name"}) == before
        loaded = ok(owner, "pdb_load", dict(selected, mode="verified"))
        assert loaded["verified"] and loaded["name_count"] > 0
        assert ok(owner, "functions", {"filter": "user_name"})["total"] == 1
        assert ok(owner, "annotations")["items"][0]["text"] == "keep this comment"
        assert user_renames(owner) == rename_state
        # Without the user's name the function is now called what the PDB says.
        assert ok(owner, "analysis_snapshot")["state"]["renames"][0]["original"] == FUNCTION
        ok(owner, "decompile", {"address": ADDRESS, "representation": "c", "limit": 32})
        assert user_renames(owner) == rename_state
        bad = owner.call("pdb_load", {"path": str(root / "absent.pdb")})
        assert bad["status"] != "ok"
        assert ok(owner, "functions", {"filter": "user_name"})["total"] == 1
        wrong = owner.call("pdb_inspect", {"path": str(FIXTURES / "safety_cases_pe_arm64.pdb")})
        assert wrong["status"] != "ok"
        assert ok(owner, "functions", {"filter": "user_name"})["total"] == 1
        snapshot = ok(owner, "analysis_snapshot")
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", snapshot)
        assert ok(replica, "functions", {"filter": "user_name"})["total"] == 1

        display = clients.enter_context(Client(executable))
        ok(display, "open", {"path": str(stripped), "analysis": False, "debug_info": False})
        original_count = ok(display, "functions")["total"]
        selected = ok(display, "pdb_inspect", {"path": str(pdb)})
        assert not selected["verified"]
        refused = display.call("pdb_load", dict(selected, mode="verified"))
        assert refused["error"]["code"] == "pdb_identity_missing"
        assert ok(display, "functions", {"filter": FUNCTION})["total"] == 0
        ok(display, "pdb_load", dict(selected, mode="display"))
        row = ok(display, "functions", {"filter": FUNCTION})["items"][0]
        assert row["name"] == FUNCTION and row["engine_name"].startswith("sub_")
        assert ok(display, "functions")["total"] == original_count
        snapshot = ok(display, "analysis_snapshot")
        display_replica = clients.enter_context(Client(executable))
        ok(display_replica, "analysis_restore", snapshot)
        assert ok(display_replica, "functions", {"filter": FUNCTION})["items"][0]["name"] == FUNCTION

        forced = clients.enter_context(Client(executable))
        ok(forced, "open", {"path": str(stripped), "read_only": True, "analysis": False, "debug_info": False})
        selected = ok(forced, "pdb_inspect", {"path": str(pdb)})
        assert not selected["verified"] and selected["supports_unverified_analysis"]
        loaded = ok(forced, "pdb_load", dict(selected, mode="analysis"))
        assert not loaded["verified"] and loaded["analysis"]
        assert loaded["mode"] == "analysis"
        row = ok(forced, "functions", {"filter": FUNCTION})["items"][0]
        # The engine itself names it; a row lists engine_name only for an alias.
        assert row["name"] == FUNCTION and "engine_name" not in row
        page = ok(forced, "decompile", {"address": ADDRESS, "representation": "c", "limit": 256})
        text = page["text"]
        # The PDB's procedure type reaches the decompiler as a matching PDB's
        # does; the image alone spells int64_t sub_140001000(void).
        assert f"void {FUNCTION}(void)" in text and "neverd.debug: return=void" in text
        snapshot = ok(forced, "analysis_snapshot")
        assert snapshot["pdb"]["mode"] == "analysis"
        forced_replica = clients.enter_context(Client(executable))
        ok(forced_replica, "analysis_restore", snapshot)
        restored = ok(forced_replica, "functions", {"filter": FUNCTION})["items"][0]
        assert restored["name"] == FUNCTION and "engine_name" not in restored
        restored_page = ok(forced_replica, "decompile", {"address": ADDRESS, "representation": "c", "limit": 256})
        assert restored_page["text"] == page["text"]

        # A saved project names its PDB outside itself. Reopening proves a
        # matching PDB again by its identity, though its stamp changed; one
        # the user confirmed without an identity must be confirmed again,
        # and the project opens without it rather than not at all.
        later = time.time() + 60
        os.utime(pdb, (later, later))
        reopened = clients.enter_context(Client(executable))
        opened = ok(reopened, "open", {"path": str(binary), "read_only": True,
                                       "analysis": False, "pdb": verified_selection})
        assert opened["pdb"]["verified"]
        assert not [warning for warning in opened["warnings"] if "PDB" in warning]
        assert ok(reopened, "functions", {"filter": "user_name"})["total"] == 1
        unconfirmed = clients.enter_context(Client(executable))
        opened = ok(unconfirmed, "open", {"path": str(stripped), "read_only": True,
                                          "analysis": False, "pdb": snapshot["pdb"]})
        assert opened["pdb"] is None
        assert [warning for warning in opened["warnings"] if "PDB was not loaded" in warning]
        assert ok(unconfirmed, "functions", {"filter": FUNCTION})["total"] == 0

        pdb.write_bytes(pdb.read_bytes() + b"changed")
        stale = clients.enter_context(Client(executable))
        assert stale.call("analysis_restore", snapshot)["error"]["code"] == "input_changed"

        pdb.unlink()
        missing = clients.enter_context(Client(executable))
        opened = ok(missing, "open", {"path": str(binary), "read_only": True,
                                      "analysis": False, "pdb": verified_selection})
        assert opened["pdb"] is None
        assert [warning for warning in opened["warnings"] if "PDB was not loaded" in warning]


def answered_before_open(executable):
    """The PDB a PE names, found before the PE opens, and both answers."""
    with tempfile.TemporaryDirectory(prefix="neverd-pdb-prompt-") as directory, ExitStack() as clients:
        root = Path(directory)
        binary = root / "input.exe"
        beside = root / "safety_cases_pe_x64.pdb"
        shutil.copyfile(FIXTURES / "safety_cases_pe_x64.exe", binary)
        shutil.copyfile(FIXTURES / "safety_cases_pe_x64.pdb", beside)

        owner = clients.enter_context(Client(executable))
        assert "pdb_reference" in owner.hello["capabilities"]
        reference = ok(owner, "pdb_reference", {"path": str(binary)})["pdb"]
        assert reference["file"] == "safety_cases_pe_x64.pdb"
        assert reference["found"] == {"path": str(beside), "location": "beside"}
        assert ok(owner, "pdb_reference", {"path": str(FIXTURES / "safety_cases_pe_x64.exe.map")})["pdb"] is None

        # No: the PDB beside the input stays unloaded, in the replica too, and
        # is not offered for download.
        opened = ok(owner, "open", {"path": str(binary), "analysis": False, "pdb": {"mode": "none"}})
        assert opened["pdb"] == {"mode": "none"} and opened["pdb_download"] is None
        assert ok(owner, "functions", {"filter": FUNCTION})["total"] == 0
        snapshot = ok(owner, "analysis_snapshot")
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", snapshot)
        assert ok(replica, "functions", {"filter": FUNCTION})["total"] == 0

        # Yes: the copy the reference found is the one that loads.
        chosen = clients.enter_context(Client(executable))
        opened = ok(chosen, "open", {"path": str(binary), "read_only": True, "analysis": False,
                                     "pdb": {"path": reference["found"]["path"], "mode": "verified"}})
        assert opened["pdb"]["verified"] and not opened["warnings"]
        assert ok(chosen, "functions", {"filter": FUNCTION})["total"] == 1


if __name__ == "__main__":
    run(sys.argv[1])
    answered_before_open(sys.argv[1])
