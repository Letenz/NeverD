"""External rules reach the same engine from Python and the pure C ABI."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
import unittest

from neverd_plugin import NeverDError, recover_bytecode
from neverd_plugin.ffi import HostAPI


class BytecodeIntegrationTests(unittest.TestCase):
    def test_external_profile_success_and_failure_without_loaded_session(self):
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(path.parent))
            self.addCleanup(directory.close)
        api = HostAPI(ctypes.CDLL(str(path)))
        profile = {
            "version": 1, "register_bytes": 8, "byte_order": "little",
            "encodings": [{"size": 1, "match": [{"offset": 0, "value": 77}],
                           "operations": [{"op": "RETURN", "inputs": []}]}],
        }
        functions = [{"entry": 0, "end": 1, "name": "python_return"}]
        for output, optimize in (("check", False), ("highc", False),
                                 ("llvmc", False), ("llvmc", True)):
            with self.subTest(output=output, optimize=optimize):
                result = recover_bytecode(b"M\0", profile, functions,
                                          output=output, optimize=optimize, api=api)
                self.assertEqual(result.decoded_bytes, 1)
                self.assertEqual(result.input_bytes, 2)
                self.assertEqual(result.decoded_instructions, 1)
                self.assertEqual(result.scope, "cfg" if output == "check" else "state-c")
                if output != "check":
                    self.assertIn("python_return", result.source)
        with self.assertRaisesRegex(NeverDError, "unknown|unsupported|no encoding"):
            recover_bytecode(b"\0", profile, functions, api=api)
        with self.assertRaises(NeverDError):
            recover_bytecode(b"M", profile, functions, optimize=True, api=api)


if __name__ == "__main__":
    unittest.main()
