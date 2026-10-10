from pathlib import Path
import copy
import hashlib
import struct
import tempfile
import unittest

from scripts import check_windows_registration_realigned as runner
from scripts import replay_windows_registration_realigned as replay


def archive_member(name=b"__CxxThrowException@8", kind=4, machine=0x14c):
    names = name + b"\0vcruntime140.dll\0"
    data = struct.pack("<HHHHIIHH", 0, 0xffff, 0, machine,
                       0, len(names), 0, kind) + names
    header = (f"import/         {0:<12}{0:<6}{0:<6}{0:<8}"
              f"{len(data):<10}`\n").encode()
    return header + data + (b"\n" if len(data) & 1 else b"")


class RealignedCallbackEvidenceTests(unittest.TestCase):
    def capture(self, root):
        data = bytearray(0x600)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3c, 0x80)
        data[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<HH", data, 0x84, 0x14c, 1)
        struct.pack_into("<H", data, 0x94, 224)
        optional = 0x98
        struct.pack_into("<H", data, optional, 0x10b)
        struct.pack_into("<I", data, optional + 28, 0x400000)
        struct.pack_into("<II", data, optional + 96 + 5 * 8, 0x1200, 12)
        section = optional + 224
        data[section:section + 8] = b".text\0\0\0"
        struct.pack_into("<4I", data, section + 8, 0x400, 0x1000, 0x400, 0x200)
        struct.pack_into("<I", data, 0x210, 0x401000)
        struct.pack_into("<IIHH", data, 0x400, 0x1000, 12, 0x3010, 0)
        (root / "frame.obj").write_bytes(b"test object")
        capture = {"schema": 1, "evidence": "generated-realigned-callback-analysis",
                   "source_sha256": hashlib.sha256(runner.SOURCE.read_bytes()).hexdigest(),
                   "object_sha256": hashlib.sha256(b"test object").hexdigest(),
                   "images": []}
        for name, expected in replay.IMAGES.items():
            image = bytearray(data)
            image[0x218] = expected
            if "rebased" in name:
                image = runner.PE32(image).rebase(0x18000000)
            path = root / name
            path.write_bytes(image)
            record = {"image": name, "expected_exit": expected,
                      "sha256": runner.image_digest(path)}
            if not expected:
                record["analysis_tests"] = 1
                (root / (Path(name).stem + ".xml")).write_text('<testsuites tests="1"/>')
            capture["images"].append(record)
        return capture

    def test_replay_authenticates_bases_files_and_the_control_matrix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            replay.validate_capture(root, capture)
            for mutation in range(7):
                changed = copy.deepcopy(capture)
                if mutation == 0:
                    changed["images"].pop()
                if mutation == 1:
                    changed["images"][-1] = changed["images"][0]
                if mutation == 2:
                    changed["images"][0]["expected_exit"] = 1
                if mutation == 3:
                    changed["images"][0]["sha256"] = "stale"
                if mutation == 4:
                    changed["object_sha256"] = "stale"
                if mutation == 5:
                    changed["source_sha256"] = "stale"
                if mutation == 6:
                    changed["images"][0]["image"] = "../probe.exe"
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            path = root / "probe-rebased.exe"
            data = bytearray(path.read_bytes())
            data[0x220] ^= 1
            path.write_bytes(data)
            capture["images"][1]["sha256"] = runner.image_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_replay_cannot_admit_skipped_analysis(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            (root / "probe.xml").write_text(
                '<testsuites tests="1"><testcase><skipped/></testcase></testsuites>')
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_stdcall_import_preserves_symbols_and_member_offsets(self):
        original = b"!<arch>\n" + archive_member() + archive_member(b"___CxxFrameHandler3")
        expected = bytearray(original)
        struct.pack_into("<H", expected, 8 + 60 + 18, 12)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.lib"
            path.write_bytes(original)
            runner.undecorate_throw_import(path)
            self.assertEqual(path.read_bytes(), bytes(expected))
            runner.undecorate_throw_import(path)
            self.assertEqual(path.read_bytes(), bytes(expected))

    def test_malformed_or_missing_import_does_not_write_partial_results(self):
        cases = [b"", b"!<arch>\n", b"!<arch>\n" + archive_member()[:-1],
                 b"!<arch>\n" + archive_member() * 2,
                 b"!<arch>\n" + archive_member(b"__CxxThrowException@4"),
                 b"!<arch>\n" + archive_member(kind=5),
                 b"!<arch>\n" + archive_member(machine=0x8664),
                 b"!<arch>\n" + archive_member() + b"bad member"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.lib"
            for original in cases:
                with self.subTest(original=original):
                    path.write_bytes(original)
                    with self.assertRaises(ValueError):
                        runner.undecorate_throw_import(path)
                    self.assertEqual(path.read_bytes(), original)

    def test_analysis_requires_executed_passing_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.xml"
            path.write_text('<testsuites tests="1" failures="0"><testcase/></testsuites>')
            self.assertEqual(runner.require_test_result(path), 1)
            for xml in ('<testsuites tests="0"/>', '<testsuites tests="-1"/>',
                        '<testsuites tests="1" failures="1"/>',
                        '<testsuites tests="1" disabled="1"/>',
                        '<testsuites tests="1"><testcase><skipped/></testcase></testsuites>'):
                path.write_text(xml)
                with self.assertRaises(ValueError):
                    runner.require_test_result(path)


if __name__ == "__main__":
    unittest.main()
