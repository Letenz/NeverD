# NeverD MCP

For the offline JavaScript/web analysis surface implemented entirely in C++,
use the separate [neverd-web-mcp](../neverd-web-mcp/README.md) executable.

`neverd-mcp` is one native executable, built with the worker and installed
beside it (`NeverD.app/Contents/MacOS/neverd-mcp` on macOS). It runs the
worker's engine in its own process, over the same C ABI library: no interpreter,
no worker process and no framing between a query and its answer. It has no Qt,
network listener, model provider, or agent loop. It implements MCP
**2025-11-25** JSON-RPC over newline-delimited stdio, and answers a client that
asks for 2025-06-18 in kind. Build it with `cmake --build build --target
neverd-mcp`.

## Headless use

Configure your MCP host with an explicit executable and arguments, for example:

```json
{
  "mcpServers": {
    "neverd": {
      "command": "/absolute/path/to/neverd-mcp",
      "args": ["--file", "/absolute/path/to/your-binary"]
    }
  }
}
```

The server opens only that input, read-only, as the workbench opens a file.
MCP callers cannot select other files, load plugins, edit annotations, run
targets or launch commands. Decompilation and some queries may trigger the
existing full-image analysis; read-only means no persistent project edits,
rather than no internal analysis work. While no request waits, the server runs
the workbench's idle-time analysis: function discovery, the reference index,
then each function's preparation for its first decompile, so a model's later
calls find that work done. The engine library must be the one the executable
was built with. `--worker`, which named the worker an earlier Python adapter
started, is refused.

Tools: `project_metadata`, `list_functions`, `read_disassembly`,
`read_decompilation`, `read_bytes`, `list_xrefs`, `list_strings`, `read_cfg`,
`list_imports`, `find_string_references` (the instructions and functions
that use strings matching a filter, in one call), `find_call_paths` (call
paths between functions or imports through waypoints, shortest first, as a
path finder shows them), `list_call_tree` (what a function calls, or what
calls it, to a depth) and `search` (hexadecimal bytes, text, or an
instruction's immediate value, in address order: 64 matches unless `limit`
says otherwise, continued from `next_address`; a match in code names its
function, and an immediate its instruction). `list_strings` and `list_imports` take a `filter`;
`list_strings` cuts texts to `text_limit` bytes, 200 unless asked otherwise,
with an ellipsis. Wherever a tool takes an `address`, a name the workbench
shows, an import's name or a label also works; it is resolved first.
`read_decompilation` starts at the function's definition and reports how many
`prelude_lines` (includes, types and declarations that make the file compile)
it left out; `prelude: true` returns them too. C and LLVM-C read in a lossless
shorter spelling (`Reading.cpp`): fixed-width types as `i64`/`u32`/`uptr`, the
stack frame on one line with its members named without `stack_storage.` and each
slot `var_mN` as `sN`, one
declaration per type run, temporaries `tB_I` numbered `tN` in the order the
whole function first names them, so every page of it reads them alike, and
typed accesses into byte-array globals named by address (`g_8A180`), each
listed with its type in the header's `legend`; `exact: true` returns the
engine's spelling.

Tool results are shaped for a model's context. They omit the worker's transport
envelope and the presentation data the workbench maps code with (line anchors,
variable and call-site tables, name spans): a page of pseudocode is about a
twentieth of the worker's response. Tables are `columns` plus `rows`; disassembly
and pseudocode are text under a one-line JSON header that carries the revision
and page continuation. All addresses are exact hexadecimal strings. Results keep
the worker's `revision`, status, page continuation and original addresses, and
no `structuredContent` copy of the text. Use `expected_revision` when continuing
a page; an analysis-triggering query can publish a newer revision.
`resources/list` provides metadata and the first function page.
`resources/templates/list` describes parametrized function, disassembly, C/IR
and xref resources; resources return the worker's complete JSON, project ID
included.

Results larger than 512 KiB return a `resource_link` and `resource_uri` pointing
to a session-local immutable snapshot, such as `neverd://result/1`. Read it with
`resources/read` and `?offset=0&limit=32768`; the response contains text
chunks, character offsets, `next_offset`, `complete`, project ID and revision.
Concatenate those chunks to recover the tool's text, or the resource's JSON. Continuations read the
cached revision without querying the engine again. At most four snapshots and
8 MiB of encoded snapshot data are retained; eviction is explicit when read,
and callers can repeat the original query to create a new snapshot.

## Attach to the GUI

Explicitly enable **Share current session** in the GUI, then use the displayed
private credential file:

```sh
/absolute/path/to/neverd-mcp --attach /private/session/credentials.json
```

The broker is enabled only by the user. It uses an OS-user-scoped local socket
or Windows named pipe plus a random session token, routes queries to the GUI's
existing worker, and closes connections when sharing is disabled or the GUI
exits. It does not reopen the project or fall back to a new worker. Credentials
are temporary, owner-only files and are removed when sharing ends. Keep that
file private. Opening another project or replacing the worker session revokes
sharing; enable it again explicitly for the new session. Every project tool
answers through the workbench's worker as through a server's own: the broker
passes the reads those tools make and refuses anything else, edits included.
GUI attachment additionally exposes `current_selection`,
`navigate_gui`, `highlight_gui` and `neverd://gui/selection`. Navigation is an
explicit tool call; resources never change selection. Highlighting selects the
address in the current workbench, while navigation also updates function views.

## GUI MCP client

The GUI's MCP panel accepts a manually chosen local stdio executable and argument
array, or a Streamable HTTP URL. Connections initialize only when requested;
tool calls require explicit user action and their results are displayed as data.
The history keeps at most 50 calls, preserving active calls for cancellation.
Arguments and result previews are limited to 4096 characters each and indicate
truncation. Inspecting history displays the saved record without repeating the
call. Cancelling sends a protocol notification, closes an HTTP response stream
if present, and ignores late responses; remote synchronous work may continue.
There are no sampling, model, shell, browser, or autonomous workflow capabilities.

HTTP accepts HTTPS with normal certificate verification; HTTP is allowed only
on loopback. A manually provisioned bearer token and an optional additional CA
certificate file are supported. The token is held in memory and is not saved.
Automatic OAuth discovery/registration/login is not implemented; configure a
server accepting your pre-provisioned token. Credentials are never forwarded
through redirects. Responses may be JSON or SSE. Session IDs and negotiated
protocol headers are sent on subsequent requests, expired sessions initialize
again, and resumable SSE responses retry at most three times. The client never
automatically repeats a tool call after session expiry. Unsolicited resource
subscriptions and legacy HTTP+SSE transports are not implemented.

## Budgets and cancellation

- MCP input: 1 MiB per newline-delimited message; workbench frames: 8 MiB.
- Server queue: 16 messages, answered in order by one executor; GUI client: 32
  requests.
- Inline tool/resource output: 512 KiB. Larger results become bounded cached
  resources; over 8 MiB returns an explicit budget error. Pages are at most 512 entries,
  bytes at most 4096, and CFG size remains bounded by the worker.
- Attached request timeout: 120 seconds. GUI broker: at most four authenticated
  clients, 16 pending requests per client and 32 total.
- Cancellation suppresses queued/cancelled MCP results. An already running
  synchronous analysis can continue until completion; the server does not claim
  to stop engine computation. A timed-out workbench request ends the
  attachment.

These are transport/result budgets, not guarantees about LLVM analysis memory.
The GUI client's server capabilities list currently shows its first page and
marks a further server cursor as partial. Tool results are never interpreted as
instructions or automatically fed to a model.

## Verification

```sh
cmake --build build --target neverd-mcp-tests neverd-mcp-process-tests
ctest --test-dir build -L '^mcp$' --output-on-failure
cmake -S tools/neverd-gui/mcp/tests -B build-mcp-tests -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build-mcp-tests
ctest --test-dir build-mcp-tests --output-on-failure
```

`NeverDMCP` checks the protocol, tools and result shapes against a recording
backend, the reading view, and attachment to a stand-in workbench (credential
privacy, handshake, idle events, load progress, fatal messages and timeouts).
`NeverDMCPProcess` launches the executable on the test's own binary with the
real engine and reads it as a client does. The Qt tests exercise version
negotiation, initialization ordering, exact addresses,
scope and argument validation, resource paging, result errors/budgets,
fragmented worker frames, GUI credentials and same-session routing, subprocess
stdio, request cancellation/history limits, malformed or oversized servers,
cached snapshot paging/eviction, and HTTP session/auth headers with fragmented
and resumed SSE. Build the Qt tests
inside the checkout on macOS; Qt's generated relative MOC includes can be wrong
when the build directory goes through the `/tmp` symlink.

Protocol references: [MCP transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports),
[MCP lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle).
