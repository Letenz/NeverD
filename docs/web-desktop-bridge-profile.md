# Desktop bridge source syntax

`tauri-1-wails-2-direct-bridge-syntax-v1` adds `desktop_bridges` pages to the
existing source-navigation operation. It recognizes bounded, directly written
Tauri 1 and Wails 2 member-call shapes and preserves exact source anchors.
Every result has `confidence: source_syntax_only`. A match does not authenticate
a framework API, identify a native callee or prove that the bridge is active.

## Selected grammar

| Form | Direct syntax admitted |
| --- | --- |
| `window_ipc_message` | `window.__TAURI_IPC__(message)` with exactly one argument |
| `window_global_invoke` | `window.__TAURI__.tauri.invoke(command[, payload])` |
| `window_go_package_struct_method` | `window.go.Package.Struct.Method(arguments)` with exactly three target member names after `go` |

The shared parser supplies decoded identifier/string spellings. Static string
brackets and optional member/call syntax are retained. Computed expressions,
aliases, imports, call-return forwarding, other roots and shorter Wails chains
remain outside this grammar. Ordinary constructors, tags and dynamic imports
keep their existing navigation records without becoming bridge candidates.
Tauri 2 `core`/internal bridges and Wails event-runtime mapping are separate work.

For the IPC form, a direct object can retain ordinary `cmd`, `callback` and
`error` value nodes. A spread, computed key, duplicate role, role accessor/method,
missing command, non-object message or wrong arity keeps `unresolved_shape`.
Spread arguments also remain unresolved. Literal command bytes and package,
struct, method or argument values are never published. `command_status`
distinguishes a redacted string literal, dynamic expression, unresolved argument
shape and redacted static Wails member names. Callback fields are value-node
anchors, not callback runtime identities.

`root_resolution` comes from the authoritative shared binding analysis.
External `window`, lexical shadowing and dynamic `with` scope stay distinct.
Even an external root does not prove the property value, its mutation history,
getters, native registration or runtime target. These limitations are explicit
in both summary and records. A zero-row page does not prove absence of bridges;
`bridge_inventory_complete` remains false.

The vocabulary is independently implemented from syntax observations in the
[Tauri 1.3.0 API source](https://raw.githubusercontent.com/tauri-apps/tauri/tauri-v1.3.0/tooling/api/src/tauri.ts)
and [Wails 2.11.0 binding runtime](https://raw.githubusercontent.com/wailsapp/wails/v2.11.0/v2/internal/frontend/runtime/desktop/bindings.js).
Those source references do not detect a captured binary's framework version or
authenticate a call with the same spelling.

## Ownership and limits

`desktop/bridge/Source` consumes the existing source, bindings and navigation
models. It validates their identities and call/child agreement, admits the
closed grammar and owns independent bridge identities. `Publication` owns
fixed metadata vocabulary. `SessionNavigation` serializes and caches all three
navigation results together; reimport revokes their source selection and pages.
Generic function/call/reference IDs and the navigation profile are unchanged.

Existing parser admission remains 1 MiB and 100,000 nodes per source. The bridge
owner adds 2,000,000 charged work steps and 1,024 records. Model validation,
indexing, chain walks, property comparisons and argument scans are charged.
Invalid models or exhausted limits discard the entire bridge candidate list;
generic navigation remains independently available. Existing session/source
cache limits and 512-row pagination apply. These are algorithm/storage bounds,
not a whole-process RSS claim.

The existing C API, worker and MCP navigation operations expose the same
summary and `desktop_bridges` record kind. `neverd web navigate <file> <mode>`
prints all existing navigation pages plus bridge pages. No new transport
operation, source execution or native-bridge invocation is added. Existing
`source_anchor` resolves a row's UTF-8 byte range through immutable source
storage, preserving compressed frames and parent container origins.

## Preserved release evidence

The [Tauri asset qualification](web-tauri-profile.md) preserves the public
lencx/ChatGPT v1.1.0 Mach-O x64 release and its original old-GNU tar/gzip delivery.
Decoded JS member 6 is 85,197 bytes, SHA-256
`e22caa4af06a1d65ff970b54a842df47333ccdee0f1150487f48410bb953453f`.
Its selected direct IPC call occupies byte offset 3,013 and length 53. The root
is external; the message spread leaves the command shape unresolved. The
existing source anchor retains decoded offset 3,013/length 53 separately from
the stored Brotli frame at native-container offset 5,069,463/length 24,569 and
the original archive chain. No command, native target or runtime callback is
inferred from that evidence. This is a fixed selected release/member, not a
latest-release claim or a complete application bridge inventory.

Authored C++ fixtures check exact direct shapes, shadowing, optional syntax,
spread/getter/duplicate/computed/arity refusals, independent record budgets and
malformed navigation. SDK/CLI and direct/framed worker/MCP tests check private
value canaries, shared anchors and pagination. The optional real cases use
`NEVERD_TAURI_CHATGPT_110_X64_JS` and `NEVERD_TAURI_CHATGPT_110_X64_TGZ`;
they never download, execute or install a target.
