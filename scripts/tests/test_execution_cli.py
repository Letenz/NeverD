"""Persistent CLI/public C command equivalence over raw CPU buffers."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pluginsdk/python"))
from neverd_plugin.abi import NeverDExecutionOptionsV1
from neverd_plugin.ffi import HostAPI

OPTIONS = None


class LiveCLI:
    def __init__(self, test, executable, config, modules=None):
        self.test = test
        command = [str(executable), "execution", str(config)]
        if modules:
            command += ["--modules=" + str(modules)]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.lines = queue.Queue()
        self.pending = {}
        self.errors = bytearray()
        self.output = threading.Thread(target=self._output, daemon=True)
        self.error = threading.Thread(target=self._error, daemon=True)
        self.output.start()
        self.error.start()
        test.addCleanup(self.close)
        self.initial = self.receive(created=True)

    def _output(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def _error(self):
        for line in self.process.stderr:
            if len(self.errors) < 1048576:
                self.errors.extend(line)

    def send(self, identity, op, **fields):
        value = {"id": identity, "op": op, **fields}
        self.raw(json.dumps(value, ensure_ascii=False).encode() + b"\n")

    def raw(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def receive(self, identity=None, created=False):
        key = "__created__" if created else identity
        if key in self.pending:
            return self.pending.pop(key)
        deadline = time.monotonic() + 8
        while True:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                self.test.fail("CLI response timed out: " + str(key))
            self.test.assertIsNotNone(line, self.errors.decode("utf-8", "replace"))
            value = json.loads(line)
            response_key = "__created__" if value.get("type") == "created" else value.get("id")
            if response_key == key:
                return value
            self.pending[response_key] = value
            self.test.assertLess(time.monotonic(), deadline, "CLI response ID was never received")

    def request(self, identity, op, **fields):
        self.send(identity, op, **fields)
        return self.receive(identity)

    def finish(self, status=0):
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        self.test.assertEqual(self.process.wait(timeout=10), status,
                              self.errors.decode("utf-8", "replace"))

    def close(self):
        if self.process.stdin and not self.process.stdin.closed:
            try:
                self.process.stdin.close()
            except BrokenPipeError:
                pass
        if self.process.poll() is None:
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
                self.test.fail("persistent CLI failed to stop and join on EOF")
        self.output.join(timeout=2)
        self.error.join(timeout=2)
        self.process.stdout.close()
        self.process.stderr.close()


class ExecutionCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if OPTIONS is None:
            raise unittest.SkipTest("configured CLI/library paths are required")
        cls.cli = Path(OPTIONS.neverd).resolve()
        library = Path(OPTIONS.library).resolve()
        cls.directory = os.add_dll_directory(str(library.parent)) if sys.platform == "win32" else None
        cls.api = HostAPI(ctypes.CDLL(str(library)))

    @classmethod
    def tearDownClass(cls):
        if cls.directory:
            cls.directory.close()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="neverd-execution-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        ident = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
        header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, 0,
                             64, 0, 0, 64, 56, 1, 64, 0, 0)
        segment = struct.pack("<IIQQQQQQ", 1, 6, 4096, 0x50000, 0, 8, 8, 4096)
        self.image = self.root / "data.elf"
        self.image.write_bytes((header + segment).ljust(4096, b"\0") + bytes([42]) + bytes(7))
        self.handles = []
        self.addCleanup(self.release)

    def take(self, pointer):
        if not pointer:
            return ""
        try:
            return ctypes.string_at(pointer).decode("utf-8")
        finally:
            self.api.function("neverd_free_string")(ctypes.cast(pointer, ctypes.c_char_p))

    def call(self, name, *args):
        error = ctypes.c_void_p()
        result = self.api.function(name)(*args, ctypes.byref(error))
        if error.value:
            return result, self.take(error.value)
        return result, None

    def release(self):
        for kind, handle in reversed(self.handles):
            self.call("neverd_" + kind + "_destroy", handle)

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
        return path

    def config(self, code="B84900000090", stop=0x10005):
        data = code.ljust(512, "0") + "4889114889D04883C002C3"
        return {"environment": {"kind": "buffer", "cpu": {"backend": "unicorn"},
            "memory_limit": 1048576, "entry": 0x10000, "stop_pc": stop,
            "stack_pointer": 0x80F00,
            "regions": [{"address": 0x10000, "size": 4096, "permissions": "rx", "bytes_hex": data},
                        *[{"address": address, "size": 4096, "permissions": "rw"}
                          for address in (0x60000, 0x80000, 0xA0000)]],
            "invocation": {"stack_base": 0xA0000, "stack_size": 4096, "return_pc": 0x10300}},
            "run": {"pause_stages": ["stopped"], "memory_transfer_limit": 4096}}

    def binding(self):
        return {"id": "data-instance", "module_id": "data", "guest_name": "data",
                "address_space": "buffer", "base": "0x50000", "size": 4096}

    def live(self, config=None, modules=None, name="execution.json"):
        path = self.write(name, self.config() if config is None else config)
        return LiveCLI(self, self.cli, path, modules)

    def direct(self, config, modules=None):
        if modules:
            source, error = self.call("neverd_module_set_from_file", str(modules).encode())
        else:
            source, error = self.call("neverd_module_set_create_v1", None)
        self.assertTrue(source, error)
        self.handles.append(("module_set", source))
        text = json.dumps(config).encode()
        options = NeverDExecutionOptionsV1(ctypes.sizeof(NeverDExecutionOptionsV1), source, text)
        handle, error = self.call("neverd_execution_create_v1", ctypes.byref(options))
        self.assertTrue(handle, error)
        self.handles.append(("execution", handle))
        return handle

    def direct_command(self, handle, identity, op, **fields):
        text = json.dumps({"id": identity, "op": op, **fields}).encode()
        result, error = self.call("neverd_execution_command_json", handle, text)
        return json.loads(self.take(result)) if result else None, error

    def test_creation_is_stopped_and_does_not_run_instructions(self):
        cli = self.live()
        self.assertEqual(cli.initial["result"]["instructions"], "0x0")
        self.assertEqual(cli.initial["result"]["event"]["state"], "ready")
        self.assertTrue(cli.request("quit", "quit")["result"]["quit"])
        cli.finish()

    def test_documented_configuration_and_call_are_executable(self):
        text = (ROOT / "docs/runtime-modules.md").read_text(encoding="utf-8")
        section = text.split("## Persistent execution C API", 1)[1]
        config = json.loads(section.split("```json\n", 1)[1].split("```", 1)[0])
        cli = self.live(config)
        cli.request("run-1", "run")
        request = {"entry": "0x11000", "abi": "win64", "arguments": [{"value": 40}, {"value": 2}]}
        cli.request("call-1", "invoke", invocation=request)
        event = cli.request("call-run-1", "run")["result"]
        self.assertEqual(event["reason"], "invocation_complete")
        self.assertEqual(event["invocation"]["value"], "0x2A")
        cli.request("quit-1", "quit")
        cli.finish()

    def test_cli_and_direct_sdk_lifecycle_and_memory_results_match(self):
        config = self.config()
        direct = self.direct(config)
        cli = self.live(config)
        for identity, op, fields in [
                ("state", "state", {}),
                ("write", "write", {"address": "0x60000", "bytes_hex": "0080ff01"}),
                ("read", "read", {"address": "0x60000", "size": 4}),
                ("run", "run", {}), ("resume", "resume", {}), ("end", "run", {})]:
            expected, error = self.direct_command(direct, identity, op, **fields)
            self.assertIsNone(error)
            actual = cli.request(identity, op, **fields)
            self.assertEqual(actual, expected)
        cli.request("quit", "quit")
        cli.finish()

    def test_error_diagnostic_matches_sdk_and_keeps_the_workload(self):
        direct = self.direct(self.config())
        _, error = self.direct_command(direct, "bad", "resume")
        cli = self.live()
        actual = cli.request("bad", "resume")
        self.assertFalse(actual["ok"])
        self.assertEqual(actual["error"], error)
        self.assertEqual(cli.request("inspect", "inspect")["result"]["instructions"], "0x0")
        cli.request("quit", "quit")
        cli.finish(2)

    def test_missing_resource_can_be_added_then_supplied_with_its_live_token(self):
        modules = self.write("modules.json", {"modules": []})
        config = self.config("48BB0000050000000000488B0348A3000006000000000090", 0x10017)
        cli = self.live(config, modules)
        event = cli.request("run", "run")["result"]
        self.assertEqual(event["state"], "awaiting_resource")
        before = cli.request("before", "inspect")["result"]["instructions"]
        self.assertTrue(cli.request("add", "module_add", module={"id": "data", "image": "data.elf", "load": "lazy"})["ok"])
        self.assertFalse(cli.request("sources", "modules")["result"]["modules"][0]["prepared"])
        self.assertEqual(cli.request("still", "state")["result"]["state"], "awaiting_resource")
        payload = {"token": event["request"]["token"], "bindings": [self.binding()]}
        self.assertTrue(cli.request("supply", "supply", supply=payload)["ok"])
        self.assertEqual(cli.request("after", "inspect")["result"]["instructions"], before)
        self.assertEqual(cli.request("continue", "run")["result"]["stage"], "stopped")
        self.assertEqual(cli.request("read", "read", address="0x60000", size=8)["result"]["bytes_hex"], "2a00000000000000")
        self.assertTrue(cli.request("instances", "instances")["result"]["instances"][0]["mapped"])
        cli.request("quit", "quit")
        cli.finish()

    def test_configured_lazy_source_prepares_on_access_only(self):
        modules = self.write("modules.json", {"modules": [{"id": "data", "image": "data.elf", "load": "lazy"}]})
        config = self.config("48BB0000050000000000488B0348A3000006000000000090", 0x10017)
        config["bindings"] = [self.binding()]
        cli = self.live(config, modules)
        self.assertFalse(cli.request("before", "modules")["result"]["modules"][0]["prepared"])
        self.assertEqual(cli.request("run", "run")["result"]["reason"], "stage")
        self.assertTrue(cli.request("after", "modules")["result"]["modules"][0]["prepared"])
        cli.request("quit", "quit")
        cli.finish()

    def test_eager_missing_input_can_be_revised_without_restarting(self):
        modules = self.write("modules.json", {"modules": [{"id": "data", "image": "missing.elf", "load": "eager"}]})
        config = self.config("48BB0000050000000000488B0348A3000006000000000090", 0x10017)
        config["bindings"] = [self.binding()]
        cli = self.live(config, modules)
        event = cli.request("run", "run")["result"]
        self.assertEqual(event["request"]["kind"], "declared_module")
        self.assertEqual(cli.request("before", "inspect")["result"]["instructions"], "0x0")
        self.assertTrue(cli.request("revise", "module_revise", module={"id": "data", "image": "data.elf"})["ok"])
        cli.request("supply", "supply", supply={"token": event["request"]["token"], "bindings": [self.binding()]})
        self.assertEqual(cli.request("continue", "run")["result"]["reason"], "stage")
        cli.request("quit", "quit")
        cli.finish()

    def test_invocation_captures_exact_binary_output_and_restores_stage(self):
        cli = self.live()
        cli.request("run", "run")
        invocation = {"entry": "0x10100", "abi": "win64", "arguments": [{"buffer": 0}, {"value": 40}], "buffers": [{"size": 8}]}
        for index in range(2):
            self.assertTrue(cli.request("invoke" + str(index), "invoke", invocation=invocation)["ok"])
            event = cli.request("call" + str(index), "run")["result"]
            self.assertEqual(event["reason"], "invocation_complete")
            self.assertEqual(event["stage"], "stopped")
            self.assertEqual(event["invocation"]["value"], "0x2A")
            self.assertEqual(event["invocation"]["buffers"][0]["bytes_hex"], "2800000000000000")
        cli.request("quit", "quit")
        cli.finish()

    def test_running_cpu_accepts_state_and_pause_with_correlated_responses(self):
        config = self.config("EBFE90", 0x10002)
        config["run"].update(instruction_quantum=1000000, actions_per_run=1000000,
                             limits={"instructions": 1000000000, "events": 1000000, "timeout_microseconds": 5000000})
        cli = self.live(config)
        cli.send("long-run", "run")
        deadline = time.monotonic() + 3
        while cli.request("state", "state")["result"]["state"] != "running":
            self.assertLess(time.monotonic(), deadline)
        busy = cli.request("busy", "read", address="0x60000", size=1)
        self.assertFalse(busy["ok"])
        self.assertIn("busy", busy["error"])
        self.assertTrue(cli.request("pause", "pause")["ok"])
        self.assertEqual(cli.receive("long-run")["result"]["reason"], "user_pause")
        cli.request("quit", "quit")
        cli.finish(2)

    def test_eof_requests_pause_and_joins_an_active_run(self):
        config = self.config("EBFE90", 0x10002)
        cli = self.live(config)
        cli.send("run", "run")
        cli.process.stdin.close()
        event = cli.receive("run")["result"]
        self.assertEqual(event["reason"], "user_pause")
        cli.finish()

    def test_guest_failure_is_a_valid_failed_event(self):
        cli = self.live(self.config("0F0B90", 0x10002))
        report = cli.request("fault", "run")
        self.assertTrue(report["ok"])
        self.assertEqual(report["result"]["state"], "failed")
        self.assertEqual(report["result"]["reason"], "guest_failure")
        cli.request("quit", "quit")
        cli.finish()

    def test_duplicate_fields_are_rejected_before_any_guest_effect(self):
        cli = self.live()
        cli.raw(b'{"id":"duplicate","op":"pause","op":"run"}\n')
        report = cli.receive("duplicate")
        self.assertFalse(report["ok"])
        self.assertIn("duplicate", report["error"])
        self.assertEqual(cli.request("inspect", "inspect")["result"]["instructions"], "0x0")
        cli.request("quit", "quit")
        cli.finish(2)

    def test_transport_rejects_nul_and_oversized_lines_then_can_continue(self):
        cli = self.live()
        cli.raw(b'{"id":"nul","op":"run"}\x00suffix\n')
        self.assertFalse(cli.receive()["ok"])
        cli.raw(b"x" * (2097152 + 1) + b"\n")
        self.assertFalse(cli.receive()["ok"])
        self.assertEqual(cli.request("inspect", "inspect")["result"]["instructions"], "0x0")
        cli.request("quit", "quit")
        cli.finish(2)

    def test_unicode_configuration_path_and_request_id(self):
        cli = self.live(name="环境/执行.json")
        self.assertEqual(cli.request("状态-甲", "state")["id"], "状态-甲")
        cli.request("quit", "quit")
        cli.finish()

    def test_relative_incremental_source_requires_an_explicit_resource_origin(self):
        cli = self.live()
        report = cli.request("add", "module_add", module={"id": "data", "image": "data.elf"})
        self.assertFalse(report["ok"])
        self.assertIn("base_directory", report["error"])
        cli.request("quit", "quit")
        cli.finish(2)

    def test_invalid_configuration_has_no_created_or_success_output(self):
        path = self.write("invalid.json", {"environment": {"kind": "unknown"}})
        result = subprocess.run([str(self.cli), "execution", str(path)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"unsupported execution environment", result.stderr)

    def test_configuration_nul_is_not_silently_truncated(self):
        path = self.root / "embedded-zero.json"
        path.write_bytes(json.dumps(self.config()).encode() + b"\0suffix")
        result = subprocess.run([str(self.cli), "execution", str(path)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"NUL", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--library", required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
