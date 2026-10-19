"""Public module CLI/C ABI equivalence over file-analysis fixtures only."""

import argparse
import ctypes
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

OPTIONS = None


class ModuleCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if OPTIONS is None:
            raise unittest.SkipTest("configured CLI/library paths are required")
        cls.cli = pathlib.Path(OPTIONS.neverd).resolve()
        cls.library_path = pathlib.Path(OPTIONS.library).resolve()
        if sys.platform == "win32":
            import os
            cls.dll_directory = os.add_dll_directory(str(cls.library_path.parent))
        cls.library = ctypes.CDLL(str(cls.library_path))
        cls.library.neverd_module_set_from_file.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        cls.library.neverd_module_set_from_file.restype = ctypes.c_uint64
        cls.library.neverd_module_set_prepare_eager.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_void_p)]
        cls.library.neverd_module_set_prepare_eager.restype = ctypes.c_int
        cls.library.neverd_module_set_inspect_json.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_void_p)]
        cls.library.neverd_module_set_inspect_json.restype = ctypes.c_void_p
        cls.library.neverd_module_set_lookup_symbol_json.argtypes = [ctypes.c_uint64, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        cls.library.neverd_module_set_lookup_symbol_json.restype = ctypes.c_void_p
        cls.library.neverd_module_set_destroy.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_void_p)]
        cls.library.neverd_module_set_destroy.restype = ctypes.c_int
        cls.library.neverd_free_string.argtypes = [ctypes.c_void_p]
        cls.library.neverd_free_string.restype = None

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="neverd-module-cli-")
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        ident = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
        header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, 0x401000,
                             64, 0, 0, 64, 56, 1, 64, 0, 0)
        segment = struct.pack("<IIQQQQQQ", 1, 5, 4096, 0x401000, 0, 6, 6, 4096)
        self.image = self.root / "probe.elf"
        self.image.write_bytes((header + segment).ljust(4096, b"\0") + b"\xb8\x2a\0\0\0\xc3")
        self.map = self.root / "probe.map"
        self.map.write_text(
            "VMA LMA Size Align Out In Symbol\n"
            "0000000000401000 0000000000401000 00000006 16 .text\n"
            "0000000000401000 0000000000401000 00000006 1 probe.o:(.text)\n"
            "0000000000401000 0000000000401000 00000006 1 mapped_probe\n", encoding="utf-8")

    def config(self, data, name="modules.json"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
        return path

    def run_cli(self, action, path, *arguments):
        return subprocess.run([str(self.cli), "modules", action, str(path), *arguments],
                              capture_output=True, text=True, encoding="utf-8", timeout=20)

    def take(self, pointer):
        if not pointer:
            return ""
        try:
            return ctypes.string_at(pointer).decode("utf-8")
        finally:
            self.library.neverd_free_string(pointer)

    def from_file(self, path):
        error = ctypes.c_void_p()
        handle = self.library.neverd_module_set_from_file(str(path).encode("utf-8"), ctypes.byref(error))
        self.assertTrue(handle, self.take(error.value))
        self.assertIsNone(error.value)
        self.addCleanup(self.library.neverd_module_set_destroy, handle, None)
        return handle

    def api_report(self, handle):
        error = ctypes.c_void_p()
        report = self.library.neverd_module_set_inspect_json(handle, ctypes.byref(error))
        self.assertTrue(report, self.take(error.value))
        return json.loads(self.take(report))

    def test_inspect_does_not_open_missing_eager_resources(self):
        path = self.config({"modules": [{"id": "later", "image": "not-yet.sys"}]})
        result = self.run_cli("inspect", path)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertFalse(report["modules"][0]["prepared"])
        self.assertEqual(report, self.api_report(self.from_file(path)))

    def test_validate_prepares_eager_only_and_matches_the_c_api(self):
        path = self.config({"module_defaults": {"load": "lazy"}, "modules": [
            {"id": "eager", "image": "probe.elf", "load": "eager"},
            {"id": "lazy", "image": "not-yet.sys"}]})
        result = self.run_cli("validate", path)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report["modules"][0]["prepared"])
        self.assertFalse(report["modules"][1]["prepared"])
        self.assertEqual(report["modules"][0]["image_sha256"], hashlib.sha256(self.image.read_bytes()).hexdigest())
        handle = self.from_file(path)
        error = ctypes.c_void_p()
        self.assertEqual(self.library.neverd_module_set_prepare_eager(handle, ctypes.byref(error)), 1, self.take(error.value))
        self.assertEqual(report, self.api_report(handle))

    def test_lookup_loads_lazy_symbols_and_reports_original_addresses(self):
        path = self.config({"modules": [{"id": "a", "image": "probe.elf", "load": "lazy",
                                          "debug": {"kind": "map", "path": "probe.map"}}]})
        result = self.run_cli("lookup", path, "--module=a", "--symbol=mapped_probe")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["symbol"]["address"], "0x401000")
        handle = self.from_file(path)
        error = ctypes.c_void_p()
        pointer = self.library.neverd_module_set_lookup_symbol_json(handle, b"a", b"mapped_probe", ctypes.byref(error))
        self.assertTrue(pointer, self.take(error.value))
        self.assertEqual(report, json.loads(self.take(pointer)))

    def test_missing_symbol_is_a_query_result_with_exit_two(self):
        path = self.config({"modules": [{"id": "a", "image": "probe.elf"}]})
        result = self.run_cli("lookup", path, "--module=a", "--symbol=absent")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(json.loads(result.stdout)["found"])

    def test_explicit_origin_overrides_the_config_directory(self):
        path = self.config({"base_directory": str(self.root), "modules": [
            {"id": "a", "image": "probe.elf"}]}, "configs/modules.json")
        result = self.run_cli("validate", path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)["modules"][0]["prepared"])

    def test_configuration_errors_match_the_engine_diagnostic(self):
        path = self.config({"unrecognized": True})
        result = self.run_cli("inspect", path)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        error = ctypes.c_void_p()
        self.assertEqual(self.library.neverd_module_set_from_file(str(path).encode("utf-8"), ctypes.byref(error)), 0)
        message = self.take(error.value)
        self.assertIn(message, result.stderr)

    def test_failed_validation_does_not_publish_a_success_report(self):
        path = self.config({"modules": [{"id": "a", "image": "missing.sys"}]})
        result = self.run_cli("validate", path)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertTrue(result.stderr)

    def test_unknown_action_and_inappropriate_arguments_fail_before_file_io(self):
        path = self.root / "missing.json"
        for action, arguments in [("invalid", []), ("lookup", []), ("inspect", ["--module=a"])]:
            result = self.run_cli(action, path, *arguments)
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("regular file", result.stderr)

    def test_unicode_configuration_names_use_utf8(self):
        path = self.config({"modules": [{"id": "a", "image": "probe.elf"}]}, "模块.json")
        result = self.run_cli("validate", path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)["modules"][0]["prepared"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--library", required=True)
    OPTIONS, rest = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
