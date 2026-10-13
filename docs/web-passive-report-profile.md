# Selected passive reports

`passive-evidence-shape-report-v2` composes explicit source-request analyses,
committed HAR captures and/or committed streams. Source-only, observation-only
and selected combinations use the same backend. It is a partial evidence report,
not OpenAPI, a server schema, a traffic replay or an execution trace.

The additive `neverd_web_passive_report_create_json` accepts `schema_version:1`
and optional `interface_analysis_id`, `har_capture_id`, `stream_capture_id`.
At least one must be selected. Observations must already be committed through
redaction preview. Unknown keys, uncommitted captures, stale revisions and
unavailable source selections refuse. `neverd_web_passive_report_records_json`
pages 1..128 rows. Worker/MCP expose corresponding `web_passive_report_create`
and `web_passive_report_records` operations with evidence IDs, never raw text.

## Evidence and schema meaning

Requests, request/result/transform relations and tool/configuration/permission/
hook/MCP candidates retain their source, node and function anchors under
`static_inference`. HTTP/stream records and observed JSON structures remain
`imported_observation`. HTTP joins delegate to the existing exact method/origin/
path owner and remain `candidate_correlation`, never observed source execution.

Direct literal JSON-RPC message objects passed as call arguments retain object,
call and function anchors. Only ordinary unique non-computed properties permit
method classification. Spread/accessor/duplicate/computed properties and dynamic
methods remain unknown. Existing `callTool`/`listTools` syntax features supply
separate method spelling candidates. One shared MCP 2025-06-18 vocabulary serves
source and streams. Only the explicitly selected MCP stream profile permits
same-known-method candidates. JSONL, generic JSON-RPC, logs and SSE inherit no
MCP identity. API/transport identity remains unverified.

Same-method candidates express spelling agreement only. They do not join
recorded request IDs, infer sessions/direction/order or reconstruct a response.
Recorded response links come only from the stream owner's typed-ID/context/
order rules. Other client feature domains and function anchors remain available;
proximity proves no configuration, permission or hook relationship to a message.

This separately selected report profile inspects HAR body text after commit,
only for unencoded `application/json` or `application/*+json` content. It retains
the top-level kind/member count and JSON type histogram, plus bounded example
trees and capture-local field aggregates. Keys, values, arbitrary
MIME strings and raw examples stay withheld; examples are observation IDs.
Optionality, server constraints and completeness remain `unknown`. Observed
presence establishes no required fields or behavior on other branches. Missing,
encoded, invalid and oversized bodies have distinct statuses. Decoded body
offsets stay null rather than inventing direct original byte positions.

### Observed field and array evidence

`observed_json_value_node` rows preserve each example's hierarchy.
`structure.node_id` and `parent_node_id` link the tree; `edge` distinguishes
roots, object fields and array elements. `ordinal` is the sorted field position
within this example or the actual array index. `observed_example_id` points to
the original request/response evidence. No body byte offsets are fabricated.

`schema_node_id` links each value to an `observed_json_schema_node` row.
Its type histogram is local to that structural field, rather than the entire
body. Private dictionaries associate identical decoded field names at the same
parent. Public IDs use capture/structural ordinals, never hashes of private names.
New fields receive stable capture-local identities in first-observed order,
with each object's keys privately sorted. Array elements share one aggregate
path, while individual examples keep their actual indexes. Fields with the same
spelling under different parents remain distinct.

Groups reuse the captured method and normalized origin/path evidence; request
and response bodies are separate. Query values do not select a group. A method
outside the fixed vocabulary, relative/unsupported URL or credentials disables
cross-example grouping for that observation. This does not change source/HAR
correlation. Response status variants in a group contribute observed type unions,
not inferred server alternatives.

For an object field, `observed_instances` counts appearances and
`eligible_parent_instances` counts observed object parents at that path.
`observed_missing_instances` is their difference. Null is an observed type;
non-object parents, missing/invalid/encoded bodies and unselected traffic cannot
inflate the denominator. Array element counts are not field presence rates.
Aggregate rows have no single example ID; their value nodes supply the example
links. Requiredness, server optionality and general schema completeness stay
unknown.

### Source request components

`source_request_structure` rows consume the existing admitted request roles.
They describe URL literal/template segments, interpolation expressions, direct
header properties and body literal/object/array syntax. A directly linked
`JSON.stringify` body exposes its first-argument syntax under a separate
`json_stringify_input` domain; it does not assert serialized output or builtin
identity. Other calls, deferred functions and unresolved aliases remain unknown.
Spreads, getters, computed/duplicate properties and special prototype keys retain
explicit unknown property rows. Earlier duplicate/spread-adjacent rows are
individual syntax occurrences, never a computed effective object.

URL `lexical_component` rows classify scheme/network prefixes, authority,
path segments/separators, query names/values/separators and fragments without
publishing their text. `component_index` relates path or query spelling
positions; path positions include empty segments. Original byte coordinates
refer to the containing AST segment or expression. `decoded_utf16_offset` and
`decoded_utf16_length` refer only to a segment's private cooked text, not raw
bytes. Escapes therefore never manufacture narrower original ranges.

Interpolations can insert delimiters. Every component is lexical syntax, and
components after a hole explicitly retain that limitation. No template route,
server parameter name, runtime request or new HAR match is inferred. Header
names use only the existing fixed/sensitive/other vocabulary; all other field
names and values remain withheld.

## CLI and ownership

```sh
neverd web report-source source.js module
neverd web report-har root HAR_INDEX passive-interface-metadata-v1 PREVIEWED_SHA256
neverd web report-correlate root module SOURCE_INDEX HAR_INDEX \
  passive-interface-metadata-v1 PREVIEWED_SHA256
neverd web report-stream root mcp-2025-06-18-stdio-jsonl-v1 STREAM_INDEX \
  passive-stream-metadata-v1 PREVIEW_RECEIPT [SOURCE_INDEX module]
```

Use existing HAR/stream preview commands for receipts. Import repeats the preview
and checks original bytes/profile/policy. Each invocation prints all report pages.

`interfaces/report/Source` consumes authoritative frontend evidence;
`SourceStructure`/`URLStructure` project admitted request syntax. `Shapes`
owns body admission, `ObservedStructure` owns private dictionaries and observed
trees, `Compose` owns selected candidate composition and
`Publication` owns closed JSON. `SessionPassiveReport` owns locking, atomic
serialize-before-publish admission and two revision-bound caches. Reimport clears
reports; identical selections reuse identities. Existing evidence IDs are
unchanged. Report/record IDs change with the v2 report profile. Selection schema
and C ABI entry points stay unchanged; consumers must accept the additional
record kinds and nullable `structure` object. Extended relation semantics use
`request-source-relations-v2`.

Limits: 16,384 records, 8,000,000 projection/composition steps, 16 MiB canonical
metadata/report and 128 rows/page. JSON bodies have 64 KiB text, depth 16 and
200,000 aggregate admission nodes across bodies, including failures. Field/group
dictionaries have a 2 MiB private-byte limit. Source value trees stop at depth
16 with an explicit syntax limitation. Aggregate
exhaustion aborts report publication; byte/depth limitations retain no fake shape.
Existing analyses retain independent limits and remain available. HAR/stream-only
reports work without the JS parser; backend omission exports unavailable ABIs.

C++ tests verify independent types, all feature domains, exact anchors, unknown
methods, framing independence, body/work refusals, canaries, paging/revocation
and SDK/CLI/worker/MCP parity. The pinned Claude 2.1.296 macOS ARM64 module 928
also passes source-only reporting through NeverD with an unusable external-tool
PATH. No real HAR/transcript was supplied for that module or invented.

The optional `WebRequestStructure.CapturedClaudeTemplatesKeepOriginalAnchorsWhenSupplied`
uses `NEVERD_CLAUDE_CODE_21296_CROSS_DIR` and NeverD's preserved macOS ARM64
exports. Module 1357 is 15,187 bytes, SHA-256
`d0e0757751dec0b768fcd07d5430eb8d3ea785fbdb135ed021fdacbe255e9dc8`;
module 1740 is 87,530 bytes, SHA-256
`c3ac807c5931807f85455223fccc717deac2148575bc8de9e73e039a6ec4ae8b`.
Both retain three template segments and two original expression anchors.
Their unresolved values remain unknown and create no invented observations.
