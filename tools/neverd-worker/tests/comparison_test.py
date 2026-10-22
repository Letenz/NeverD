#!/usr/bin/env python3
"""Shared structural comparison through the framed worker, never execute inputs."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

from transport_test import Client


def ok(client, operation, payload=None):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parents[3] / "unittests/diff/patch_fixture.c"
    with tempfile.TemporaryDirectory(prefix="neverd-comparison-worker-") as folder:
        root = Path(folder)
        for limit in (17, 9):
            subprocess.run([args.cc, "-c", "-O1", "-fno-asynchronous-unwind-tables",
                            f"-DPATCH_LIMIT={limit}", str(fixture), "-o", str(root / f"{limit}.o")],
                           check=True, capture_output=True)
        a, b = root / "17.o", root / "9.o"
        with Client(args.worker) as client:
            assert "comparison_start" in client.hello["capabilities"]
            assert {"comparison_start", "comparison_slice", "comparison_impact", "comparison_observations",
                    "comparison_function_page", "comparison_function",
                    "comparison_cfg_summary", "comparison_cfg_viewport", "comparison_cfg_focus"}.issubset(
                        client.hello["comparison_cancellable_operations"])
            assert client.call("comparison_functions")["error"]["code"] == "comparison_not_loaded"
            start = {"primary": {"path": str(a)}, "secondary": {"path": str(b)}, "options": {"regions": True, "dataflow": True, "flow_regions": {"min_nodes": 2}}}
            summary = ok(client, "comparison_start", start)
            assert summary["schema"] == "neverd.bindiff.summary"
            assert summary["counts"]["changed"] > 0 and "matches" not in summary
            assert not summary["stale"] and summary["job"]["computation_stopped"]
            assert client.call("metadata")["error"]["code"] == "not_loaded"
            flow = ok(client, "comparison_dataflow", {"limit": 10})
            function = next(row["function"] for row in flow["rows"] if row["availability"] == "available")
            nodes = ok(client, "comparison_dataflow", {"address": function, "kind": "nodes", "limit": 200})
            node = next(row["id"] for row in nodes["rows"] if row["opcode"] == "INT_MULT")
            sliced = ok(client, "comparison_slice", {"address": function, "seeds": [node], "limit": 1})
            assert sliced["state"] == "complete" and len(sliced["rows"]) == 1, sliced
            assert sliced["comparison_revision"] == nodes["comparison_revision"]
            assert sliced["semantic_validation"] == "not_performed"
            assert "comparison_flow_regions" in client.hello["capabilities"]
            ssa_regions = ok(client, "comparison_flow_regions", {"limit": 1})
            assert ssa_regions["total"] > 0, ssa_regions
            ssa_id = ssa_regions["items"][0]["id"]
            ssa_anchors = ok(client, "comparison_flow_regions", {"region_id": ssa_id, "limit": 1})
            ssa_nodes = ok(client, "comparison_flow_regions", {"anchor": ssa_anchors["items"][0]["anchor"], "limit": 1})
            assert len(ssa_nodes["rows"]) == 1 and ssa_nodes["view"] == "nodes"
            assert ssa_nodes["analysis"]["semantic_validation"] == "not_performed"
            assert ssa_nodes["comparison_revision"] == nodes["comparison_revision"]
            region_page = ok(client, "comparison_regions", {"limit": 1})
            assert region_page["total"] > 0, region_page
            hypothesis = region_page["items"][0]
            anchors = ok(client, "comparison_regions", {"region_id": hypothesis["id"], "limit": 1})
            shared = ok(client, "comparison_region_page", {"region_id": hypothesis["id"], "anchor": anchors["items"][0]["anchor"], "limit": 1})
            assert len(shared["rows"]) == 1 and shared["hypothesis"]["semantic_validation"] == "not_performed"
            page = ok(client, "comparison_functions", {"search": "checked", "limit": 1})
            assert page["total"] == 1, page
            entry = page["items"][0]
            address, other = entry["address"], entry["counterpart"]["address"]
            observations = ok(client, "comparison_observations", {"address": address})
            assert observations["profile"] == "native-ssa-changes-v1"
            assert observations["state"] == "complete" and observations["rows"], observations
            assert observations["fix_presence"] == "not_assessed"
            assert observations["comparison_revision"] == page["comparison_revision"]
            assert any("constant" in row["kinds"] for row in observations["rows"])
            limited_changes = ok(client, "comparison_observations", {"address": address, "max_work": 1})
            assert limited_changes["state"] == "resource_limited" and not limited_changes["rows"]
            assert "comparison_impact" in client.hello["capabilities"]
            impact_query = {"seeds": [address]}
            impact = ok(client, "comparison_impact", impact_query)
            assert impact["state"] == "complete" and impact["function_count"] >= 2, impact
            caller = next(row for row in impact["rows"] if row["name"] == "entry")
            assert caller["depth"] == 1 and caller["witness"]["instruction"]["bytes"]
            assert impact["assessment"] == "possible_call_reachability"
            assert impact["semantic_validation"] == "not_performed"
            assert impact["comparison_revision"] == page["comparison_revision"]
            witness = ok(client, "comparison_impact", {**impact_query, "kind": "path", "witness": caller["row"]})
            assert witness["total"] == 1 and witness["work"] == impact["work"]
            frontier = ok(client, "comparison_impact", {**impact_query, "max_depth": 0, "kind": "frontier"})
            assert frontier["state"] == "resource_limited" and frontier["total"] > 0
            detail = ok(client, "comparison_function", {"address": address})
            assert any(row["kind"] == "constant" for row in detail["rows"])
            first = ok(client, "comparison_function_page", {"address": address, "limit": 1})
            assert first["state"] == "complete" and first["computation_complete"]
            assert detail["state"] == "complete" and detail["computation_complete"]
            refreshed = ok(client, "comparison_function_page", {"address": address, "limit": 1, "refresh": True})
            assert refreshed == first
            assert first["total"] == len(detail["rows"]) and first["rows"] == detail["rows"][:1]
            graph = ok(client, "comparison_cfg_summary", {"address": address})
            bounds = graph["bounds"]
            view = ok(client, "comparison_cfg_viewport", {"address": address,
                      "layout_revision": graph["layout_revision"], **bounds, "scale": 1})
            assert view["nodes"] and all("comparison" in n for n in view["nodes"])
            block = view["nodes"][0]["address"]
            focus = ok(client, "comparison_cfg_focus", {"address": address, "block": block})
            assert focus["width"] > 0 and focus["height"] > 0
            revision = page["comparison_revision"]
            edited = ok(client, "comparison_override", {"kind": "confirm", "primary": address,
                        "secondary": other, "expected_comparison_revision": revision})
            assert edited["can_undo"] and not edited["can_redo"]
            refused = client.call("comparison_override", {"kind": "unmatch", "primary": address,
                        "secondary": other, "expected_comparison_revision": revision})
            assert refused["error"]["code"] == "stale_comparison", refused
            assert ok(client, "comparison_undo")["can_redo"]
            assert ok(client, "comparison_redo")["can_undo"]
            reviews = ok(client, "comparison_reviews", {"address": address})
            assert reviews["head"] == "" and reviews["selection"]["pair_active"]
            record = {"primary": address, "secondary": other, "case": "owned-worker-case",
                      "author": "Worker fixture analyst", "note": "Manual test assessment without proof.",
                      "assessment": "candidate_fix", "reference": "owned-test:worker", "expected_head": reviews["head"]}
            edited = ok(client, "comparison_review_record", record)
            assert edited["counts"]["review_records"] == 1
            history = ok(client, "comparison_reviews", {"address": address})
            assert history["total"] == 1 and history["rows"][0]["source"] == "analyst"
            assert history["rows"][0]["semantic_validation"] == "not_performed"
            assert client.call("comparison_review_record", record)["status"] == "error"
            saved = root / "review.nddiff"
            ok(client, "comparison_save", {"path": str(saved)})
            document = ok(client, "comparison_export")
            assert json.loads(saved.read_text()) == document
            ok(client, "comparison_override", {"kind": "unmatch", "primary": address, "secondary": other})
            inactive = ok(client, "comparison_reviews", {"address": address})
            assert not inactive["rows"][0]["pair_active"] and inactive["head"] == history["head"]
            assert ok(client, "comparison_restore", {"path": str(saved)})["counts"] == edited["counts"]
            ok(client, "comparison_start", {**start, "restore_path": str(saved)})
            assert ok(client, "comparison_export") == document
            reopened_reviews = ok(client, "comparison_reviews", {"address": address})
            assert reopened_reviews["rows"] == history["rows"] and reopened_reviews["head"] == history["head"]
            reopened_impact = ok(client, "comparison_impact", impact_query)
            assert reopened_impact["rows"] == impact["rows"]
            # Failure does not replace the previous result or touch the owner's project.
            failed = client.call("comparison_start", {**start, "options": {"profile": "unsupported"}})
            assert failed["error"]["code"] == "comparison_failed", failed
            assert ok(client, "comparison_export") == document
            # CLI reopens the identical shared file including manual evidence and times.
            cli = subprocess.run([args.neverd, "diff", "--structural", "-a", str(a), "-b", str(b),
                                  "--reopen", str(saved), "--json"], capture_output=True, text=True, timeout=30)
            assert cli.returncode in (2, 3), cli.stderr
            assert json.loads(cli.stdout) == document
            # A changed file invalidates browsing and mapping but can still export its snapshot.
            a.write_bytes(a.read_bytes() + b"changed")
            assert ok(client, "comparison_summary")["stale"]
            assert client.call("comparison_functions")["error"]["code"] == "stale_input"
            assert client.call("comparison_impact", impact_query)["error"]["code"] == "stale_input"
            assert ok(client, "comparison_export") == document
            # A stopped detail remains stopped across pages and graph requests;
            # only explicit refresh supplies new comparison annotations.
            detail_path = root / "large-detail.bin"
            detail_path.write_bytes(b"\x90" * 60000 + b"\xc3")
            detail_input = {"path": str(detail_path), "load_options": {
                "loader": "binary", "processor": "x86_64", "base": "0x1000", "platform": "sysv"}}
            ok(client, "comparison_start", {"primary": detail_input, "secondary": detail_input,
                                             "options": {"discover": False}})
            detail_query = {"address": "0x1000", "limit": 1, "timeout_ms": 1}
            stopped = ok(client, "comparison_function_page", {**detail_query, "refresh": True})
            assert stopped["state"] == "timed_out" and not stopped["computation_complete"], stopped
            assert ok(client, "comparison_function_page", detail_query) == stopped
            stopped_graph = client.call("comparison_cfg_summary", {"address": "0x1000", "timeout_ms": 1})
            assert stopped_graph["error"]["code"] == "timed_out", stopped_graph
            fresh = ok(client, "comparison_function_page", {"address": "0x1000", "limit": 1, "refresh": True})
            assert fresh["state"] == "complete" and fresh["total"] == 60001
            assert ok(client, "comparison_cfg_summary", {"address": "0x1000"})["analysis_complete"]
            expired_changes = ok(client, "comparison_observations", {**detail_query, "refresh": True})
            assert expired_changes["state"] == "timed_out", expired_changes
            assert ok(client, "comparison_observations", detail_query) == expired_changes
            fresh_changes = ok(client, "comparison_observations", {"address": "0x1000", "refresh": True})
            assert fresh_changes["state"] == "complete" and fresh_changes["visited_native_rows"] == 60001
            # A replacement stopped during either input load never replaces
            # the existing comparison, its mapping revision or its query state.
            loading = root / "loading.bin"
            with loading.open("wb") as stream:
                stream.truncate(64 * 1024 * 1024)
            loading_input = {"path": str(loading), "load_options": {
                "loader": "binary", "processor": "x86_64", "base": "0x1000",
                "platform": "sysv"}}
            retained_summary = ok(client, "comparison_summary")
            retained_rows = ok(client, "comparison_functions")
            for side in ("primary", "secondary"):
                replacement = {"primary": start["primary"], "secondary": start["secondary"]}
                replacement[side] = loading_input
                request = client.send("comparison_start", replacement)
                progress = client.next(
                    lambda m: m.get("request_id") == request and m.get("status") == "progress"
                    and m.get("payload", {}).get("phase") == f"{side}:load:hash"
                    and m["payload"].get("done", 0) > 0, timeout=30)
                assert progress["payload"]["done"] < progress["payload"]["total"], progress
                load_stop_began = time.monotonic()
                acknowledged = ok(client, "cancel", {"request_id": request})
                assert acknowledged["accepted"] and not acknowledged["requires_restart"], acknowledged
                stopped_load = client.response(request)
                assert stopped_load["status"] == "error" and stopped_load["error"]["code"] == "cancelled", stopped_load
                assert stopped_load["calculation_stopped"] and not stopped_load["completed_before_cancellation"], stopped_load
                assert not stopped_load["partial_result_retained"], stopped_load
                assert ok(client, "comparison_summary") == retained_summary
                assert ok(client, "comparison_functions") == retained_rows
                print(f"{side} input loading stop and retained comparison: {time.monotonic() - load_stop_began:.3f}s")
            retried = ok(client, "comparison_start", start)
            assert retried["state"] == "complete", retried
            # Actual cooperative cancellation: acknowledge request while work is running;
            # publish 'stopped' and retained partial state only in the terminal response.
            large = root / "large.bin"
            large.write_bytes(b"\x90" * (8 * 1024 * 1024) + b"\xc3")
            raw = {"path": str(large), "load_options": {"loader": "binary", "processor": "x86_64", "base": "0x1000"}}
            request = client.send("comparison_start", {"primary": raw, "secondary": raw})
            client.next(lambda m: m.get("request_id") == request and m.get("status") == "progress"
                        and m.get("payload", {}).get("phase") == "primary:discovery", timeout=30)
            began = time.monotonic()
            cancel = ok(client, "cancel", {"request_id": request})
            assert cancel["accepted"] and not cancel["stopped"] and not cancel["requires_restart"], cancel
            terminal = client.response(request)
            elapsed = time.monotonic() - began
            assert terminal["status"] == "ok", terminal
            assert terminal["payload"]["state"] == "cancelled", terminal
            assert terminal["payload"]["stopped_phase"] == "discovery", terminal
            assert not terminal["payload"]["primary"]["discovery_complete"], terminal
            assert not terminal["payload"]["secondary"]["discovery_complete"], terminal
            assert terminal["partial_result_retained"] and not terminal["completed_before_cancellation"]
            assert terminal["payload"]["job"]["computation_stopped"]
            assert elapsed < 5, elapsed
            print(f"worker comparison parity, edits, stale/failure isolation and cancellation passed ({elapsed:.3f}s)")


if __name__ == "__main__":
    main()
