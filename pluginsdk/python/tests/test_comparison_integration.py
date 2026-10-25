"""Exercise callback, ownership-transfer and report ABI against libneverd."""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import tempfile
import unittest


class ComparisonIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        self.host = HostAPI(ctypes.CDLL(str(library_path)))
        directory = tempfile.TemporaryDirectory(prefix="neverd-python-comparison-")
        self.addCleanup(directory.cleanup)
        self.binary = Path(directory.name) / "return.raw"
        self.binary.write_bytes(bytes.fromhex("b80700000083c003c3"))

    def session(self):
        from neverd_plugin import abi

        handle = abi.SessionHandle(self.host.call("neverd_session_create"))
        self.assertTrue(handle.value)
        # run_owned clears this same slot when it takes ownership.
        self.addCleanup(lambda: self.host.call("neverd_session_destroy", handle)
                        if handle.value else None)
        self.assertEqual(self.host.call(
            "neverd_session_set_load_options", handle,
            b'{"loader":"binary","processor":"x86_64","platform":"sysv","base":4096}',
        ), 0)
        return handle

    def comparison(self):
        from neverd_plugin import abi

        handle = abi.ComparisonHandle(self.host.call("neverd_comparison_create"))
        self.assertTrue(handle.value)
        self.addCleanup(self.host.call, "neverd_comparison_destroy", handle)
        return handle

    def test_cancellation_callbacks_preserve_retry_and_context(self) -> None:
        from neverd_plugin import abi

        session = self.session()
        contexts = []
        stop = abi.CancelProbeCallback(lambda context: contexts.append(context) or 1)
        context = ctypes.c_void_p(0x12345678)
        path = str(self.binary).encode("utf-8")
        self.assertEqual(self.host.call(
            "neverd_session_load_cancellable", session, path, stop, context,
        ), -2)
        self.assertEqual(contexts, [context.value])
        self.assertEqual(self.host.call(
            "neverd_session_load_cancellable", session, path,
            abi.CancelProbeCallback(), None,
        ), 1)
        self.assertEqual(self.host.call(
            "neverd_session_discover_functions_cancellable", session, stop, context,
        ), -2)
        self.assertEqual(contexts, [context.value, context.value])
        self.assertGreaterEqual(self.host.call(
            "neverd_session_discover_functions_cancellable", session,
            abi.CancelProbeCallback(), None,
        ), 1)

    def test_loop_progress_keeps_proof_and_source_binding(self) -> None:
        from neverd_plugin import abi

        # Owned x64 O0 loop fixture; only inspect its bytes.
        self.binary.write_bytes(bytes.fromhex(
            "554889e5897dfc8975f88b45f883e0078945f48b45f489c183c1ff894df483f80074228b"
            "45fc83f0176bc00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fcebce8b45fc"
            "3345f883c0035dc3"))
        left, right = self.session(), self.session()
        for session in (left, right):
            self.assertEqual(self.host.call("neverd_session_load", session,
                                            str(self.binary).encode()), 1)
        comparison = self.comparison()
        options = b'{"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v1"}}'
        self.assertEqual(self.host.call("neverd_comparison_run", comparison,
            left, right, options, abi.ComparisonProgressCallback(), None), 1)
        page = json.loads(self.host.owned_string("neverd_comparison_dataflow_json",
            comparison, b'{"kind":"nodes","address":"0x1000","limit":200}'))
        self.assertEqual(page["schema_version"], 5)
        self.assertEqual(page["progress"]["kind"], "counter_loops")
        self.assertNotIn("loops", page["progress"])
        self.assertTrue(any(node["progress_loop"] for node in page["rows"]))
        saved = self.host.owned_string("neverd_comparison_export_json", comparison)
        self.assertEqual(self.host.call("neverd_comparison_restore_json", comparison,
                                        saved.encode()), 1)
        self.assertEqual(self.host.owned_string("neverd_comparison_export_json", comparison), saved)
        forged = json.loads(saved)
        proof = forged["dataflow"]["primary"]["entries"][0]["graph"]["progress"]
        proof["source_contract"] = proof["observed_contract"]
        self.assertEqual(self.host.call("neverd_comparison_restore_json", comparison,
                                        json.dumps(forged).encode()), 0)
        self.assertEqual(self.host.owned_string("neverd_comparison_export_json", comparison), saved)

    def test_multiblock_progress_keeps_header_and_source_binding(self) -> None:
        from neverd_plugin import abi

        # Owned x64 O0 loop fixture; only inspect its bytes.
        self.binary.write_bytes(bytes.fromhex("554889e5897dfc8975f88b45f883e0078945f48b45f489c183c1ff894df483f800746e8b45fc83e00183f80074528b45fc83f0176bc00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fc8b45fc05674523013345f88945fc8b45fc8b4dfcc1e90b31c86bc00d8945fc8b45fcc1e0078b4dfcc1e91909c88945fceb0f8b45fc83c0116b4df80d31c88945fceb828b45fc3345f883c0035dc3"))
        left, right = self.session(), self.session()
        for session in (left, right):
            self.assertEqual(self.host.call("neverd_session_load", session,
                                            str(self.binary).encode()), 1)
        comparison = self.comparison()
        options = b'{"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v2"}}'
        self.assertEqual(self.host.call("neverd_comparison_run", comparison,
            left, right, options, abi.ComparisonProgressCallback(), None), 1)
        page = json.loads(self.host.owned_string("neverd_comparison_dataflow_json",
            comparison, b'{"kind":"nodes","address":"0x1000","limit":200}'))
        self.assertEqual(page["schema_version"], 6)
        self.assertEqual(page["progress"]["kind"], "counter_loops")
        self.assertNotIn("loops", page["progress"])
        loops = [node["progress_loop"] for node in page["rows"] if node["progress_loop"]]
        self.assertTrue(loops)
        self.assertTrue(all(loop["header"] != loop["block"] for loop in loops))
        saved = self.host.owned_string("neverd_comparison_export_json", comparison)
        self.assertEqual(self.host.call("neverd_comparison_restore_json", comparison,
                                        saved.encode()), 1)
        self.assertEqual(self.host.owned_string("neverd_comparison_export_json", comparison), saved)
        forged = json.loads(saved)
        proof = forged["dataflow"]["primary"]["entries"][0]["graph"]["progress"]
        proof["source_contract"] = proof["observed_contract"]
        self.assertEqual(self.host.call("neverd_comparison_restore_json", comparison,
                                        json.dumps(forged).encode()), 0)
        self.assertEqual(self.host.owned_string("neverd_comparison_export_json", comparison), saved)

    def test_nested_progress_pages_and_authenticates_both_layers(self) -> None:
        from neverd_plugin import abi

        # Owned family4 x64 O0 fixture; inspect its two loops without execution.
        self.binary.write_bytes(bytes.fromhex(
            "554889e5897dfc8975f88b45f883e0078945f48b45f489c183c1ff894df483f800745b"
            "8b45fc83e00783c0018945f08b45f089c183c1ff894df083f800742e8b45fc83f0176b"
            "c00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fc8b45fc0345f43345f889"
            "45fcebc28b45fcc1e0070345f43345fc8945fceb958b45fc3345f883c0035dc3"))
        left, right = self.session(), self.session()
        for session in (left, right):
            self.assertEqual(self.host.call("neverd_session_load", session,
                                            str(self.binary).encode()), 1)
        comparison = self.comparison()
        options = b'{"discover":false,"dataflow":{"profile":"llvm-progress-ssa-v3"}}'
        self.assertEqual(self.host.call("neverd_comparison_run", comparison,
            left, right, options, abi.ComparisonProgressCallback(), None), 1)
        latches = set()
        offset, total = 0, 1
        while offset < total:
            query = {"kind": "nodes", "address": "0x1000", "limit": 1, "offset": offset}
            page = json.loads(self.host.owned_string("neverd_comparison_dataflow_json",
                comparison, json.dumps(query).encode()))
            self.assertEqual(page["schema_version"], 7)
            self.assertEqual(page["progress"]["kind"], "counter_loops")
            self.assertEqual(page["progress"]["loop_count"], 2)
            self.assertNotIn("loops", page["progress"])
            self.assertEqual(len(page["rows"]), 1)
            row = page["rows"][0]
            if row["progress_loop"]:
                self.assertEqual(row["progress_loop"]["block"], row["block"])
                latches.add(row["progress_loop"]["block"])
            offset, total = offset + 1, page["total"]
        self.assertEqual(len(latches), 2)
        saved = self.host.owned_string("neverd_comparison_export_json", comparison)
        self.assertEqual(self.host.call("neverd_comparison_restore_json", comparison,
                                        saved.encode()), 1)
        self.assertEqual(self.host.owned_string("neverd_comparison_export_json", comparison), saved)
        for layer in range(2):
            forged = json.loads(saved)
            proof = forged["dataflow"]["primary"]["entries"][0]["graph"]["progress"]
            proof["loops"][layer]["header"] = 0
            self.assertEqual(self.host.call("neverd_comparison_restore_json", comparison,
                                            json.dumps(forged).encode()), 0)
            self.assertEqual(self.host.owned_string("neverd_comparison_export_json", comparison), saved)

    def test_branch_progress_pages_every_guard_and_authenticates_counts(self) -> None:
        # Owned family5 x64 O0; two guarded updates join one backedge. Inspect only.
        self._check_counter_progress(
            "554889e5897dfc8975f88b45f883e00783c0018945f48b45fc83e00183f80074328b45"
            "fc83f0176bc00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fc8b45f483c0"
            "ff8945f483f8007402ebc5eb308b45fc83c0116b4df80d31c88945fc8b45fcc1e00703"
            "45f83345fc8945fc8b45f483c0ff8945f483f8007402eb95eb008b45fc3345f883c003"
            "5dc3", 4, 2)

    def test_modular_progress_pages_slices_and_authenticates_basis(self) -> None:
        # Owned modular_progress_fixture.c, x64 O0. Inspect only.
        self._check_counter_progress(
            "554889e5897dfc8975f88b45f88945f48b45fc83e00183f80074228b45fc83f0176b"
            "c00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fceb1e8b45fc83c0116b"
            "4df40d31c88945fc8b45fcc1e0070345f83345fc8945fc8b45f483c0038945f4837d"
            "f40173a68b45fc3345f883c0035dc3", 5, 1, "modular")
        # Owned family6 x64 O2, now explicitly naming its preferred proof.
        self._check_counter_progress(
            "89f183e1078d04768d1486eb1e0f1f0083f7178d04fd0000000029f883c009c1c01b83"
            "f9027c1dffc989c740f6c70175df83c71131d789f8c1e00701f031f883f9017fe331f0"
            "83c003c3", 5, 2, "nowrap")

    def _check_counter_progress(self, code, version, guard_count, proof_kind=None):
        from neverd_plugin import abi

        self.binary.write_bytes(bytes.fromhex(code))
        left, right = self.session(), self.session()
        for session in (left, right):
            self.assertEqual(self.host.call("neverd_session_load", session,
                                            str(self.binary).encode()), 1)
        comparison = self.comparison()
        options = json.dumps({"discover": False,
            "dataflow": {"profile": f"llvm-progress-ssa-v{version}"}}).encode()
        self.assertEqual(self.host.call("neverd_comparison_run", comparison,
            left, right, options, abi.ComparisonProgressCallback(), None), 1)
        guards, latches = set(), set()
        offset, total = 0, 1
        while offset < total:
            query = {"kind": "nodes", "address": "0x1000", "limit": 1, "offset": offset}
            page = json.loads(self.host.owned_string("neverd_comparison_dataflow_json",
                comparison, json.dumps(query).encode()))
            self.assertEqual(page["schema_version"], 4 + version)
            self.assertEqual(page["progress"]["kind"], "counter_loops")
            self.assertEqual(page["progress"]["loop_count"], 1)
            self.assertEqual(page["progress"]["backedge_count"], 1)
            self.assertEqual(page["progress"]["guard_count"], guard_count)
            self.assertNotIn("loops", page["progress"])
            self.assertEqual(len(page["rows"]), 1)
            row = page["rows"][0]
            if row["progress_loop"]:
                witness = row["progress_loop"]
                self.assertEqual(witness["block"], row["block"])
                self.assertEqual(witness.get("proof"), proof_kind)
                if witness["block"] not in guards:
                    query = {"address": "0x1000", "seeds": [witness["compare"]], "limit": 1}
                    sliced = json.loads(self.host.owned_string("neverd_comparison_slice_json",
                        comparison, json.dumps(query).encode()))
                    self.assertEqual(sliced["schema_version"], 4 + version)
                    self.assertTrue(sliced["rows"])
                guards.add(witness["block"])
                latches.add(witness["latch"])
            offset, total = offset + 1, page["total"]
        self.assertEqual(len(guards), guard_count)
        self.assertEqual(len(latches), 1)
        saved = self.host.owned_string("neverd_comparison_export_json", comparison)
        self.assertEqual(self.host.call("neverd_comparison_restore_json", comparison,
                                        saved.encode()), 1)
        self.assertEqual(self.host.owned_string("neverd_comparison_export_json", comparison), saved)
        fields = ["loop_count", "backedge_count", "guard_count", "header", "latch"]
        if version == 5:
            fields += ["proof", "missing_proof"]
        for field in fields:
            for layer in range(guard_count):
                forged = json.loads(saved)
                proof = forged["dataflow"]["primary"]["entries"][0]["graph"]["progress"]
                if field in ("header", "latch"):
                    proof["loops"][layer][field] = 0
                elif field == "proof":
                    proof["loops"][layer][field] = "nowrap" if proof_kind == "modular" else "modular"
                elif field == "missing_proof":
                    del proof["loops"][layer]["proof"]
                else:
                    proof[field] = 99
                self.assertEqual(self.host.call("neverd_comparison_restore_json", comparison,
                                                json.dumps(forged).encode()), 0)
                self.assertEqual(self.host.owned_string("neverd_comparison_export_json", comparison), saved)


    def test_native_parameter_views_use_the_shared_report_abi(self) -> None:
        from neverd_plugin import abi

        # Owned x64 O2 control fixture; only inspect its bytes.
        self.binary.write_bytes(bytes.fromhex(
            "39f7731183f7178d04fd0000000029f883c009eb0b83c7118d04768d0486"
            "31f831f083c003c3"))
        left, right = self.session(), self.session()
        for session in (left, right):
            self.assertEqual(self.host.call("neverd_session_load", session,
                                            str(self.binary).encode()), 1)
        comparison = self.comparison()
        options = b'{"discover":false,"dataflow":{"profile":"llvm-parameter-ssa-v1"}}'
        self.assertEqual(self.host.call("neverd_comparison_run", comparison,
            left, right, options, abi.ComparisonProgressCallback(), None), 1)
        page = json.loads(self.host.owned_string("neverd_comparison_dataflow_json",
            comparison, b'{"kind":"nodes","address":"0x1000","limit":200}'))
        self.assertEqual(page["schema_version"], 4)
        self.assertEqual(page["projected_parameter_count"], 1)
        self.assertNotIn("parameter_views", page)
        self.assertTrue(any(view["source_bits"] == 64 and view["view_bits"] == 32
                            for node in page["rows"] for view in node["parameter_views"]))
        saved = self.host.owned_string("neverd_comparison_export_json", comparison)
        self.assertEqual(self.host.call("neverd_comparison_restore_json", comparison,
                                        saved.encode()), 1)
        self.assertEqual(self.host.owned_string("neverd_comparison_export_json", comparison), saved)

    def test_comparison_progress_transfer_and_owned_reports(self) -> None:
        from neverd_plugin import abi

        left, right = self.session(), self.session()
        for session in (left, right):
            self.assertEqual(self.host.call(
                "neverd_session_load", session, str(self.binary).encode("utf-8"),
            ), 1)
        comparison = self.comparison()
        events = []
        progress = abi.ComparisonProgressCallback(
            lambda context, phase, done, total: events.append((context, phase, done, total))
        )
        context = ctypes.c_void_p(0x12345678)
        self.assertEqual(self.host.call(
            "neverd_comparison_run_owned", comparison,
            ctypes.byref(left), ctypes.byref(right), b'{"discover":false}',
            progress, context,
        ), 1)
        self.assertIsNone(left.value)
        self.assertIsNone(right.value)
        self.assertTrue(events)
        self.assertTrue(all(event[0] == context.value and event[1] for event in events))
        self.assertTrue(all(0 <= event[2] <= event[3] for event in events))
        summary = json.loads(self.host.owned_string("neverd_comparison_summary_json", comparison))
        self.assertEqual(summary["schema"], "neverd.bindiff.summary")
        self.assertEqual(summary["counts"]["matches"], 1)
        self.assertEqual(self.host.owned_string("neverd_comparison_last_error", comparison), "")
        self.host.call("neverd_comparison_cancel", comparison)
        status = json.loads(self.host.owned_string("neverd_comparison_status_json", comparison))
        self.assertTrue(status["cancel_requested"])
        self.assertTrue(status["result_available"])


if __name__ == "__main__":
    unittest.main()
