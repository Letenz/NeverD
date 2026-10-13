# Offline web MCP profile

Profile `neverd-offline-web-mcp-v1` implements the stdio transport and tool
surface of MCP **2025-06-18** in C++. This is a separate executable from the
existing native-binary Python MCP adapter. It adds no analysis semantics or
new C ABI: both C++ transports use `tools/common/web/Backend`, and the engine's
source-domain sessions remain authoritative for evidence and publication.
Build and client configuration are in the [tool README](../tools/neverd-web-mcp/README.md).

## Protocol and input contract

Supported methods are `initialize`, `ping`, `tools/list` and `tools/call`, plus
`notifications/initialized`. Initialization transitions through uninitialized,
awaiting initialized notification and ready. The response selects the supported
`2025-06-18` even when the client proposes another version; the client decides
whether it can proceed. Repeated initialization fails without resetting
publication. Ping works before initialization. Notifications never invoke
analysis or receive replies; unsolicited responses are ignored. There are no
server-initiated requests, HTTP, prompts, resources or GUI attachment.

Each UTF-8 JSON-RPC object ends with LF; CRLF is also accepted. Batches, null,
floating or Boolean request IDs and duplicate decoded object keys are refused.
String IDs (including empty) and signed/unsigned integer IDs retain their exact
JSON values. IDs are the intentional correlation echo; callers should not put
private evidence in them. Arguments have closed schemas. Bounded standard
`params._meta` is accepted but is neither forwarded to the engine nor echoed.
Protocol errors use fixed messages. Unknown/unavailable tools and malformed
arguments use `-32602`; analysis failures after argument validation use MCP
`isError` with a fixed C API diagnostic code. Parser exceptions and input paths
are never returned as diagnostic text.

At most eight repeatable `--input PATH` options configure private inputs. Tool
calls select an `input_index`, never another path. No startup capture occurs;
the existing no-follow C++ reader captures bytes at import preview. Commit
publishes that exact preview and revokes the previous revision. Selection is
process-local configuration, not a persistent project identifier or OS access
sandbox. The backend still owns all capture budgets and filesystem rules.

## Tools and evidence

The sorted catalog is the intersection of the MCP descriptors and the loaded
backend's advertised operations. Discovery returns at most 16 tools per page,
with a validated opaque `nextCursor`. The capability tool is always present;
an omitted backend exposes no analysis tools. Parser-only tools disappear
when the embedded parser is disabled. No-input launches omit import preview.

Tools use `neverd_web_` names and schema version 1; `schema_version` defaults to
1 when omitted. The catalog covers import, metadata, artifacts, source/maps,
bindings, primitive semantics and primitive/lexical rewrite receipts,
modules/bundles/navigation, structural views,
anchors, Bun/SEA/nexe/pkg/ASAR/Wails/Tauri extraction, package/archive/integrity/diff evidence,
selected Node source/package behavior metadata and installation traces,
Electron/HTML, passive HAR/streams/interfaces and native loader metadata.
Published IDs, decimal-string revisions/coordinates, evidence classes and
backend pagination fields remain unchanged. Tool responses contain the same
evidence object in `structuredContent` and equivalent JSON in one text block.
Clients should discover schemas rather than assume disabled capabilities.

Tauri selection adds a canonical decimal-string `initializer_address` to the
explicit artifact/profile/revision selection. The shared native pipeline
derives the descriptor; tools cannot accept caller-supplied tables or pointer
proofs. Complete PHF/Brotli validation precedes metadata-only member pages.
Raw Tauri export is omitted from MCP. See the [Tauri profile](web-tauri-profile.md).

Existing source navigation also publishes the independently versioned
`desktop_bridges` record kind. Direct Tauri/Wails candidates retain shared
source/call/binding IDs, redacted argument roles and incomplete API/native-target
claims. No additional tool is added; parser omission removes navigation as
before. The same source-anchor operation resolves each row through its original
or decoded storage. See the [bridge profile](web-desktop-bridge-profile.md).

The same records tool also admits `config_merges`, under its independent
configuration-object-order profile. Ordered terms and lexical loading syntax
links retain source spans and unknown-key barriers while configuration values
stay private. Loader identity and effective configuration remain unverified.
See the [configuration profile](web-configuration-order-profile.md).

`neverd_web_package_install_analyze` requires revision and package analysis ID;
the matching package behavior ID is optional. No source selection is needed to
report manifest-only or lock-only coverage gaps. Its records tool pages
`scripts`, `sources`, `requests` and `associations`; all retain the backend's
static confidence and privacy limits. See the
[installation profile](web-package-install-profile.md).

`neverd_web_package_behavior_diff_analyze` selects a retained `package_diff_id`
and optional `before_package_behavior_id` / `after_package_behavior_id` from
its exact analyses. Its records tool pages `pairs`, `patterns` and `occurrences`.
Missing or degraded source coverage remains incomparable; pattern counts do
not establish call correspondence or a safety verdict. See the
[comparison profile](web-package-behavior-diff-profile.md).

The four package advisory tools import captured OSV/source/time artifacts,
page affected/severity declarations, match explicit advisory IDs to an existing
package analysis and page original match anchors. They remain available with
JavaScript parsing omitted. Missing or unsupported evidence remains explicit;
the tools never fetch a feed, validate a publisher or infer exploitability.
See the [advisory profile](web-package-advisory-profile.md).

The four package attestation tools import explicit captured registry/in-toto/
DSSE metadata, page its signature/subject/digest declarations, compare one
selected digest with original bytes and retrieve that receipt. They remain
available without JavaScript parsing. No tool accepts a key path, fetch URL,
verification flag or raw-payload switch. Signature verification and package
inventory binding remain unverified; see the
[attestation profile](web-package-attestation-profile.md).

The DataPack tools select explicit standard v5/u16 or measured WebView2 v5/u32
resource tables, page exact original/alias ranges and explicitly decode a selected
gzip stream. They use the same C++ SDK and retained storage origins; numeric IDs
imply no filenames, framework identity or runtime routes. The derived selection
uses ordinary source/HTML consumers. Raw DataPack export is excluded.

MCP does not expose source review options or raw export. Structural source
views replace private identifiers/literals/comments according to the shared
view policy. A preview token remains a backend publication token, not proof
that a human reviewed or authorized disclosure. Native handoff is limited to
loader metadata; `native_analyze` is omitted. HAR/stream tools never replay
traffic or access the network. Source inference and recorded observations keep
their separate evidence classes. Tool annotations mark metadata reads as
read-only; analyses and publication can mutate process-local caches.
Rewrite tools expose the explicitly selected `string_recovery`,
`constant_folding`, `scope_renaming` or `boolean_branches` pass and paged
profile-specific receipts. Lexical graph validation and Boolean-condition
selection stay distinct from primitive SameValue evidence. Chosen branch
effects remain in the candidate and are not assumed absent.
Original/replacement text and per-patch literal
hashes stay private; `source_rewrite_export` is local C API/CLI only.

## Resource bounds and ownership

| Boundary | Limit |
| --- | --- |
| Request line, before LF | 64 KiB (an optional CR counts) |
| JSON nesting | 64 levels |
| ID / method / tool name | 128 UTF-8 bytes |
| Configured inputs | 8 paths, at most 32 KiB each |
| Evidence JSON before content duplication | 2 MiB |
| Complete encoded response before LF | 8 MiB |
| Metadata page | 128 or 512 items, according to operation |
| Structural view chunk | 64 KiB |

The shared JSON serializer uses a capped stream buffer, including escape
expansion. A complete bounded response is constructed before stdout writes.
An oversized or unterminated input causes a fixed error and process exit
without unbounded draining. Parsing and dispatch are serial, with no transport
queue or background workers. There is no preemptive cancellation or hard
wall-clock backend deadline: closing the client does not interrupt an analysis
already executing. Backend budgets still apply. Deep native analysis is not
in this initial profile.

`tools/common/transport/Json` owns generic bounded JSON admission and output.
Worker-specific framing and native address validation stay in worker Protocol.
`Catalog` owns MCP schemas; `WebTools` adapts input indices and envelopes;
`Server` owns lifecycle/RPC; `Stdio` owns byte framing; `Main` owns launch options.
Neither common transport target links LLVM nor spawns an analysis executable.

## Qualification and limits

The C++ suites `NeverDWebMCPProtocol`, `NeverDWebMCPCatalog`, `NeverDWebMCPWeb`
and `NeverDWebMCPProcess` cover protocol admission, capability subsets,
redaction, backend parity and a real stdio server with an unusable PATH.
The separate `NeverDWebMCPClaude` case uses the optional
`NEVERD_CLAUDE_CODE_21296_ELF` input. It checks the pinned size/hash and all
2,589 module records over actual MCP pages, preserving the 2,345 JS / 244 asset
partition and 2,343 opaque caches. It skips when the sample is absent or capture
is unavailable; tests never download or execute this sample.
The process harness currently runs on POSIX hosts; it explicitly skips on
Windows. Windows Unicode launch conversion and binary stdio are implemented,
but Windows native capture, packaging and runtime qualification remain open.
Standalone dependency/ABI compatibility must be tested with the actual shipped
engine. The existing Python capability-owner checker does not validate this
MCP surface; its C++ catalog and integration suites do.

Raw desktop-resource export is also excluded from MCP. Selected passive reports
use committed IDs and the shared metadata policy. The current
[acceptance audit](web-epic-acceptance.md) owns five-epic completion claims;
the [implementation ledger](web-analysis-implementation.md) retains exact checks.

Primary protocol references: [stdio](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports),
[lifecycle](https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle),
[tools](https://modelcontextprotocol.io/specification/2025-06-18/server/tools).
