"""Exercise kernel data declarations through the public Python C API binding.

Set NEVERD_TEST_LIBNEVERD and NEVERD_TEST_DRIVER_DATA_FIXTURE to built artifacts.
Test discovery never downloads SDKs or compiles external drivers.
"""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import unittest


class DriverDataIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixture = os.environ.get("NEVERD_TEST_DRIVER_DATA_FIXTURE")
        if not library or not fixture:
            self.skipTest("built library and original data fixture are not configured")
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        self.host = HostAPI(ctypes.CDLL(str(library_path)))
        self.session = ctypes.c_void_p(self.host.call("neverd_session_create"))
        self.assertTrue(self.session.value)
        self.addCleanup(self.host.call, "neverd_session_destroy", self.session)
        self.fixture = os.fsencode(Path(fixture).resolve(strict=True))

    def run_scenario(self, scenario: dict) -> dict:
        raw = self.host.owned_string(
            "neverd_emulate_driver_scenario_json", self.session, self.fixture,
            json.dumps(scenario).encode("utf-8"), None,
        )
        self.assertIsNotNone(raw, self.host.owned_string("neverd_last_error", self.session))
        return json.loads(raw)

    def test_declared_debugger_bytes_round_trip(self) -> None:
        for enabled in (False, True):
            for not_present in (False, True):
                for action in ("1", "4"):
                    state = {"enabled": enabled, "not_present": not_present}
                    with self.subTest(state=state, action=action):
                        result = self.run_scenario({
                            "service_name": "NeverDData" + action,
                            "unload": True, "debugger_state": state,
                            "load_address": "0x1c0000000",
                        })
                        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                        self.assertEqual(result["nt_status"], 0)
                        self.assertEqual(result["messages"],
                                         [f"debugger bytes: {int(enabled)},{int(not_present)}"])
                        self.assertTrue(result["unload_completed"])
                        self.assertEqual(result["configuration"]["debugger_state"], state)

    def test_omission_and_invalid_declaration_do_not_supply_defaults(self) -> None:
        result = self.run_scenario({"service_name": "NeverDData1"})
        self.assertNotEqual(result["stop_reason"], "returned")
        self.assertNotIn("debugger_state", result["configuration"])
        raw = self.host.owned_string(
            "neverd_emulate_driver_scenario_json", self.session, b"missing.sys",
            b'{"debugger_state":{"enabled":true}}', None,
        )
        self.assertIsNone(raw)
        self.assertIn("debugger_state", self.host.owned_string("neverd_last_error", self.session))


if __name__ == "__main__":
    unittest.main()
