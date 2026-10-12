# Passive interface evidence profiles

This increment of [#717](https://github.com/NeverSight/NeverD/issues/717) supports
HAR-only, source-only and explicitly selected source/HAR comparison. All
implementation and fixtures are C++; the already embedded Ada 4.0.0 parser
handles URL serialization. No input code, URL, body, vendor extension or
declared script is executed, requested, replayed or uploaded.

## HAR admission and redaction

`har-1.2-metadata-v1` accepts UTF-8 JSON, including a leading UTF-8 BOM,
with `log.version = "1.2"` and an entries array. This is a documented subset of
the [HAR 1.2 format](https://github.com/ahmadnassri/har-spec/blob/master/versions/1.2.md),
not the abandoned W3C HAR draft or a claim of full HAR validation.

Original artifact identity and SHA-256 cover every supplied byte, including
the BOM. Entry order is preserved. Observation, request and response IDs use
the capture identity and occurrence ordinal; they never reuse vendor IDs or
hash private endpoint values. A request/response ID can identify an absent
side, so consumers must inspect its `*_present` field.

The reader retains only declared recognized methods, HTTP status 0 or 100..599,
field presence/counts and timestamps in a narrow validated RFC3339 subset.
Timestamps require a valid Gregorian date, seconds 00..59, optional 1..9
fractional digits and Z or a numeric offset. Missing/invalid timestamps remain
distinct. Capture creator names/versions, arbitrary methods, unknown header
names, URLs, query names/values, cookies, bodies, redirects, IP addresses,
comments and vendor values are withheld from normal output. Only a fixed
allowlist of header-name classifications can be displayed. This blanket
policy also covers credentials or personal data embedded in paths and names;
it does not depend on recognizing a token pattern.

Headers/cookies/query/parameter lists report absent versus present-empty and
incomplete entries. Missing requests, responses and body text are supported
partial evidence. Wrong types, duplicate decoded JSON keys, invalid status,
conflicting postData text/params, truncated JSON or budget excess fail
atomically with fixed diagnostics. Bodies, including base64 content, are
never decoded. Custom fields are counted as unsupported; known
`_webSocketMessages` presence is reported, but no frames are imported.
This profile does not validate timings, HTTP sizes or capture authenticity.

HAR admission has two explicit phases. A current-revision preview that passes
the API/transport argument boundary revokes any earlier pending preview before
lookup or parsing, including if the new preview then fails. It returns only
redaction/coverage counts and a token bound to the original artifact/hash,
revision, profile, policy and preview sequence. Observation pages become
readable only after commit. Every same-revision commit attempt consumes the
pending token. A stale-revision request cannot disturb the current preview.
Reimport clears pending and published analyses and comparisons.

## Source candidates

`fetch-websocket-xhr-wrappers-syntax-v4` consumes the shared parser, lexical binding
and primitive-value models. It recognizes direct external `fetch(...)` and
`new WebSocket(...)` syntax. A lexically external name is **not proof of a
platform intrinsic**: every result remains static inference with unknown
runtime identity and reachability. Shadowed/written names are excluded;
possible direct eval refuses the analysis. General aliases, runtime wrapper
identity and multi-layer request flow remain unsupported. The v4 identity
replaces v3; operation names remain available, with additive wrapper metadata.

### Member spellings and argument boundaries

V3 also retains non-computed `receiver.fetch(...)`, optional-member and
optional-call spellings as `fetch_member_call`. `receiver_node_id` and an
available lexical `receiver_binding_id` provide navigation anchors. The receiver
is `member_spelling_syntax_only`; its method is unknown and options remain
`wrapper_options_not_interpreted`. A property name, including `this.fetch`,
does not authenticate a framework, wrapper implementation or browser API.
It also cannot assign URL/options/header/body roles to arguments: those node
fields and private endpoint comparison material remain unavailable even for
literal arguments. The complete call anchor still permits source navigation.
Computed names, other method spellings and member constructors remain outside
this profile. These rows never participate in the direct-fetch HAR join.

Every direct fetch, WebSocket constructor and member-fetch call now classifies
its arguments as `positional` or `spread_unresolved`. Following
[ECMAScript argument-list evaluation](https://tc39.es/ecma262/#sec-argument-lists-runtime-semantics-argumentlistevaluation),
an AST spread ordinal cannot establish a runtime argument position or count.
Any spread withholds URL/options/method/header/body role nodes, private endpoint
comparison material and all default/absence claims, even when a static-looking
argument precedes it. The call and any receiver remain navigable. Iterable
contents are never evaluated or guessed. In particular, `fetch(...args)` cannot
create an omitted-init GET candidate. XHR keeps the same conservative policy.

### Direct request argument evidence

For admitted non-spread direct calls, literal strings, supported primitive
concatenation and fully evaluated template literals provide private URL
candidates. Unresolved template expressions,
identifiers and other dynamic expressions retain source node/range links and
unknown values. Only omitted fetch init supplies a default GET candidate.
An explicit object must contain distinct non-computed ordinary data properties;
spread, getters, methods, duplicates and `__proto__` prevent selecting options.
Absent own `method` is unknown because inherited properties can participate
in [Web IDL dictionary conversion](https://webidl.spec.whatwg.org/#es-dictionary).
Only the six methods specified by
[Fetch normalization](https://fetch.spec.whatwg.org/#concept-method-normalize)
are uppercased; lowercase `patch` is not promoted to `PATCH`.
CONNECT/TRACE/TRACK candidates carry a forbidden-method classification,
including their ASCII case variants, through one shared Fetch/XHR rule.

Records link call, URL, options, method, headers and body syntax nodes to exact
source byte coordinates. No source text or private endpoint strings are
published by these APIs. Existing source navigation and explicitly reviewed
source views supply the separate local inspection path.

### XMLHttpRequest syntax

Following the [XHR interface declarations](https://xhr.spec.whatwg.org/), v2
adds `xhr_open`, `xhr_set_request_header`, `xhr_send` and `xhr_event_handler`.
Admitted receivers are a direct `new XMLHttpRequest()` expression or a reference
to a simple lexical `const` declarator initialized directly by that constructor.
The constructor name must be external and unwritten. Shadowed constructors,
conflicting or assigned bindings, destructuring, let/var, aliases, namespaces,
computed members and dynamic-scope receiver references are excluded.

Each record links its constructor and, where present, its lexical binding.
That link does not establish a runtime object, initialization before use, native
method identity, one request session or an execution sequence. A const receiver
can have replaced methods. Multiple opens, conditional sends and closure uses
remain separate syntax sites; methods, headers and bodies are not combined.

`open` retains method, URL and async argument nodes. The existing primitive
evaluator supplies finite method/URL candidates. Missing required arguments
stay explicit, and a missing method never defaults to GET. Username/password
arguments expose only `credential_arguments_status`, never values or their
node IDs. `setRequestHeader` retains name/value node IDs with a fixed header
classification. Unknown names are `other`; sensitive names are `sensitive`.
`send` retains a body argument node or `absent_argument`, without guessing the
body that would be sent.

Known event properties (`onload`, `onreadystatechange`, `onerror`, `onprogress`,
`onabort`, `ontimeout`, `onloadstart`, `onloadend`) accept only ordinary `=`
assignments. `addEventListener` accepts the corresponding finite event names.
Handler nodes identify RHS/argument syntax: direct functions, null values,
object values and other expressions have distinct classifications. These are
not proof of callable callbacks, dispatch, a registered listener or response
handling. Compound/logical event assignments, custom/dynamic events, `this`
and upload-object inference are excluded. Listener options stay syntax only.

`arguments_status` also applies to the direct/member call forms above. Any
spread in an XHR call sets it to `spread_unresolved`, clears all
parameter-role nodes, and withholds argument-presence and default claims. The
call, constructor and receiver link remain navigable. This applies to every
XHR call, including `send` and `addEventListener`; spread listener options are
also `spread_unresolved`, never asserted absent.

### One-layer lexical parameter forwarding

`direct-lexical-fetch-forwarding-v1` adds `fetch_wrapper_call` rows. A uniquely
declared, unwritten function declaration or a simple const identifier initialized
directly by a function/arrow expression can supply a wrapper definition. The
function has one or two distinct simple identifier parameters and either an
arrow call body or a block containing exactly one return of a direct, external,
unwritten `fetch` call. Each of the inner call's one or two arguments must be
an exact lexical reference to one of those parameters. Their positions can
differ from the caller's positions. Async/generator functions, default/rest/
destructured parameters, other statements, argument transformations, extra inner
arguments, optional inner dispatch and spreads do not supply a definition.

Only direct lexical calls to that exact binding receive a row; spelling matches,
aliases, member calls and recursive wrapper-chain composition are unsupported.
The caller, wrapper function, internal fetch call, binding, parameter nodes and
zero-based caller argument positions remain independently navigable. Caller
argument syntax supplies URL/options/header/body anchors through the existing
finite-value and fetch-option rules. A missing forwarded argument stays
`wrapper_argument_missing`; an actually omitted inner fetch init alone supplies
the existing default-method candidate. Caller spreads withhold all actual
argument-role nodes, caller position fields and private URLs while retaining
definition/parameter links. A missing actual argument also has no caller position.
Optional caller syntax is recorded without asserting that the call executes.

This is a source forwarding candidate, not a proof of initialization, runtime
dispatch, API identity, effects, reachability or source execution. The internal
fetch remains a separate source occurrence; its parameter value is not replaced
by the caller's value. Independent lexical definitions with equal names stay
separate. All discovery and projection share the existing work/record/private
budgets, and exhaustion clears records and definition counts atomically. Public
pages publish IDs, fixed classifications and positions, never wrapper names,
source text, URLs, headers or body values. Wrapper rows are excluded from the
unchanged HAR correlation and separate `request-source-relations-v1` result
grammar. General response dataflow, parameter schemas, wrapper ownership,
serialization/compression/crypto chains and semantic behavior remain outside
this increment.

## Explicit correlation

`absolute-http-method-origin-path-v1` compares one selected source analysis
with one committed HAR capture. Only fetch candidates with recognized methods
and absolute HTTP(S) URLs qualify. Ada-serialized full origin and path must
match exactly; no suffix, wildcard, route-template or inferred-base matching
occurs. Special URL forms without explicit authority, such as
`https:host/path` and `https:/host/path`, remain base-dependent; successful
standalone URL parsing cannot substitute for the unknown environment base.
Query strings, fragments, headers, bodies and timing are explicitly
excluded from comparison. Distinct queries can therefore yield multiple
candidate pairs; this is never evidence that the source caused an observation.

Credentials and fragment presence are classified before private values are
discarded. Credential-bearing fetch URLs retain their rejection under the
[Request constructor](https://fetch.spec.whatwg.org/#dom-request); they cannot
become eligible after redaction. WebSocket URLs with fragments, including an
empty fragment, retain the
[constructor limitation](https://websockets.spec.whatwg.org/#dom-websocket).
WebSocket and XHR source records never join ordinary HAR HTTP observations.
The correlation profile is unchanged: an XHR `open` syntax site alone cannot
establish a send or its effective parameters. Capabilities and source summaries
explicitly report `xhr_correlation: false`.

All matching occurrences remain separate, with pair IDs derived from public
source/observation evidence IDs. Results preserve static inference and imported
observation classes, never assert execution, authenticated capture, analyst
confirmation or server completeness. Zero pairs means no match under this
rule, not proof of absent behavior. Relative/dynamic URLs stay unresolved.

## Budgets and interfaces

| Resource | Bound |
|---|---|
| HAR original | 8 MiB |
| JSON depth / nodes / one string | 64 / 200,000 / 4 MiB |
| HAR entries or source interface records | 4,096 |
| HAR header/cookie/query/post-parameter records | 32,768 aggregate |
| One raw or serialized URL / private origin+path bytes per analysis | 16 KiB / 2 MiB |
| Source/HAR analysis steps | 2,000,000 |
| Correlation pairs | 32,768; excess fails atomically |
| Retained captures / source analyses / comparisons | 4 each |
| Pending capture previews | 1 |
| Page records | 1..128 |

The public C API adds `neverd_web_har_{preview,commit,records}_json`,
`neverd_web_interfaces_{analyze,compare}_json`,
`neverd_web_interface_records_json` and
`neverd_web_interface_correlation_records_json`. C++ worker operations use
the same names without `neverd_`/`_json` and reject unknown request fields.
HAR works with the JS parser disabled; source analysis and comparison report
`capability_unavailable`. All symbols retain unavailable stubs when the whole
web backend is omitted.

CLI examples for a single captured file (artifact index 0):

```sh
neverd web har-preview capture.har
neverd web har-import capture.har 0 passive-interface-metadata-v1 <previewed-sha256>
neverd web interfaces client.js module
```

For an explicitly inventoried root:

```sh
neverd web interface-correlate ROOT module SOURCE_INDEX HAR_INDEX \
  passive-interface-metadata-v1 PREVIEWED_HAR_SHA256
```

Each CLI invocation creates a new session. The second invocation explicitly
accepts the displayed policy and original SHA-256, repeats the bounded preview,
and commits only if both match; it never attempts to reuse another session's
token. Changed original bytes or a different policy refuse publication.

## Qualification scope

`WebInterfaces`, `WebSourceInterfaces`, `WebSourceXHR`, `WebInterfaceSDK` and the C++ worker
regression use self-authored inert fixtures. They exercise secret canaries in
URLs, names, headers, cookies, query, bodies, errors and extensions; preview
revocation, immutable storage, missing fields, duplicate occurrences,
shadowing/eval, prototype/method/credential exclusions and fanout refusal.
XHR cases cover exact constructor/receiver/argument anchors, scope identity,
forbidden methods, missing/dynamic values, spreads, callback-value distinctions,
aliases/writes/computed members and atomic budget/model refusal. CLI, worker
and MCP use the same v3 metadata; private URL, credential, header and callback
canaries stay withheld.
CLI and framed worker checks use an unusable external-tool PATH.

The optional captured Claude 2.1.296 macOS ARM64 module 928 was exported by
NeverD from the original native SHA-256
`c9b5341637becbd423ddffc5b254afb645682a3868cb708bbc6cc0e7bb419937`.
Its 159,827 decoded bytes have SHA-256
`a5e52526ae954aa74deb80cf1bb2372d1f4b77e1886bba69d55e30c51efa2cd2`.
Using its declared module source type, the v3 CLI retains one member-fetch
candidate at UTF-8 byte 150,734 for 36 bytes, with spread arguments unresolved
and method unknown. The prior direct-only profile emitted zero candidates;
neither result establishes an absent or authenticated runtime interface. The
same pinned case runs through the C++ core with no target execution. This is
one selected source unit, not a whole-client interface inventory.

The separately selected [request source relationship profile](web-interface-relations-profile.md)
adds function/direct-return anchors, direct awaited const-result origins,
fixed lexical result-member uses and direct then-handler syntax. These are
independently bounded syntax relations, with response API/value identity and
execution unverified; they do not broaden HAR joins.

This does not complete #717. Wrapper implementation/argument flow and general response/transformation
tracing, schema exports, reviewed endpoint displays and
broader host qualification remain required. Separately selected
[SSE/JSONL/JSON-RPC/MCP-shape/log adapters](web-stream-profile.md) now provide
passive framing and recorded-context candidates; source-to-stream correlation
remains unsupported.
No Claude Code traffic or transcript is invented.

With the optional `NEVERD_CLAUDE_CODE_21296_ELF` input, a C++ regression
re-extracts module 1,897 from the hash-pinned original using NeverD's Bun
reader. It checks one fetch candidate at source byte 65,473 with length 52,
retaining its dynamic URL and unresolved options as unknown. This qualifies
the container→source→interface path; it does not recover that runtime endpoint
or supply traffic observations.
