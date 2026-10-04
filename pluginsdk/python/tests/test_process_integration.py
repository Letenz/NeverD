"""Run ELF and Mach-O processes through Python and the built shared engine.

Set NEVERD_TEST_LIBNEVERD and NEVERD_TEST_PROCESS_FIXTURES explicitly. These
tests use the generated freestanding ELF corpus, without invoking host programs.
NEVERD_TEST_DARWIN_FIXTURES enables the macOS/iOS Mach-O corpus.
"""
from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest


class ProcessIntegrationTests(unittest.TestCase):
    def test_android_local_memory_input_exceeds_json_limit(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android native fixtures are not configured")
        from neverd_plugin import NeverDError, Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        image = str(Path(fixtures) / "relr.so")
        with TemporaryDirectory(prefix="neverd-memory-") as directory:
            path = Path(directory) / "input-数据.bin"
            source = bytes((i * 37 + (i >> 8)) % 256 for i in range(65539))
            path.write_bytes(source)
            expected = 14695981039346656037
            whole = len(source) // 8 * 8
            for i in range(0, whole, 8):
                word = int.from_bytes(source[i:i + 8], "little")
                expected = ((expected ^ word) * 1099511628211) % (1 << 64)
            for b in source[whole:]:
                expected = ((expected ^ b) * 1099511628211) % (1 << 64)
            region = {"address": 0x20000000, "size": 18 * 4096, "path": str(path)}
            options = {"backend": "unicorn", "instruction_limit": 1000000,
                       "timeout_microseconds": 20000000, "android": {
                "entry_symbol": "inspect_memory_input",
                "arguments": [0x20000000, len(source)], "memory": [region],
                "read_memory": [{"address": 0x20000000, "size": 18 * 4096}],
            }}
            request = json.dumps(options)
            self.assertLess(len(request), 65536)
            result = session.emulate_process(image, "android-aarch64-api28-v1", request)
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["return_value"], format(expected, "x"))
            actual = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
            mutated = bytes([source[0] ^ 255]) + source[1:] + bytes([165])
            self.assertEqual(actual, mutated.ljust(region["size"], b"\0"))
            self.assertEqual(path.read_bytes(), source)
            region["size"] = 4096
            with self.assertRaisesRegex(NeverDError, "file exceeds its region"):
                session.emulate_process(image, "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(path.read_bytes(), source)

    def test_explicit_clocks_share_values_and_dynamic_api_names(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android native fixtures are not configured")
        from neverd_plugin import Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        options = {"backend": "unicorn", "linux_time": {"clocks": [
            {"id": 0, "seconds": "4294967297", "nanoseconds": 987654321},
            {"id": 1, "seconds": 123, "nanoseconds": 456789}]}, "android": {
                "entry_symbol": "time_dynamic", "initialize": False,
                "arguments": [0x20000000, 0],
                "memory": [{"address": 0x20000000, "size": 4096}],
                "read_memory": [{"address": 0x20000000, "size": 40}],
                "libraries": {"libclock-model.so": ["time", "clock_gettime", "gettimeofday"]}}}
        path = str(Path(fixtures) / "time-O2-relr.so")
        result = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(result["return_value"], "0")
        calls = result["android"]["native_calls"]
        for name in ("time", "clock_gettime", "gettimeofday"):
            lookup = next(c for c in calls if c["name"] == "dlsym" and c["symbol"] == name)
            call = next(c for c in calls if c["name"] == name)
            self.assertEqual(call["pc"], lookup["result"])
            self.assertEqual(call["library"], "libclock-model.so")
        values = [4294967297, 123, 456789, 4294967297, 987654]
        expected = b"".join(v.to_bytes(8, "little") for v in values)
        self.assertEqual(bytes.fromhex(result["android"]["memory"][0]["bytes_hex"]), expected)
        del options["linux_time"]
        result = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "unsupported_service")
        self.assertIsNone(result["android"]["native_calls"][-1]["result"])
        self.assertIn("no explicit linux_time input", result["diagnostic"])

    def test_darwin_profiles_preserve_bsd_errors_and_platform_identity(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_DARWIN_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Darwin fixtures are not configured")
        from neverd_plugin import NeverDError, Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        options = json.dumps({"backend": "unicorn", "arguments": ["guest", "normal", "argument"],
                              "environment": ["MODE=test"]})
        for profile, architectures in (("macos", ("arm64", "x86_64")),
                                       ("ios", ("arm64",)),
                                       ("ios-simulator", ("arm64", "x86_64"))):
            for architecture in architectures:
                with self.subTest(profile=profile, architecture=architecture):
                    path = str((Path(fixtures) / f"{profile}-{architecture}").resolve(strict=True))
                    result = session.emulate_process(path, f"{profile}-macho64-v1", options)
                    self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                    self.assertEqual(result["profile"], f"{profile}-macho64-v1")
                    self.assertEqual(result["exit_status"], 37)
                    self.assertEqual(bytes.fromhex(result["stdout_hex"]), b"darwin\x00\xff\n")
                    self.assertEqual([e["error"] for e in result["services"]], [True, False, True, False])
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                    wrong = "macos-macho64-v1" if profile == "ios" else "ios-macho64-v1"
                    with self.assertRaises(NeverDError):
                        session.emulate_process(path, wrong, options)

    def test_both_architectures_execute_and_retain_binary_output(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_PROCESS_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and ELF process fixtures are not configured")
        from neverd_plugin import NeverDError, Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        # Standalone test owns the actual native session. Only capsule/address
        # adaptation is supplied here; every operation calls the built library.
        bridge = SimpleNamespace(session_address=lambda _: address)
        session = Session(handle, _native=bridge, _host=host)
        options = json.dumps({"backend": "unicorn", "arguments": ["fixture", "normal"],
                              "environment": ["NEVERD_GUEST=explicit"]})
        for architecture in ("X64", "AArch64"):
            with self.subTest(architecture=architecture):
                path = str((Path(fixtures) / (architecture + ".elf")).resolve(strict=True))
                result = session.emulate_process(path, "linux-elf64-v1", options)
                self.assertEqual(result["stop_reason"], "exited", result["diagnostic"])
                self.assertEqual(result["exit_status"], 37)
                self.assertEqual(bytes.fromhex(result["stdout_hex"]), b"linux process ok\n")
                self.assertEqual(bytes.fromhex(result["stderr_hex"]), bytes([0, 255, 127]))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
                with self.assertRaises(NeverDError):
                    session.emulate_process(path, "linux-elf64-v1", '{"instruction_limit":0}')

    def test_android_native_call_and_trace(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android native fixtures are not configured")
        from neverd_plugin import Session
        from neverd_plugin.ffi import HostAPI

        library_path = Path(library).resolve(strict=True)
        if hasattr(os, "add_dll_directory"):
            directory = os.add_dll_directory(str(library_path.parent))
            self.addCleanup(directory.close)
        host = HostAPI(ctypes.CDLL(str(library_path)))
        address = int(host.call("neverd_session_create") or 0)
        self.assertGreater(address, 0)
        handle = ctypes.c_void_p(address)
        self.addCleanup(host.call, "neverd_session_destroy", handle)
        session = Session(handle, _native=SimpleNamespace(session_address=lambda _: address), _host=host)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "add_arguments", "arguments": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
            "trace_limit": 1024,
        }})
        result = session.emulate_process(str(Path(fixtures) / "relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 402)
        self.assertEqual(len(result["android"]["trace"]), result["instructions"])
        self.assertFalse(result["android"]["trace_truncated"])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "dynamic_lookup",
            "libraries": {"libfixture.so": ["strlen"]},
        }})
        result = session.emulate_process(str(Path(fixtures) / "relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 4)
        lookup = next(e for e in result["android"]["native_calls"] if e["name"] == "dlsym")
        call = next(e for e in result["android"]["native_calls"] if e["name"] == "strlen")
        self.assertEqual(lookup["symbol"], "strlen")
        self.assertEqual(lookup["library"], "libfixture.so")
        self.assertEqual(call["library"], "libfixture.so")
        self.assertEqual(call["pc"], lookup["result"])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        names = ("getuid", "geteuid", "getgid", "getegid")
        for scope, stop, expected in ((["libfixture.so"], "returned", 6),
                                      ([], "returned", 100),
                                      (None, "unsupported_service", None)):
            native = {"entry_symbol": "default_call",
                      "libraries": {"libfixture.so": ["strlen"]}}
            if scope is not None:
                native["default_scope"] = scope
            result = session.emulate_process(
                str(Path(fixtures) / "relr.so"), "android-aarch64-api28-v1",
                json.dumps({"backend": "unicorn", "android": native}))
            self.assertEqual(result["stop_reason"], stop, result["diagnostic"])
            if expected is not None:
                self.assertEqual(int(result["return_value"], 16), expected)
            if scope:
                lookup, call = result["android"]["native_calls"]
                self.assertEqual(lookup["arguments"][0], "0")
                self.assertEqual(lookup["library"], "libfixture.so")
                self.assertEqual(call["name"], "strlen")
                self.assertEqual(call["pc"], lookup["result"])
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "dynamic_identities", "arguments": [0x20000000, 0],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 16}],
            "libraries": {"libidentity.so": list(names)},
        }})
        result = session.emulate_process(str(Path(fixtures) / "relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 0)
        for name in names:
            with self.subTest(symbol=name):
                events = result["android"]["native_calls"]
                lookup = next(e for e in events
                              if e["name"] == "dlsym" and e["symbol"] == name)
                call = next(e for e in events if e["name"] == name)
                self.assertEqual(lookup["library"], "libidentity.so")
                self.assertEqual(call["library"], "libidentity.so")
                self.assertEqual(call["pc"], lookup["result"])
                self.assertEqual(int(call["result"], 16), 1000)
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "once_values", "arguments": [0x20000000],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 44}],
        }})
        result = session.emulate_process(str(Path(fixtures) / "once-O2-relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 73)
        events = result["android"]["native_calls"]
        self.assertEqual([e["name"] for e in events],
                         ["pthread_once"] * 4 + ["getuid", "pthread_once"])
        self.assertEqual([int(e["result"], 16) for e in events],
                         [0, 0, 0, 0, 1000, 0])
        memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
        self.assertEqual([int.from_bytes(memory[i:i + 4], "little")
                          for i in range(0, len(memory), 4)],
                         [2, 2, 1, 1, 1, 1, 2, 1000, 1, 2, 1])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "token_dynamic", "initialize": False,
            "arguments": [0x20000000, 0],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 40}],
            "libraries": {"libtokens.so": ["strtok_r"]},
        }})
        result = session.emulate_process(str(Path(fixtures) / "token-O2-relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 0)
        events = result["android"]["native_calls"]
        lookup = next(e for e in events if e["name"] == "dlsym")
        self.assertEqual(lookup["symbol"], "strtok_r")
        self.assertEqual(lookup["library"], "libtokens.so")
        calls = [e for e in events if e["name"] == "strtok_r"]
        self.assertEqual(len(calls), 2)
        for call in calls:
            self.assertEqual(call["library"], "libtokens.so")
            self.assertEqual(call["pc"], lookup["result"])
        memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
        self.assertEqual([int.from_bytes(memory[i:i + 8], "little")
                          for i in range(0, len(memory), 8)], [0, 6, 0, 6, 0])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "mutex_dynamic", "initialize": False,
            "arguments": [0x20000000, 0],
            "memory": [{"address": 0x20000000, "size": 4096}],
            "read_memory": [{"address": 0x20000000, "size": 24}],
            "libraries": {"libpthread-model.so": ["pthread_mutex_lock", "pthread_mutex_unlock"]},
        }})
        result = session.emulate_process(str(Path(fixtures) / "mutex-O2-relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 0)
        for name in ("pthread_mutex_lock", "pthread_mutex_unlock"):
            events = result["android"]["native_calls"]
            lookup = next(e for e in events if e["name"] == "dlsym" and e["symbol"] == name)
            call = next(e for e in events if e["name"] == name)
            self.assertEqual(call["library"], "libpthread-model.so")
            self.assertEqual(call["pc"], lookup["result"])
            self.assertEqual(int(call["result"], 16), 0)
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)
        options = json.dumps({"backend": "unicorn", "android": {
            "entry_symbol": "syscall_dynamic", "initialize": False,
            "arguments": [0], "libraries": {"libservice.so": ["syscall"]},
        }})
        result = session.emulate_process(str(Path(fixtures) / "syscall-O2-relr.so"),
                                         "android-aarch64-api28-v1", options)
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(int(result["return_value"], 16), 0)
        events = result["android"]["native_calls"]
        lookup = next(e for e in events if e["name"] == "dlsym" and e["symbol"] == "syscall")
        call = next(e for e in events if e["name"] == "syscall")
        self.assertEqual(call["library"], "libservice.so")
        self.assertEqual(call["pc"], lookup["result"])
        self.assertEqual(int(call["arguments"][0], 16), 178)
        self.assertEqual(int(call["result"], 16), 1000)
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)


if __name__ == "__main__":
    unittest.main()
