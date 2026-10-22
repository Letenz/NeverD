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
