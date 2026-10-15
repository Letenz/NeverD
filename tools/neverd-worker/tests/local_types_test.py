#!/usr/bin/env python3
"""The program's local types: a list, one type laid out, and source."""
from contextlib import ExitStack
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

        missing = owner.call("local_type", {"name": "_NO_SUCH_TYPE"})
        assert missing["status"] == "error" and missing["error"]["code"] == "local_types_failed", missing
        malformed = owner.call("local_types_source", {"asserts": "yes"})
        assert malformed["status"] == "error" and malformed["error"]["code"] == "invalid_request", malformed


if __name__ == "__main__":
    run(sys.argv[1])
