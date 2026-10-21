#!/usr/bin/env python3
"""NeverD read-only MCP adapter, protocol 2025-11-25, standard newline stdio."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import OrderedDict
import json
import re
import signal
import sys
import threading
from urllib.parse import parse_qs, urlparse

from transport import Backend

PROTOCOL = "2025-11-25"
MAX_LINE = 1024 * 1024
MAX_RESULT = 512 * 1024
MAX_CACHED_RESULTS = 8 * 1024 * 1024
# An exact hexadecimal address, or a name a function, import or label goes by.
ADDRESS = {"type": "string", "maxLength": 512, "pattern": "^(0x[0-9a-fA-F]{1,16}|(?!0[xX]).+)$"}
HEX_ADDRESS = re.compile(r"0x[0-9a-fA-F]{1,16}")
PAGE = {"offset": {"type": "integer", "minimum": 0, "maximum": 2147483647},
        "limit": {"type": "integer", "minimum": 1, "maximum": 512}}
REVISION = {"type": "string", "maxLength": 128}
FILTER = {"type": "string", "maxLength": 256}
# A function or import by name, or by exact hexadecimal address.
NODE = {"type": "string", "minLength": 1, "maxLength": 512}
NODES = {"type": "array", "items": NODE, "maxItems": 32}
FLAG = {"type": "boolean"}
DEPTH = {"type": "integer", "minimum": 1, "maximum": 64}
# Strings a listing shows before it cuts them, unless the caller asks otherwise.
DEFAULT_STRING_TEXT_LIMIT = 200


def tool(name, operation, description, properties=None, required=()):
    properties = dict(properties or {})
    # A revision guards the continuation of a page against analysis that
    # publishes a newer one meanwhile.
    if "offset" in properties:
        properties["expected_revision"] = REVISION
    return {"name": name, "description": description, "operation": operation,
            "inputSchema": {"type": "object", "properties": properties,
                "required": list(required), "additionalProperties": False},
            "annotations": {"readOnlyHint": True, "destructiveHint": False,
                            "idempotentHint": True, "openWorldHint": False}}


TOOLS = [
    tool("project_metadata", "metadata", "Read metadata for the explicitly opened NeverD input."),
    tool("list_functions", "functions", "Read a bounded function page; preserve revision across pages.",
         {**PAGE, "filter": FILTER}),
    tool("read_disassembly", "disasm", "Read an instruction window at an address (0x... or a name).",
         {"address": ADDRESS, "limit": PAGE["limit"]}, ("address",)),
    tool("read_decompilation", "decompile",
         "Read one function's pseudocode (\"source\": its own language; or C, Rust, Go) or IR. Starts at the "
         "definition; prelude=true adds the includes and declarations that make it compile.",
         {**PAGE, "limit": {"type": "integer", "minimum": 1, "maximum": 2048}, "address": ADDRESS, "prelude": FLAG,
          "representation": {"type": "string", "enum": [
             "source", "c", "rust", "go", "llvmc", "low", "med", "high", "llvm"]}}, ("address",)),
    tool("read_bytes", "bytes", "Read up to 4096 mapped bytes at a virtual address.",
         {"address": ADDRESS, "size": {"type": "integer", "minimum": 1, "maximum": 4096}}, ("address",)),
    tool("list_xrefs", "xrefs", "Read references associated with a virtual address.",
         {**PAGE, "address": ADDRESS, "direction": {"type": "string", "enum": ["to", "from"]}}, ("address",)),
    tool("list_strings", "strings", "Read a bounded string page; filter matches text or address. Long texts are "
         "cut to text_limit bytes (default 200) with an ellipsis.",
         {**PAGE, "filter": FILTER, "text_limit": {"type": "integer", "minimum": 1, "maximum": 1024}}),
    tool("read_cfg", "cfg", "Read the control-flow graph for one function.", {"address": ADDRESS}, ("address",)),
    tool("list_imports", "imports", "Read imported symbols by module; filter matches name or module.",
         {**PAGE, "filter": FILTER}),
    tool("find_string_references", "string_references",
         "Find the instructions, and their functions, that use strings matching filter (text, function or address).",
         {**PAGE, "filter": FILTER}),
    tool("find_call_paths", "call_paths",
         "Call paths from one function or import to another (name or 0x address), through optional via "
         "waypoints in order; shortest first. data_refs also follows taken addresses.",
         {"from": NODE, "to": NODE, "via": NODES, "exclude": NODES, "max_depth": DEPTH, "shortest_only": FLAG,
          "data_refs": FLAG, "max_paths": {"type": "integer", "minimum": 1, "maximum": 200}}, ("from", "to")),
    tool("list_call_tree", "call_tree",
         "What a function (name or 0x address) calls, or with callers what calls it, to max_depth levels "
         "(default 1).",
         {"function": NODE, "callers": FLAG, "max_depth": DEPTH, "data_refs": FLAG, "exclude": NODES},
         ("function",)),
]
SELECTION = tool("current_selection", "selection", "Read the current selection in the attached GUI session.")
NAVIGATION = [tool("navigate_gui", "navigate", "Navigate the attached GUI to an explicit virtual address.", {"address": ADDRESS}, ("address",)),
              tool("highlight_gui", "highlight", "Highlight an explicit address in the attached GUI.", {"address": ADDRESS}, ("address",))]
for _navigation in NAVIGATION:
    _navigation["annotations"]["readOnlyHint"] = False


class RpcError(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message


def validate(arguments, schema):
    if not isinstance(arguments, dict):
        raise RpcError(-32602, "arguments must be an object")
    properties = schema["properties"]
    if set(arguments) - set(properties) or set(schema["required"]) - set(arguments):
        raise RpcError(-32602, "Unknown or missing tool argument")
    for key, value in arguments.items():
        rule = properties[key]
        if rule["type"] == "integer":
            valid = type(value) is int and rule["minimum"] <= value <= rule["maximum"]
        elif rule["type"] == "boolean":
            valid = type(value) is bool
        elif rule["type"] == "array":
            item = rule["items"]
            valid = (isinstance(value, list) and len(value) <= rule["maxItems"] and
                     all(isinstance(v, str) and item["minLength"] <= len(v) <= item["maxLength"] for v in value))
        else:
            valid = isinstance(value, str) and rule.get("minLength", 0) <= len(value) <= rule.get("maxLength", 4096)
            if "enum" in rule:
                valid = valid and value in rule["enum"]
            if "pattern" in rule:
                valid = valid and re.fullmatch(rule["pattern"], value) is not None
        if not valid:
            raise RpcError(-32602, "Invalid argument: " + key)


# Model-facing results.  A tool returns what an agent reads: the worker's
# envelope, the presentation data the workbench maps code with (rows,
# variables, call sites, name spans) and repeated fields are left out, tables
# become columns and rows, and code reads as text under a one-line JSON
# header.  Resources keep the worker's complete JSON.

# Worker transport and session fields an agent never acts on.
ENVELOPE_FIELDS = ("type", "protocol_major", "request_id", "operation", "analysis_state", "project_id")
# Project bookkeeping of the workbench in a metadata payload.
METADATA_BOOKKEEPING = ("analysis_state", "dirty", "read_only", "load_options", "project")


def _pruned(value):
    """The value without null, empty-string, empty-list and empty-object members."""
    if isinstance(value, dict):
        kept = {key: _pruned(item) for key, item in value.items()}
        return {key: item for key, item in kept.items() if item not in (None, "", [], {})}
    if isinstance(value, list):
        return [_pruned(item) for item in value]
    return value


def _page_fields(payload, out):
    for key in ("total", "offset", "next_offset", "complete"):
        if key in payload:
            out[key] = payload[key]
    return out


def _table(payload, out, columns, row):
    _page_fields(payload, out)
    out["columns"] = columns
    out["rows"] = [row(item) for item in payload.get("items", []) if isinstance(item, dict)]
    return out


def _code(header, text):
    return json.dumps(header, ensure_ascii=False, separators=(",", ":")) + "\n" + text


def _metadata(payload, out):
    sha256 = (payload.get("project") or {}).get("input", {}).get("sha256")
    out.update(_pruned({key: value for key, value in payload.items() if key not in METADATA_BOOKKEEPING}))
    if sha256:
        out["sha256"] = sha256
    return out


def _functions(payload, out):
    return _table(payload, out, ["address", "name", "size", "flags"], lambda item: [
        item.get("address"), item.get("name"), item.get("size"),
        ",".join(flag for flag in ("library", "thunk", "exported") if item.get(flag))])


def _strings(payload, out):
    return _table(payload, out, ["address", "type", "length", "text"], lambda item: [
        item.get("address"), item.get("type"), item.get("length"), item.get("text")])


def _xrefs(payload, out, direction):
    other = "to" if direction == "from" else "from"
    return _table(payload, out, [other, "kind", "location"], lambda item: [
        item.get(other, item.get("address")), item.get("kind"), item.get("text") or item.get("function")])


def _imports(payload, out):
    # Names resolve wherever a tool takes an address, so a page groups them
    # by module rather than repeating module and slot on every row.
    _page_fields(payload, out)
    modules = out["modules"] = {}
    for item in payload.get("items", []):
        if isinstance(item, dict):
            modules.setdefault(item.get("module") or "", []).append(item.get("name"))
    return out


def _cut(text, limit=DEFAULT_STRING_TEXT_LIMIT):
    """A display preview of at most limit UTF-8 bytes, as the worker cuts string texts."""
    if not isinstance(text, str) or len(text.encode("utf-8")) <= limit:
        return text
    return text.encode("utf-8")[:limit].decode("utf-8", "ignore") + "…"


def _string_references(payload, out):
    return _table(payload, out, ["address", "function", "kind", "text", "via"], lambda item: [
        item.get("address"), item.get("function"), item.get("kind"), _cut(item.get("text")), item.get("via")])


def _disassembly(payload, out):
    lines = []
    for item in payload.get("items", []):
        line = f"{item.get('address')}  {item.get('mnemonic', '')} {item.get('operands', '')}".rstrip()
        if item.get("comment"):
            line += "  ; " + item["comment"]
        lines.append(line)
    out.update({key: payload[key] for key in ("address", "next_address", "complete") if key in payload})
    return _code(out, "\n".join(lines))


def _decompilation(payload, out):
    out["address"] = payload.get("address")
    out["representation"] = payload.get("dialect") or payload.get("representation")
    for key in ("prelude_lines", "offset", "total_lines", "next_offset", "complete", "unread"):
        if payload.get(key) not in (None, [], ""):
            out[key] = payload[key]
    return _code(out, payload.get("text", ""))


def _cfg(payload, out):
    out.update({key: payload[key] for key in ("name", "entry", "complete") if key in payload})
    out["nodes"] = [[node.get("id"), node.get("start"), node.get("end"), node.get("insn_count"),
                     [line.rstrip() for line in node.get("disasm", [])]] for node in payload.get("nodes", [])]
    out["edges"] = [[edge.get("from"), edge.get("to"), edge.get("type")] for edge in payload.get("edges", [])]
    out["node_columns"] = ["id", "start", "end", "insn_count", "disasm"]
    return out


def _node_labels(payload):
    """How a path or tree names each node: a function by name and address,
    an import by name; an automatic sub_ name already spells its address."""
    labels = {}
    for node in payload.get("nodes", []):
        address, name = node.get("address"), node.get("name") or node.get("address")
        if node.get("kind") == "import":
            labels[address] = name + "@import"
        elif name.lower() == "sub_" + address[2:].lower():
            labels[address] = name
        else:
            labels[address] = f"{name}@{address}"
    return labels


def _call_paths(payload, out):
    labels = _node_labels(payload)
    out["max_depth"] = payload.get("max_depth")
    lines = []
    for segment in payload.get("segments", []):
        head = labels.get(segment.get("from"), segment.get("from")) + " -> " + \
            labels.get(segment.get("to"), segment.get("to"))
        if segment.get("shortest") is None:
            lines.append(head + ": no path either way")
            continue
        notes = [f"shortest {segment['shortest']}", f"{segment.get('node_count')} nodes on paths"]
        if segment.get("reversed"):
            notes.append("only reversed")
        if not segment.get("complete", True):
            notes.append("more paths not listed")
        lines.append(head + ": " + ", ".join(notes))
        lines.extend("  " + " -> ".join(labels.get(step, step) for step in path) for path in segment.get("paths", []))
    return _code(out, "\n".join(lines))


def _call_tree(payload, out):
    labels = _node_labels(payload)
    callees = payload.get("direction") == "callees"
    out.update({key: payload[key] for key in ("direction", "depth_limited", "complete") if key in payload})
    depth = {node["address"]: node.get("depth", 0) for node in payload.get("nodes", [])}
    children, parent = {}, {}
    for edge in payload.get("edges", []):
        near, far = (edge[0], edge[1]) if callees else (edge[1], edge[0])
        if near in depth and far in depth and depth[far] == depth[near] + 1 and far not in parent:
            parent[far] = near
            children.setdefault(near, []).append(far)
    lines = []
    def walk(node, indent):
        lines.append("  " * indent + labels.get(node, node))
        for child in children.get(node, []):
            walk(child, indent + 1)
    if payload.get("nodes"):
        walk(payload["nodes"][0]["address"], 0)
    return _code(out, "\n".join(lines))


SHAPERS = {
    "metadata": _metadata, "functions": _functions, "strings": _strings, "imports": _imports,
    "string_references": _string_references, "disasm": _disassembly, "decompile": _decompilation,
    "cfg": _cfg, "call_paths": _call_paths, "call_tree": _call_tree,
}


def compact_result(operation, arguments, response):
    """(text, is_error) an agent reads for one worker response."""
    if not isinstance(response, dict):
        return json.dumps(response), True
    out = {"revision": response.get("revision")}
    status = response.get("status")
    if status not in ("ok", "success"):
        out.update({"status": status, "error": response.get("error")})
        return json.dumps(_pruned(out), ensure_ascii=False, separators=(",", ":")), True
    payload = response.get("payload")
    if not isinstance(payload, dict):
        payload = {} if payload is None else {"value": payload}
    if operation == "xrefs":
        shaped = _xrefs(payload, out, arguments.get("direction", "to"))
    elif operation in SHAPERS:
        shaped = SHAPERS[operation](payload, out)
    else:
        out.update({key: value for key, value in payload.items() if key not in ENVELOPE_FIELDS})
        shaped = out
    if isinstance(shaped, str):
        return shaped, False
    return json.dumps(shaped, ensure_ascii=False, separators=(",", ":")), False


class Server:
    def __init__(self, backend):
        self.backend = backend
        self.initialized = False
        self.ready = False
        self.tools = TOOLS + ([SELECTION, *NAVIGATION] if backend.attached else [])
        self.result_resources = OrderedDict()
        self.cached_bytes = 0
        self.result_serial = 0

    def handle(self, request):
        if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or not isinstance(request.get("method"), str):
            raise RpcError(-32600, "Invalid JSON-RPC request")
        if "id" in request and (type(request["id"]) not in (str, int)):
            raise RpcError(-32600, "Invalid request id")
        method = request["method"]
        params = request.get("params", {})
        if not isinstance(params, dict):
            raise RpcError(-32602, "params must be an object")
        if method == "initialize":
            if self.initialized:
                raise RpcError(-32600, "Session already initialized")
            if not isinstance(params.get("protocolVersion"), str) or not isinstance(params.get("capabilities"), dict) or not isinstance(params.get("clientInfo"), dict):
                raise RpcError(-32602, "Missing initialization parameters")
            self.initialized = True
            return {"protocolVersion": PROTOCOL, "capabilities": {"tools": {}, "resources": {}},
                    "serverInfo": {"name": "neverd-mcp", "version": "0.1.0"}}
        if method == "ping":
            return {}
        if method == "notifications/initialized":
            if self.initialized:
                self.ready = True
            return None
        if not self.ready:
            raise RpcError(-32002, "Initialize the MCP session first")
        if method == "tools/list":
            if params.get("cursor"):
                raise RpcError(-32602, "Unknown tool cursor")
            return {"tools": [{k: v for k, v in item.items() if k != "operation"} for item in self.tools]}
        if method == "tools/call":
            selected = next((t for t in self.tools if t["name"] == params.get("name")), None)
            if selected is None:
                raise RpcError(-32602, "Unknown tool")
            arguments = params.get("arguments", {})
            validate(arguments, selected["inputSchema"])
            arguments = dict(arguments)
            revision = arguments.pop("expected_revision", None)
            if selected["operation"] == "strings":
                arguments.setdefault("text_limit", DEFAULT_STRING_TEXT_LIMIT)
            elif selected["operation"] == "call_paths":
                arguments = {"waypoints": [arguments.pop("from"), *arguments.pop("via", []), arguments.pop("to")],
                             **arguments}
            elif selected["operation"] == "call_tree":
                arguments = {"root": arguments.pop("function"),
                             "direction": "callers" if arguments.pop("callers", False) else "callees", **arguments}
            try:
                if "address" in arguments and not HEX_ADDRESS.fullmatch(arguments["address"]):
                    # A name, resolved as the workbench resolves what it shows;
                    # code views take the entry of the function it lies in.
                    resolved = self.backend.query("resolve", {"query": arguments["address"]}, revision)
                    if resolved.get("status") not in ("ok", "success"):
                        text, _ = compact_result("resolve", {}, resolved)
                        return self.text_result(text, True, resolved)
                    found = resolved.get("payload") or {}
                    entry = found.get("function_address") if selected["operation"] in ("decompile", "cfg") else None
                    arguments["address"] = entry or found.get("address")
                prelude = arguments.pop("prelude", False)
                result = self.backend.query(selected["operation"], arguments, revision)
                lines = ((result.get("payload") or {}).get("prelude") or {}).get("lines") \
                    if selected["operation"] == "decompile" and isinstance(result, dict) else None
                if not prelude and lines and not arguments.get("offset"):
                    # A source page starts at the definition: the lines before it
                    # make the file compile and are read on request.
                    result = self.backend.query(selected["operation"], {**arguments, "offset": lines},
                                                result.get("revision"))
                    if isinstance(result.get("payload"), dict):
                        result["payload"]["prelude_lines"] = lines
                text, is_error = compact_result(selected["operation"], arguments, result)
                return self.text_result(text, is_error, result)
            except (OSError, EOFError, ValueError, RuntimeError, TimeoutError) as error:
                return {"content": [{"type": "text", "text": str(error)}], "isError": True}
        if method == "resources/list":
            if params.get("cursor"):
                raise RpcError(-32602, "Unknown resource cursor")
            resources = [{"uri": "neverd://project/metadata", "name": "Project metadata", "mimeType": "application/json"},
                         {"uri": "neverd://project/functions", "name": "Functions (first page)", "mimeType": "application/json"}]
            if self.backend.attached:
                resources.append({"uri": "neverd://gui/selection", "name": "GUI selection", "mimeType": "application/json"})
            for uri in self.result_resources:
                resources.append({"uri": uri, "name": "Result snapshot " + uri.rsplit("/", 1)[-1],
                                  "mimeType": self.result_resources[uri]["mime_type"]})
            return {"resources": resources}
        if method == "resources/templates/list":
            return {"resourceTemplates": [
                {"uriTemplate": "neverd://project/functions{?offset,limit,filter,expected_revision}", "name": "Function page", "mimeType": "application/json"},
                {"uriTemplate": "neverd://project/disasm{?address,limit,expected_revision}", "name": "Disassembly", "mimeType": "application/json"},
                {"uriTemplate": "neverd://project/decompile{?address,representation,offset,limit,expected_revision}", "name": "C and IR", "mimeType": "application/json"},
                {"uriTemplate": "neverd://project/xrefs{?address,direction,offset,limit,expected_revision}", "name": "Cross references", "mimeType": "application/json"}]}
        if method == "resources/read":
            return self.read_resource(params.get("uri"))
        if method.startswith("notifications/"):
            return None
        raise RpcError(-32601, "Method not found")

    def tool_result(self, result):
        """The worker's complete response as JSON, as resources return it."""
        encoded = json.dumps(result, ensure_ascii=False, separators=(",", ":"))
        return self.text_result(encoded, result.get("status") not in ("ok", "success"), result,
                                "application/json", "json-text")

    def text_result(self, text, is_error, result, mime_type="text/plain", encoding="text"):
        """The text inline, or a bounded snapshot link when it is too large to be."""
        size = len(text.encode("utf-8"))
        if MAX_RESULT < size <= MAX_CACHED_RESULTS:
            while self.result_resources and (len(self.result_resources) >= 4 or self.cached_bytes + size > MAX_CACHED_RESULTS):
                _, expired = self.result_resources.popitem(last=False)
                self.cached_bytes -= expired["bytes"]
            self.result_serial += 1
            uri = "neverd://result/" + str(self.result_serial)
            self.result_resources[uri] = {"text": text, "bytes": size, "mime_type": mime_type, "encoding": encoding,
                                          "revision": result.get("revision"), "project_id": result.get("project_id")}
            self.cached_bytes += size
            reference = {"status": result.get("status"), "project_id": result.get("project_id"),
                         "revision": result.get("revision"), "resource_uri": uri,
                         "total_characters": len(text), "inline_complete": False}
            return {"content": [{"type": "resource_link", "uri": uri, "name": "NeverD result snapshot",
                                 "mimeType": mime_type, "description": "Read this snapshot in character pages using offset and limit URI parameters."},
                                {"type": "text", "text": json.dumps(reference)}],
                    "structuredContent": reference, "isError": is_error}
        if size > MAX_CACHED_RESULTS:
            text = json.dumps({"status": "budget_exceeded", "revision": result.get("revision"),
                               "error": {"message": "Result exceeds MCP 8 MiB snapshot budget; request a smaller page/window."}})
            is_error = True
        # No structuredContent: no tool declares an output schema, and a copy
        # of the text would double what a client may forward to its model.
        return {"content": [{"type": "text", "text": text}], "isError": is_error}

    def read_resource(self, uri):
        if not isinstance(uri, str):
            raise RpcError(-32602, "uri must be a string")
        parsed = urlparse(uri)
        if parsed.scheme != "neverd" or parsed.fragment or parsed.username or parsed.port:
            raise RpcError(-32602, "Unsupported resource URI")
        if parsed.netloc == "result":
            return self.read_result_resource(uri, parsed)
        operation = parsed.path.lstrip("/")
        if parsed.netloc == "gui" and operation == "selection" and self.backend.attached:
            selected = SELECTION
        elif parsed.netloc == "project":
            selected = next((t for t in TOOLS if t["operation"] == operation), None)
        else:
            selected = None
        if selected is None:
            raise RpcError(-32002, "Resource not found")
        arguments = {}
        for key, values in parse_qs(parsed.query, keep_blank_values=True).items():
            if len(values) != 1:
                raise RpcError(-32602, "Repeated resource parameter")
            value = values[0]
            if key in ("offset", "limit", "size"):
                try:
                    value = int(value)
                except ValueError as error:
                    raise RpcError(-32602, "Invalid numeric resource parameter") from error
            arguments[key] = value
        validate(arguments, selected["inputSchema"])
        revision = arguments.pop("expected_revision", None)
        result = self.tool_result(self.backend.query(operation, arguments, revision))
        if result["isError"]:
            raise RpcError(-32002, result["content"][-1]["text"])
        return {"contents": [{"uri": uri, "mimeType": "application/json", "text": result["content"][-1]["text"]}]}

    def read_result_resource(self, uri, parsed):
        if re.fullmatch(r"/[1-9][0-9]*", parsed.path) is None:
            raise RpcError(-32002, "Result snapshot not found")
        key = "neverd://result" + parsed.path
        record = self.result_resources.get(key)
        if record is None:
            raise RpcError(-32002, "Result snapshot expired or not found; repeat the original query")
        parameters = parse_qs(parsed.query, keep_blank_values=True)
        if set(parameters) - {"offset", "limit", "expected_revision"} or any(len(values) != 1 for values in parameters.values()):
            raise RpcError(-32602, "Invalid result page parameters")
        try:
            offset = int(parameters.get("offset", ["0"])[0])
            limit = int(parameters.get("limit", ["32768"])[0])
        except ValueError as error:
            raise RpcError(-32602, "Invalid result page range") from error
        if offset < 0 or offset > len(record["text"]) or not 1 <= limit <= 65536:
            raise RpcError(-32602, "Result page is outside its bounds")
        expected = parameters.get("expected_revision", [None])[0]
        if expected is not None and expected != record["revision"]:
            raise RpcError(-32602, "Result snapshot revision does not match")
        end = min(offset + limit, len(record["text"]))
        page = {"project_id": record["project_id"], "revision": record["revision"],
                "encoding": record["encoding"], "offset": offset, "total_characters": len(record["text"]),
                "text": record["text"][offset:end], "next_offset": end if end < len(record["text"]) else None,
                "complete": end == len(record["text"])}
        return {"contents": [{"uri": uri, "mimeType": "application/json", "text": json.dumps(page, ensure_ascii=False)}]}


def serve(server, source, sink):
    lock = threading.Lock()
    pending = {}
    executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="neverd-mcp")

    def emit(response):
        with lock:
            sink.write(json.dumps(response, ensure_ascii=False, separators=(",", ":")) + "\n")
            sink.flush()

    def respond(request, cancel, slot):
        request_id = request.get("id") if isinstance(request, dict) else None
        if cancel.is_set():
            with lock:
                pending.pop(slot, None)
            return
        try:
            result = server.handle(request)
            response = {"jsonrpc": "2.0", "id": request_id, "result": result}
        except RpcError as error:
            response = {"jsonrpc": "2.0", "id": request_id,
                        "error": {"code": error.code, "message": error.message}}
        except Exception as error:
            response = {"jsonrpc": "2.0", "id": request_id,
                        "error": {"code": -32603, "message": str(error)}}
        invalid_request = response.get("error", {}).get("code") == -32600
        if not cancel.is_set() and (invalid_request or not isinstance(request, dict) or "id" in request):
            emit(response)
        with lock:
            pending.pop(slot, None)

    try:
        while True:
            line = source.readline(MAX_LINE + 1)
            if not line:
                break
            if len(line) > MAX_LINE:
                emit({"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "MCP message exceeds 1 MiB"}})
                break
            try:
                request = json.loads(line)
            except (ValueError, UnicodeError):
                emit({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Invalid JSON"}})
                continue
            if isinstance(request, dict) and request.get("method") == "notifications/cancelled":
                params = request.get("params", {})
                request_id = params.get("requestId") if isinstance(params, dict) else None
                if type(request_id) in (str, int):
                    with lock:
                        cancellation = pending.get(request_id)
                    if cancellation:
                        cancellation.set()
                continue
            request_id = request.get("id") if isinstance(request, dict) else None
            if request_id is not None and type(request_id) not in (str, int):
                emit({"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Invalid request id"}})
                continue
            cancel = threading.Event()
            slot = request_id if request_id is not None else object()
            with lock:
                overloaded = len(pending) >= 16 or (request_id is not None and request_id in pending)
                if not overloaded:
                    pending[slot] = cancel
            if overloaded:
                if request_id is not None:
                    emit({"jsonrpc": "2.0", "id": request_id, "error": {"code": -32000, "message": "Request queue full or duplicate id"}})
                continue
            executor.submit(respond, request, cancel, slot)
    finally:
        executor.shutdown(wait=True)


def main():
    if sys.version_info < (3, 10):
        print("neverd-mcp: Python 3.10+ is required; configure a supported interpreter on PATH", file=sys.stderr)
        return 1
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", help="Absolute path to the matching neverd-worker executable")
    parser.add_argument("--file", help="Local input, explicitly scoped to this MCP session")
    parser.add_argument("--attach", help="Private credential file exported by an open GUI session")
    args = parser.parse_args()
    if bool(args.attach) == bool(args.worker or args.file) or (not args.attach and not (args.worker and args.file)):
        parser.error("Use either --worker ABSOLUTE_PATH --file INPUT, or --attach CREDENTIAL_FILE")
    backend = None
    try:
        backend = Backend(worker=args.worker, path=args.file, attach=args.attach)
        def terminate(_signal, _frame):
            backend.close()
            raise SystemExit(0)
        signal.signal(signal.SIGTERM, terminate)
        signal.signal(signal.SIGINT, terminate)
        serve(Server(backend), sys.stdin.buffer, sys.stdout)
        return 0
    except (OSError, ValueError, RuntimeError, EOFError) as error:
        print("neverd-mcp: " + str(error), file=sys.stderr)
        return 1
    finally:
        if backend:
            backend.close()


if __name__ == "__main__":
    sys.exit(main())
