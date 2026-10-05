#!/usr/bin/env python3
"""Published libc++ evidence through the real worker and its revision cache."""
from pathlib import Path
import sys
import tempfile

from transport_test import Client


def run(worker, fixture, pack):
    client = Client(worker)
    try:
        assert "signatures_load" in client.hello["capabilities"]
        opened = client.call("open", {"path": str(Path(fixture).resolve()), "read_only": True})
        assert opened["status"] == "ok", opened
        functions = client.call("functions", {"limit": 512})["payload"]["items"]
        function = next(f for f in functions if "nd_vector_u32_data_inline" in f["name"])
        address = function["address"]
        before = client.call("decompile", {"address": address, "representation": "c"})
        assert before["status"] == "ok", before
        assert before["payload"]["library_regions"] == []
        loaded = client.call("signatures_load", {"path": str(Path(pack).resolve())})
        assert loaded["status"] == "ok", loaded
        assert loaded["revision"] != before["revision"]
        for stage in ("c", "llvmc"):
            offset, source, identity = 0, "", None
            while True:
                reply = client.call("decompile", {"address": address, "representation": stage, "offset": offset, "limit": 2})
                assert reply["status"] == "ok", reply
                page = reply["payload"]
                assert page["byte_offset"] == len(source.encode())
                source += page["text"]
                regions = page["library_regions"]
                assert len(regions) == 1, regions
                region = regions[0]
                assert region["rule_id"] == "libcxx.vector-u32.data"
                assert region["foldable"] and region["scope"] == "inline-expression", region
                assert region["linkage_name"] == ""
                assert region["occurrences"] and region["spans"]
                assert identity is None or identity == region["id"]
                identity = region["id"]
                if page["complete"]:
                    break
                assert page["next_offset"] > offset
                offset = page["next_offset"]
            if stage == "c":
                assert source == before["payload"]["text"]
        with tempfile.TemporaryDirectory(prefix="neverd-features-") as directory:
            invalid = Path(directory) / "broken.json"
            invalid.write_text("{}")
            bad = client.call("signatures_load", {"path": str(invalid)})
            assert bad["status"] == "error" and bad["revision"] == reply["revision"], bad
            preserved = client.call("decompile", {"address": address, "representation": "c"})
            assert len(preserved["payload"]["library_regions"]) == 1
            empty = Path(directory) / "empty-tree"
            empty.mkdir()
            cleared = client.call("signatures_load", {"path": str(empty), "mode": "auto"})
            assert cleared["status"] == "ok", cleared
            withdrawn = client.call("decompile", {"address": address, "representation": "c"})
            assert withdrawn["payload"]["library_regions"] == []
            assert withdrawn["payload"]["text"] == before["payload"]["text"]
        after = client.call("functions", {"limit": 512})["payload"]["items"]
        assert next(f for f in after if f["address"] == address)["name"] == function["name"]
        print("published library fixture: pack reload, unchanged source/name, paged HighC/LLVMC evidence and conservative foldability passed")
    finally:
        client.close()


if __name__ == "__main__":
    run(*sys.argv[1:])
