"""Run with python3 -m unittest discover -s tools/neverd-mcp/tests -v."""
import io
import json
import queue
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import (DEFAULT_STRING_TEXT_LIMIT, MAX_CACHED_RESULTS, MAX_RESULT, PROTOCOL, RpcError, Server,
                    compact_result, serve)
from transport import Backend, MAX_FRAME, read_frame, write_frame


class FakeBackend:
    def __init__(self, attached=False):
        self.attached = attached
        self.calls = []
        self.response = {"type": "response", "status": "ok", "revision": "42", "payload": {"items": []}}

    def query(self, operation, payload, revision=None):
        self.calls.append((operation, payload, revision))
        return self.response


def request(method, params=None, request_id=1):
    value = {"jsonrpc": "2.0", "method": method}
    if request_id is not None:
        value["id"] = request_id
    if params is not None:
        value["params"] = params
    return value


INIT = request("initialize", {"protocolVersion": PROTOCOL, "capabilities": {},
                              "clientInfo": {"name": "test", "version": "1"}})


class McpProtocolTests(unittest.TestCase):
    def setUp(self):
        self.backend = FakeBackend()
        self.server = Server(self.backend)

    def ready(self, server=None):
        server = server or self.server
        self.assertEqual(server.handle(INIT)["protocolVersion"], PROTOCOL)
        server.handle(request("notifications/initialized", request_id=None))

    def test_handshake_and_lifecycle(self):
        with self.assertRaises(RpcError) as error:
            self.server.handle(request("tools/list"))
        self.assertEqual(error.exception.code, -32002)
        self.ready()
        tools = self.server.handle(request("tools/list"))["tools"]
        self.assertTrue(all("operation" not in t for t in tools))
        self.assertTrue(all(t["annotations"]["readOnlyHint"] for t in tools))
        with self.assertRaises(RpcError):
            self.server.handle(INIT)

    def test_only_pages_take_a_revision_guard(self):
        self.ready()
        for listed in self.server.handle(request("tools/list"))["tools"]:
            properties = listed["inputSchema"]["properties"]
            self.assertEqual("expected_revision" in properties, "offset" in properties, listed["name"])

    def test_address_and_revision_remain_strings(self):
        self.ready()
        result = self.server.handle(request("tools/call", {"name": "list_xrefs", "arguments": {
            "address": "0xffffffffffffffff", "limit": 2, "expected_revision": "18446744073709551615"}}))
        self.assertFalse(result["isError"])
        self.assertEqual(self.backend.calls, [("xrefs", {"address": "0xffffffffffffffff", "limit": 2}, "18446744073709551615")])

    def test_validation_rejects_path_changes_and_unbounded_pages(self):
        self.ready()
        cases = [("list_functions", {"limit": 513}), ("list_functions", {"offset": -1}),
                 ("list_functions", {"limit": True}), ("project_metadata", {"path": "/new/input"}),
                 ("read_bytes", {"address": "0x10000000000000000"}),
                 ("read_bytes", {"address": 9007199254740993}),
                 ("read_decompilation", {"address": "0x1", "representation": "shell"}),
                 ("open", {"path": "/new/input"}), ("rename", {"name": "new"})]
        for name, arguments in cases:
            with self.subTest(name=name, arguments=arguments), self.assertRaises(RpcError):
                self.server.handle(request("tools/call", {"name": name, "arguments": arguments}))
        self.assertEqual(self.backend.calls, [])

    def test_gui_resources_and_navigation_require_attachment(self):
        self.ready()
        with self.assertRaises(RpcError):
            self.server.handle(request("resources/read", {"uri": "neverd://gui/selection"}))
        attached = Server(FakeBackend(attached=True))
        self.ready(attached)
        result = attached.handle(request("tools/call", {"name": "navigate_gui", "arguments": {"address": "0x123"}}))
        self.assertFalse(result["isError"])
        self.assertEqual(attached.backend.calls[0][0], "navigate")
        with self.assertRaises(RpcError):
            attached.handle(request("resources/read", {"uri": "neverd://project/navigate?address=0x1"}))

    def test_resources_are_scoped_and_paginated(self):
        self.ready()
        self.server.handle(request("resources/read", {"uri": "neverd://project/functions?offset=20&limit=10&expected_revision=42"}))
        self.assertEqual(self.backend.calls[-1], ("functions", {"offset": 20, "limit": 10}, "42"))
        for uri in ("file:///etc/passwd", "https://example.com/", "neverd://other/metadata", "neverd://project/functions?limit=1&limit=2"):
            with self.subTest(uri=uri), self.assertRaises(RpcError):
                self.server.handle(request("resources/read", {"uri": uri}))

    def test_errors_and_budget_are_not_successful_empty_results(self):
        self.ready()
        self.backend.response = {"status": "error", "revision": "42", "error": {"code": "stale_revision"}}
        result = self.server.handle(request("tools/call", {"name": "project_metadata"}))
        self.assertTrue(result["isError"])
        self.backend.response = {"status": "ok", "revision": "42", "payload": {"text": "x" * MAX_CACHED_RESULTS}}
        result = self.server.handle(request("tools/call", {"name": "project_metadata"}))
        self.assertTrue(result["isError"])
        self.assertEqual(json.loads(result["content"][0]["text"])["status"], "budget_exceeded")

    def test_large_results_return_bounded_revision_stable_resource_pages(self):
        self.ready()
        expected = {"status": "ok", "revision": "42", "project_id": "project-1", "payload": {"text": "中文\n" * MAX_RESULT}}
        self.backend.response = expected
        result = self.server.handle(request("tools/call", {"name": "project_metadata"}))
        self.assertFalse(result["isError"])
        self.assertEqual(result["content"][0]["type"], "resource_link")
        uri = result["content"][0]["uri"]
        chunks = []
        offset = 0
        self.backend.response = {"status": "ok", "revision": "43"}
        while True:
            response = self.server.handle(request("resources/read", {"uri": uri + f"?offset={offset}&limit=65536&expected_revision=42"}))
            page = json.loads(response["contents"][0]["text"])
            self.assertEqual(page["revision"], "42")
            chunks.append(page["text"])
            if page["complete"]:
                break
            offset = page["next_offset"]
        self.assertEqual("".join(chunks), compact_result("metadata", {}, expected)[0])
        self.assertEqual(json.loads("".join(chunks))["revision"], "42")
        self.assertEqual(len(self.backend.calls), 1)
        with self.assertRaises(RpcError):
            self.server.handle(request("resources/read", {"uri": uri + "?expected_revision=43"}))

    def test_tools_return_what_an_agent_reads(self):
        self.ready()
        self.backend.response = {"type": "response", "protocol_major": 1, "request_id": "9", "operation": "decompile",
                                 "analysis_state": "complete", "project_id": "project-1", "revision": "7", "status": "ok",
                                 "payload": {"address": "0x401000", "representation": "c", "dialect": "c",
                                             "text": "int main(void) {\n    return 0;\n}\n", "offset": 0,
                                             "total_lines": 3, "next_offset": None, "complete": True,
                                             "rows": [{"line": 0, "address": "0x401000"}] * 50,
                                             "source_variables": [{"name": "v"}] * 50,
                                             "code_names": [{"begin_byte": 4}] * 50, "source_calls": []}}
        result = self.server.handle(request("tools/call", {"name": "read_decompilation", "arguments": {"address": "0x401000"}}))
        self.assertFalse(result["isError"])
        self.assertNotIn("structuredContent", result)
        header, code = result["content"][0]["text"].split("\n", 1)
        self.assertEqual(json.loads(header), {"revision": "7", "address": "0x401000", "representation": "c",
                                              "offset": 0, "total_lines": 3, "complete": True})
        self.assertEqual(code, "int main(void) {\n    return 0;\n}\n")
        # The complete worker response stays available as a resource.
        contents = self.server.handle(request("resources/read", {"uri": "neverd://project/decompile?address=0x401000"}))
        self.assertEqual(len(json.loads(contents["contents"][0]["text"])["payload"]["rows"]), 50)

    def test_tables_are_columns_and_rows(self):
        self.ready()
        self.backend.response = {"status": "ok", "revision": "3", "payload": {"items": [
            {"addr": "0x1000", "address": "0x1000", "name": "main", "display_name": "main", "engine_name": "sub_1000",
             "linkage_name": "sub_1000", "name_origin": "synthesized", "size": 42, "library": False, "thunk": True,
             "exported": True, "recognition_state": "disabled", "library_annotations": []}],
            "total": 1, "offset": 0, "next_offset": None, "complete": True}}
        result = self.server.handle(request("tools/call", {"name": "list_functions", "arguments": {"limit": 1}}))
        self.assertEqual(json.loads(result["content"][0]["text"]), {
            "revision": "3", "total": 1, "offset": 0, "next_offset": None, "complete": True,
            "columns": ["address", "name", "size", "flags"], "rows": [["0x1000", "main", 42, "thunk,exported"]]})
        self.server.handle(request("tools/call", {"name": "list_strings", "arguments": {"filter": "http"}}))
        self.assertEqual(self.backend.calls[-1][:2], ("strings", {"filter": "http", "text_limit": DEFAULT_STRING_TEXT_LIMIT}))
        self.server.handle(request("tools/call", {"name": "find_string_references", "arguments": {"filter": "passw"}}))
        self.assertEqual(self.backend.calls[-1][:2], ("string_references", {"filter": "passw"}))
        self.backend.response = {"status": "ok", "revision": "3", "payload": {"total": 3, "offset": 0, "next_offset": None,
            "complete": True, "items": [{"address": "0x10", "module": "ws2_32.dll", "name": "connect"},
                                        {"address": "0x18", "module": "kernel32.dll", "name": "CreateFileW"},
                                        {"address": "0x20", "module": "ws2_32.dll", "name": "send"}]}}
        result = self.server.handle(request("tools/call", {"name": "list_imports", "arguments": {}}))
        self.assertEqual(json.loads(result["content"][0]["text"])["modules"],
                         {"ws2_32.dll": ["connect", "send"], "kernel32.dll": ["CreateFileW"]})

    def test_names_resolve_to_addresses_first(self):
        self.ready()
        replies = [{"status": "ok", "revision": "5", "payload": {"address": "0x2a90", "function_address": "0x2a90"}},
                   {"status": "ok", "revision": "5", "payload": {"text": "int main(void);\n"}}]
        self.backend.query = lambda operation, payload, revision=None: (
            self.backend.calls.append((operation, payload, revision)) or replies[len(self.backend.calls) - 1])
        result = self.server.handle(request("tools/call", {"name": "read_decompilation", "arguments": {"address": "main"}}))
        self.assertFalse(result["isError"])
        self.assertEqual([call[:2] for call in self.backend.calls],
                         [("resolve", {"query": "main"}), ("decompile", {"address": "0x2a90"})])

    def test_graph_tools_take_names_and_waypoints(self):
        self.ready()
        self.backend.response = {"status": "ok", "revision": "1", "payload": {
            "segments": [{"from": "0x10", "to": "0x30", "shortest": 2, "node_count": 3, "complete": True,
                          "paths": [["0x10", "0x20", "0x30"]]}], "unconnected": 0, "max_depth": 16,
            "nodes": [{"address": "0x10", "name": "main", "kind": "function"},
                      {"address": "0x20", "name": "sub_20", "kind": "function"},
                      {"address": "0x30", "name": "connect", "kind": "import"}]}}
        result = self.server.handle(request("tools/call", {"name": "find_call_paths", "arguments": {
            "from": "main", "via": ["sub_20"], "to": "connect", "shortest_only": True}}))
        self.assertEqual(self.backend.calls[-1][:2], ("call_paths", {"waypoints": ["main", "sub_20", "connect"],
                                                                     "shortest_only": True}))
        self.assertEqual(result["content"][0]["text"].splitlines()[1:],
                         ["main@0x10 -> connect@import: shortest 2, 3 nodes on paths",
                          "  main@0x10 -> sub_20 -> connect@import"])
        self.server.handle(request("tools/call", {"name": "list_call_tree", "arguments": {"function": "connect", "callers": True}}))
        self.assertEqual(self.backend.calls[-1][:2], ("call_tree", {"root": "connect", "direction": "callers"}))
        for arguments in ({"from": "main", "to": ""}, {"from": "main", "to": "x", "via": "y"},
                          {"from": "main", "to": "x", "shortest_only": 1}):
            with self.subTest(arguments=arguments), self.assertRaises(RpcError):
                self.server.handle(request("tools/call", {"name": "find_call_paths", "arguments": arguments}))

    def test_source_pages_start_at_the_definition(self):
        self.ready()
        replies = [{"status": "ok", "revision": "2", "payload": {
                        "address": "0x10", "dialect": "c", "text": "#include <stdint.h>\n\nint f(void) {\n",
                        "offset": 0, "total_lines": 5, "next_offset": 3, "complete": False,
                        "prelude": {"lines": 2, "end_byte": 21}}},
                   {"status": "ok", "revision": "2", "payload": {
                        "address": "0x10", "dialect": "c", "text": "int f(void) {\n    return 0;\n}\n",
                        "offset": 2, "total_lines": 5, "next_offset": None, "complete": True}}]
        self.backend.query = lambda operation, payload, revision=None: (
            self.backend.calls.append((operation, payload, revision)) or replies[len(self.backend.calls) - 1])
        result = self.server.handle(request("tools/call", {"name": "read_decompilation", "arguments": {"address": "0x10"}}))
        header, code = result["content"][0]["text"].split("\n", 1)
        self.assertEqual(self.backend.calls[-1][:2], ("decompile", {"address": "0x10", "offset": 2}))
        self.assertEqual(json.loads(header)["prelude_lines"], 2)
        self.assertEqual(code, "int f(void) {\n    return 0;\n}\n")
        # With prelude=true, the page is the one the worker gives.
        self.backend.calls.clear()
        result = self.server.handle(request("tools/call", {"name": "read_decompilation",
                                                           "arguments": {"address": "0x10", "prelude": True}}))
        self.assertEqual(len(self.backend.calls), 1)
        self.assertTrue(result["content"][0]["text"].split("\n", 1)[1].startswith("#include"))

    def test_snapshot_cache_eviction_is_explicit(self):
        self.ready()
        self.backend.response = {"status": "ok", "revision": "42", "payload": {"text": "x" * MAX_RESULT}}
        first = None
        for _ in range(5):
            result = self.server.handle(request("tools/call", {"name": "project_metadata"}))
            first = first or result["structuredContent"]["resource_uri"]
        self.assertEqual(len(self.server.result_resources), 4)
        self.assertLessEqual(self.server.cached_bytes, MAX_CACHED_RESULTS)
        with self.assertRaises(RpcError):
            self.server.handle(request("resources/read", {"uri": first}))

    def test_standard_stdio_not_worker_framing(self):
        source = io.BytesIO((json.dumps(INIT) + "\n" + json.dumps(request("notifications/initialized", request_id=None)) + "\n").encode())
        output = io.StringIO()
        serve(self.server, source, output)
        lines = output.getvalue().splitlines()
        self.assertEqual(len(lines), 1)
        response = json.loads(lines[0])
        self.assertEqual(response["id"], 1)
        self.assertEqual(response["result"]["protocolVersion"], PROTOCOL)

    def test_malformed_json_and_batch_report_protocol_errors(self):
        output = io.StringIO()
        serve(self.server, io.BytesIO(b"{bad}\n[]\n{}\n"), output)
        errors = [json.loads(line)["error"]["code"] for line in output.getvalue().splitlines()]
        self.assertEqual(errors, [-32700, -32600, -32600])

    def test_worker_framing_partial_reads_and_size_limit(self):
        class Partial(io.BytesIO):
            def read(self, size=-1):
                return super().read(min(size, 2))
        output = io.BytesIO()
        expected = {"address": "0xffffffffffffffff", "text": "中文\nsecond line"}
        write_frame(output, expected)
        self.assertEqual(read_frame(Partial(output.getvalue())), expected)
        for size in (0, MAX_FRAME + 1):
            with self.assertRaises(ValueError):
                read_frame(io.BytesIO(struct.pack(">I", size)))
        with self.assertRaises(EOFError):
            read_frame(io.BytesIO(b"\0\0\0\x10{}"))

    def test_idle_heartbeats_do_not_fill_response_queue(self):
        frames = io.BytesIO()
        for _ in range(100):
            write_frame(frames, {"type": "heartbeat", "revision": "1"})
        write_frame(frames, {"type": "response", "request_id": "next", "status": "ok"})
        backend = Backend.__new__(Backend)
        backend.closed = False
        backend.output = io.BytesIO(frames.getvalue())
        backend.messages = queue.Queue(maxsize=32)
        backend._read()
        self.assertEqual(backend.messages.get_nowait()["request_id"], "next")
        self.assertIsInstance(backend.messages.get_nowait(), EOFError)

    def test_load_progress_responses_do_not_replace_the_result(self):
        backend = Backend.__new__(Backend)
        backend.closed = False
        backend.serial = 0
        backend.timeout = 1
        backend.input = io.BytesIO()
        backend.messages = queue.Queue(maxsize=32)
        for done in (0, 1):
            backend.messages.put({"type": "response", "request_id": "1", "status": "progress",
                                  "payload": {"phase": "image", "done": done, "total": 1, "detail": ""}})
        backend.messages.put({"type": "response", "request_id": "1", "status": "ok", "payload": {"opened": True}})
        result = backend.query("open", {"path": "/input", "read_only": True})
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["payload"], {"opened": True})
        self.assertTrue(backend.messages.empty())


if __name__ == "__main__":
    unittest.main()
