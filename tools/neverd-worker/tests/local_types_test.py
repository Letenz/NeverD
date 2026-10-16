#!/usr/bin/env python3
"""The program's local types: a list, one type laid out, and source."""
from contextlib import ExitStack
import json
from pathlib import Path
import shutil
import sys
import tempfile

from transport_test import Client


FIXTURES = Path(__file__).resolve().parents[3] / "unittests/types/fixtures"


def ok(client, operation, payload=None):
    result = client.call(operation, payload or {})
    assert result["status"] == "ok", result
    return result["payload"]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="neverd-local-types-") as directory, ExitStack() as clients:
        root = Path(directory)
        binary = root / "local_types.exe"
        shutil.copyfile(FIXTURES / "local_types.exe", binary)
        shutil.copyfile(FIXTURES / "local_types.pdb", root / "local_types.pdb")

        owner = clients.enter_context(Client(executable))
        for operation in ("local_types", "local_type", "local_types_source"):
            assert operation in owner.hello["capabilities"]
        ok(owner, "open", {"path": str(binary), "analysis": False})

        listing = ok(owner, "local_types", {"filter": "device"})
        assert listing["source"] == "pdb" and listing["dataModel"] == "LLP64"
        assert [row["name"] for row in listing["types"]] == ["_DEVICE"]
        assert listing["types"][0]["size"] == 0x58

        device = ok(owner, "local_type", {"name": "_DEVICE", "spelling": "ida"})
        links = next(member for member in device["members"] if member["name"] == "Links")
        assert links["offset"] == 8 and links["refers"] == "_LIST_ENTRY"
        assert device["members"][0]["type"] == "unsigned __int16"

        rust = ok(owner, "local_types_source", {"language": "rust", "types": ["_PACKED"],
                                                "dependencies": False})
        assert rust["language"] == "rust" and "#[repr(C, packed(1))]" in rust["source"]
        # Python renders as ctypes, and declares nothing.
        python = ok(owner, "local_types_source", {"language": "python", "types": ["_PACKED"],
                                                  "dependencies": False})
        assert python["language"] == "python", python
        assert "class _PACKED(ctypes.Structure):" in python["source"], python
        assert '    _layout_ = "ms"\n' in python["source"], python
        unread = ok(owner, "local_types_declare", {"language": "python", "source": "X = 1"})
        assert unread["types"] == [] and "do not read" in unread["problems"][0]["message"], unread
        # JavaScript reads through a DataView, and declares nothing.
        script = ok(owner, "local_types_source", {"language": "javascript", "types": ["_PACKED"],
                                                  "dependencies": False})
        assert script["language"] == "javascript", script
        assert "export class _PACKED {" in script["source"], script
        unread = ok(owner, "local_types_declare", {"language": "javascript", "source": "export {}"})
        assert unread["types"] == [] and "do not read" in unread["problems"][0]["message"], unread
        # Prototypes render in C and C++ only, and fail elsewhere.
        refused = owner.call("local_types_source", {"language": "python", "prototypes": True})
        assert refused["status"] == "error" and "c and cpp" in refused["error"]["message"], refused

        # Declared types join the program's, and the project keeps them.
        assert "local_types_declare" in owner.hello["capabilities"]
        reply = owner.call("local_types_declare", {
            "language": "rust",
            "source": "#[repr(C)] pub struct OWNER { pub Device: *mut _DEVICE, pub Count: u32 }"})
        assert reply["status"] == "ok", reply
        declared = reply["payload"]
        assert declared["types"] == ["OWNER"] and declared["problems"] == [], declared
        # A type no prototype names leaves the decompilation and the views.
        assert declared["decompilation"] is False, declared
        assert ok(owner, "local_type", {"name": "OWNER"})["size"] == 16
        problem = ok(owner, "local_types_declare", {"language": "c", "source": "struct B {\n  NOPE x;\n};"})
        assert problem["problems"][0]["line"] == 2 and problem["problems"][0]["column"] == 3, problem
        forgot = ok(owner, "local_types_forget", {"names": ["_PACKED"]})
        assert forgot["removed"] == ["_PACKED"], forgot
        sidecar = binary.with_name(binary.name + ".neverd-types.json")
        assert sidecar.exists()
        rows = json.loads(sidecar.read_text())
        assert [row.get("language") for row in rows] == ["rust", None], rows
        assert rows[1] == {"forget": ["_PACKED"]}
        bad = owner.call("local_types_forget", {"names": "_PACKED"})
        assert bad["status"] == "error" and bad["error"]["code"] == "invalid_request", bad
        # A prototype takes the decompilation, and every view reads anew.
        prototype = owner.call("local_types_declare", {
            "language": "c", "source": "long Probe(struct _DEVICE *Device);"})
        assert prototype["status"] == "ok", prototype
        assert prototype["payload"]["prototypes"] == ["Probe"], prototype
        assert prototype["payload"]["decompilation"] is True, prototype
        assert prototype["revision"] != reply["revision"], (prototype, reply)

        # The analysis replica reads the project's types as they were left,
        # and declares none.
        replica = clients.enter_context(Client(executable))
        ok(replica, "analysis_restore", ok(owner, "analysis_snapshot"))
        assert ok(replica, "local_type", {"name": "OWNER"})["size"] == 16
        assert ok(replica, "local_types", {"filter": "packed"})["types"] == []
        refused = replica.call("local_types_declare", {"source": "struct C { int c; };"})
        assert refused["error"]["code"] == "read_only", refused

        missing = owner.call("local_type", {"name": "_NO_SUCH_TYPE"})
        assert missing["status"] == "error" and missing["error"]["code"] == "local_types_failed", missing
        malformed = owner.call("local_types_source", {"asserts": "yes"})
        assert malformed["status"] == "error" and malformed["error"]["code"] == "invalid_request", malformed


if __name__ == "__main__":
    run(sys.argv[1])
