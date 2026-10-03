"""External rules reach the same engine from Python and the pure C ABI."""

from __future__ import annotations

import ctypes
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import unittest

from neverd_plugin import NeverDError, recover_bytecode, recover_bytecode_with_decoder
from neverd_plugin.ffi import HostAPI


class BytecodeIntegrationTests(unittest.TestCase):
    def load_api(self):
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(path.parent))
            self.addCleanup(directory.close)
        return HostAPI(ctypes.CDLL(str(path)))

    def test_external_profile_success_and_failure_without_loaded_session(self):
        api = self.load_api()
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

    def test_callback_pc_context_reentrancy_and_concurrent_recovery(self):
        api = self.load_api()
        layout = {"version": 1, "register_bytes": 8, "byte_order": "little"}

        def recover(key, output="check", optimize=False, nested=False):
            base = 0x100000003
            windows = []

            def decode(data, pc):
                windows.append((pc, len(data)))
                opcode = data[0] ^ ((pc + key) & 255)
                if opcode == 0x41:
                    if nested:
                        self.assertEqual(recover(key + 1).decoded_bytes, 4)
                    return {"size": 3, "operations": [{
                        "op": "COPY", "output": {"space": "reg", "size": 2, "value": {}},
                        "inputs": [{"space": "const", "size": 2,
                                    "value": {"addend": int.from_bytes(data[1:3], "little")}}],
                    }]}
                if opcode == 0xfe:
                    return {"size": 1, "operations": [{"op": "RETURN", "inputs": []}]}
                return {"error": "unknown contextual opcode"}

            code = bytes([0x41 ^ ((base + key) & 255), 0x21, 0x43,
                          0xfe ^ ((base + 3 + key) & 255), 0xff])
            result = recover_bytecode_with_decoder(
                code, layout, [{"entry": base, "end": base + 4, "name": "dynamic_python"}],
                decode, base=base, output=output, optimize=optimize, api=api,
            )
            self.assertEqual(windows, [(base, 4), (base + 3, 1)])
            self.assertEqual(result.decoded_instructions, 2)
            self.assertEqual(result.decoded_bytes, 4)
            self.assertEqual(result.input_bytes, 5)
            if output != "check":
                self.assertIn("dynamic_python", result.source)
            return result

        for output, optimize in (("check", False), ("highc", False),
                                 ("llvmc", False), ("llvmc", True)):
            recover(29, output, optimize, nested=True)
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(recover, range(8)))
        self.assertEqual(len(results), 8)

    def test_callback_error_stops_and_preserves_original_python_exception(self):
        api = self.load_api()
        layout = {"version": 1, "register_bytes": 8, "byte_order": "little"}
        functions = [{"entry": 0, "end": 2, "name": "failure"}]
        error = RuntimeError("context unavailable")
        calls = []

        def decode(data, pc):
            calls.append(pc)
            raise error

        with self.assertRaises(RuntimeError) as caught:
            recover_bytecode_with_decoder(b"ab", layout, functions, decode, api=api)
        self.assertIs(caught.exception, error)
        self.assertEqual(calls, [0])
        for recipe in ({"error": "unknown instruction"}, {"size": 0, "operations": []},
                       {"size": 1, "match": [], "operations": []}):
            with self.assertRaises(NeverDError):
                recover_bytecode_with_decoder(b"ab", layout, functions,
                                              lambda data, pc: recipe, api=api)


if __name__ == "__main__":
    unittest.main()
