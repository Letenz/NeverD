"""External rules reach the same engine from Python and the pure C ABI."""

from __future__ import annotations

import ctypes
import copy
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
                self.assertFalse(result.with_context)
                if output != "check":
                    self.assertIn("python_return", result.source)
        with self.assertRaisesRegex(NeverDError, "unknown|unsupported|no encoding"):
            recover_bytecode(b"\0", profile, functions, api=api)
        with self.assertRaises(NeverDError):
            recover_bytecode(b"M", profile, functions, optimize=True, api=api)

    def test_source_context_abi_is_shared_by_profile_and_decoder(self):
        api = self.load_api()
        layout = {"version": 1, "register_bytes": 8, "byte_order": "little"}
        recipes = {
            0xd2: {"size": 1, "operations": [{"op": "CALL", "inputs": [
                {"space": "const", "size": 8, "value": {"addend": 8192}},
            ]}]},
            0xfe: {"size": 1, "operations": [{"op": "RETURN", "inputs": []}]},
        }
        profile = dict(layout, encodings=[
            dict(recipe, match=[{"offset": 0, "value": opcode}])
            for opcode, recipe in recipes.items()
        ])
        functions = [{"entry": 0, "end": 2, "name": "contextual_source"}]
        bindings = [{"address": 8192, "name": "runtime_service"}]
        for output, optimize in (("check", False), ("highc", False),
                                 ("llvmc", False), ("llvmc", True)):
            with self.subTest(output=output, optimize=optimize):
                options = dict(output=output, optimize=optimize, with_context=True,
                               bindings=bindings, api=api)
                static = recover_bytecode(b"\xd2\xfe", profile, functions, **options)
                dynamic = recover_bytecode_with_decoder(
                    b"\xd2\xfe", layout, functions, lambda data, pc: recipes[data[0]],
                    **options)
                self.assertEqual(static, dynamic)
                self.assertTrue(static.with_context)
                self.assertEqual(static.decoded_bytes, 2)
                if output != "check":
                    self.assertIn("runtime_service", static.source)
                    self.assertIn("context", static.source)

    def test_state_source_contract_reconstructs_only_proven_calls(self):
        api = self.load_api()
        layout = {"version": 1, "register_bytes": 48, "byte_order": "little"}
        def operand(space, size, value):
            return {"space": space, "size": size, "value": {"addend": value}}
        copies = [{"op": "COPY", "output": operand("reg", 8, offset),
                   "inputs": [operand("const", 8, value)]}
                  for offset, value in ((8, 7), (16, 32), (24, 4), (40, 99))]
        recipes = {
            0x41: {"size": 1, "operations": copies + [{"op": "CALL", "inputs": [
                operand("const", 8, 8192)]}]},
            0xfe: {"size": 1, "operations": [{"op": "RETURN", "inputs": []}]},
        }
        profile = dict(layout, encodings=[dict(recipe, match=[{"offset": 0, "value": op}])
                                         for op, recipe in recipes.items()])
        slot = lambda offset: {"offset": offset, "bytes": 8}
        contract = {"schemaVersion": 1, "state_bytes": 48, "isolated_bank": True,
                    "zero_status_gateways": True, "gateways": [{
                        "name": "runtime_service", "selector": slot(8),
                        "arguments": [slot(40)], "result_offset": slot(16),
                        "result_bytes": slot(24), "writes": [], "imports": [{
                            "selector": 7, "name": "example_query",
                            "parameters": ["u64"], "result": "i32"}]}]}
        options = dict(bindings=[{"address": 8192, "name": "runtime_service"}],
                       output="llvmc", optimize=True, source_contracts=contract, api=api)
        functions = [{"entry": 0, "end": 2, "name": "source_query"}]
        static = recover_bytecode(b"A\xfe", profile, functions, **options)
        dynamic = recover_bytecode_with_decoder(b"A\xfe", layout, functions,
                                                lambda data, pc: recipes[data[0]], **options)
        self.assertEqual(static, dynamic)
        self.assertEqual((static.reconstructed_calls, static.unresolved_contract_calls), (1, 0))
        self.assertIn("example_query((uint64_t)(99))", static.source)
        self.assertNotIn("__neverd_state_probe", static.source)
        for change in ({"isolated_bank": False}, {"state_bytes": 47}, {"unknown": 1}):
            with self.assertRaisesRegex(NeverDError, "source_contracts"):
                recover_bytecode(b"A\xfe", profile, functions,
                                 **dict(options, source_contracts=dict(contract, **change)))
        for change in ({"output": "highc"}, {"optimize": False}, {"observations": {}}):
            with self.assertRaisesRegex(NeverDError, "source_contracts"):
                recover_bytecode(b"A\xfe", profile, functions, **dict(options, **change))
        contract["gateways"][0]["imports"][0]["selector"] = 8
        unknown = recover_bytecode(b"A\xfe", profile, functions, **options)
        self.assertEqual((unknown.reconstructed_calls, unknown.unresolved_contract_calls), (0, 1))
        self.assertIn("runtime_service(state)", unknown.source)

    def test_pointer_source_candidates_keep_address_and_count_guards(self):
        api = self.load_api()
        layout = {"version": 1, "register_bytes": 80, "byte_order": "little"}
        def operand(space, value):
            return {"space": space, "size": 8, "value": {"addend": value}}
        operations = [
            {"op": "INT_ADD", "output": operand("reg", 8),
             "inputs": [operand("reg", 0), operand("const", 5)]},
            {"op": "LOAD", "output": operand("reg", 16),
             "inputs": [operand("reg", 8)]},
            {"op": "COPY", "output": operand("reg", 32),
             "inputs": [operand("reg", 16)]},
        ]
        operations += [{"op": "COPY", "output": operand("reg", offset),
                        "inputs": [operand("const", value)]}
                       for offset, value in ((40, 24), (48, 4), (64, 999))]
        operations.append({"op": "CALL", "inputs": [operand("const", 8192)]})
        recipes = {65: {"size": 1, "operations": operations},
                   254: {"size": 1, "operations": [{"op": "RETURN", "inputs": []}]}}
        profile = dict(layout, encodings=[dict(recipe, match=[{"offset": 0, "value": op}])
                                         for op, recipe in recipes.items()])
        slot = lambda offset: {"offset": offset, "bytes": 8}
        contract = {"schemaVersion": 1, "state_bytes": 80, "isolated_bank": True,
                    "zero_status_gateways": True,
                    "images": [{"base": slot(0), "readable": True, "bytes": "00" * 32}],
                    "gateways": [{"name": "runtime_service", "function_pointer": slot(32),
                                  "argument_count": slot(72), "arguments": [slot(64)],
                                  "result_offset": slot(40), "result_bytes": slot(48),
                                  "writes": [{"slot": slot(72), "value": 0}],
                                  "imports": [{"pointer_source": {"image": 0, "offset": 5},
                                               "name": "_synthetic_query",
                                               "parameters": ["i32"], "result": "i32"}]}]}
        options = dict(bindings=[{"address": 8192, "name": "runtime_service"}],
                       output="llvmc", optimize=True, source_contracts=contract, api=api)
        functions = [{"entry": 0, "end": 2, "name": "source_pointer"}]
        static = recover_bytecode(b"A\xfe", profile, functions, **options)
        dynamic = recover_bytecode_with_decoder(b"A\xfe", layout, functions,
                                                lambda data, pc: recipes[data[0]], **options)
        self.assertEqual(static, dynamic)
        self.assertEqual((static.reconstructed_calls, static.guarded_contract_calls,
                          static.unresolved_contract_calls), (1, 1, 0))
        self.assertIn("_synthetic_query((int32_t)(999))", static.source)
        self.assertIn("runtime_service(state)", static.source)
        variadic = copy.deepcopy(contract)
        variadic["gateways"][0]["arguments"].append(slot(64))
        variadic_import = variadic["gateways"][0]["imports"][0]
        variadic_import["parameters"].append("u64")
        variadic_import["variadic_fixed"] = 1
        variadic_result = recover_bytecode(b"A\xfe", profile, functions,
                                          **dict(options, source_contracts=variadic))
        self.assertIn("_synthetic_query(int32_t, ...)", variadic_result.source)
        self.assertIn("(uint64_t)(999)", variadic_result.source)
        for path, value in [(("gateways", 0, "selector"), slot(32)),
                            (("gateways", 0, "function_pointer", "bytes"), 4),
                            (("gateways", 0, "imports", 0, "selector"), 0),
                            (("gateways", 0, "imports", 0, "pointer_source"), None),
                            (("gateways", 0, "imports", 0, "pointer_source", "offset"), 25),
                            (("gateways", 0, "imports", 0, "pointer_source", "image"), 1),
                            (("gateways", 0, "imports", 0, "variadic_fixed"), 0),
                            (("gateways", 0, "imports", 0, "variadic_fixed"), 2),
                            (("gateways", 0, "imports", 0, "variadic_fixed"), 1.5)]:
            bad = copy.deepcopy(contract)
            parent = bad
            for key in path[:-1]:
                parent = parent[key]
            parent[path[-1]] = value
            with self.subTest(path=path), self.assertRaises(NeverDError):
                recover_bytecode(b"A\xfe", profile, functions,
                                 **dict(options, source_contracts=bad))
        contract["gateways"][0]["imports"][0]["pointer_source"]["offset"] = 6
        unknown = recover_bytecode(b"A\xfe", profile, functions, **options)
        self.assertEqual((unknown.reconstructed_calls, unknown.unresolved_contract_calls), (0, 1))

    def test_source_string_candidates_use_shared_native_contracts(self):
        api = self.load_api()
        layout = {"version": 1, "register_bytes": 80, "byte_order": "little"}
        def operand(space, value):
            return {"space": space, "size": 8, "value": {"addend": value}}
        operations = [{"op": "INT_ADD", "output": operand("reg", 16),
                       "inputs": [operand("reg", 0), operand("const", 3)]}]
        operations += [{"op": "COPY", "output": operand("reg", offset),
                        "inputs": [operand("const", value)]}
                       for offset, value in ((8, 16), (32, 7), (40, 24), (48, 8), (64, 90))]
        operations.append({"op": "CALL", "inputs": [operand("const", 8192)]})
        recipes = {65: {"size": 1, "operations": operations},
                   254: {"size": 1, "operations": [{"op": "RETURN", "inputs": []}]}}
        profile = dict(layout, encodings=[dict(recipe, match=[{"offset": 0, "value": op}])
                                         for op, recipe in recipes.items()])
        slot = lambda offset: {"offset": offset, "bytes": 8}
        image = b"abc" + bytes(c ^ 90 for c in b"example.dll\0")
        contract = {"schemaVersion": 1, "state_bytes": 80, "isolated_bank": True,
                    "zero_status_gateways": True,
                    "images": [{"base": slot(0), "readable": True, "bytes": image.hex()}],
                    "gateways": [{"name": "runtime_service", "selector": slot(32),
                                  "arguments": [slot(8), slot(24)], "indirect_arguments": [0],
                                  "result_offset": slot(40), "result_bytes": slot(48),
                                  "writes": [{"slot": slot(56), "value": 0}],
                                  "strings": [{"argument": 0, "image": 0, "by_value": True,
                                               "mode": slot(56), "xor_key": slot(64)}],
                                  "excluded_arguments": [{"argument": 1, "value": 2**64 - 1}],
                                  "imports": [{"selector": 7, "name": "lookup_item",
                                               "parameters": ["pointer", "u64"], "result": "pointer"}]}]}
        options = dict(bindings=[{"address": 8192, "name": "runtime_service"}],
                       output="llvmc", optimize=True, source_contracts=contract, api=api)
        functions = [{"entry": 0, "end": 2, "name": "source_lookup"}]
        static = recover_bytecode(b"A\xfe", profile, functions, **options)
        dynamic = recover_bytecode_with_decoder(b"A\xfe", layout, functions,
                                                lambda data, pc: recipes[data[0]], **options)
        self.assertEqual(static, dynamic)
        self.assertEqual((static.reconstructed_calls, static.guarded_contract_calls,
                          static.unresolved_contract_calls), (1, 1, 0))
        self.assertIn('"example.dll"', static.source)
        self.assertIn("lookup_item(", static.source)
        self.assertIn("runtime_service(state)", static.source)
        for path, value in [(("images",), {}), (("images", 0, "bytes"), "not-hex"),
                            (("images", 0, "readable"), False),
                            (("gateways", 0, "strings"), {}),
                            (("gateways", 0, "strings", 0, "by_value"), False),
                            (("gateways", 0, "strings", 0, "image"), 1),
                            (("gateways", 0, "indirect_arguments"), [2]),
                            (("gateways", 0, "excluded_arguments"), [{"argument": 0}])]:
            bad = copy.deepcopy(contract)
            parent = bad
            for key in path[:-1]:
                parent = parent[key]
            parent[path[-1]] = value
            with self.assertRaises(NeverDError):
                recover_bytecode(b"A\xfe", profile, functions,
                                 **dict(options, source_contracts=bad))

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
