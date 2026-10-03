from __future__ import annotations

import ctypes
import gc
import json
import unittest
import weakref

from neverd_plugin import NeverDError, recover_bytecode, recover_bytecode_with_decoder
from neverd_plugin import abi
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
        self.replies = []
        self.accept_reply = 1
        self.callback_ref = None
        self.neverd_bytecode_recover_json_v1 = Function(self.recover)
        self.neverd_bytecode_recover_decoder_json_v1 = Function(self.recover_decoder)
        self.neverd_free_string = Function(self.free)

    def recover(self, code, size, request, length):
        self.code = ctypes.string_at(code, size)
        self.request = json.loads(ctypes.string_at(request, length))
        return ctypes.addressof(self.buffer)

    def free(self, pointer):
        self.freed.append(ctypes.cast(pointer, ctypes.c_void_p).value)

    def recover_decoder(self, code, size, request, length, decoder, context):
        result = self.recover(code, size, request, length)
        self.callback_ref = weakref.ref(decoder)

        @abi.BytecodeInstructionSinkV1
        def sink(opaque, text, count):
            self.replies.append(json.loads(ctypes.string_at(text, count)))
            return self.accept_reply

        # Deliberately keep invoking even after an error: the wrapper must not
        # re-enter the Python decoder after its first exception.
        for offset in (0, 1):
            remaining = (ctypes.c_ubyte * (size - offset)).from_buffer_copy(
                self.code[offset:])
            decoder(context, remaining, size - offset,
                    self.request["base"] + offset, sink, None)
        return result


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

    def test_decoder_windows_context_and_result_ownership(self):
        library = Library(self.success())
        windows = []
        layout = {"version": 1, "register_bytes": 8, "byte_order": "little"}
        functions = [{"entry": 0x100000003, "end": 0x100000006, "name": "sample"}]

        def decode(data, pc):
            windows.append((data, pc))
            return {"size": 1, "operations": [{"op": "RETURN", "inputs": []}]}

        result = recover_bytecode_with_decoder(
            b"A\0B", layout, functions, decode, base=0x100000003,
            output="llvmc", optimize=True, api=HostAPI(library))
        self.assertEqual(windows, [(b"A\0B", 0x100000003), (b"\0B", 0x100000004)])
        self.assertEqual(library.request["layout"], layout)
        self.assertNotIn("profile", library.request)
        self.assertEqual(library.request["functions"], functions)
        self.assertEqual([reply["size"] for reply in library.replies], [1, 1])
        self.assertEqual(result.functions, 1)
        self.assertEqual(library.freed, [ctypes.addressof(library.buffer)])
        gc.collect()
        self.assertIsNone(library.callback_ref())

    def test_decoder_exceptions_rethrow_after_free_without_crossing_c(self):
        for error in (ValueError("bad instruction"), KeyboardInterrupt(), SystemExit(9)):
            with self.subTest(error=type(error).__name__):
                library = Library(dict(schemaVersion=1, ok=False, error="native error"))
                calls = []

                def decode(data, pc):
                    calls.append(pc)
                    raise error

                with self.assertRaises(type(error)) as raised:
                    recover_bytecode_with_decoder(b"abc", {}, [], decode,
                                                  api=HostAPI(library))
                self.assertIs(raised.exception, error)
                self.assertEqual(calls, [0])
                self.assertEqual(len(library.replies), 2)
                self.assertTrue(all("error" in reply for reply in library.replies))
                self.assertEqual(library.freed, [ctypes.addressof(library.buffer)])

    def test_invalid_python_replies_and_sink_rejection_fail_after_free(self):
        for value, error in (([], TypeError), ({"size": float("nan")}, ValueError),
                             ({"size": object()}, TypeError)):
            library = Library(self.success())
            with self.assertRaises(error):
                recover_bytecode_with_decoder(b"abc", {}, [], lambda data, pc: value,
                                              api=HostAPI(library))
            self.assertEqual(len(library.freed), 1)
        library = Library(self.success())
        library.accept_reply = 0
        with self.assertRaisesRegex(NeverDError, "rejected the reply"):
            recover_bytecode_with_decoder(b"abc", {}, [], lambda data, pc: {},
                                          api=HostAPI(library))
        self.assertEqual(len(library.freed), 1)
        with self.assertRaisesRegex(TypeError, "callable"):
            recover_bytecode_with_decoder(b"abc", {}, [], None)


if __name__ == "__main__":
    unittest.main()
