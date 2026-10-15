# Client source syntax navigation

`javascript-client-syntax-features-v1` adds a `features` record kind to the
existing source navigation operation. It provides bounded source anchors for
client patterns in five domains: tools, configuration, permissions, MCP and
hooks. Every match has `confidence: source_syntax_only`. A spelling or shape
match does not authenticate a framework, identify an effective permission
policy, establish configuration precedence, resolve a runtime callback or
prove server behavior. Even an unrelated logger can contain a hook-event
literal. Zero matches means `no_pattern_match`, not absence of the feature.

## Fixed vocabulary

| Pattern | Exact syntax admitted |
| --- | --- |
| Tool definition | One object expression with `name`, `inputSchema` and one of `call`, `execute` or `handler` properties |
| Tool registration call | `registerTool` as an identifier or static member spelling |
| Tool dispatch call | `executeTool` or `dispatchTool` |
| Configuration call | `loadSettings`, `readSettings` or `getSettings` |
| Permission call | `checkPermission`, `checkPermissions` or `canUseTool` |
| MCP call | `setRequestHandler`, `setNotificationHandler`, `registerPrompt`, `registerResource`, `listTools` or `callTool` |
| Hook call | `registerHook`, `runHooks`, `executeHooks` or `dispatchHooks` |
| Settings property | `permissions`, `permissionMode`, `mcpServers` or `hooks` on an object expression |
| Hook event property | A catalog event key in an ordinary `hooks: { ... }` property |
| Hook event argument | A catalog string literal used directly as a call argument; its ordinal is syntactic, not an overload-resolved parameter role |

The hook catalog is deliberately fixed to `PreToolUse`, `PostToolUse`,
`PostToolUseFailure`, `PermissionRequest`, `UserPromptSubmit`, `SessionStart`,
`SessionEnd`, `Stop`, `SubagentStart`, `SubagentStop`, `PreCompact` and
`Notification`. Other events remain outside this version's vocabulary.
The [MCP server documentation](https://modelcontextprotocol.io/docs/2026-07-28/develop/build-server),
[Claude settings reference](https://code.claude.com/docs/en/settings) and
[hook reference](https://code.claude.com/docs/en/hooks) guide fixture vocabulary;
they do not prove the identity or behavior of code in a captured release.
Generic helper spellings in this table are self-authored patterns, not claims
that a target uses those internal function names.

Identifier escapes and directly written string-literal keys use the shared
parser's decoded UTF-16 spelling. Computed expressions, concatenation,
identifier aliases and templates are not evaluated to manufacture a match.
Computed member calls are counted as an explicit boundary and remain available
in the ordinary `calls` inventory. Constructors, tags and dynamic imports do
not become named-call candidates.

## Relationships and unresolved shapes

Records retain node/trigger identities, exact UTF-8 source ranges, enclosing
function and call IDs, name/callback argument anchors, syntactic argument count
and spread presence. A directly written callback function links to its source
function. An identifier can link to the existing lexical declaration or
initializer relationship. That relationship survives reassignment as source
evidence; it is never promoted to a runtime call target. Dynamic scope,
external references and ambiguous initializer bindings leave the function link
null. No additional alias, property, import or call-return propagation is added.

`registerTool`, prompt/resource registration, request/notification handlers and
`registerHook` use the last argument as a callback **candidate**, subject to
the catalog's minimum syntactic arity. This does not resolve overloads. Spread
or insufficient arguments retain an `unresolved_shape` record and a reason.
Tool objects with spread/computed keys, duplicate selected fields, accessors or
method properties also retain a reason without inventing a callback link.
Non-object settings sections remain unresolved. The configuration contents,
commands, destinations, schemas and tool names remain private.

A permission-named call records a containing `if` or conditional-expression
test only when its own syntax lies in that test without crossing a nested
function body. Calls in a branch body do not inherit the test. This is source
containment, not a true/false policy decision, control-flow proof or permission
bypass mechanism.

## Ownership, limits and publication

`SourceNavigation` continues to own generic source containment and lexical
links. `features/Analysis` admits those retained models and builds indexes;
`features/Recognize` owns the closed pattern vocabulary; `features/Publication`
owns metadata. The dependency points from feature analysis to generic
navigation. `SessionNavigation` composes and caches both results, serializes
before publication and clears them together on reimport. The generic navigation
profile, function/call/reference IDs and direct analysis algorithm are unchanged.
The feature result has its own profile and analysis/record identities.

Interactive parsing keeps its 1 MiB/100,000-node admission. Feature analysis
adds at most 1,024 records and 2,000,000 work steps per selected source. Model
validation, indexing, parent walks, pattern comparisons and argument scans are
charged. Exceeding either feature limit discards its complete candidate list
and reports `budget_exceeded`; the independently valid generic navigation
remains available. There are at most sixteen cached source/navigation results
per session under the existing source/node limits. Pages admit up to 512 rows.
These limits do not claim a whole-process RSS bound. Explicit large-source
selection retains one additional source, uses shared profile node/work limits
and keeps the 1,024-record ceiling. The pinned macOS ARM64 Claude corpus
qualifies all 2,364 units under that profile, including 4 tool, 129 configuration,
261 permission, 139 MCP and 99 hook candidates. Their existing uncertainty and
callback/source-only meaning remain unchanged; all named feature inventories
remain partial evidence. See the
[selection contract](web-passive-report-profile.md#explicit-large-source-selections).

The navigation summary adds `client_features`: profile, analysis ID, status,
domain counts, unresolved counts, computed-member boundary count, steps and
fixed diagnostics. Available feature coverage is always `partial`, even when
the fixed pattern inventory finishes. `feature_inventory_complete` is false;
effective policy, precedence and server behavior are `not_analyzed`.
No arbitrary source text, names, values, paths or per-value hashes enter these
records. Ordinary output retains `metadata-only-v1`; this operation grants no
source disclosure, sample upload or target execution.

SDK, CLI, worker and MCP use the same existing C APIs and backend owner. CLI
`neverd web navigate FILE script` includes feature pages alongside generic
navigation. SDK/worker/MCP explicitly select `record_kind: features` in
`source_navigation_records`. JavaScript/backend omission preserves the existing
ABI/unavailable behavior and removes source navigation from discovery.
Feature/navigation C API entry points remain unchanged; explicit large-source
selection uses a separately documented additive operation. No runtime,
external analyzer or extension mechanism is introduced.

This profile supplies inspection candidates. Import-rooted framework attribution,
configuration merge/dataflow, property dispatch, effective policy and registration
reachability remain outside this profile. The
[epic acceptance audit](web-epic-acceptance.md) records current criterion
coverage across the source and consumer profiles.
