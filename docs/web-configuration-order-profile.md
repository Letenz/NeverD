# Configuration object source order

`javascript-configuration-object-order-v1` adds `config_merges` pages to the
existing source-navigation operation. It connects admitted client settings
property candidates to their ordered object definitions/spreads and, where
available, lexical variable initializer and named configuration-call syntax.
This is source navigation with `confidence: source_syntax_only`; it does not
compute an effective configuration or authenticate a configuration loader.

## Grammar and uncertainty

The shared [client feature owner](web-client-feature-profile.md) supplies the
`permissions`, `permissionMode`, `mcpServers` and `hooks` property candidates.
For each containing direct object expression, this owner records its complete
property list in source order. Terms retain their exact node/range, value node,
ordinary/computed/accessor/method/spread kind and possible unknown keys.
No property value, path, permission entry, server definition or hook command
is published. Static string keys use the shared parser's decoded spelling.

Each observed field keeps its direct definition count and last direct
definition node. A subsequent spread or unresolved computed key sets
`possible_later_overwrite: true` and `precedence_status: later_unknown_term`.
A subsequent direct definition of the same selected field clears that barrier
for that field. `last_direct_definition` describes syntax order, not a proved
runtime value. Accessor/method terms remain explicit; their evaluation, getter/
setter pairing, property reads and actual object completion are not analyzed.
Unknown fields are never invented merely because a spread may contain them.

For example, `{...defaults, permissions: {}, ...project, permissionMode: mode}`
has an unknown later term after the permission definition, while the mode has
a later direct definition. The report retains both observations without
evaluating `defaults`, `project` or `mode`.

A spread/value identifier uses the authoritative binding result. A uniquely
identified simple variable initializer can link to its source node and a
preexisting `loadSettings`/`readSettings`/`getSettings` syntax feature. Multiple
variable declarations keep the initializer ambiguous; dynamic scope does not
gain an initializer link. Reassignment does not erase the source relationship
and does not make it a runtime value proof. Destructuring, imports, alias chains,
call-return propagation, `Object.assign`, runtime object mutation and separate
configuration-file layer precedence remain outside this grammar.

Object terms follow the ordering described by ECMAScript's
[PropertyDefinitionEvaluation](https://tc39.es/ecma262/multipage/ecmascript-language-expressions.html#sec-runtime-semantics-propertydefinitionevaluation).
This independent syntax model does not execute that algorithm, evaluate keys,
invoke getters/proxies or treat successful object creation as observed evidence.

## Ownership, budgets and consumers

`features/configuration/Analysis` consumes retained source/binding/navigation/
client-feature models and checks their linked identities. It uses
`SourceModel::indexSourceDeclarations` for authoritative unique
declaration identities and owns separate order/term identities. `Publication`
owns fixed metadata. `SessionNavigation` serializes and caches this result with
generic navigation, client syntax and desktop bridges; no generic function,
call or feature identity changes. Reimport revokes every selection together.

The existing parser limits remain 1 MiB and 100,000 nodes. This owner adds
256 objects, 4,096 terms and 2,000,000 charged steps per selected source.
Validation, indexing, model walks and property scans are charged. Exhaustion
or malformed/foreign evidence clears the entire order result; other navigation
remains available. Existing source/session cache limits and 512-row paging
apply. These bounds do not establish a whole-process RSS ceiling.

The existing C API/worker/MCP navigation operation publishes the independent
summary as `configuration_order` and the records as `config_merges`.
`neverd web navigate <file> <mode>` includes these metadata pages. Source anchors
resolve every term's UTF-8 span through its unchanged immutable storage and
container chain. No transport operation, raw-source export or execution option
is added. Parser omission removes navigation and its advertised profiles.

## Captured Claude Code selections

The exact [Claude Code 2.1.296 macOS ARM64 npm qualification](web-claude-code-cross-platform-qualification.md)
supplies the original archive/executable/Bun graph. The sequential C++ corpus
visits all 2,364 decoded units and 45,035,899 bytes. In the recorded run, the
existing interactive frontend admits 2,354 units and explicitly refuses ten
under its byte/work/node limits. Configuration-order analysis completes for
all 2,354 admitted feature inventories, yielding 273 candidate objects, 2,279
terms and 19 observed fields with unknown later terms. Named configuration
loading links are zero. These are spelling/shape candidates; source bytes and
larger readable recovery remain separately preserved for every original unit.

Two preserved decoded files provide narrower selections. Module 1676 has
SHA-256 `c377d5191165ae221a042d4ea22bd3333969097edb10eb0ef6791610378324da`,
one candidate object and four terms. The first corpus unit with an unknown
later field is module 6, SHA-256
`5ec1302ba8afecce27dd404105070c2cbb3048d7c3a6dbf9265d6500b7d50daa`.
Its selected object is at UTF-8 byte offset 2,451/length 96 and retains one
observed field with a possible later overwrite.
These indices/hashes belong to this exact platform graph. Direct file and
archive/Bun selections retain different typed origins; equal bytes do not
fabricate equal source identities. No captured source is redistributed.

The source receipt is `/tmp/neverd-configuration-order-source-corpus.xml`.
Optional local tests use `NEVERD_CLAUDE_CODE_21296_CONFIG_JS`,
`NEVERD_CLAUDE_CODE_21296_CONFIG_UNKNOWN_JS` and the existing cross-platform
corpus variable. Self-authored C++ cases cover known loading relationships,
unknown/duplicate/computed/accessor definitions, redeclarations, reassignment,
dynamic scope, foreign models and independent object/term budgets. SDK/CLI and
direct/framed worker/MCP cases preserve canaries, source anchors, pagination,
cached identity and revision revocation. They do not install or execute input.
