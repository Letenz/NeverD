#!/usr/bin/env python3
"""Real worker/CLI lineage, exact source persistence and transactional stops."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from transport_test import Client


def reply(client, operation, payload=None):
    request = client.send(operation, payload)
    return client.next(lambda m: m.get("request_id") == request
                       and m.get("type") == "response" and m.get("status") != "progress", timeout=90)


def ok(client, operation, payload=None):
    result = reply(client, operation, payload)
    assert result["status"] == "ok", result
    return result["payload"]


def cli(args, *options, code=0):
    result = subprocess.run([args.neverd, "lineage", *map(str, options)],
                            capture_output=True, timeout=90)
    assert result.returncode == code, (options, result.returncode, result.stderr.decode())
    return result.stdout


def run(args, root):
    with Client(args.worker) as client:
        assert "lineage_load" in client.hello["capabilities"], client.hello
        inputs = []
        for number in (1, 2, 3):
            binary = root / f"release-{number}.bin"
            binary.write_bytes(bytes([0xB8, number, 0, 0, 0, 0xC3]))
            inputs.append({"path": str(binary), "load_options": {
                "loader": "binary", "processor": "x86_64", "platform": "sysv", "base": "0x1000"}})
        reports, originals = [], []
        for left, right in ((0, 1), (1, 2), (0, 2)):
            ok(client, "comparison_start", {"primary": inputs[left], "secondary": inputs[right],
                                            "options": {"discover": True}})
            path = root / f"{left + 1}-{right + 1}.nddiff"
            ok(client, "comparison_save", {"path": str(path)})
            reports.append(path)
            originals.append(path.read_bytes())
        # Native sessions are disposable; offline evidence outlives them.
        ok(client, "comparison_close")
        loaded = ok(client, "lineage_load", {"reports": list(map(str, reports))})
        assert loaded["versions"] == loaded["nodes"] == loaded["edges"] == 3, loaded
        assert loaded["conflicts"] == loaded["incomplete_reports"] == 0
        assert loaded["load_completed"] and not loaded["native_authenticated"]
        revision = loaded["revision"]
        versions = ok(client, "lineage_query", {"query": {"kind": "versions", "limit": 1}})
        assert versions["total"] == 3 and versions["next_offset"] == 1
        version = versions["items"][0]["id"]
        trace = {"kind": "trace_nodes", "version": version, "address": "0x1000", "limit": 1}
        first = ok(client, "lineage_query", {"query": trace})
        assert first["total"] == 3
        last = ok(client, "lineage_query", {"query": {**trace, "offset": 2}})
        assert last["next_offset"] is None and last["items"] != first["items"]
        edges = ok(client, "lineage_query", {"query": {**trace, "kind": "trace_edges", "limit": 10}})
        assert edges["total"] == 3  # Direct A-C survives the BFS predecessor tree.
        refused = reply(client, "lineage_query", {"query": {**trace, "max_work": 1}})
        assert refused["error"]["code"] == "resource_limited", refused
        assert ok(client, "lineage_status")["loaded_revision"] == revision
        assert ok(client, "lineage_query", {"query": trace}) == first
        saved = root / "releases.ndlineage"
        ok(client, "lineage_save", {"path": str(saved)})
        bundle = json.loads(saved.read_text())
        assert sorted(r.encode() for r in bundle["reports"]) == sorted(originals)
        sources = ok(client, "lineage_query", {"query": {"kind": "reports"}})["items"]
        exported = root / "source.nddiff"
        ok(client, "lineage_save_report", {"path": str(exported), "report": sources[0]["id"]})
        assert exported.read_bytes() in originals
        assert cli(args, "--reopen", saved, "--export-report", sources[0]["id"]) == exported.read_bytes()
        assert json.loads(cli(args, "--reopen", saved, "--list", "trace_edges", "--snapshot", version,
                              "--address", "0x1000", "--json")) == edges
        for options in (("--offset", "1"), ("--limit", "1"), ("--history=false",),
                        ("--export-report", sources[0]["id"], "--source", sources[0]["id"]),
                        ("--list", "versions", "--snapshot", version),
                        ("--query", '{"kind":"versions","revision":"stale"}')):
            assert cli(args, "--reopen", saved, *options, code=1) == b""
        # Failure must not publish a replacement, a partial stdout or a temp file.
        target = root / "protected.ndlineage"
        target.write_bytes(b"original target")
        assert cli(args, "--reopen", saved, "--options", '{"max_work":1}', "--save", target, code=4) == b""
        assert target.read_bytes() == b"original target"
        assert not list(root.glob("protected.ndlineage.tmp-*"))
        malformed = root / "malformed.ndlineage"
        malformed.write_text("{bad")
        for payload in ({"path": str(malformed)}, {"reports": [str(reports[0])], "options": {"max_nodes": 1}},
                        {"path": str(saved), "unknown": True}):
            assert reply(client, "lineage_load", payload)["status"] != "ok"
            assert ok(client, "lineage_summary")["revision"] == revision
        # Partial metadata edits retain labels from other snapshots.
        rows = ok(client, "lineage_query", {"query": {"kind": "versions"}})["items"]
        for index, row in enumerate(rows[:2]):
            changed = ok(client, "lineage_load", {"append": True, "version_updates": [
                {"id": row["id"], "label": f"Release {index + 1} 中文", "branch": "stable"}]})
            assert changed["revision"] != revision
        labelled = ok(client, "lineage_query", {"query": {"kind": "versions"}})["items"]
        assert [row["label"] for row in labelled[:2]] == ["Release 1 中文", "Release 2 中文"]
        before = ok(client, "lineage_summary")["revision"]
        duplicate = [{"id": rows[0]["id"], "label": "wrong", "branch": ""}] * 2
        assert reply(client, "lineage_load", {"append": True, "version_updates": duplicate})["status"] == "error"
        assert ok(client, "lineage_summary")["revision"] == before
        assert reply(client, "lineage_query", {"expected_lineage_revision": revision,
                                                "query": {"kind": "versions"}})["error"]["code"] == "stale_lineage"
        ok(client, "lineage_save", {"path": str(saved)})
        # This exceeds the unrelated backend JSON limit. No full source crosses IPC.
        large = root / "large.nddiff"
        large.write_bytes(originals[0] + b" " * (40 * 1024 * 1024))
        request = client.send("lineage_load", {"reports": [str(large)]})
        client.next(lambda m: m.get("request_id") == request and m.get("status") == "progress"
                    and m.get("payload", {}).get("phase") == "lineage:read", timeout=30)
        ack = ok(client, "cancel", {"request_id": request})
        assert ack["accepted"] and not ack["requires_restart"], ack
        stopped = client.next(lambda m: m.get("request_id") == request and m.get("status") != "progress", timeout=90)
        assert stopped["error"]["code"] == "cancelled" and stopped["calculation_stopped"], stopped
        assert not stopped["partial_result_retained"] and not stopped["completed_before_cancellation"]
        assert ok(client, "lineage_summary")["revision"] == before
        large_summary = ok(client, "lineage_load", {"reports": [str(large)]})
        assert large_summary["reports"] == 1 and large_summary["versions"] == 2
        large_id = ok(client, "lineage_query", {"query": {"kind": "reports"}})["items"][0]["id"]
        ok(client, "lineage_save_report", {"path": str(exported), "report": large_id})
        assert exported.read_bytes() == large.read_bytes()
        ok(client, "lineage_save", {"path": str(root / "large.ndlineage")})
        ok(client, "lineage_close")
        assert ok(client, "lineage_load", {"path": str(root / "large.ndlineage")})["revision"] == large_summary["revision"]
        assert json.loads(cli(args, "--report", large, "--json"))["revision"] == large_summary["revision"]
        # Historical stopping is source evidence, not the outcome of this load.
        historical = json.loads(originals[0])
        historical["state"], historical["stopped_phase"] = "cancelled", "measure"
        incomplete = root / "incomplete.nddiff"
        incomplete.write_text(json.dumps(historical))
        summary = ok(client, "lineage_load", {"reports": [str(incomplete)]})
        assert summary["job"]["operation_state"] == "complete" and summary["incomplete_reports"] == 1
        assert json.loads(cli(args, "--report", incomplete, "--json", code=3))["incomplete_reports"] == 1
        ok(client, "lineage_close")
        assert not ok(client, "lineage_status")["result_available"]
    print("Lineage worker/CLI: native sources, cycles, pages, exact bytes, metadata, budgets, stale requests, cancellation and >32 MiB persistence passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("worker")
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
        run(args, args.artifacts)
    else:
        with tempfile.TemporaryDirectory(prefix="neverd-lineage-worker-") as directory:
            run(args, Path(directory))


if __name__ == "__main__":
    main()
