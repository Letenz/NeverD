"""Exercise genuine KMDF idle/wake through the owned JSON C binding."""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import struct
import unittest


class DriverPowerPolicyIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        normal = os.environ.get("NEVERD_TEST_KMDF_PNP_FIXTURE")
        if not library or not normal:
            self.skipTest("built libneverd and genuine KMDF PnP fixture are not configured")
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        self.host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(self.host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        self.session = ctypes.c_void_p(address)
        self.addCleanup(self.host.call, "neverd_session_destroy", self.session)
        self.fixtures = [Path(normal).resolve(strict=True)]
        cfg = os.environ.get("NEVERD_TEST_KMDF_PNP_CFG_FIXTURE")
        if cfg:
            self.fixtures.append(Path(cfg).resolve(strict=True))
        example = (Path(__file__).resolve().parents[3] / "docs" / "examples"
                   / "driver-kmdf-power-policy-scenario.json")
        self.scenario = json.loads(example.read_text(encoding="utf-8"))

    def run_scenario(self, fixture: Path, scenario: dict) -> dict:
        raw = self.host.owned_string(
            "neverd_emulate_driver_scenario_json", self.session, os.fsencode(fixture),
            json.dumps(scenario).encode("utf-8"), None,
        )
        self.assertIsNotNone(raw, self.host.owned_string("neverd_last_error", self.session))
        return json.loads(raw)

    def test_idle_wake_preserves_requests_and_observation_times(self) -> None:
        for fixture in self.fixtures:
            with self.subTest(fixture=fixture):
                result = self.run_scenario(fixture, self.scenario)
                self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                self.assertTrue(result["scenario_success"], result["diagnostic"])
                self.assertTrue(result["unload_completed"])
                events = result["power_policy_events"]
                self.assertEqual([event["action"] for event in events], ["idle", "wake"])
                self.assertEqual([event["occurred_at_100ns"] for event in events], [0, 10017])
                self.assertTrue(all(event["device_epoch"] > 0 for event in events))
                requests = result["requests"]
                wake = [r for r in requests if r["origin"] == "framework_wait_wake"]
                self.assertEqual(len(wake), 1)
                self.assertTrue(wake[0]["completed"])
                self.assertEqual(wake[0]["dispatch_status"], 0x103)
                self.assertEqual(wake[0]["io_status"], 0)
                self.assertEqual(wake[0]["power"]["minor"], "wait_wake")
                self.assertEqual(wake[0]["power"]["bus_received_at_100ns"], 10000)
                self.assertEqual(wake[0]["power"]["bus_completed_at_100ns"], 10017)
                scenario_requests = [r for r in requests if r["origin"] == "scenario"]
                snapshot = bytes.fromhex(scenario_requests[3]["output_hex"])
                self.assertEqual(struct.unpack("<6I", snapshot), (2, 1, 1, 1, 1, 1))

    def test_unarmed_wake_fails_without_inventing_power_requests(self) -> None:
        self.scenario["requests"][2]["power_policy_events"] = [{
            "device_id": "policy0", "after_100ns": 0, "action": "wake",
        }]
        for fixture in self.fixtures:
            with self.subTest(fixture=fixture):
                result = self.run_scenario(fixture, self.scenario)
                self.assertEqual(result["stop_reason"], "model_error")
                self.assertFalse(result["scenario_success"])
                self.assertIn("successfully armed", result["diagnostic"])
                self.assertTrue(all(r["origin"] == "scenario" for r in result["requests"]))
                self.assertIsNone(result["power_policy_events"][0]["occurred_at_100ns"])


if __name__ == "__main__":
    unittest.main()
