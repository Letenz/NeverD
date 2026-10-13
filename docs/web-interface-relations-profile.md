# Request source relationships

`request-source-relations-v2` projects bounded syntax relationships from an
explicit existing `fetch-websocket-xhr-wrappers-syntax-v4` analysis. It consumes the same
source and lexical-binding models and the authoritative
`javascript-source-navigation-v1` function/call index. It accepts only direct
fetch and unverified member-fetch candidates for these request/result links.
It does not change request eligibility, URL/method interpretation or HAR joins.
V4 wrapper-call rows carry their own one-layer forwarding anchors; this separate
result-relation profile does not consume those rows or compose wrapper chains.

## Selected relationships

| Kind | Exact source condition | Published anchors |
| --- | --- | --- |
| `request_function_containment` | Navigation places the request call inside a function | Request call and enclosing function |
| `direct_request_return` | A return argument or arrow expression body is exactly the request call or its direct `await` | Return/arrow, request and enclosing function |
| `await_result_const_binding` | A simple, unique, unwritten const identifier is initialized by the direct awaited request | Declarator, identifier, lexical binding and request |
| `bound_result_member_call` | A non-computed fixed member call uses that exact lexical const binding | Consumer call, receiver, binding and request |
| `awaited_result_member_call` | A non-computed fixed member call has the direct awaited request as its receiver | Consumer call, await receiver and request |
| `then_callback_syntax` | A direct request call is the receiver of non-computed `.then(...)` call syntax | Call, request and first argument syntax when positional |
| `request_body_transform_syntax` | Fixed transformation call syntax is contained in a directly admitted fetch body without crossing a function boundary | Transform, body, request and containment depth |
| `result_transform_argument_syntax` | A direct first transform argument, optionally awaited, is a fixed result member call | Transform, input member call and original request |

Fixed result-member spelling classes are `json`, `text`, `array_buffer`, `blob`,
`form_data`, `bytes` and `clone`. The `arrayBuffer` and `formData` source spellings
map to those fixed classes. Unknown and computed names, member constructors,
general aliases, mutable/rewritten bindings, destructuring, nested promise
chains and inferred callback parameters are unsupported. `.then` arguments with
any spread retain the whole call but withhold the first-argument/handler anchor.
An ordinary consumer call can retain its whole anchor with `spread_unresolved`;
no consumer argument roles or values are interpreted.

For example, `const result = await fetch(url); result.json()` supplies a
const-initializer link and a lexical member-use link. `const copy = result;
copy.json()` does not supply an alias link. A property write such as
`result.json = replacement` does not change the identity of the const binding,
but reinforces why a `json` spelling is not response API authentication.

All relations remain partial static inference. The explicit qualifications are
`source_syntax_only`, `result_value_identity:not_verified`,
`response_api_identity:member_spelling_unverified`,
`callback_parameter_roles:not_assigned` and
`wrapper_argument_flow:not_analyzed`. Runtime caller targets, callback execution,
reachability, ordering, server schema, body decoding and response transformations
remain unverified. A direct return is a navigation anchor for a possible wrapper;
it does not map caller arguments or prove the wrapper runs. A lexical reference
inside a nested function is not proof it executes after initialization.

## Ownership and bounds

`interfaces/RelationContext` owns shared-model admission, lexical indexes,
fixed publication identities and budgets. `RequestLinks` owns direct return and
const-initializer syntax; `ResultLinks` owns the fixed member-use grammar.
`TransformationLinks` owns fixed JSON/base64/URI, crypto.subtle and zlib call
spellings. Their identities, arguments, effects and actual output stay unverified.
Aliases, computed properties, callbacks and deferred body functions are not
followed. `SourceRelations` coordinates these owners. Function containment stays owned by
`SourceNavigation`, and unique declaration admission stays owned by
`indexSourceDeclarations`. The relation layer never computes a replacement
call graph, interprets private URL material or modifies request evidence.

The profile caps 4,096 relations and 2,000,000 work steps, including the exact
navigation work count. A cached navigation index and a freshly computed one
have identical identities and charges. Core callers may select smaller budgets
for qualification; larger limits are refused. Exhaustion clears every relation
and reports a fixed reason. The independently valid request analysis remains
available. Foreign, malformed, dynamic-eval and unavailable models refuse
clearly. Zero rows do not prove absence of response handling.

## API, CLI and transports

The additive C ABI remains exported when JavaScript or the complete Web backend
is omitted, and returns `capability_unavailable`. Available capability metadata
advertises `source_interface_relations` separately from source requests.

- `neverd_web_interface_relations_analyze_json` selects an existing request
  analysis with an exact current revision.
- `neverd_web_interface_relation_records_json` pages one relation analysis,
  with a limit of 1..128.
- Worker and MCP expose `web_interface_relations_analyze` with `analysis_id`
  and `web_interface_relation_records` with `relations_id`; argument schemas
  are closed. There is no execute, network, raw-text or capture option.
- `neverd web interface-relations <root> <source-type> [source-index]` imports
  the explicitly selected local source, publishes the request and relation
  summaries, then emits relation pages. It delegates to the same C API.

The session caches at most four relation analyses. Repeated selection reuses
the same result. Reimport clears relations with their request/source models;
stale revisions and old relation IDs cannot read prior pages. Pages contain
opaque request/node/function/binding/handler IDs, UTF-8 byte ranges and fixed
classifications only. Source text, identifier/property names outside the fixed
classes, URLs, headers, argument/body values and diagnostics containing private
input remain withheld. Source navigation/export policies remain separate.

## Qualification boundary

C++ core cases exercise exact return/await/consumer anchors, lexical shadowing,
aliases, mutation, computed names, constructors, direct then syntax, all-call
spreads, foreign/malformed inputs and exact/short atomic budgets. SDK cases
exercise selected analyses, closed metadata, paging, revocation, unchanged HAR
eligibility, cached/fresh navigation equality and graph exhaustion preserving
request pages. Worker/MCP compare the same production backend, reject extra
fields and withhold private canaries. CLI qualification uses an unusable
external-tool PATH. Tests never execute the input program.

The optional real Claude 2.1.296 macOS ARM64 module 928 is the same
SHA-256-pinned NeverD export qualified by the [request profile](web-interface-profile.md).
Its member-fetch call at UTF-8 byte 150,734 for 36 bytes now has request-function
and direct-return source links. This is one selected unit, not a whole-client
flow inventory. Detailed receipts and omission/restoration results belong in
[the implementation ledger](web-analysis-implementation.md).

Wrapper argument/dataflow, general response transformations, dynamic URL
resolution, request ordering, source/stream joins, authenticated browser API
identity and broader platform/framework qualification remain outside this
profile. Selected source/stream reports and observed type summaries are covered
by the separate [report profile](web-passive-report-profile.md). The final
[acceptance audit](web-epic-acceptance.md) owns epic completion claims.
