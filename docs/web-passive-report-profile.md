# Selected passive reports

`passive-evidence-shape-report-v1` composes explicit source-request analyses,
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
`static_inference`. HTTP/stream records and JSON type summaries remain
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
the top-level kind/member count and JSON type histogram. Keys, values, arbitrary
MIME strings and raw examples stay withheld; examples are observation IDs.
Optionality, server constraints and completeness remain `unknown`. Observed
presence establishes no required fields or behavior on other branches. Missing,
encoded, invalid and oversized bodies have distinct statuses. Decoded body
offsets stay null rather than inventing direct original byte positions.

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

`interfaces/report/Source` consumes authoritative frontend evidence; `Shapes`
owns body inspection, `Compose` owns selected candidate composition and
`Publication` owns closed JSON. `SessionPassiveReport` owns locking, atomic
serialize-before-publish admission and two revision-bound caches. Reimport clears
reports; identical selections reuse identities. Existing evidence IDs are
unchanged. Extended relation semantics use `request-source-relations-v2`.

Limits: 16,384 records, 8,000,000 projection/composition steps, 16 MiB canonical
metadata/report and 128 rows/page. JSON bodies have 64 KiB text, depth 16 and
200,000 aggregate admission nodes across bodies, including failures. Aggregate
exhaustion aborts report publication; byte/depth limitations retain no fake shape.
Existing analyses retain independent limits and remain available. HAR/stream-only
reports work without the JS parser; backend omission exports unavailable ABIs.

C++ tests verify independent types, all feature domains, exact anchors, unknown
methods, framing independence, body/work refusals, canaries, paging/revocation
and SDK/CLI/worker/MCP parity. The pinned Claude 2.1.296 macOS ARM64 module 928
also passes source-only reporting through NeverD with an unusable external-tool
PATH. No real HAR/transcript was supplied for that module or invented.
