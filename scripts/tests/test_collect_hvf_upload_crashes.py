import json
from pathlib import Path
import tempfile
import unittest

from scripts.collect_hvf_upload_crashes import collect, matches, read_report, MAX_REPORT_BYTES


class UploadCrashTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.upload = {"pid": 123, "parent_pid": 100, "executable": "/runner/node24/bin/node",
                       "started_at": "2026-10-04T04:05:18.000Z",
                       "completed_at": "2026-10-04T04:05:18.900Z", "signal": "SIGSEGV"}
        self.report = {"pid": 123, "parentPid": 100, "procName": "node",
                       "captureTime": "2026-10-04 04:05:18.800 +0000"}

    def test_exact_child_identity_and_time_are_required(self):
        self.assertTrue(matches(self.report, self.upload))
        for key, value in [("pid", 124), ("parentPid", 101), ("procName", "NeverDHvfTests"),
                           ("captureTime", "2026-10-03T04:05:18Z"), ("captureTime", "invalid")]:
            self.assertFalse(matches({**self.report, key: value}, self.upload))

    def test_only_matching_report_is_copied_without_modification(self):
        uploads = self.root / "evidence/uploads"
        uploads.mkdir(parents=True)
        (uploads / "progress-004.json").write_text(json.dumps(self.upload))
        reports = self.root / "reports"
        reports.mkdir()
        payload = (json.dumps({"app_name": "node"}) + "\n" + json.dumps(self.report)).encode()
        (reports / "node-match.ips").write_bytes(payload)
        (reports / "node-other.ips").write_text(json.dumps({}) + "\n" + json.dumps({**self.report, "pid": 1}))
        (reports / "node-unrelated-private-timestamp.ips").write_text('unrelated invalid report')
        result = collect(self.root / "evidence", [reports], 0)
        self.assertEqual(result["missing"], [])
        self.assertEqual(len(result["captured"]), 1)
        self.assertEqual((uploads / "crashes/progress-004.ips").read_bytes(), payload)
        self.assertEqual(result["errors"], {"JSONDecodeError": 1})
        self.assertNotIn("unrelated-private", (uploads / "crashes/collection.json").read_text())

    def test_partial_oversize_and_symlink_reports_do_not_escape_collection(self):
        partial = self.root / "partial.ips"
        partial.write_text('{}\n{"pid":')
        with self.assertRaises(ValueError):
            read_report(partial)
        large = self.root / "large.ips"
        with large.open("wb") as output:
            output.truncate(MAX_REPORT_BYTES + 1)
        with self.assertRaises(ValueError):
            read_report(large)
        link = self.root / "link.ips"
        link.symlink_to(partial)
        with self.assertRaises(OSError):
            read_report(link)

    def test_no_signal_does_not_wait_or_claim_a_crash(self):
        uploads = self.root / "uploads"
        uploads.mkdir()
        (uploads / "plan.json").write_text(json.dumps({**self.upload, "signal": None}))
        result = collect(self.root, [self.root], 30)
        self.assertEqual(result["targets"], [])
        self.assertFalse((uploads / "crashes").exists())


if __name__ == "__main__":
    unittest.main()
