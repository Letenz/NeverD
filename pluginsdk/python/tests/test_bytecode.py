from __future__ import annotations

import ctypes
import json
import unittest

from neverd_plugin import NeverDError, recover_bytecode
from neverd_plugin.ffi import HostAPI


class Function:
    def __init__(self, implementation):
        self.implementation = implementation

    def __call__(self, *args):
        return self.implementation(*args)


class Library:
    def __init__(self, response):
        self.buffer = ctypes.create_string_buffer(json.dumps(response).encode())
        self.freed = []
        self.request = None
        self.code = None
        self.neverd_bytecode_recover_json_v1 = Function(self.recover)
        self.neverd_free_string = Function(self.free)

    def recover(self, code, size, request, length):
        self.code = ctypes.string_at(code, size)
        self.request = json.loads(ctypes.string_at(request, length))
        return ctypes.addressof(self.buffer)

    def free(self, pointer):
        self.freed.append(ctypes.cast(pointer, ctypes.c_void_p).value)


class BytecodeTests(unittest.TestCase):
    def success(self):
        return dict(schemaVersion=1, ok=True, functions=1, blocks=2,
                    decoded_instructions=2, decoded_bytes=2, input_bytes=3,
                    scope="state-c", source="uint64_t sample(void *state);")

    def test_forwards_rules_and_binary_lengths_and_frees_result(self):
        library = Library(self.success())
        profile = {"version": 1, "register_bytes": 8}
        functions = [{"entry": 0, "end": 3, "name": "sample"}]
        result = recover_bytecode(b"\x41\0\xff", profile, functions,
                                  output="llvmc", optimize=True,
                                  api=HostAPI(library))
        self.assertEqual(library.code, b"\x41\0\xff")
        self.assertEqual(library.request["profile"], profile)
        self.assertEqual(library.request["functions"], functions)
        self.assertTrue(library.request["optimize"])
        self.assertEqual(library.request["output"], "llvmc")
        self.assertEqual(result.decoded_bytes, 2)
        self.assertEqual(result.input_bytes, 3)
        self.assertEqual(library.freed, [ctypes.addressof(library.buffer)])

    def test_failure_and_malformed_success_always_release_owned_response(self):
        reports = [dict(schemaVersion=1, ok=False, error="unsupported instruction")]
        for key, value in (("schemaVersion", 2), ("decoded_bytes", 4),
                           ("input_bytes", 4), ("scope", "original-c"),
                           ("source", None), ("blocks", True)):
            bad = self.success()
            bad[key] = value
            reports.append(bad)
        for response in reports:
            with self.subTest(response=response):
                library = Library(response)
                with self.assertRaises(NeverDError):
                    recover_bytecode(b"abc", {}, [], api=HostAPI(library))
                self.assertEqual(library.freed, [ctypes.addressof(library.buffer)])

    def test_nonfinite_rules_are_not_serialized_to_nonstandard_json(self):
        with self.assertRaises(ValueError):
            recover_bytecode(b"a", {"value": float("nan")}, [])


if __name__ == "__main__":
    unittest.main()
