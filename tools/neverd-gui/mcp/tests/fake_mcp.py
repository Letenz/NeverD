"""Protocol fixture: stdin/stdout only, no NeverD dependency."""
import json
import signal
import sys

if len(sys.argv) > 1 and sys.argv[1] in ("malformed", "oversized"):
    # Console processes can still deliver buffered stdout after termination
    # was requested on Windows. Exercise that ordering on every host.
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    print("not JSON" if sys.argv[1] == "malformed" else "x" * (8 * 1024 * 1024 + 1), flush=True)
    sys.stdin.read()
    print("late invalid JSON", flush=True)
    raise SystemExit(0)

for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        if request["method"] == "notifications/cancelled":
            print(json.dumps({"jsonrpc": "2.0", "id": request["params"]["requestId"],
                              "result": {"content": [{"type": "text", "text": "late response"}]}}), flush=True)
        continue
    method = request["method"]
    result = {}
    if method == "initialize":
        result = {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}, "resources": {}},
                  "serverInfo": {"name": "fixture", "version": "1"}}
    elif method == "tools/list":
        result = {"tools": [{"name": "echo", "inputSchema": {"type": "object"}}]}
    elif method == "resources/list":
        result = {"resources": [{"name": "Fixture", "uri": "fixture://data"}]}
    elif method == "tools/call":
        if request["params"]["name"] == "hold":
            continue
        result = {"content": [{"type": "text", "text": json.dumps(request["params"]["arguments"])}]}
    elif method == "resources/read":
        result = {"contents": [{"uri": "fixture://data", "text": "fixture resource"}]}
    print("fixture diagnostic", file=sys.stderr, flush=True)
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
