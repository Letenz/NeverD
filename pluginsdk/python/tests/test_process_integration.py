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
    def test_android_integer_scanning_preserves_provider_and_output(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for mode, name in enumerate(("sscanf", "vsscanf")):
            for closed in (0, 1):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": "scan_dynamic", "initialize": False,
                    "arguments": [0x180000000, closed, mode],
                    "memory": [{"address": 0x180000000, "size": 4096}],
                    "read_memory": [{"address": 0x180000000, "size": 16}],
                    "libraries": {"libscan-model.so": [name]},
                }}
                result = session.emulate_process(str(Path(fixtures) / "scan-O2-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                calls = result["android"]["native_calls"]
                lookup = next(c for c in calls if c["name"] == "dlsym")
                call = next(c for c in calls if c["name"] == name)
                self.assertEqual(lookup["symbol"], name)
                self.assertEqual(call["library"], "libscan-model.so")
                self.assertEqual(call["pc"], lookup["result"])
                self.assertEqual(call["result"], None if closed else "1")
                memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
                self.assertEqual(memory, bytes(16) if closed else bytes([15]) + bytes(15))
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_fortified_search_preserves_names_bounds_and_errno(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        path = str(Path(fixtures) / "search-O2-relr.so")
        for reverse, name in enumerate(("__strchr_chk", "__strrchr_chk")):
            options = {
                "backend": "unicorn", "android": {
                    "entry_symbol": "search_dynamic", "initialize": False,
                    "arguments": [0x20000000, 0, reverse],
                    "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 24}],
                    "libraries": {"libsearch.so": [name]},
                },
            }
            result = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
            self.assertEqual(result["return_value"], "0")
            calls = result["android"]["native_calls"]
            lookup = next(c for c in calls if c["name"] == "dlsym")
            self.assertEqual(lookup["symbol"], name)
            searches = [c for c in calls if c["name"] == name]
            self.assertEqual(len(searches), 2)
            for call in searches:
                self.assertEqual(call["library"], "libsearch.so")
                self.assertEqual(call["pc"], lookup["result"])
            memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
            self.assertEqual([int.from_bytes(memory[i:i + 8], "little")
                              for i in range(0, len(memory), 8)],
                             [10 if reverse else 4, 15, 733])
            options["android"]["arguments"][1] = 1
            closed = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(closed["stop_reason"], "unsupported_service")
            self.assertIsNone(closed["android"]["native_calls"][-1]["result"])
            options = {"backend": "unicorn", "android": {
                "entry_symbol": "search_supplied", "initialize": False,
                "arguments": [1, 58, 0, reverse + 2],
            }}
            failed = session.emulate_process(path, "android-aarch64-api28-v1", json.dumps(options))
            self.assertEqual(failed["stop_reason"], "runtime_failure")
            self.assertIn("FORTIFY", failed["diagnostic"])
            self.assertEqual(failed["android"]["native_calls"][-1]["name"], name)
            self.assertIsNone(failed["android"]["native_calls"][-1]["result"])
        self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_memory_files_share_descriptors_with_guest_threads(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for entry in ("files_sequence", "files_faults", "files_bionic",
                          "files_status", "files_status_bionic", "files_status_at",
                          "files_status_at_bionic", "files_access",
                          "files_access_faults", "files_access_bionic",
                          "files_directory_errors", "files_directory_bionic",
                          "files_trailing_paths", "files_trailing_paths_bionic",
                          "files_filesystem_status", "files_filesystem_status_bionic"):
                with self.subTest(optimization=optimization, entry=entry):
                    options = {
                        "backend": "unicorn", "instruction_quantum": 31,
                        "linux_files": {"files": [{"path": "/fixture/data", "bytes_hex": "00ff410a805a"}]},
                        "android": {"entry_symbol": entry, "initialize": False, "thread_limit": 2},
                    }
                    if entry in ("files_access", "files_directory_errors", "files_trailing_paths"):
                        options["linux_files"]["descriptor_limit"] = 4
                    if entry.startswith("files_status"):
                        options["linux_files"]["files"][0]["metadata"] = {
                            "device": 0xfe12cd34, "inode": str(0xfedcba9876543210),
                            "mode": 0o100644, "link_count": 0x89abcdef,
                            "uid": 0x87654321, "gid": 0xfedcba98, "size": 0,
                            "block_size": 16384, "blocks": 0x1234567890,
                            "access_time": {"seconds": str(-0x7fffffffffffffff),
                                            "nanoseconds": 123456789},
                            "modification_time": {"seconds": 4294967297,
                                                  "nanoseconds": 987654321},
                            "change_time": {"seconds": str(0x7fffffffffffffff),
                                            "nanoseconds": 999999999},
                        }
                    result = session.emulate_process(
                        str(Path(fixtures) / f"files-{optimization}-relr.so"),
                        "android-aarch64-api28-v1", json.dumps(options))
                    self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
                    self.assertEqual(result["return_value"], "0")
                    if entry == "files_bionic":
                        reads = [e for e in result["android"]["native_calls"]
                                 if e["name"] == "read" and e["thread_id"] == 1001]
                        self.assertEqual(len(reads), 1)
                        self.assertEqual(reads[0]["arguments"][0], "3")
                        self.assertEqual(reads[0]["result"], "2")
                    if entry == "files_status_bionic":
                        calls = [e for e in result["android"]["native_calls"]
                                 if e["name"] == "fstat64" and e["thread_id"] == 1001]
                        self.assertEqual(len(calls), 1)
                        self.assertEqual(calls[0]["arguments"][0], "3")
                        self.assertEqual(calls[0]["result"], "0")
                    self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_guest_threads_keep_identity_and_named_imports(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for closed in (False, True):
                options = {"backend": "unicorn", "instruction_quantum": 7, "android": {
                    "entry_symbol": "threads_dynamic", "thread_limit": 4, "trace_limit": 1000,
                    "arguments": [0x20000000, int(closed)], "initialize": False,
                    "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 64}],
                    "libraries": {"libthread-model.so": ["pthread_create", "pthread_join"]},
                }}
                result = session.emulate_process(str(Path(fixtures) / f"threads-{optimization}-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                android = result["android"]
                self.assertEqual(len(android["threads"]), 1 if closed else 2)
                names = ("pthread_create",) if closed else ("pthread_create", "pthread_join")
                for name in names:
                    lookup = next(e for e in android["native_calls"] if e["name"] == "dlsym" and e["symbol"] == name)
                    call = next(e for e in android["native_calls"] if e["name"] == name)
                    self.assertEqual(call["thread_id"], 1000)
                    self.assertEqual(call["pc"], lookup["result"])
                    self.assertEqual(call["library"], "libthread-model.so")
                    self.assertEqual(call["result"], None if closed else "0")
                if not closed:
                    child = android["threads"][1]
                    self.assertEqual(child["thread_id"], 1001)
                    self.assertTrue(child["finished"] and child["retired"])
                    self.assertEqual({span["thread_id"] for span in android["trace_threads"]}, {1000, 1001})
                    memory = bytes.fromhex(android["memory"][0]["bytes_hex"])
                    self.assertEqual(int.from_bytes(memory[32:40], "little"), 0x1234567800000042)
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_thread_attributes_keep_bytes_and_dynamic_names(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for closed in (False, True):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": "thread_attribute_dynamic", "arguments": [0x20000000, int(closed)],
                    "initialize": False, "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 80}],
                    "libraries": {"libthread-model.so": ["pthread_attr_init", "pthread_attr_getstacksize"]},
                }}
                result = session.emulate_process(
                    str(Path(fixtures) / f"thread-attributes-{optimization}-relr.so"),
                    "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                events = result["android"]["native_calls"]
                names = ("pthread_attr_init",) if closed else ("pthread_attr_init", "pthread_attr_getstacksize")
                for name in names:
                    lookup = next(e for e in events if e["name"] == "dlsym" and e["symbol"] == name)
                    call = next(e for e in events if e["name"] == name)
                    self.assertEqual(call["library"], "libthread-model.so")
                    self.assertEqual(call["pc"], lookup["result"])
                    self.assertEqual(call["result"], None if closed else "0")
                memory = bytes.fromhex(result["android"]["memory"][0]["bytes_hex"])
                expected = bytearray([0xa5] * 56)
                expected[:4] = bytes(4)
                expected[8:40] = bytes(32)
                expected[16:24] = (0xfc000).to_bytes(8, "little")
                expected[24:32] = (4096).to_bytes(8, "little")
                self.assertEqual(memory[16:], bytes(64) if closed else (0xfc000).to_bytes(8, "little") + expected)
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_finalizers_execute_guest_callbacks(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for closed in (False, True):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": "cxa_dynamic", "arguments": [0x20000000, int(closed)],
                    "initialize": False, "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 256}],
                    "libraries": {"libfinalize-model.so": ["__cxa_atexit", "__cxa_finalize"]},
                }}
                result = session.emulate_process(str(Path(fixtures) / f"finalizers-{optimization}-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                calls = result["android"]["native_calls"]
                lookup = next(e for e in calls if e["name"] == "dlsym" and e["symbol"] == "__cxa_finalize")
                call = next(e for e in calls if e["name"] == "__cxa_finalize")
                self.assertEqual(call["library"], "libfinalize-model.so")
                self.assertEqual(call["pc"], lookup["result"])
                self.assertEqual(call["result"], None if closed else "0")
                if not closed:
                    self.assertEqual(int(result["return_value"], 16), 73)
                expected = bytearray(256)
                if not closed:
                    for offset, value in ((0, 1), (48, 1000), (64, 0x100000009)):
                        expected[offset:offset + 8] = value.to_bytes(8, "little")
                self.assertEqual(bytes.fromhex(result["android"]["memory"][0]["bytes_hex"]), expected)
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

    def test_android_formatting_uses_guest_variadic_calls(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        for optimization in ("O0", "O2"):
            for closed in (False, True):
                options = {"backend": "unicorn", "android": {
                    "entry_symbol": "format_dynamic", "arguments": [0x20000000, int(closed)],
                    "initialize": False, "memory": [{"address": 0x20000000, "size": 4096}],
                    "read_memory": [{"address": 0x20000000, "size": 64}],
                    "libraries": {"libformat-model.so": ["snprintf"]},
                }}
                result = session.emulate_process(str(Path(fixtures) / f"format-{optimization}-relr.so"),
                                                 "android-aarch64-api28-v1", json.dumps(options))
                self.assertEqual(result["stop_reason"], "unsupported_service" if closed else "returned",
                                 result["diagnostic"])
                calls = result["android"]["native_calls"]
                lookup = next(e for e in calls if e["name"] == "dlsym")
                call = next(e for e in calls if e["name"] == "snprintf")
                self.assertEqual(call["library"], "libformat-model.so")
                self.assertEqual(call["pc"], lookup["result"])
                self.assertEqual(call["result"], None if closed else "12")
                if not closed:
                    self.assertEqual(int(result["return_value"], 16), 18)
                expected = bytes(64) if closed else b"symbol=0x10000000a" + bytes(46)
                self.assertEqual(bytes.fromhex(result["android"]["memory"][0]["bytes_hex"]), expected)
                self.assertEqual(host.call("neverd_session_is_loaded", handle), 0)

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

    def test_relative_sleep_retains_input_and_completes_original_event(self) -> None:
        library = os.environ.get("NEVERD_TEST_LIBNEVERD")
        fixtures = os.environ.get("NEVERD_TEST_ANDROID_FIXTURES")
        if not library or not fixtures:
            self.skipTest("built libneverd and Android fixtures are not configured")
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
        options = {"backend": "unicorn", "linux_time": {
            "advance_on_idle": True,
            "clocks": [{"id": 0, "seconds": "4294967297", "nanoseconds": 999999998}]},
            "android": {"entry_symbol": "sleep_call", "initialize": False, "thread_limit": 2,
                "arguments": [2, 0x20000000, 0x20000000, 0x20000020],
                "memory": [{"address": 0x20000000, "size": 4096,
                            "bytes_hex": "00000000000000000500000000000000"}],
                "read_memory": [{"address": 0x20000000, "size": 64}]}}
        result = session.emulate_process(str(Path(fixtures) / "sleep-O2-relr.so"),
                                         "android-aarch64-api28-v1", json.dumps(options))
        self.assertEqual(result["stop_reason"], "returned", result["diagnostic"])
        self.assertEqual(result["return_value"], "0")
        self.assertEqual(len(result["services"]), 1)
        self.assertEqual(result["services"][0]["result"], "0")
        expected = b"".join(v.to_bytes(8, "little") for v in
                            [0, 5, 0, 0, 73, 4294967298, 3, 4294967298])
        self.assertEqual(bytes.fromhex(result["android"]["memory"][0]["bytes_hex"]), expected)

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
