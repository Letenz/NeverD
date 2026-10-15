# Offline package installation traces

`node-install-entry-traces-v1` connects retained lifecycle declarations to exact
captured file candidates and selected Node behavior records. It is implemented
entirely in C++, without a shell, JavaScript runtime, package manager, network
access or external analyzer. It does not prove that npm activates a hook, that
`node` resolves to a particular executable, or that any associated call runs.

The required input is a `node-package-evidence-v1` package analysis. An optional
package-behavior selection must belong to that same analysis and revision. The
exact namespace used by the package owner is retained; traces do not reconstruct
it from host paths. No behavior selection means zero selected sources, while
declarations and coverage gaps remain reportable. Older package-behavior and
ESM-link profile output is unchanged.
Trace identities bind the shared binding, module, origin and Node-link profiles
as well as the package/namespace/selection identities; changed semantic owners
cannot reuse an old result identity.

## Admission and boundaries

The package owner remains authoritative for lifecycle classification. This
profile visits its `lifecycle_declaration` rows, without introducing another
event list. Lockfile `hasInstallScript` without a captured manifest produces a
presence row with a null script anchor and no invented command. Ordering,
implicit node-gyp, other lifecycle events, npm recursion and pre/post expansion
remain outside this profile.

Commands must be complete members of this literal subset:

```text
node <relative-file> [literal-argument ...]
node -- <relative-file> [literal-argument ...]
```

Spaces and tabs separate words. Only ASCII letters, digits, `_`, `-`, `.`, `/`
and those separators are admitted anywhere in the command. Quoting, escapes,
expansion, assignments, operators, comments, globs and other commands refuse.
Before the file, only the optional `--` is admitted; the file cannot start with
`-` or be the reserved bare `inspect` subcommand. `./inspect` is a distinct file
candidate. Arguments are neither interpreted nor propagated. Reported command
offsets index the decoded command bytes, not raw manifest JSON.

The existing package path policy admits one optional leading `./`, then a
confined explicit path beneath the declaring package location. It rejects
absolute paths, empty/dot/parent segments, backslashes, colons, control bytes,
paths above 4,096 bytes and depth above 64. There is no extension, index,
package, symlink or host-filesystem fallback. Contradictory manifests, package
links, directory targets, absent directory inventories and unselected or
unavailable source selections have distinct statuses. Readable archive
namespaces omit links, encrypted members and unsupported payloads; their absence
means `not_captured_or_unavailable`, never proof that a file does not exist.

The separate `admitted-node-relative-file-v1` adapter shares the existing exact
captured-file comparison. Static ESM import/reexport declarations are candidates.
A nonoptional, literal, single-argument `require` call additionally needs the
shared origin owner's unprojected request evidence. Shadows, writes and dynamic
scope never acquire a second resolver here. `external` and
`caller_selected_commonjs_parameter` remain distinct evidence classes; neither
proves runtime CommonJS identity. Dynamic import, `require.resolve`, bare names,
extension/index inference and unsupported path syntax remain visible boundaries.

Only direct files in the package inventory can participate in propagation.
Embedded Bun/SEA/nexe/pkg source selections retain explicit delivery boundaries.
Cross-package files retain their own package identity; no dependency edge or
version satisfaction is inferred. All selected requests remain visible even
when no script reaches their file. A deterministic breadth-first walk chooses
one shortest chain, ordered by request trace ID, for each source. Cycles stop
at visited sources. Alternative paths are not enumerated. Deferred or dead code
still carries only syntactic file association, never call-graph reachability.

## API and limits

`neverd_web_package_install_analyze_json` takes revision, package analysis ID
and `{"schema_version":1}` with an optional `package_behavior_id`.
`neverd_web_package_install_records_json` pages `scripts`, `sources`, `requests`
or `associations` at 1–512 rows. Source rows preserve unavailable/embedded
coverage; request rows preserve original node/request and target-file anchors;
associations preserve script, entry file, source, behavior record and ordered
request trace IDs. They also copy the original fixed API/kind/origin class and
node/byte span, so a standalone CLI report remains useful without populating
the independent Node behavior cache. Metadata contains no command, path, package name, argument,
specifier or per-short-value hash. Coverage is always partial and no safety
verdict is produced.
Direct and embedded counts describe inventory relations. The unavailable count
separately describes missing source, behavior or module-model coverage; it may
overlap the direct count. Zero unresolved request rows does not mean that an
unavailable source contains no requests.

Limits are 1,024 declaration/presence rows, 16 selected sources, 8 MiB selected
source bytes, 1,000,000 selected AST nodes, 4,096 requests, 8,192 associations,
chain depth 16, 8,000,000 charged work units and two cached results. Shared
parser/binding/module/origin limits still apply. Namespace comparisons, model
work, retained anchors and chain copying consume the aggregate budget. Budget
failure publishes no partial result or cache entry; preexisting unavailable
source evidence remains an ordinary coverage row. Imports revoke all results.

CLI compositions use the same C API, including when no sources are selected:

```console
neverd web package-install ./capture 3 package-json 1:commonjs 2:module
neverd web package-install ./package.json 0 package-json
neverd web archive-package-install ./package.tgz tgz 0 3 package-json 0:commonjs 1:commonjs
```

Worker and MCP expose `web_package_install_analyze` with required revision and
package analysis ID plus optional package behavior ID, and
`web_package_install_records` with the resulting package install ID. MCP uses
the usual `neverd_` prefix and has no raw-export or execution capability.

The pinned [Node 22.15 CLI](https://nodejs.org/download/release/v22.15.0/docs/api/cli.html)
and its [entry dispatch](https://github.com/nodejs/node/blob/v22.15.0/src/node.cc#L379-L385)
define the admitted command boundary. [npm scripts](https://docs.npmjs.com/cli/v11/using-npm/scripts/)
documents working directories and shell-dependent activation; those facts do
not establish activation for a supplied package. Source-behavior version diffs
use the [comparison profile](web-package-behavior-diff-profile.md). The
[assessment profile](web-package-assessment-profile.md) composes selected
installation, comparison, advisory/provenance and analyst-review evidence.
