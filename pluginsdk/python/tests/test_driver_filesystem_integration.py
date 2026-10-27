"""Verify declared filesystem state through the public Python C API binding.

Set NEVERD_TEST_LIBNEVERD and NEVERD_TEST_DRIVER_FILESYSTEM_FIXTURE to built
artifacts. Test discovery does not download or build external drivers.
"""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import unittest


def volume() -> dict:
    root = dict(path="\\", kind="directory", file_id=0, attributes=16,
                allocation_size=0, creation_time_100ns=0,
                last_access_time_100ns=0, last_write_time_100ns=0,
                change_time_100ns=0)
    data = dict(root, path="\\Data", kind="file", file_id=2**64 - 1,
                attributes=32, allocation_size=4096, data="007f80ff",
                creation_time_100ns=1, last_access_time_100ns=2,
                last_write_time_100ns=3, change_time_100ns=4)
    return dict(device_name="\\Device\\DeclaredVolume", access_policy="full_access",
                sector_size=4096, alignment_mask=511, nodes=[root, data])


class DriverFileSystemIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixture = os.environ.get("NEVERD_TEST_DRIVER_FILESYSTEM_FIXTURE")
        if not library or not fixture:
            self.skipTest("built library and filesystem fixture are not configured")
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

    def test_exact_state_survives_success_and_execution_stop(self) -> None:
        for mode in ("S", "W"):
            for rebase in (False, True):
                with self.subTest(mode=mode, rebase=rebase):
                    declared = [volume()]
                    scenario = dict(service_name="DeclaredFileSystem" + mode,
                                    unload=True, filesystem=declared)
                    if rebase:
                        scenario["load_address"] = "0x1c0000000"
                    result = self.run_scenario(scenario)
                    self.assertEqual(result["stop_reason"],
                                     "returned" if mode == "S" else "model_error",
                                     result["diagnostic"])
                    self.assertEqual(result["unload_completed"], mode == "S")
                    self.assertEqual(result["configuration"]["filesystem"], declared)
                    observed = result["filesystem"]
                    self.assertEqual(len(observed), 1)
                    self.assertGreater(int(observed[0].pop("device_object"), 0), 0)
                    self.assertEqual(observed[0]["nodes"][0].pop("end_of_file"), 0)
                    self.assertEqual(observed[0]["nodes"][1].pop("end_of_file"), 4)
                    self.assertEqual(observed, declared)

    def test_omission_empty_and_invalid_never_invent_state(self) -> None:
        for declared in (False, True):
            scenario = dict(service_name="DeclaredFileSystemN", unload=True)
            if declared:
                scenario["filesystem"] = []
            result = self.run_scenario(scenario)
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["filesystem"], [] if declared else None)
        for scenario in ({"filesystem": None}, {"filesystem": [{}]}):
            raw = self.host.owned_string(
                "neverd_emulate_driver_scenario_json", self.session, b"missing.sys",
                json.dumps(scenario).encode("utf-8"), None,
            )
            self.assertIsNone(raw)
            self.assertIn("filesystem", self.host.owned_string("neverd_last_error", self.session))

    def test_driver_opens_have_independent_report_identity(self) -> None:
        for mode in ("C", "P", "X", "U", "R", "Q"):
            for rebase in (False, True):
                with self.subTest(mode=mode, rebase=rebase):
                    scenario = dict(service_name="FileOpen" + mode, unload=True,
                                    filesystem=[volume()])
                    if rebase:
                        scenario["load_address"] = "0x1c0000000"
                    result = self.run_scenario(scenario)
                    self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                    self.assertEqual(result["nt_status"], 0)
                    self.assertTrue(result["unload_completed"])
                    self.assertTrue(result["requests"])
                    identities = {}
                    for request in result["requests"]:
                        self.assertIsNone(request["file"])
                        self.assertTrue(request["completed"])
                        self.assertGreater(int(request["file_object"], 0), 0)
                        self.assertIn(request["filesystem_file_id"], (0, 2**64 - 1))
                        identity = (request["file_object"], request["filesystem_device"],
                                    request["filesystem_file_id"])
                        key = request["file_open_operation"]
                        self.assertEqual(identities.setdefault(key, identity), identity)
                    self.assertEqual(result["filesystem"][0]["nodes"][1]["data"], "007f80ff")
                    if mode == "Q":
                        metadata = [
                            (4, "010000000000000002000000000000000300000000000000"
                                "04000000000000002000000000000000"),
                            (5, "001000000000000004000000000000000100000000000000"),
                            (6, "ffffffffffffffff"),
                            (9, "0a0000005c004400610074006100"),
                            (34, "010000000000000002000000000000000300000000000000"
                                 "040000000000000000100000000000000400000000000000"
                                 "2000000000000000"),
                            (35, "2000000000000000"),
                        ]
                        queries = [r for r in result["requests"]
                                   if r["kind"] == "query_information"]
                        expected = [(c, 0, b) for c, b in metadata] * 2
                        expected += [(5, 0xc0000004, ""), (9, 0x80000005, "0a0000005c004400"),
                                     (16, 0, "20000000"), (17, 0, "ff010000")]
                        self.assertEqual(len(queries), len(expected))
                        for index, (query, (information_class, status, data)) in enumerate(
                                zip(queries, expected)):
                            self.assertEqual(query["origin"], "IoQueryFileInformation" if index < 6
                                             else "ZwQueryInformationFile")
                            self.assertEqual(query["file_information_class"], information_class)
                            self.assertEqual(query["io_status"], status)
                            self.assertEqual(query["information"], len(data) // 2)
                            self.assertEqual(query["output_hex"], data)


if __name__ == "__main__":
    unittest.main()
