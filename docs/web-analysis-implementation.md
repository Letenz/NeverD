# Offline web analysis implementation ledger

This work implements issues [714](https://github.com/NeverSight/NeverD/issues/714),
[715](https://github.com/NeverSight/NeverD/issues/715),
[716](https://github.com/NeverSight/NeverD/issues/716),
[717](https://github.com/NeverSight/NeverD/issues/717), and
[718](https://github.com/NeverSight/NeverD/issues/718).
The required operations are implemented within their documented profiles.
The [2026-10-15 acceptance audit](web-epic-acceptance.md) distinguishes satisfied
issue criteria from future adapters and unknown runtime semantics. Final
shared-path regression and closure receipts are recorded below.

## Authoritative implementation constraint

The user approved implementation on 2026-10-10 and required **all new
implementation code to be C++, without external scripts or external tools**.
This supersedes the draft's Node/Babel helper, JavaScript analyzer, Python
binding additions, Python MCP changes, and script-based fixture generators.
The new feature must operate through compiled, in-process C++ libraries and
NeverD's C++ SDK/CLI/worker. It must not shell out to Node, Bun, package
managers, archive utilities, parsers, or a separate analyzer executable.
Development builds still use the repository's normal CMake/compiler tools.

A pinned embedded C++ JavaScript parser is integrated. Hermes' parser
and AST libraries are used without its VM, runtime execution, bytecode
compiler or command-line tools. Semantic rules, source mapping, evaluation,
rewrites, adapters and protocol consumers remain C++. New tests use C++.
The existing Python MCP adapter is not the implementation path for the new
feature: web MCP transport/tool dispatch must be implemented in C++ as well.

The independent source-domain C API is included explicitly through
`neverd/sdk/NeverDCAPIWeb.h`. It is installed beside the existing SDK headers,
but is not part of the native-session umbrella consumed by the existing
Python binding. That binding and its complete native-API drift check retain
their existing scope; no Python support for web APIs is claimed.

## Required work and current evidence

The table summarizes the final implementation. The increment records below
preserve historical counts, limitations and issue states at their recorded
baselines.

| Work | Implemented evidence | Explicit boundary |
|---|---|---|
| P0 identity/schema/profile contract | [Schema](web-artifact-schema.md), [support matrix](web-support-matrix.md), pinned parser and producer manifests | Versioned profiles do not imply unknown-layout support |
| P1 artifact store and safe input | Immutable bounded spool; POSIX and Windows NTFS handle owners; shared portable component validation and NFC/full-fold collision checks | Other filesystems/hosts need independent qualification; no global process quota |
| P2 SDK/CLI/worker | Shared C++ source domain, immutable input, metadata/query/range APIs, CLI and framed worker consumers | Web APIs remain independent of native sessions and the Python binding |
| P3 parser/scope/query/maps | Parser, lexical/value/effect/module/bundle/navigation models, private views and exact source-map anchors | No general runtime dataflow, foreign export resolution or erased-source recovery |
| P4A standalone extraction | Bun ELF/thin Mach-O/PE x64/ARM64, compiler corpus, serialized maps, local raw/readable export and eight captured Claude packages | Unknown layouts refuse; JSC cache contents remain opaque |
| Shared native handoff | Snapshot/file loader parity, independent native session lifetime, original container/member origins | Native loading is separate from extraction and runtime behavior |
| P4B desktop/VSIX extraction | ASAR/unpacked, ZIP32, Electron/NW.js/VSIX manifests and selected entries; HTML/import-map and native consumers | Runtime routing and unsupported framework versions remain unknown |
| P4C package graph/diff | npm graph/archive/SRI, behavior/install/diff, offline advisory/registry/provenance evidence, assessments and persistent Qt review | pnpm/Yarn are separate future adapters; declarations do not authenticate publishers |
| P4D passive interfaces/HAR | Redacted preview/commit, source request components, bounded source/HAR joins and v2 observed field trees | Syntax is not executed; observed presence is not server optionality |
| P5 reversible projections | Primitive rewrites, whole-binding renaming, Boolean expression selection, exact undo and verified same-tree readable export | No general statement/control-flow recovery or whole-program equivalence |
| P6 stream/log consumers | Versioned SSE/JSONL/JSON-RPC/MCP/log readers, recorded-ID candidates, malformed/budget cases and selected-source relations | Real transcripts are optional and never fabricated; runtime attribution remains unknown |
| P7 framework/Node profiles | Measured Tauri/Wails assets, direct bridge syntax, actual CEF/WebView2 DataPack profiles; independent SEA/nexe/pkg producers and consumers | Each version/target has its own contract; opaque snapshots stay opaque |
| P8 transport/distribution | C++ MCP and worker parity; published Windows Server 2025 x64 CLI/SDK/worker/MCP product with same-source producer/resource gates | Other host/product profiles require separate qualification |
| P9 acceptance | [Issue-by-issue and per-input audit](web-epic-acceptance.md) records mandatory criteria, exact evidence and boundaries | Closure relies on required criteria; future adapters remain explicitly unsupported |

## Historical foundation qualification

Original bytes at this milestone use `posix-unlinked-spool-v1`: a private,
immediately unlinked file with one close-on-exec descriptor per snapshot. Import streams and hashes
64 KiB chunks, seals before publication and exposes only bounded immutable ranges
internally. Admission is capped at 512 MiB aggregate / 256 MiB per original file,
10,000 entries and depth 64. Preview retains only its candidate digest, so one
session uses at most its published spool plus one pending capture (1 GiB).
Archive expansion adds at most 512 MiB across retained and pending expanded
streams, giving a conservative 1.5 GiB session spool bound. Independently held
native sessions may retain prior immutable storage; this is not a global quota.
This is not a global quota across arbitrary SDK sessions, nor a whole-process
RSS bound. Source/map materialization remains limited to 1 MiB / 8 MiB.

The local maximum-size C++ test captured two 256 MiB members and compared their
hashes; the 2026-10-10 macOS arm64 Release run recorded 65,536 bytes of additional
process peak RSS during capture and bounded reads. This is a high-water-delta
measurement for that test, not total resident memory or a promise for other
hosts. The test enforces a less-than-32-MiB growth bound and also checks refusal
before materializing an over-limit range. Other cases cover concurrent reads,
slice lifetime after input deletion, shared-descriptor cleanup and write failure
in a C++ child with a lowered file-size limit. SDK/CLI cases admit an original
above the old 64-MiB ceiling, then verify smaller parser budgets still refuse it.
Current POSIX directory traversal retains descriptor-relative no-follow opens.
Windows input was unavailable at this foundation milestone. The later
[Windows host qualification](#native-windows-host-and-extracted-product-qualification-2026-10-15)
records the implemented handle-based reader and its native test evidence.

Ordinary inventory returns hashes, sizes, parent identities and fixed kinds;
target-derived names and contents are omitted. That is the current
`metadata-only-v1` output policy, not universal secret recognition. Explicit
source-view policy is implemented below; broader consumer redaction and export
remain pending.

Before Bun extraction, 115 web cases passed through their owning CTest directory, with
the compiled-capability check and three worker cases also passing. A root
CTest invocation was blocked during unrelated semantic-test discovery by an old
`NeverDSemanticTests` executable referencing a missing `BinaryPatcher` symbol in
the current shared library. It was not counted as a successful whole-repository
run. Worker checks passed both directly and through CTest after enabling testing
before configuring tool subdirectories. With the JS parser disabled, all 41
remaining web cases (including the new disk-storage and large-original cases),
the compiled-capability check and three worker checks passed; the
backend-disabled ABI case was explicitly skipped in that profile.
With the whole backend disabled, both ABI availability tests passed and the CLI
returned `capability_unavailable` with exit status 1. Existing protocol/graph
checks passed; the actual web worker case explicitly skipped. This disabled
backend check includes binding, semantic, module and both new bundle ABI entry
points. The ABI omission case is inapplicable and explicitly skipped in enabled
profiles. The current 115-case run also includes private primitive metadata,
deferred effects and a lexical-budget exhaustion case
that retains values while returning null effect fields.
Module cases additionally cover class inner/outer binding identity, exported
destructuring, duplicate names/assertions, dynamic import boundaries, shadowed
or reassigned require, confined relative member comparison, UTF-16/path refusal,
whole-inventory budget failure, immutable cache links and output canaries.
Bundle cases verify the pinned upstream document, derived fixture and hashes;
object/sparse-array keys, false removed slots, exact loader structure, lexical
alpha matching, writes/dynamic lookup, partial bundles and whole-analysis limits
are covered. The upstream example does not establish the version that originally
produced it; producer verification remains false.
Both build options are restored to ON. After restoration, all 115 web cases,
the compiled-capability check and three worker cases passed again; the
backend-omission-only ABI case was explicitly skipped in that enabled profile.
`git diff --check` and formatting checks passed.

The storage regression exposed this LLVM revision's mismatch between the
temporary-file header's claimed `0600` mode and its actual `0666` creation mode
(observed as `0644` under the host umask). Storage now requests `0600` explicitly
through exclusive creation before unlinking, and validates permissions and
close-on-exec before copying any input bytes. The private/unlinked descriptor
case and write-failure case passed after that correction.

Hermes and webpack notices are staged beside the binaries and in the SDK.
Their install rules now belong to `lib/web`, because the embedded parser's
`EXCLUDE_FROM_ALL` directory does not participate in normal installation.
The generated Web install rules were exercised in an isolated prefix; every
installed Hermes license and the webpack MIT license matched the preserved
source files byte-for-byte. This verifies those notice rules, not a complete
product distribution or installation of all targets.
No new Python, JavaScript or shell implementation/test scripts were added.

## Historical package metadata increment

The Node metadata increment adds `node-package-evidence-v1` through C++ core,
session, SDK, CLI and worker layers. Twenty-six core tests include the pinned
npm CLI v11.9.0 lockfile: 1,230 instances and 2,426 dependency declarations.
Three SDK/CLI tests and five selected C++ worker regressions passed. Package
checks run with an unusable external-tool PATH. Review regressions distinguish
legacy fetch specifications from package versions, optional normalization,
workspace peer context, hidden-lock bases, unrecorded package boundaries and
missing manifest coverage. See the [package profile](web-package-profile.md).
The owning web regression completed 327 cases: 320 passed and seven explicitly
skipped (six unavailable LLVM Zstd map cases and the inapplicable ASAR-policy
omission case). This includes complete Claude Code readable-source recovery
and the six-platform Bun corpus. The enabled-backend capability test passed;
the backend-omission-only API case was inapplicable and skipped.

The official Claude Code 2.1.296 registry response was separately inspected as
explicit `package-json` evidence: one root, eight optional platform dependency
declarations, two scripts and one bin declaration. Its 3,531 bytes have SHA-256
`0fb79dc0c21d02fc14feeb7f9a86fa15ca3937598f9f3a6a03816bb15e7fa6e4`.
This is a projection of supplied registry metadata, not inspection of an npm
tarball or proof that its native binary matches the standalone distribution.
No declared script ran and no dependency was resolved. Archive/hash comparison
was the next package boundary at that checkpoint; the qualified archive/SRI
increment below supplies that comparison. At that checkpoint, #715 remained
partial.

The user-required LLVM filename/project/Doxygen headers now cover the web
implementation and its C++ fixtures/adapters. Package algorithms have their
own `lib/web/packages` directory. Fixed diagnostics moved to `web/Error.h`,
removing the bottom-up dependency from readers/algorithms to the session API.
The existing `Session.h` still includes that contract for source compatibility.

## Passive interfaces and reviewed HAR — 2026-10-11

Native C++ HAR preview/commit, direct fetch/WebSocket source candidates and
explicit method/origin/path comparison now share SDK, CLI and worker results.
Private URLs, header/cookie/query/body values and arbitrary names remain
excluded from metadata. Tokens bind the immutable original and revision;
the CLI requires an explicit hash/policy receipt. Candidate matches preserve
separate source inference and imported-observation classes. Independent review
corrected base-dependent URL coercion and missing URL-query/form preview counts.
See the [interface profile](web-interface-profile.md).

The restored macOS arm64 Release run exercised 373 owning web cases: 366
passed and seven explicitly skipped (six unavailable LLVM Zstd map cases and
the inapplicable ASAR-policy omission case). The original Bun/Claude/npm corpus
remained enabled, including all 2,345 recovered JS modules and the new
container-to-source interface check. That check locates one fetch in Claude
module 1,897 at byte 65,473, length 52; its runtime URL/options remain unknown.
No traffic was supplied or invented. Enabled API availability and all five
selected C++ worker regressions passed.

With only JS disabled, all 11 HAR/SDK cases and five worker cases passed;
source-dependent APIs reported unavailable. With the entire web backend
disabled, both ABI cases and four generic worker cases passed, while the web
worker case explicitly skipped. Both feature switches are restored. Formatting
and whitespace checks passed. #717 remains partial: broader source request
flow, response/transformation evidence, schema export and stream adapters remain.

## Passive stream and protocol records — 2026-10-11

Six explicit native C++ profiles now cover JSONL, SSE, JSON-RPC object shapes,
MCP 2025-06-18 stdio base-message shapes, recorded envelopes and diagnostic
lines. Core framing, protocol classification and recorded-ID candidates have
separate owners under `lib/web/streams`; session/SDK/CLI/worker publish only
metadata after redaction preview/commit. Receipts bind original hash, profile
and policy, so changing interpretation requires a matching preview. No real
Claude Code transcript was supplied or invented; source-to-stream correlation
remains unimplemented. See the [stream profiles](web-stream-profile.md).

Independent review added original-token numeric ID checks, whole-capture
coverage-gap refusal, shared JSON node accounting including failed records,
and separate exact-decimal integrality for JSON-RPC error codes. Private
session/ID strings exist only during inspection; joins borrow that storage
instead of duplicating it. Default pages exclude payloads, log text, custom
names, recorded IDs, sessions and timestamps.

After merging dev through `a40f1c633`, the restored macOS arm64 Release run
exercised 390 owning web cases: 383 passed and seven explicitly skipped
(six unavailable LLVM Zstd map cases and the inapplicable ASAR-policy omission
case). The full optional Bun/Claude/npm corpus remained enabled, including
all 2,345 recovered Claude Code JavaScript modules. Enabled API availability,
all five selected C++ worker regressions and the repository capability manifest
check passed. The new three public APIs have explicit ledger ownership.

With only JS disabled, all 22 stream/SDK/shared-JSON cases and five worker
cases passed. With the entire backend disabled, both ABI cases and four
generic worker cases passed; the web worker case explicitly skipped. Both
feature switches are restored. Scoped LLVM formatting and whitespace checks
passed. These results qualify the supplied input profiles on this host,
not full MCP transport/schema conformance, authenticated sessions, other
hosts or completion of the five epics.

## Original npm archives and SRI — 2026-10-11

`PackageArchive` now admits bounded USTAR/local-PAX and single-gzip originals
through native linked zlib, rejecting ambiguous names, unsupported extensions,
bad checksums, trailing streams and expanded-budget excess before publication.
Links remain unavailable metadata. Derived bytes use the shared private spool.
Archive members enter package/source/HTML/Bun/native consumers through
`ArtifactView` and the common member namespace. Original, expanded and nested
container offsets remain distinct; review found and corrected a tar→Bun asset
anchor ambiguity, covered by explicit tar and tgz regressions.

`PackageIntegrity` owns one shared SRI declaration parser and verification over
original bytes. Callers explicitly select captured registry metadata or a
package instance in an admitted npm lock. The verifier uses the strongest
supported algorithm, accepts any matching equal-strength candidate, and keeps
missing/invalid/unsupported/Git/mismatch outcomes separate. SHA-384/512 have
independent standard-vector and chunk-boundary coverage. Digest equality does
not authenticate a publisher or establish a behavior verdict.

NeverD admitted both official Claude Code 2.1.296 npm originals and matched
their captured SHA-512 declarations. The 114,864,541-byte Linux x64 archive
contains the exact 257,068,216-byte standalone image already qualified for
2,589 Bun modules and 2,345 recovered JS modules. Original and member SHA-256
values are pinned by optional C++ tests. Targets and lifecycle scripts never
ran. See the [archive/integrity profile](web-package-archive-profile.md) and
[real-artifact qualification](web-claude-code-qualification.md).

After merging dev through `6db31ba1e`, the restored macOS arm64 Release
configuration exercised 352 owning web cases: 345 passed and seven explicitly
skipped (six unavailable LLVM Zstd map cases and the inapplicable ASAR-policy
omission case). This includes both real npm originals, the full Bun corpus
and all 2,345 Claude Code JavaScript modules. The compiled-capability case
and all five selected C++ worker cases passed; the API backend-omission case
was inapplicable and skipped. With JS and zlib disabled, 39 of 42 focused core
and 10 of 11 SDK cases passed, with unavailable gzip cases explicitly skipped;
both official original SRI checks still passed. With the entire backend
disabled, both ABI availability cases and four generic worker cases passed,
and the web worker case explicitly skipped. Web, JS and zlib support are
restored. Formatting and whitespace checks passed. These are feature-owning
checks, not a whole-repository or cross-host qualification.

On 2026-10-11 the Bun container reader was separated from the shared graph
decoder and qualified for Linux, macOS and Windows x64/ARM64 input images on
the macOS arm64 Release host. Twenty additional full pinned compiler images
cover each new target's plain source, UTF-16, assets/maps and bytecode/maps.
Independent C++ manifests retain full-container and member hashes; the graph
bytes are checked in, and native runtimes stay outside the repository.
The existing five Linux x64 images and official Claude Code input still pass.
Mach-O tests additionally mutate typed commands, metadata aliases and platform
declarations, including the real x64/ARM64 images. A metadata read in the new
test initially exceeded the store's 8-MiB per-read bound; changing that test to
bounded reads fixed it without changing production limits.

The owning web run exercised 298 cases: 291 passed after that test repair;
six map cases require unavailable LLVM Zstd support, and one ASAR-policy
omission case is inapplicable in this enabled build. All seven selected worker
regressions passed. The new SDK case checks all six targets' profile metadata,
decoded sources and verified local export. Manual CLI analysis of real Linux,
macOS and Windows ARM64 files succeeded with an unusable external-tool PATH.
No generated target or downloaded application was executed. These results
qualify extraction, not a Windows analyzer host, cache decoding, signature
validation or arbitrary future Bun versions. See the
[Bun profile](web-bun-profile.md) for precise admission and corpus contracts.

The C++ source model now retains field roles, list ordinals and exact UTF-16
values privately. Thirteen binding cases cover shadowing/hoisting, parameter
default environments, destructuring, loops/switch/catch, class/function names,
imports/exports, CommonJS/arguments, dynamic lookup, declaration conflicts,
Unicode names and deterministic budget refusal. The two-million-step limit
was exercised with a parsed 40,000-reference source; no partial graph escaped.
SDK/CLI/worker queries expose only metadata and stable relationships.

The finite primitive evaluator now uses LLVM APFloat for binary64 operations,
bounded APInt arithmetic for BigInts, exact private UTF-16 strings and explicit
short-circuit rules. Number formatting uses the pinned embedded Hermes support
library in a separate translation unit. No LLVM/LLVH headers are mixed.
Parser/formatter calls reject an unqualified host rounding mode; APFloat
arithmetic on an admitted model retains its explicit rounding mode.
Number exponentiation and mixed BigInt comparison remain explicit refusals.

Effect summaries separate immediate evaluation from deferred function bodies,
default parameters and instance-field initializers. Static class work, computed
keys, getter/proxy uncertainty and declaration preservation have focused tests.
The source-tree validator is shared by bindings, values, effects and modules. SDK, CLI
and worker expose only fixed categories and evidence identities; computed
primitive values remain private and never grant rewrite permission.

The module profile now inventories ESM requests/entries and dynamic import /
require candidates with lexical evidence. Static relative specifiers can match
only exact admitted directory members; those candidates remain unverified runtime
targets. Source-map names and single-file imports never supply a trusted host
directory base. Every output surface keeps specifiers, names, paths and assertion
values private. Static `with` import attributes, require aliases, CommonJS export
inference and foreign export linkage are explicitly unimplemented.

The fixed `webpack5-basic-loader-v1` now recovers original source partitions,
their wrapper/body hashes and candidate dependency edges without reparsing away
outer closure context. This qualifies one basic loader shape, not arbitrary
webpack output, execution, original filenames or an entrypoint reconstruction.
An archived upstream document and its exact code-fence derivation are recorded
under `unittests/web/fixtures/webpack-manifest.json`; MIT notices are shipped.

The fixed Bun Linux x64 profile now uses bounded disk-backed ranges. Five
self-authored compiler outputs cover plain code, Unicode, actual UTF-16
storage, minified assets/maps and CJS with JSC cache/maps. Their pinned compiler
and template hashes, exact graph bytes and independent member manifests are
preserved under `unittests/web/fixtures/bun`; the native runtime is not checked
in. Full-container qualification passed locally with the original image hashes;
that test explicitly skips when the separately supplied corpus is absent.
Eight C++ core cases cover original offsets/hashes, immutable ownership,
malformed footer/ELF/pointers, aliasing, counts, encoding and refusal. Three
SDK/CLI cases and worker checks cover paging, revisions, private names and
decoded source identities reaching the C++ parser/binding analysis. Ordinary
output also hides isolated name/origin hashes. No runtime is invoked by the
analyzer or tests. Manual fixture preparation used the pinned trusted compiler
as a development activity; the optional independent recorder is C++.

The enabled profile passed all 126 owning web tests, including the full corpus
check, and the compiled-capability check. The backend-omission-only case was
explicitly skipped in that enabled configuration. Bun reference notices were
checked byte-for-byte beside binaries, in the SDK and via the generated install
rules in an isolated prefix. This does not claim a full product install.

With only the JavaScript parser disabled, all 52 remaining web cases, the
compiled-capability check and the three C++ worker protocol/graph/web cases
passed. Thus Bun extraction does not depend on the parser being enabled;
source-analysis requests correctly report unavailable.

The broader worker regression exposed a performance defect in the previously
added duplicate-key check: nlohmann 3.11.3's DOM filter callback scans the entire
parent array at every object end, even when the callback discards nothing.
This made native history save/read operations intermittently exceed their
existing request timeout. A separate optimized C++ experiment on the same
7,209,891-byte / 29,000-object input measured 45 ms for ordinary parsing and
6,135 ms for the callback path. The reader now uses the public SAX interface
for decoded-key admission followed by ordinary DOM parsing. The same local
experiment measured 43 ms ordinary / 150 ms strict after the change, with equal
results. These are local measurements, not a hard latency guarantee.
A C++ regression preserves the large array and rejects late/nested duplicate
keys. All nine worker tests then passed, including the native history case;
neither its timeout nor the history implementation was changed.

With the entire web backend disabled, both ABI availability cases passed,
including the new Bun functions. The CLI Bun command returned
`capability_unavailable` with exit status 1; protocol/graph worker tests passed
and the actual web worker case explicitly skipped. These disabled configurations
do not establish extraction support on another host or target platform.

Both build options have been restored to ON. All 126 owning web cases passed
again after restoration, including the separately supplied full Bun corpus,
and the compiled-capability case passed; the backend-omission case skipped as
expected. Formatting checks and `git diff --check` passed.

The next increment adds `bun-1.4.2-serialized-source-map-v1`: an independent
C++ reader for the pinned window/varint format and its per-source Zstd frames.
It reuses the in-process LLVM Support decoder after strict frame/size/window
preflight. No new runtime, external tool, script or package installation is
involved. The original map region, compressed source range/hash and derived
UTF-8 source identity remain separately queryable. A malformed map refuses
without losing the original extraction or publishing a partial decoded map.

The existing map C API/worker operations accept a module's
`source_map_region_id`; the CLI adds `web bun-map FILE MODULE_INDEX`.
The producer drops names, unmapped segments, source roots, file metadata and
ignore lists, so replies expose that loss rather than inferring absent data.
Container association remains unverified, and lookup never projects a retained
anchor into an intervening mapped span.

All 134 owning web tests passed with the full corpus supplied, and all nine
worker regressions passed. Six new C++ decoder cases cover both preserved
compiler maps, independently recorded original-source hashes and text anchors,
rare/multiwindow deltas, duplicate anchors, every truncation boundary, malformed
varints/masks/indexes and native frame corruption. Decompression admits exactly
8 MiB total (two 4 MiB sources) and refuses one additional byte before decoding.
Two new SDK/CLI cases check source parsing, association distinctions, cache
publication/revisions and private output with an unusable external-tool PATH.
The worker compares direct and framed map/source/segment results.

With `NEVERD_ENABLE_WEB_JAVASCRIPT=OFF`, all 60 remaining web cases passed,
including both real serialized-map fixtures and the separate full-container
corpus. Compiled-capability and three C++ worker protocol/graph/web checks also
passed; the backend-omission-only ABI case skipped as expected. A separate build
with LLVM Zstd disabled has not been qualified in this increment; the capability
and fixed unavailability path are explicit. No new public C entry point was
needed for map decoding.

The JavaScript parser is restored to ON; the web backend and LLVM Zstd are ON.
All 134 web cases, the compiled-capability case and the actual web worker case
passed after restoration; the backend-omission-only case skipped. The new
`bun-map` CLI also decoded both complete pinned `asset-map.elf` and
`cache-map.elf` files with an unusable external-tool PATH, zero exit status and
empty stderr. Their raw map hashes match the preserved manifests and storage
offsets refer to the original 81 MB containers. Run logs and these metadata-only
JSONL outputs are preserved under `/tmp/neverd-bun-qualification-1.4.2/` with
the `serialized-map-` prefix. Formatting and diff-whitespace checks passed.

This checkpoint preceded source-to-storage coordinates and source navigation/
redaction, implemented in the increments below. Other container/consumer
milestones remain in scope.
Binding identities do not establish runtime values, initialization, reflection
safety or API attribution. Annex B cases remain partial; unknown semantics fail
closed. Ordinary queries retain metadata-only output; explicit source views
follow their separate preview/publication policy.
No source transformation or runtime module loader is claimed. Bun extraction
does not authenticate the producer/version; PE/Mach-O and independent Node
family profiles remain unavailable. The five epics remain open implementation work.

## Reviewed source display increment — 2026-10-10

The C++ parser now owns a bounded lexer/comment inventory separately from syntax
validity. `javascript-source-view-v1` preserves fixed syntax and ASCII layout,
uses lexical/occurrence aliases for identifiers and masks values, comments,
hashbangs, regex/template text and unclassified regions by default. Keyword
property names are treated as target names. Originals and licenses remain in
immutable source evidence; the view is inert display text, not transformed JS.

Source-view preview publishes only counts, categories, hashes and exact review
ranges. Segment pages are available for review before publication. A matching
single-use token commits the candidate; only then can callers read UTF-8/CRLF
aligned chunks. Nonempty original ranges require an explicit local-review
caller assertion, whole lexical/layout regions and a distinct policy identity.
Policy replacement revokes a source's previous view, and import clears all views.
The two published views plus one pending candidate have independent text/record
limits. SDK and worker share the backend contract; `neverd web view` uses only
the structural default policy, with no raw-disclosure option or subprocess.

Eight new C++ core cases and four SDK/CLI cases passed. They cover private
canaries, regex/template rescans, keyword properties, private/escaped names,
dynamic/shadowed bindings, the lexer retention boundary, exact range coverage,
explicit local review, preview consumption, policy/revision invalidation,
Unicode/CRLF chunks and the published-view cache limit. The worker matches
direct/framed preview, record, commit and chunk results and refuses uncommitted
text and undeclared disclosure fields.

All 146 web cases passed with the pinned full Bun corpus enabled, including
70 source, 28 SDK/CLI, 15 map and 33 artifact/storage cases. All nine worker
regressions passed. The compiled-capability check passed; the omission-only ABI
case skipped in the enabled profile. Logs use `/tmp/neverd-source-view-*.log`.
With the JS parser disabled, all 60 remaining web cases passed, including the
new source-view unavailable-path checks. The compiled-capability check and
three C++ worker checks passed; the backend-omission ABI case skipped as expected.
With the entire web backend disabled, both ABI availability cases passed,
including all four new public entry points. CLI capabilities returned
`capability_unavailable` with exit status 1. Worker protocol/graph checks passed;
the actual web worker case explicitly skipped in that profile.

Both web options and LLVM Zstd are restored to ON. All 146 web cases passed
again with the pinned full Bun corpus, including decoded Bun source/hash to
reviewed-view integration. The compiled-capability and actual framed web worker
checks passed after restoration; the omission-only ABI case skipped as expected.
Formatting and diff-whitespace checks passed. This qualification is local macOS
arm64 Release; broader host qualification remains pending.

Feature-specific navigation/tool attribution, semantic transformation receipts,
export and consumer-wide redaction remain pending. Basic navigation and Bun
storage coordinates are implemented in the next increment.
This increment does not complete #714 or the five-epic goal.

## Source navigation and storage anchors — 2026-10-10

The bounded C++ navigation index now retains functions, calls and references,
source containment, body/name nodes, declaration/initializer binding links and
syntactic direct-function-expression links. Runtime callees remain unproven;
member access, reassignment, initialization and dynamic environments cannot be
resolved by source-name matching. Dynamic-import source arguments are not
misclassified as callees. Unavailable bindings preserve syntax while omitting
lexical links; failed navigation publishes no partial records.

The metadata-only anchor query validates UTF-8/CRLF boundaries and returns
line/UTF-16 coordinates, original storage and optional committed-view coverage.
Bun member offsets use the same Latin-1/UTF-16LE/client-UTF-8 decoder as source
extraction. Stored terminators are excluded. Compressed map sources identify
their entire original Zstd frame; standard JSON sources retain map/source IDs
with unavailable encoded-member offsets. Hidden display interiors map to whole
labels, never invented interior positions. Parse status, storage precision,
runtime uncertainty and producer authenticity remain separate.

SDK/worker operations and `navigate`/`anchor` CLI commands share these owners.
`bun-navigate`, `bun-view` and `bun-anchor` select a module index and explicit
source type in the same process. No target execution, external extractor,
script, temporary JS file or raw-source export is involved.

Five new core navigation cases, one display-location case, two Bun range cases
and six SDK/CLI cases passed. Existing preserved compiler-graph cases now also
check navigation/storage/view integration. Actual framed worker results match
direct navigation and anchor queries, including canonical-offset and unknown-
field refusal. All 160 web cases passed with the full pinned Bun corpus:
76 source, 34 SDK/CLI, 35 artifact/storage and 15 map cases. All nine worker
regressions and the compiled-capability check passed. The omission-only ABI
case skipped in the enabled profile. Logs use `/tmp/neverd-source-navigation-*.log`.
With the JS parser disabled, all 62 remaining web cases passed, including the
navigation/anchor unavailable-path checks and Bun range tests. The compiled-
capability check and three C++ worker cases passed; the backend-omission ABI
case skipped as expected. With the entire web backend disabled, both ABI
availability cases passed, including all three new entry points. CLI
capabilities returned `capability_unavailable` with exit status 1. Worker
protocol/graph checks passed; the real web worker case explicitly skipped.
Both web options and LLVM Zstd are restored to ON. All 160 web cases passed
again with the pinned full Bun corpus, and the compiled-capability and actual
framed web worker checks passed. The omission-only ABI case skipped as
expected. Formatting and diff-whitespace checks passed. This remains local
macOS arm64 Release qualification; other hosts need separate evidence.

Tool registration/configuration/permission/MCP/hook attribution, reversible
semantic transforms, other formats/platforms and the remaining
package/desktop/protocol consumers are still required for the five epics.

## Immutable native handoff increment

`immutable-native-handoff-v1` selects an original artifact or Bun asset region
by captured revision and occurrence ID. The SDK copies that immutable selection
into a bounded buffer, checks the loader's complete input SHA-256 and publishes
a new native handle only with its successful metadata response. The origin
retains either artifact/parent IDs or exact Bun container, module and byte-range
IDs. No input path is reopened. Encoded source, maps, caches and partial native
regions cannot stand in for an independent native image. Universal Mach-O is
refused until explicit slice selection exists.

ELF, COFF and Mach-O file/buffer entry points now share their format parser.
The SDK likewise shares decoder preparation and image publication. Memory
sessions omit debug-companion discovery and sidecars, reject path-dependent
persistence/patching, and retain their captured file size and hash after source
deletion. Successful ordinary file reload clears handoff provenance; failed
reload leaves the existing image usable. Explicit `native_analyze` uses the
existing static pipeline and returns metadata, not recovered original source.
CLI adds `native`, `native-analyze`, `bun-native` and `bun-native-analyze`;
the worker owns an independent handoff without replacing its native project.

The enabled-profile build passed. All 169 web cases passed with the pinned
Bun corpus, including nine new handoff cases; all 65 existing Session C API
cases passed. Handoff cases cover x64 ELF/PE/thin Mach-O and AArch64 ELF,
a PE signature beyond a 4-KiB prefix, malformed/truncated inputs, source and
web-session lifetime, sidecar/patch refusal, transactional reload, directory
occurrence identity, Bun native assets and metadata-only offline CLI output.
The full pinned Bun container was loaded and hash-checked without execution;
its complete native pipeline was not run. Both SDK and CLI static-analysis
checks use the small synthetic fixture. All new tests and fixture construction
are C++.

The first parallel worker run had four eight-second startup-handshake timeouts
before any requested operation; the new C++ web worker and the other native
operation cases passed. Without code or timeout changes, all nine worker cases
then passed sequentially and in a repeated four-way parallel run. The initial
startup cause has not been isolated; these results do not establish a hard
startup-latency bound. Logs are retained as
`/tmp/neverd-native-handoff-*-tests.log` and
`/tmp/neverd-native-handoff-worker-parallel-recheck.log`.

With only the JavaScript parser disabled, all 71 registered web cases passed,
including the nine handoff cases and full Bun corpus. The compiled-capability
case and three C++ worker cases passed; the backend-omission ABI case skipped
as expected. With the entire web backend disabled, both availability cases
passed, including all three new native entry points and output-handle clearing.
CLI `web capabilities` and `web native` returned `capability_unavailable` with
exit status 1. Worker protocol/graph cases passed and its real web case skipped.

The web backend, JavaScript parser and LLVM Zstd options were restored to ON.
The restored build passed all 169 web cases with the full Bun corpus and all
nine worker cases in parallel. Its compiled-capability case passed; the
backend-omission-only case skipped as expected. Native handoff cases explicitly
skip on Windows, where the web input reader is not implemented; that is missing
host coverage, not a successful native-import qualification. The root native
aggregate was not run for this increment; the 65-case Session C API suite and
the cross-format handoff cases are the native regression evidence.

Native input materialization is capped at 256 MiB. Existing native loader and
pipeline work/allocations have their own contracts, not the JS parser's budgets
or a new hard process bound. Standard native SDK queries retain their existing
raw-name/text disclosure contracts; the web handoff report is metadata-only.
Other hosts, universal-slice selection and broader native resource qualification
remain pending. This increment does not complete desktop/package extractors,
other Bun layouts or any of the five epics.

## ASAR and captured unpacked members — 2026-10-10

`asar-pickle-json-v1` reads explicitly selected captured occurrences entirely
in C++. It validates Pickle framing, bounded JSON, safe Unicode path identities,
all ranges and non-overlap before reading member payloads. Optional SHA-256
declarations verify the whole member and every block, including the pinned
writer's empty terminal block. Unknown integrity algorithms, mismatches,
missing unpacked files and unfollowed links remain distinct unavailable states.
Available files without a declaration retain `integrity_status: missing`.
Extraction completeness covers declared members; unreferenced payload bytes
are counted separately and preserved in the original archive. No integrity
state authenticates an application publisher.

An unpacked root must be selected from the same immutable input snapshot.
There is no host companion discovery. Exact relative names select captured
occurrences; Unicode comparison keys only reject collisions and never rename
evidence. The cumulative path allowance charges both archive declarations and
every entry indexed under the selected root, including unreferenced entries.
Fixed upstream archives, original member bytes and their hashes are recorded
in the [fixture manifest](../unittests/web/fixtures/asar/README.md).

`ArtifactView` is the shared direct-byte/origin owner for original files, Bun
assets and available ASAR members. Source, map, source-storage anchors and
native handoff consume it. Source module comparisons use the extraction's own
virtual namespace; unavailable members cannot become candidate file bytes.
Packed anchors point to archive ranges; unpacked anchors and native reports
retain the actual captured file together with its archive declaration.
Native handles keep their immutable bytes independently of source lifetime.
Session metadata and artifact pages use one analysis-status owner.

The C ABI, CLI and actual framed C++ worker expose extraction and member pages.
Queries retain revisions, cache ceilings and metadata-only output. Explicit
CLI selection also supports source, views, navigation, anchors, maps and native
handoff. The C++ CLI tests make external analysis tools unavailable through
`PATH`. Failed structural extraction publishes no cache entry; reimport
invalidates old extraction identities and four cached selections are bounded.

The native Unicode policy requires ICU 77.1 and Unicode 16.0. ASAR and ICU
licenses are preserved, staged and installed alongside existing notices.
ICU runtime distribution/static-link closure is still a release requirement;
the current host supplies its native ICU libraries. This does not qualify a
portable release, Windows capture, general archive export or the remaining
desktop adapters. See the [ASAR profile](web-asar-profile.md).

With ASAR alone disabled, the SDK exercised an admitted capture and received
`archive_path_policy_unavailable` without creating an extraction. The compiled
capability check and three C++ worker cases passed; the backend-omission ABI
case skipped as expected. With the JavaScript parser disabled and ASAR enabled,
87 web cases passed and the ASAR-omission-only case skipped. This included the
new selected-index path-budget regression, all fixed ASAR archives and the
full pinned Bun corpus. The compiled-capability and three C++ worker checks
passed; the backend-omission-only ABI case skipped in that profile.
With the whole backend disabled, both ABI availability checks passed, including
the two new ASAR exports. CLI `web capabilities` and `web asar` returned
`capability_unavailable` with exit status 1. Worker protocol/graph cases passed;
the real web worker case explicitly skipped. Logs use
`/tmp/neverd-asar-no-parser-*.log` and `/tmp/neverd-asar-no-backend-*.log`.

The backend, parser, ASAR and LLVM Zstd options are restored to ON. The restored
build passed 185 web cases, with one expected ASAR-omission-only skip out of
186 registered cases. This includes 11 ASAR core cases, five ASAR SDK/CLI cases,
the selected unpacked CLI flow, and the full pinned Bun corpus. All 65 Session
C API cases and all nine worker cases passed; the compiled-capability case
passed and its backend-omission-only counterpart skipped. The final worker run
was sequential and followed the web/native checks. Logs use
`/tmp/neverd-asar-restored-*.log`.

An earlier full worker run in this increment failed the transport mock's
eight-second initial hello deadline before any requested operation; its other
eight cases passed. The same unchanged test then passed three isolated runs,
and the final full worker run passed. Neither code nor timeout was changed to
obtain these results. The startup cause remains unisolated, consistent with
the earlier native-handoff observation; it is not reported as fixed. The
original and isolated evidence remains in
`/tmp/neverd-asar-full-worker-tests.log` and
`/tmp/neverd-asar-transport-isolated-tests.log`.

ASAR/ICU notices matched the preserved originals byte-for-byte beside binaries,
in the staged SDK and after exercising the owning Web install rules in an
isolated prefix. This qualifies those notice rules, not a full product install
or ICU runtime redistribution. Formatting and diff-whitespace checks passed.
Qualification remains local macOS arm64 Release. The existing root semantic
test discovery limitation recorded above was not bypassed or counted as a
whole-repository success. Electron manifest/entry/IPC analysis, safe export,
NW.js/VSIX and the other four epics remain required work; this increment does
not complete issue #716 or the active five-epic goal.

## Electron manifest and source boundaries — 2026-10-10

`SourceOrigins` now owns finite syntactic module provenance over the existing
binding and module owners. ESM imports, literal require candidates, unwritten
initializers, object destructuring and static members retain their module request.
Construction stays distinct from its constructor. Shadowing, writes (including
unbound require writes), dynamic references, direct eval, alias cycles, unsupported
patterns, call results and budgets never supply invented origins. This does not
establish runtime module values, initialization, reachability or call targets.

`ElectronSource` consumes those private origins and the existing finite value
analysis. Its records cover window construction, uniquely declared preload
options, constructed-window renderer/webContents operations, ipcMain/ipcRenderer
and contextBridge candidates. Each record keeps original node/range and evidence
IDs, value and callback/API nodes. Constants stay private; failed origin/value
analysis remains explicit. Getter/spread/duplicate/computed option cases cannot
invent a unique preload. Runtime targets, permissions and channel correlation
are unverified. The result is always partial source evidence, including when
no supported candidate is found.

`ElectronManifest` is independent of the JS parser. It checks the captured hash,
parses bounded JSON and compares the main entry only with exact available members
of its manifest directory. It supports the pinned Electron v40.0.0 main fallback
and extension/type entry-mode rule without selecting or executing a runtime.
ASAR manifests can link to explicitly captured unpacked main members with the
same occurrence IDs; single files have no implicit host directory. Declared
names/versions/dependencies remain private metadata, not runtime authentication.

The three C ABI exports, worker operations and direct/ASAR/Bun CLI consumers use
the shared C++ owners. Manifest caches are bounded at 16; source results share
the existing source-cache lifetime. Import replacement clears both. All new
implementation and fixture/test logic is C++; targets remain inert strings.

The enabled build passed 201 web cases with one expected ASAR-omission-only
skip out of 202 registered cases. Four new origin, four Electron source, four
manifest and four SDK/CLI cases passed, including exact anchor joins, canaries,
source/manifest lifetime, cache/revision/page refusal, explicit unpacked main
and offline CLI operation. The existing Bun CLI case also exercised the new
consumer through decoded source identities. All nine worker checks passed;
its C++ client compared actual framed manifest/source/record results with direct
calls and rejected unknown fields and stale requests. The compiled-capability
case passed; the backend-omission-only ABI case skipped in that enabled profile.
Logs use `/tmp/neverd-electron-full-*.log`.

With the JS parser disabled, 95 web cases passed and the ASAR-omission-only
case skipped, out of 96 registered cases. This included manifest comparison
and the full pinned Bun corpus; source-boundary API/CLI refusal was exercised
explicitly. The compiled-capability check and three C++ worker checks passed;
the backend-omission-only ABI check skipped as expected. Logs use
`/tmp/neverd-electron-no-parser-*.log`.

With the whole backend omitted, both ABI availability cases passed, including
all three Electron exports. Manifest/source CLI commands returned
`capability_unavailable` with exit status 1; worker protocol/graph checks passed
and the actual web check explicitly skipped. The backend, parser, ASAR and LLVM
Zstd settings were then restored to ON. The restored build again passed 201 of
202 web cases (one expected ASAR-omission-only skip), the compiled-capability
check and all nine worker checks. Logs use
`/tmp/neverd-electron-no-backend-*.log` and
`/tmp/neverd-electron-restored-*.log`. These local results do not qualify another
host or a complete Electron distribution.

Application-scoped preload/renderer association, HTML entries, IPC channel
correlation, broader dataflow/wrappers and distribution/version qualification
remain required. The existing archive/export and other desktop adapters are
also incomplete. See the [profile](web-electron-profile.md); these candidates
do not complete Electron, #716 or any of the five epics.

## Explicit Electron IPC scope — 2026-10-10

`ElectronIPC` now compares private channels from explicitly selected source
evidence in one captured manifest directory namespace. The declared exact-file
main candidate and its candidate source type are required. Original and ASAR
occurrences remain distinct even for equal bytes. Inputs are canonicalized by
source ID; no host discovery, implicit imports or source execution occurs.
Sources with unavailable origin analysis remain visible with their refusal.

Channel equality preserves exact UTF-16 including NUL, lone surrogates and
normalization distinctions. Group IDs bind the complete evidence scope and
occurrence ordinal; no channel text or isolated channel hash is output.
Endpoint records join the original source/artifact/boundary/node/range. The
three compatible direction categories report candidate pair counts instead
of allocating a quadratic edge list. Removal order, runtime roles, routing and
selection completeness remain explicitly unverified. Dynamic arguments retain
unresolved endpoints. ContextBridge keys do not become IPC channels.

The C++ C ABI, CLI and worker expose analysis plus source/channel/endpoint pages.
Limits cover 16 sources, 10,000 aggregate selected boundary records, 1,048,576
channel code units and 2,000,000 work steps including comparison units. Four
immutable scopes may be cached per revision; failure cannot publish partial
correlation state and import replacement clears them.

Five new core cases and all six Electron SDK/CLI cases passed after fixing the
CLI request's source-ID lifetime: LLVM JSON `StringRef` values borrowed each
temporary source response, so the aggregate now stores owned string copies.
Both original-directory and ASAR CLI paths pass with an unusable PATH. The
three C++ worker checks passed, including actual framed two-source correlation,
all three page kinds, private-output canaries and unknown-field refusal. Logs
use `/tmp/neverd-electron-ipc-focused-tests.log` and
`/tmp/neverd-electron-ipc-worker-tests.log`.

The fully enabled local Release build passed 208 of 209 registered web cases,
with one expected ASAR-omission-only skip. This includes the separately supplied
full pinned Bun corpus, all web source/map/native cases and the new IPC increment.
All nine worker regressions and the compiled-capability check passed; the
backend-omission-only ABI case skipped as expected. Logs use
`/tmp/neverd-electron-ipc-full-*.log`.

With the parser disabled, all five applicable Electron SDK/CLI cases, the
compiled-capability check and three C++ worker checks passed. Manifest inspection
remained available; both new IPC operations and CLI analysis returned explicit
unavailability. This focused omission run did not rerun unrelated web cases.
Logs use `/tmp/neverd-electron-ipc-no-parser-*.log`.

With the entire backend disabled, both availability cases passed, including
the two new IPC exports. The original/ASAR IPC CLI forms returned
`capability_unavailable` with exit status 1. Worker protocol/graph cases passed;
the web case explicitly skipped. Logs use
`/tmp/neverd-electron-ipc-no-backend-*.log`.

The backend, parser, ASAR and LLVM Zstd options are restored to ON. The restored
build again passed 208 web cases with one expected skip out of 209 registered,
the compiled-capability case and all nine worker checks. The backend-omission
ABI case skipped in that enabled configuration. Logs use
`/tmp/neverd-electron-ipc-restored-*.log`. Focused formatting and
`git diff --check` passed. This remains local macOS arm64 Release qualification,
not a whole-repository or other-host success claim.

Preload/renderer/HTML entry association, source import closure, window-specific
and runtime routing, broader dataflow, distribution/version qualification and
the other adapters/export consumers remain required. This is an explicit
source selection contract, not a complete application graph or completion of
any epic.

## Captured Electron entry candidates — 2026-10-10

This increment is implementation progress toward #716. `ElectronSelection`
now owns shared manifest/source admission for IPC and entry consumers.
`SourcePaths` owns finite path-expression analysis in a captured namespace;
`ElectronEntries` compares only exact available member occurrences. Relative
loadFile values use the manifest application directory, while dirname-based
expressions use their captured source directory. Relative preload literals
stay unresolved. No host filesystem lookup or target code execution occurs.

The path profile covers unwritten CommonJS wrapper roots, app.getAppPath(),
ESM import.meta roots, simple initializer aliases, string concatenation and
templates, constant conditionals, finite native path operations and captured
file URL conversion/serialization. Raw concatenation, join, resolve and
normalization retain separate rules. Unsupported path flavors, unknown cwd,
external absolute paths, unsafe names/encoding, namespace escapes, cycles,
dynamic bindings and unsupported URL references refuse. All roots, callees
and target links remain unverified runtime candidates; source provenance does
not prove initialization order, immutable properties or runtime reachability.
The complete contract and pinned API references are in the
[Electron profile](web-electron-profile.md#captured-preload-and-renderer-entry-candidates).

C ABI, CLI and framed worker expose summaries and source/entry pages, including
original node/range links, captured target artifact IDs and optionally selected
target source IDs. Private paths and URLs remain absent. ASAR occurrence
membership is retained despite equal bytes in another captured file; unavailable
unpacked members cannot supply links. Four immutable scopes may be cached per
revision; import replacement revokes them. Path work/allocation failures clear
the entire source path result, and aggregate admission failure publishes no
entry cache. Requested nodes are sorted and deduplicated for both identity and
evaluation, so reordering or repeating an admitted request cannot change its
budget outcome.

The initial focused run passed seven C++ path cases, four entry-association cases
and all nine Electron SDK/CLI cases. They cover distinct roots, ESM file URLs, unknown/refused entries,
actual independent work/string budget exhaustion, canonical request order,
scope and cache failures, source anchors, immutable capture after deletion and
private-output canaries. Original/ASAR CLI requests pass with an unusable PATH.
The direct/framed worker checks both new operations, links/refusal states,
pages and unknown-field rejection. No new scripts or external analyzers were
introduced. Logs use `/tmp/neverd-electron-entry-core-tests.log`,
`/tmp/neverd-electron-entry-sdk-tests.log` and
`/tmp/neverd-electron-entry-worker-tests.log`.

After canonicalization and URL href support, the enabled macOS arm64 Release
run passed 222 of 223 registered web cases, with one expected ASAR-omission-only
skip. It includes the separately supplied full pinned Bun corpus, 100 source
cases, all 49 SDK cases and the nine native cases. All nine worker regressions
and the compiled-capability check passed; the backend-omission-only ABI case
skipped as expected. Logs use `/tmp/neverd-electron-entry-full-*.log`.

With the JS parser disabled, all seven applicable Electron SDK/CLI cases and
three C++ worker cases passed. Manifest inspection remained available while
both entry operations and original/ASAR entry CLI forms returned explicit
unavailability. The compiled-capability check passed and the backend-omission
ABI case skipped as expected. Logs use
`/tmp/neverd-electron-entry-no-parser-*.log`.

With the entire backend disabled, both ABI availability cases passed, including
the new entry exports. Both original/ASAR entry CLI forms returned
`capability_unavailable` and exit status 1. Worker protocol/graph passed; the web
case explicitly skipped. Logs use `/tmp/neverd-electron-entry-no-backend-*.log`.

The final review added charging for repeated target normalization even when a
path expression is memoized, and for entry-index path comparison units. Its
additional long-alias aggregate-budget fixture passed in the restored build.
The enabled configuration is restored: backend, parser, ASAR and LLVM Zstd are
ON. The restored run passed 223 of 224 registered web cases, with one expected
ASAR-omission-only skip. This includes all 101 source cases, all 49 SDK cases
and the supplied full pinned Bun corpus. All nine worker regressions and the
compiled-capability case passed; the backend-omission-only ABI case skipped in
this enabled configuration. Logs use `/tmp/neverd-electron-entry-restored-*.log`.
Focused C++ formatting and `git diff --check` passed. Qualification remains
local macOS arm64 Release; no whole-repository or other-host success is claimed.

HTML script entries, source import closure, runtime/window routing, distribution
and release qualification, safe export and the remaining adapters/consumers
are still required. This does not complete Electron or any of the five epics.

## Captured HTML script candidates — 2026-10-10

This C++ increment advances #716 with a bounded UTF-8 HTML source scanner,
pinned named-character-reference data and captured local URL comparisons.
Script/base metadata keeps original ranges, first-attribute and preceding-base
evidence, fixed type/context categories and explicit refusals. It does not
construct a DOM, authenticate browser encoding or establish runtime activation.
The full contract and primary references are in the
[HTML profile](web-html-profile.md).

Eligible inline IDs select immutable raw body slices through `ArtifactView`.
Existing source parsing, navigation, reviewed display and anchors share those
bytes. Anchors compose the body and parent storage offsets, preserving ASAR
occurrence identity. Inline IDs do not authorize recursive HTML or native-image
analysis. Local external references compare only with exact available members;
unsupported URLs/bases, absent directories and namespace escapes remain explicit.
No script, browser, subprocess analyzer or remote retrieval is introduced.

Both HTML C ABI operations, worker operations and four CLI forms use the same
owners. The cache holds at most four documents per revision and import replacement
revokes derived IDs. Scan failures clear declarations; link-budget failure clears
links while retaining bounded raw inventory. Repeated inline base copies are
charged against the link work budget. The 2,231-entry WHATWG snapshot and its
license are pinned by SHA-256; its optional development recorder and all new
test logic are C++. Binary, SDK and local install notice copies match exactly.

The first focused run passed all 16 new core/SDK/CLI cases and all three C++
worker checks. Tests include every named reference and scalar array, HTML
escape/type/comment/text states, malformed inputs, real scan/link exhaustion,
portable URL refusals, source-order base candidates, immutable capture after
deletion, private-output canaries, packed ASAR offsets and namespace isolation.
CLI cases use an unusable PATH. Logs use `/tmp/neverd-html-focused-tests.log`
and `/tmp/neverd-html-worker-tests.log`.

The enabled local macOS arm64 Release run passed 239 of 240 registered web
cases, with one expected ASAR-omission-only skip. This includes the supplied
full pinned Bun corpus, 62 artifact, 101 source, 15 map, 9 native and 53 SDK
cases. All nine worker regressions and the compiled-capability check passed;
the backend-omission ABI case skipped as expected. Logs use
`/tmp/neverd-html-full-{web,api,worker}.log`.

The subsequent unpacked-HTML case checks explicit captured-directory admission,
missing-member refusal and the actual unpacked storage origin. With the JS parser
disabled, all 17 HTML core/SDK/CLI cases and three C++ worker checks passed.
Inventory and local file comparison remain available; inline parsing and source
CLI forms explicitly report `capability_unavailable`. The compiled-capability
case passed and the backend-omission ABI case skipped as expected. Logs use
`/tmp/neverd-html-no-parser-{tests,api,worker}.log`.

With the backend disabled, both availability cases passed, including the two
new public exports. Worker protocol/graph passed and the web case explicitly
skipped. All four HTML CLI forms returned `capability_unavailable` with exit
status 1. Logs use `/tmp/neverd-html-no-backend-{api,worker}.log`.

Final review added an invalid end-tag opener regression: its bogus-comment
content cannot manufacture a script candidate. The inline module integration
case also verifies that the existing file comparator reports unavailable
directory origin rather than fabricating an HTML member path.

Backend, JS parser, ASAR and LLVM Zstd are restored to ON. The final enabled
macOS arm64 Release run passed 240 of 241 registered web cases with one expected
ASAR-omission-only skip: 62 artifact, 101 source, 15 map, 9 native and 54 SDK
cases, including the supplied full pinned Bun corpus. All nine worker cases and
the compiled-capability check passed; the backend-omission ABI case skipped in
this enabled configuration. Logs use `/tmp/neverd-html-restored-{web,api,worker}.log`.
Focused C++ formatting and `git diff --check` passed. No implementation scripts
or external analyzers were introduced.

Base-aware inline module links, import-map semantics, full application import
closure, runtime/window routing, distribution qualification, safe export and
the remaining framework/package/protocol consumers are still required. No epic
is complete and no cross-host or whole-repository success is claimed.

## HTML inline module file candidates — 2026-10-10

This C++ increment advances #716 with
`html-inline-module-file-candidates-v1`. `HTMLModules` compares literal static
imports, re-exports and dynamic imports from eligible raw inline sources with
exact captured members. It shares `HTMLFiles`, declared-base selection and the
local URL owner with HTML entry linking. The inline source retains its own
occurrence identity; its document supplies explicit context without inventing
a member path. Packed/unpacked ASAR files remain in their extraction namespace.

Summary context binds HTML/script/base/source and captured namespace evidence;
query/fragment presence is nullable metadata and values remain private. Equal
inline bytes under different document/base occurrences stay distinct. HTML
selection never rebases an ordinary external file's existing analysis or cache.
Import-map declarations currently refuse association, including relative imports
that a map could replace or block. Nonliteral/unsupported requests, remote or
missing bases and source-type mismatches remain explicit. Results are partial
file candidates, not browser module identity, activation or runtime routing.
The full contract is in the [HTML profile](web-html-profile.md#inline-module-file-candidates).

Existing source-module C ABI and worker operations select this profile for
derived inline sources. `html-modules` and `asar-html-modules` select a script
record and emit all module page kinds. The first focused run exposed a CLI
status mismatch: the shared adapter accepted only `ok`, so a completed HTML
`partial` summary stopped before request pages. The adapter now accepts both
completed states; the CLI regression requires actual dynamic-import records.

The corrected focused run passed all 36 HTML/module cases and three C++ worker
checks. Tests cover preceding/later bases, UTF-8 percent-encoded paths, nullable
URL metadata, literal/nonliteral requests, map boundaries, invalid context/cache
evidence, independent work/count exhaustion and comparison after a separately
exhausted HTML entry budget. SDK cases cover context isolation, immutable capture,
packed/unpacked occurrence and anchor preservation, and external-file cache
stability. Both CLI forms run with an unusable PATH. Direct/framed worker results
agree and omit private reference canaries. Logs use
`/tmp/neverd-html-modules-focused-tests.log` and
`/tmp/neverd-html-modules-worker-tests.log`.

The first enabled local macOS arm64 Release run passed 248 of 249 registered web
cases with one expected ASAR-omission-only skip: 62 artifact, 108 source, 15 map,
9 native and 55 SDK cases, including the supplied full pinned Bun corpus.
All nine worker cases and the compiled-capability case passed; the backend-only
ABI case skipped in the enabled configuration. Logs use
`/tmp/neverd-html-modules-full-{web,api,worker}.log`.

With the JS parser disabled, 17 of 18 HTML cases passed; the new inline-context
case explicitly skipped because it requires parsing. Inventory, captured entry
links and original/ASAR CLI inspection remained available. Inline source/module
commands returned `capability_unavailable` with exit status 1. All three C++
worker checks and the compiled-capability case passed; the backend-omission
ABI case skipped as expected. Logs use
`/tmp/neverd-html-modules-no-parser-{tests,api,worker}.log`.

With the backend disabled, both availability cases passed. Worker protocol and
graph checks passed; the web case explicitly skipped. Both new CLI forms returned
`capability_unavailable` with exit status 1 before input lookup. Logs use
`/tmp/neverd-html-modules-no-backend-{api,worker}.log`.

Final review found that an unavailable inventory or early link-budget failure
could incorrectly report no import-map declaration before inspecting declarations.
The initial status is now `not_analyzed`; absence is reported only after a
complete check. A malformed inline source with a captured import map and an
early request-count failure cover this distinction. This parser-only correction
was added after the omission-profile runs above.

The backend, JS parser, ASAR and LLVM Zstd options are restored to ON, with the
existing ICU 77.1 selection. All eight inline-module core cases passed. The final
enabled local macOS arm64 Release run passed 249 of 250 registered web cases,
with one expected ASAR-omission-only skip: 62 artifact, 109 source, 15 map,
9 native and 55 SDK cases, including the supplied full pinned Bun corpus.
All nine worker regressions and the compiled-capability case passed; the
backend-omission ABI case skipped in this enabled configuration. Logs use
`/tmp/neverd-html-modules-restored-{focused,web,api,worker}.log`.
Focused C++ formatting and `git diff --check` passed. All new implementation and
test logic is C++; no subprocess analyzer, target execution or external script
was introduced.

Import-map semantics, external-source document contexts, complete application
import closure, runtime/window routing, distribution qualification, safe export
and the remaining framework/package/protocol consumers are still required.
This increment does not complete an epic or establish whole-repository or
cross-host qualification.

## Integration with current dev

The web checkpoint was integrated with `dev` at `22793fc9d`, retaining its
loader-choice/identification APIs, COFF object view, unwind discovery ordering,
UTF-8 file paths, transactional debug loading, user edits and worker background
indexing. File and captured-buffer loading continue through the same native
format readers. Rich Header and object metadata parsing consume the selected
buffer, and immutable-buffer loading explicitly refuses file-loader overrides.

The newer function, data-item, operand-format and load-option persistence APIs
now enforce the existing file-backed-session boundary. Snapshot loads skip all
sidecar discovery. The standard input-SHA query uses the captured digest and
never falls back to opening a snapshot path. C++ regression assertions cover
these additions alongside the existing snapshot/file parity and lifetime cases.

The integration build uses macOS arm64 Release, the matching prebuilt NeverD
LLVM 23.0.0 r4 package, pinned Capstone, Hermes and ICU 77.1. Web analysis, the JS
parser and ASAR are enabled; emulation, semantic tests, GUI and the Python
plugin host are disabled. This LLVM package omits Zstd, so compressed Bun-map
decoding reports its explicit unavailable capability. Earlier integrated-LLVM
Zstd-enabled results above remain separate evidence.

The clean-worktree run caught an omitted 182-byte preserved ASAR member fixture:
the original checkout contained it under an ignore rule, but the checkpoint did
not. The fixture is now tracked with its existing independently checked SHA-256
`cc402b796dc92b2b1f3a6d09515003d8400e63d8acaffc967e49c0cf015fcffe`.
All 11 ASAR core cases then passed. The repeated full web run passed 243 of
250 registered cases: six Bun-map cases require unavailable Zstd and one tests
ASAR omission only. The full pinned Bun container corpus was supplied. All nine
native handoff cases passed, including the newer persistence/SHA assertions.
The loader-choice regression and compiled-capability test passed; the
backend-omission-only ABI test skipped in this enabled build.

The current native Session C API suite passed 91 of 99 cases; eight explicitly
require ELF/glibc/System V fixtures unavailable on this macOS host. Logs are
`/tmp/neverd-js-dev-{web-fixed,asar-fixed,buffer,api,session}-tests.log`.

The first parallel worker run failed a temporary-path equality check and timed
out on a large source-cache request. The path check compares its supplied path
with the worker's canonical path; macOS's default temporary directory traverses
a symlink. With `TMPDIR=/private/tmp`, both focused cases passed in a serial
run. The complete serial worker suite then passed 21 of 24 cases; the remaining
three explicitly require a glibc host compiler/strip. No timeout or assertion was
relaxed. The retained logs are `/tmp/neverd-js-dev-worker-tests.log`,
`/tmp/neverd-js-dev-worker-focused.log` and
`/tmp/neverd-js-dev-worker-serial-tests.log`.

Build logs are `/tmp/neverd-js-dev-configure.log` and
`/tmp/neverd-js-dev-build-current.log`. The original first build was deliberately
stopped when `dev` advanced, before the incremental current-dev build completed.
Scoped formatting and the complete integration diff's whitespace check passed.
This is local integration evidence, not a whole-repository, GUI, cross-host or
Zstd-enabled qualification of the new dev baseline. The five epics remain open.

Before publication, `dev` advanced to `710b8368c` with opt-in native-to-LLVM
state-preservation proofs. That revision was merged without conflicts. The same
build targets required no recompilation. The repeated web run passed 243 of 250
cases with the same seven skips; Session C API passed 91 of 99 with the same
eight skips. Native-buffer loading and the compiled-capability check passed,
with the omission-only ABI check skipped. All five affected worker smoke suites
(Web, RealEngine, PEBrowse, NativeMapping and FunctionEdits) passed serially with
`TMPDIR=/private/tmp`. The complete worker result above remains separate from
this final smoke run. Logs are `/tmp/neverd-js-dev-final-build.log`,
`/tmp/neverd-js-dev-final-web-tests.log` and
`/tmp/neverd-js-dev-final-session-tests.log`. The newly merged proof suites were
not part of this web integration profile.

The checkpoint and integration evidence were pushed to GitHub `dev` at
`44966cccb1cf3faf31df07d4af26aa68c4a75b01`; the remote ref was checked after
the successful fast-forward push. The original working tree's unrelated changes
and dirty signature checkout were preserved.

## Captured import maps — 2026-10-10

`ImportMap` now owns bounded JSON/URL normalization and exact, prefix, scoped and
blocking resolution. It embeds unchanged Ada 4.0.0 C++ sources at
`b12a893a45809da8103bb4f1e2f6f5ee13f9100b`, with a verified archive hash and
preserved Ada/IDNA/Unicode notices. Only the library sources are compiled;
upstream generators, tools, URLPattern and external runtimes are excluded.
The dependency's symbols use hidden visibility. Its generated notice-install
rules were exercised under `/tmp/neverd-import-map-license-install`; every
installed Ada notice matched the preserved source directory byte for byte.
This checks notice installation, not a complete product distribution.

`HTMLImportMaps` retains each declaration's captured body, preceding base and
refusal state. Maps preceding all ordinary classic/module declarations compose
in source order with first definitions retained. Late/interleaved maps, unknown
absolute key origins, unsupported local key contexts, malformed maps and
exhausted budgets cannot silently fall back to an ordinary relative file.
Map and script declarations have independent bases. URL normalization preserves
query, fragment and percent spelling before a separate confined file projection.
Private context-bound URL groups distinguish several URLs naming one captured
file; no isolated URL/string hash is published. Raw references are checked before
normalization so origin-root clamping cannot erase traversal evidence. Absolute
requests cannot alias the synthetic capture origin.

The HTML module profile is now `html-inline-module-file-candidates-v2`.
SDK/CLI/worker use the same owners and publish `import_maps` pages, declaration
IDs/body ranges, counts, refusal states and private request-mapping identities.
Integrity declarations are inventoried without validating a fetch or digest.
No browser execution, activation/history equivalence or per-entry byte span is
claimed. External-source contexts and full application closure remain pending.

The first focused run passed 45 HTML/import-map cases and all three C++ worker
checks. Review then added explicit absolute/escaping-request refusal, invalid
UTF-8 admission and CLI failure for exhausted map comparisons. A compiler warning
had identified temporary JSON-key string views; they were replaced with owned
strings before the test runs. All new implementation and test logic is C++.

With JS parsing omitted, 28 of 31 HTML/import-map cases passed; three source-only
SDK cases explicitly skipped. Map inventory and native URL processing remained
available. All three worker checks and the compiled-capability check passed;
the backend-omission-only ABI case skipped. With the complete web backend omitted,
both ABI cases passed; protocol/graph checks passed and the web worker case
explicitly skipped. `neverd web html` returned `capability_unavailable` with exit
status 1 before input lookup. Logs use
`/tmp/neverd-import-maps-no-parser-{tests,api,worker}.log` and
`/tmp/neverd-import-maps-no-backend-{api,worker,cli}.log`. The final UTF-8 and
parser-dependent budget corrections were made after these omission runs.

Both web build options are restored to ON, with the same prebuilt LLVM/ICU
profile described above. The complete restored web run passed 264 of 271 cases:
72 artifact, 116 source, 15 map, nine native and 59 SDK registrations, with six
Zstd-dependent map skips and one ASAR-omission-only skip. The pinned Bun corpus
was supplied. The compiled-capability case and three C++ worker checks passed;
the backend-omission ABI case skipped as expected. Logs use
`/tmp/neverd-import-maps-restored-{build,web,api,worker}.log`.

Final link verification passed all 47 focused cases and three worker checks.
Two further parsed-fixture tests independently exhaust normalization with long
bases and repeated module/map comparison work; both clear candidate results and
pass. The final complete run passed 266 of 273 registrations with the same seven
skips (73 artifact, 117 source, 15 map, nine native and 59 SDK cases). All three
worker checks passed. Logs use `/tmp/neverd-import-maps-final-focused.log`,
`/tmp/neverd-import-maps-budget-tests.log` and
`/tmp/neverd-import-maps-publication-{build,web,worker}.log`.
Ada notices are also staged beside the shared library and inside the SDK; both
staged directories matched `LICENSES/ada` byte for byte. Scoped formatting and
`git diff --check` passed. Existing duplicate-library linker warnings remain;
there were no new compiler diagnostics in the final build.

Publication also incorporated `dev` at `aea8fd619`, including GUI text selection
and realigned x86 native EH work, through a conflict-free merge. The affected
native libraries rebuilt successfully. All 59 selected HTML/import-map/native
and ASAR CLI cases passed, including the pinned Bun native handoff. Session C API
passed 91 of 99 cases with its same eight platform skips; the native-buffer
loader case and seven web/native worker smoke suites passed. Logs use
`/tmp/neverd-import-maps-dev-{build,tests,session,loader,worker}.log`.
The upstream GUI and dedicated realigned-EH suites were not part of this web
integration profile; their qualification is not inferred from these checks.

The five epics remain open. This increment qualifies captured import-map
candidates locally; package graphs/diffs, passive observations, other desktop
and native JS formats, semantic rewrites, C++ MCP and cross-host distribution
still need their planned implementation and evidence.

## Claude Code standalone practical recovery — 2026-10-10

The official Claude Code 2.1.296 Linux x64 artifact drove the next implementation
increment. NeverD first refused its new Bun graph flags, then exposed a large
source materialization ceiling and nine async-rest parser-location failures.
The fixes remain in the shared C++ owners: a versioned prelinked graph profile,
streaming Unicode conversion, explicit local export and a separate sequential
readability profile with reparse/tree comparison. The parser keeps exact
source locations and rejects the newly exposed illegal rest trailing comma.

NeverD exported all 2,345 JS modules (44,768,763 UTF-8 bytes), 244 assets and
10,005 exact regions, with a full original and per-file read-back hash checks.
At that checkpoint, 2,303 modules had verified readable copies; 42 preserved raw
source and parser
diagnostics, including confirmed unsupported resource-management syntax. The
artifact contains no source maps. Original TypeScript, erased names/types and
native/JSC decompilation are not claimed. A repeated run with an unusable
external-tool PATH produced the identical completion-manifest hash.

The full web suite registered 284 cases: 277 passed and seven skipped (six
need LLVM Zstd, one requires ASAR disabled). Both full Bun corpora were supplied.
The five C++ worker suites passed. A broader worker run hit a SourceCache
timeout against the separately built mock and a macOS temporary-path alias
assumption. Canonical `TMPDIR=/private/tmp` made AnalysisSnapshot pass.
SourceCache passed one retry after rebuilding the mock but timed out again
after dev integration, including a serial retry. That separate mock-worker
large-source test remains an explicit validation limitation; its server does
not link the changed web backend. No worker source was modified.
Logs use `/tmp/neverd-claude-{full-web-tests,worker-verified,worker-canonical-path}.log`.

Integration with `dev` at `6543c4935` rebuilt the native dependencies and repeated
all 284 web cases with the same 277 passes and seven skips. C API availability
passed, with the backend-omission-only case skipped. A separate C++ probe compiled
the actual C API translation unit without the web feature and checked that the
new export entry point returns `capability_unavailable`. The export owner and
session translation units also compiled without the JS parser; this is not a
claim of a complete parser-disabled build. The integration logs are
`/tmp/neverd-claude-dev-{build,web,worker,api,source-cache}.log`.

Input hash, official provenance, reproducible commands, measured recovery
counts and precise limitations are recorded in
[the qualification](web-claude-code-qualification.md). Proprietary input and
recovered code remain outside the repository. This increment supplies practical
evidence for #714/#718; it does not close the five epics.

## Complete shipped-JavaScript recovery — 2026-10-11

The remaining 42 syntax failures are resolved. The private C++ Hermes extension
retains `using` and `await using` kinds, original ranges and lexer newline state.
Lexical binding and conservative disposal effects use the same retained model;
resource declarations are never rewritten as ordinary constants. Grammar and
consumer checks are described in the
[resource-management profile](web-resource-management-profile.md).

Once the large module parsed, its 1,165,398 nodes exceeded the old recovery cap.
The separate offline profile now admits two million nodes; the interactive
limits remain unchanged. NeverD exported and reparse-verified all **2,345** JS
modules with **zero** parse, readability or projection failures. The completed
directory has 14,698 files and 615,294,413 bytes. An optional C++ regression
repeats recovery and original-byte preservation across every module of the
hash-pinned official artifact. The target still has zero source maps; erased
TypeScript and original repository structure are not claimed.

All 291 web cases completed with 284 passes, seven explicit capability/config
skips and zero failures, including the full 2,345-module C++ recovery regression.

The former SourceCache timeout is also resolved: C++ profiling isolated
whole-document regex scans over long comments. Worker decoration now uses its
comment/literal-aware tokens for asm linkage and anchored image-marker checks.
The 9,000-row regression and existing cache transport tests pass without
relaxing their deadlines. A canonical macOS fixture temporary path removes the
snapshot alias assumption. Expanded native worker checks retain the separate
libc++ fixture mismatch and one address-mapping failure that passed on retry,
as documented in the [qualification](web-claude-code-qualification.md).

## Independent Node SEA extraction — 2026-10-11

The [SEA profile](web-sea-profile.md) implements the Node 22.15.0 LE64
preparation format and six native resource profiles: ELF64, thin Mach-O64 and
PE32+, each on x64 and ARM64. Versioned container readers and one serialized
blob reader own the semantics. Session publication, the C API, CLI and C++
worker use the same immutable regions and revision checks. This is independent
of Bun. Layout compatibility is not producer authentication, runtime activation
or complete native-loader validation.

Stored CommonJS and assets reach the existing source, map, package and native
consumers. A selected asset supplies one package document without inventing a
filesystem namespace from its private key. Nested tar/tgz selections preserve
original storage identity, expanded coordinates and compressed-frame ancestry.
Snapshots and V8 caches remain opaque and cannot become source selections.
Private paths, asset keys and their individual hashes stay out of ordinary
metadata. Malformed structure and work-limit failures publish no extraction.

Four small blobs produced by the official pinned Node compiler preserve plain,
minified, asset and cache variants, with independently recorded source/asset
hashes. An optional real snapshot and six complete official Node images with
the same 250-byte compiler blob injected provide additional local evidence.
The original downloads matched the official release checksums. The image
goldens record whole-image hashes and resource offsets independently of the
production readers. Native images were never executed. Manual fixture creation
used trusted upstream compiler/packager tools; no product or automated test
invokes those tools. The [fixture manifest](../unittests/web/fixtures/sea/README.md)
records the versions, hashes and reproduction boundary.

On macOS arm64 Release, all 18 focused SEA cases passed, including the optional
snapshot and all six real images. The complete owning web suite registered
408 cases: 401 passed, six Zstd-dependent map cases skipped because LLVM Zstd
is unavailable, and one ASAR-omission-only case skipped in the enabled profile.
The pinned Bun, Claude Code and npm corpora were supplied, including complete
2,345-module readable-source recovery. All five selected C++ worker suites
passed, including direct/framed SEA parity. Enabled C API availability passed;
its backend-omission-only case skipped. The capability ownership check passed
after correcting the two new exports' lexical ordering. Logs are
`/tmp/neverd-sea-{focused-tests,full-web-tests,worker-tests,api-tests,capabilities}.log`.

Node license/notices are preserved and staged beside the shared library and
in the SDK. Both staged files match the preserved upstream file byte for byte.
Independent plan/implementation review led to regressions for ELF raw-prefix
ambiguity, cumulative repeated-note work, Mach-O runtime name-padding aliases
and derived-asset package admission. Scoped LLVM formatting and `git diff
--check` passed. No Node/V8 runtime dependency or script implementation was added.

With the JS parser disabled, all 28 selected SEA and shared package SDK cases,
the enabled-backend availability case and all five C++ worker suites passed.
The nested source-only SEA case is not compiled in that profile. With the
whole Web backend disabled, both C ABI availability cases passed, four worker
suites passed and the Web worker explicitly skipped. The CLI omission check
exposed a silent session-creation failure; it now emits the backend capability
result and a fixed diagnostic. Logs use `/tmp/neverd-sea-nojs-*` and
`/tmp/neverd-sea-disabled-*`.

Both Web flags are restored to ON. Integration merged `dev` at `e442f57e7`
without conflicts and rebuilt the affected native dependencies. The restored
owning run repeated all 408 cases with the same 401 passes and seven skips;
all five selected worker suites, enabled C API availability and capability
ownership checks passed. The SEA CLI omission commands both returned status 1
and fixed `capability_unavailable` metadata. Existing native LLVM/libc++
deprecation and duplicate-library linker warnings remain; this is not a run
of the upstream x86 floating-point semantic suites. Logs use
`/tmp/neverd-sea-dev-{build,web,worker,api,capabilities}.log`.

This qualifies extraction of six target layouts on one macOS arm64 host.
Native Windows/Linux host capture and distribution have not been qualified.
The five epics remain open: semantic rewrites, remaining desktop adapters,
package behavior/provenance, wider passive source mapping, other standalone
formats and native C++ MCP still require their planned implementation and tests.

## Native C++ offline MCP transport — 2026-10-11

The optional `neverd-web-mcp` target implements MCP 2025-06-18 stdio directly
in C++, through the same Web C API client as the worker. Generic JSON admission
now belongs to `tools/common/transport`; the Web adapter belongs to
`tools/common/web`. Worker framing, native protocol fields and operation
semantics remain in their existing owners. The [MCP profile](web-mcp-profile.md)
records the exact catalog, lifecycle, bounds and omissions.

Startup configures at most eight private input paths without capturing them.
MCP requests select input indices and cannot pass arbitrary paths, reviewed
source ranges or raw export options. Tools follow the backend's available
operations. C API evidence, revision checks, source anchors and redaction
preview/commit remain authoritative. Results carry identical structured JSON
and JSON text under capped serialization. Unsupported methods/arguments,
analysis failures, notifications, duplicate initialization and EOF have
separate fixed behavior. No new implementation or test invokes an analysis
script, JavaScript runtime, external extractor or sample executable.

On macOS arm64 Release, the initial four native MCP suites passed: protocol,
catalog, shared-backend evidence parity and real stdio with an unusable PATH.
The evidence suite exercises source views/anchors/maps, Bun, SEA, package
scripts, HAR, streams, revocation and session separation with private-value
canaries. The independent imported-library build passed the same four suites
and installed its executable and unchanged JSON license. Logs are
`/tmp/neverd-mcp-{tests,final-tests}.log` and
`/tmp/neverd-mcp-standalone-{configure,build,tests,install}.log`.

All eleven affected worker suites passed serially. The first parallel run
timed out in HistoryContributions; its isolated retry and the complete serial
run passed without changes to that behavior. Independent review found the
GUI's separate mock SDK still compiling Protocol without the extracted JSON
dependency. It now links the shared JSON target with PIC enabled; the standalone
mock SDK built successfully. Existing fixture C-linkage warnings remain.
Logs use `/tmp/neverd-mcp-worker-{tests,history-retry,serial}.log` and
`/tmp/neverd-mcp-gui-{configure,build}.log`. This did not run the Qt GUI suites.

Review also found that an over-budget string method could produce an error
reply to a notification. Such notifications are now silently refused, with a
C++ regression covering long, empty and NUL-containing method strings. GUI CI
now watches `tools/common/**`; the main CI configuration enables the new MCP
target and its C++ tests. These workflow edits have not yet run remotely.

With JavaScript parsing disabled, all four MCP suites and five C++ worker
suites passed, and enabled-backend C ABI availability passed (the omission-only
case skipped). With the whole Web backend disabled, all four MCP suites and
both C ABI availability cases passed; four C++ worker suites passed and its
Web-only suite skipped. MCP's omitted-backend test checks capabilities and
absence of analysis tools, including through a real stdio process. Logs use
`/tmp/neverd-mcp-nojs-{tests,worker,api}.log` and
`/tmp/neverd-mcp-disabled-{tests,api,worker}.log`. Python plugins remain disabled
in these local runtime-free engine profiles.

After restoring both Web options to ON, all 408 registered Web cases completed:
401 passed and seven explicitly skipped (six LLVM-Zstd-dependent Bun map cases
and the ASAR-omission-only case). This included the supplied full SEA images,
Bun cross-container corpus, npm archives and all 2,345 shipped Claude Code JS
modules. All eleven affected worker suites passed; the enabled C ABI case
passed with the omission-only case skipped. The existing capability checker
validated 23 native capabilities. The independent MCP build and installation
were also repeated successfully. Logs use
`/tmp/neverd-mcp-restored-{web,worker,api,capabilities}.log` and
`/tmp/neverd-mcp-standalone-final-{build,tests,install}.log`.

The new optional real-process `NeverDWebMCPClaude` case pins the official
2.1.296 linux-x64 artifact by size and SHA-256 and reads all 2,589 modules over
MCP pages with an unusable PATH. It verifies 2,345 source artifacts, 244 assets,
2,343 opaque caches and no maps, without launching the sample. Its first run
hit the harness's 15-second response limit. Fixed operation names and elapsed
times were added to test diagnostics; no product behavior or timeout was
changed. The isolated retry and complete five-case MCP run passed in 19.88 and
19.30 seconds respectively. The retry's preview, commit and extraction calls
took 6.06, 7.06 and 5.16 seconds. The initial timeout's exact request/cause was
not captured and remains unisolated; the later passes do not establish a fix.
Logs are `/tmp/neverd-mcp-claude-{tests,trace,final-tests}.log`.

This completes the initial native C++ offline MCP profile, not the five epics.
Semantic rewrite receipts/undo, the remaining desktop and standalone adapters,
package behavior/provenance/reporting, broader passive source mapping, and
Windows/Linux host and distribution qualification remain outstanding.

## Native ZIP32 container evidence — 2026-10-11

`zip32-local-central-v1` adds explicit ZIP input to the existing archive API.
`archives/Zip`, `ZipPayload` and shared `PathIndex` own framing, payload decoding
and portable namespace admission respectively. The session retains one sealed
spool only after all available members validate. Unknown ordinary methods or
encryption retain bounded metadata without consumer bytes; unsupported newer
envelopes refuse the archive. Native zlib supplies raw deflate in process.
See the [profile](web-zip-profile.md) for exact envelopes and budgets.

Stored members compose direct original-byte anchors; compressed members retain
expanded coordinates and their complete local member frame. Independent review
found that nested Bun source/native origins lacked the direct ZIP payload base;
the shared origin now includes it, and SDK regressions verify the arithmetic
against original bytes. Malformed extra-field framing was also tightened before
qualification. Late CRC failures preserve earlier published archives and do not
consume cache entries. Shared tar namespace semantics remain covered.

On macOS arm64 Release, the final owning Web run registered 421 cases:
414 passed and seven skipped (six Bun source-map cases require unavailable LLVM
Zstd, and the path-policy-omission case does not apply to this enabled build).
This includes all eight hostile/independent ZIP cases, the optional real VSIX,
nine package archive SDK cases, and the payload-owner deflate-omission case.
The latter compiles `ZipPayload.cpp` without its zlib macro; it does not qualify
a whole-engine zlib-free build. The existing full-image SEA/Bun/npm corpora and
the all-2,345-module Claude source recovery case were supplied and passed.
All five MCP cases passed, including real Claude stdio pages. The worker run
registered 24 cases: 21 passed and three optional native-file cases skipped.
The new ZIP parity case initially reused a stale test revision between imports;
capturing each import's prior revision corrected the harness, and both the
focused retry and final worker run passed. Formatting and diff checks passed.

The pinned Marketplace Claude Code 2.1.296 linux-x64 VSIX produced 29 members,
268,754,709 decoded bytes and an embedded helper identical to the previously
qualified standalone CLI. The real CLI `archive-bun ... zip 0 15` succeeded with
an unusable external-tool PATH and produced 2,589 modules / 10,005 regions.
The first downloaded body used HTTP gzip content encoding and was correctly
refused as ZIP; its wire bytes and decoded ZIP have separately recorded hashes.
See the [qualification record](web-claude-code-qualification.md).

Logs: `/tmp/neverd-zip-full-web-tests.log`,
`/tmp/neverd-zip-mcp-real-tests.log`, `/tmp/neverd-zip-all-worker-tests.log`,
`/tmp/neverd-zip-sdk-fixed-tests.log`, and `/tmp/neverd-zip-real-cli.log`.
The prior MCP milestone's parser/backend omission checks were not repeated for
this ZIP increment; only the ZIP payload owner omission was newly executed.
No Windows/Linux host, ZIP64, prefixed executable, full framework distribution
or safe raw export qualification is claimed. NW.js/VSIX manifest adapters are
the next separate phase; ZIP extraction does not finish #716 or the five epics.

ZIP implementation commit `3c36cad04` was integrated with upstream `dev`
`8be7675b9` as `613dc8f3b` and pushed. The full rebuild after that merge passed.
The 28 selected integration cases then recorded 27 passes and one optional
native-file skip; worker Web parity and all five MCP cases passed. Logs are
`/tmp/neverd-zip-merge-{tests,worker,mcp}.log`.

## Captured NW.js and VSIX manifest file candidates — 2026-10-11

The independent `nwjs-manifest-file-candidates-v1` and
`vscode-extension-manifest-file-candidates-v1` profiles add declaration shapes
and exact captured-file candidates. `desktop/Manifest` owns bounded JSON,
identity and parent-relative comparison; `NWManifest` and `VSIXManifest` own
framework fields. `SessionDesktop` publishes bounded, revision-bound results.
The C API, CLI, worker and MCP delegate to that owner. See the
[profile](web-desktop-manifest-profile.md) for exact fields and limitations.

Captured plain, `package.nw`, `app.nw` and macOS resource-directory layouts have
C++ fixtures. ZIP members retain their original/expanded coordinates; Bun/SEA
assets without a directory namespace cannot borrow unrelated captured files.
URLs, command-line syntax, runtime fallback, absent declarations, invalid field
types, directories and missing files remain distinct. All ordinary results
omit raw paths, manifest strings, event names and contribution keys. Version,
permissions, activation and helper association are not inferred from names.

The first focused run passed twelve cases, including the pinned real VSIX, and
failed the CLI harness's line-by-line JSON check. Diagnostic output showed
bytes from the preceding longer NW.js result surviving at the end of a reused
redirect file. Unique redirect files for each process fixed the harness; no
product parser change was needed. All thirteen cases then passed, including
the six CLI invocations with an unusable external-tool PATH. Four native MCP
suites and worker Web parity passed. Enabled C ABI availability passed; its
backend-omission-only case skipped as expected. Independent review also
corrected the documentation to distinguish declaration `entry_id` from the
consumer's `artifact_id`.

The real CLI selected manifest member 4 from the pinned Claude Code 2.1.296
VSIX and linked `main` to the available member in that same archive; `browser`
is absent. Manifest SHA-256, source identity and frame coordinates are recorded
in the [qualification](web-claude-code-qualification.md). Logs are
`/tmp/neverd-desktop-{focused-tests,fixed-tests,worker-tests,mcp-tests,real-cli}.log`.

With JavaScript parsing disabled, all thirteen desktop cases passed, including
the supplied real VSIX. Worker Web and four MCP suites passed; enabled C ABI
availability passed with the omission-only case skipped. With the whole Web
backend disabled, both C ABI availability cases and all four MCP suites passed;
worker Web correctly skipped. These runs qualify the additive ABI symbol and
capability-filtered transport without relying on the source parser. Logs are
`/tmp/neverd-desktop-nojs-{tests,api,worker,mcp}.log` and
`/tmp/neverd-desktop-disabled-{api,worker,mcp}.log`.

After restoring both Web options to ON, the owning Web suite registered 434
cases: 427 passed and seven skipped (six require unavailable LLVM Zstd, and
one only applies when the archive path policy is omitted). Supplied SEA, Bun,
npm and VSIX corpora were enabled; all 2,345 Claude Code JavaScript modules
again passed byte-preserving readable recovery and reparse checks. Worker Web
parity and enabled C ABI availability passed. Logs are
`/tmp/neverd-desktop-full-web-tests.log` and
`/tmp/neverd-desktop-restored-{worker,api}.log`.
All five MCP suites also passed, including the pinned Claude native stdio
case; its log is `/tmp/neverd-desktop-restored-mcp.log`.

This advances #716. Full distribution detection, Tauri/Wails/CEF assets and
bridges, runtime entry/activation analysis, general safe export and other host
qualification remain separate work. Semantic rewrite receipts, package
behavior/advisory/reporting, broader passive source mapping and independently
versioned pkg/nexe readers still prevent completion of the five epics.

## XMLHttpRequest source evidence — 2026-10-11

`fetch-websocket-xhr-syntax-v2` adds bounded direct/const receiver links for
XHR open, header, send and known event-value syntax. `SourceContext` owns the
shared admission/index/publication boundary; `SourceFetch` and `SourceXHR`
own distinct rules, leaving session and transports as serializers. Ordinary
records retain opaque source IDs and fixed classifications. They do not expose
URL, credential, arbitrary header, body or callback text. Shared receiver
bindings do not assert runtime identity or request ordering. The existing HAR
comparison remains fetch-only. See the [profile](web-interface-profile.md).

Independent plan/implementation consistency review found two default-state
misclaims: spread listener options were still marked absent, and the new XHR
argument-role field initially marked legacy spread calls positional. Both were
corrected with regression assertions. Forbidden Fetch/XHR method classification
also now includes TRACK and all ASCII case variants in one shared owner.

After those corrections, all 25 focused source/interface/SDK cases passed,
including the existing hash-pinned Claude Code module 1,897 regression. That
module still has one dynamic fetch candidate and no admitted XHR pattern; no
runtime endpoint or traffic was invented. C++ worker Web parity and four MCP
suites passed, along with enabled C ABI availability (its backend-omission
case correctly skipped). CLI and framed transport runs used an unusable
external-tool PATH. Logs are `/tmp/neverd-xhr-{focused-tests,worker,mcp,availability}.log`.

With JavaScript parsing disabled, all five remaining interface SDK cases,
worker Web and four MCP suites passed. C ABI availability passed and confirmed
the source-interface profile/operations are absent; the backend-omission-only
case skipped. Logs are `/tmp/neverd-xhr-nojs-{sdk,worker,mcp,api}.log`.

After restoring the parser, the owning Web suite registered 444 cases: 437
passed and seven skipped (six require unavailable LLVM Zstd; one only applies
when the ASAR path policy is omitted). Supplied Bun/SEA/npm/VSIX fixtures were
enabled, and all 2,345 Claude Code JavaScript modules again passed readable
recovery/reparse checks. Worker Web and enabled ABI availability passed. Logs
are `/tmp/neverd-xhr-full-web-tests.log` and
`/tmp/neverd-xhr-restored-{worker,api}.log`.
All five restored MCP suites passed, including the supplied Claude Code native
stdio case; see `/tmp/neverd-xhr-restored-mcp.log`.

This advances #717 without completing it. Request wrappers, response dataflow,
encoding/compression/crypto tracing, schema exports and reviewed endpoint
displays remain required.

## Reversible primitive source rewrites — 2026-10-11

`javascript-primitive-rewrite-v1` adds explicit string recovery and other
primitive folding. The shared frontend, bindings, values and effects remain
the semantic owners. `rewrite/Plan`, `Primitive`, `Apply` and `Validate` own
selection, closed emission, exact inverse and complete outside-tree checks;
`SessionRewrite` publishes bounded metadata and two private cached results.
`rewrite/Export` separately owns fresh private files and ownership-aware
rollback. It retains created-file descriptors through the transaction so
unlink/inode reuse cannot turn a foreign replacement into a cleanup target.
The C API and CLI expose pass/source/revision-bound local export; worker and
MCP only expose metadata. See the [profile](web-source-rewrite-profile.md).

Independent plan review required complete lexer evidence, ASI outside-tree
comparison, pass-bound disclosure receipts and a distinct transactional writer.
The implementation and C++ tests cover those contracts, private literal
canaries, UTF-16/signed-zero/BigInt cases, comment preservation, no-effect
admission, tampered inverse evidence, bounded growth, partial disk write
failure and preservation of foreign files or replaced output directories.
The first integration run passed all 18 core/export cases but its four SDK
cases failed because the fixture omitted the captured root directory when
counting artifacts. Correcting the fixture's source/CLI indices made all four
pass; the product import semantics were unchanged.

The actual CLI extracted Claude Code module 1,897 from the pinned original and
exported both selected passes with an unusable external-tool PATH. String
recovery accepted 5 substitutions and constant folding accepted 201; both
retained 1,496 refused candidates and passed exact inverse verification.
The exported originals also matched the prior raw recovery byte for byte.
Exact hashes and retained local files are in the
[qualification record](web-claude-code-qualification.md#reversible-primitive-rewrites--2026-10-11).

The enabled owning Web suite registered 466 cases: 459 passed and seven skipped
(six need unavailable LLVM Zstd; one only applies with ASAR path policy omitted).
All supplied Bun/SEA/npm/VSIX corpora were enabled, including readable recovery
and reparse of every one of the 2,345 Claude Code JavaScript modules. Worker Web,
all five MCP suites including the real native Claude stdio case, and enabled
C ABI availability passed. Its backend-omission-only case skipped as expected.
Logs: `/tmp/neverd-rewrite-full-web-tests.log`,
`/tmp/neverd-rewrite-sdk-fixed-tests.log`, `/tmp/neverd-rewrite-worker-tests.log`,
`/tmp/neverd-rewrite-mcp-tests.log`, `/tmp/neverd-rewrite-mcp-claude.log` and
`/tmp/neverd-rewrite-api-tests.log`.

With JavaScript parsing disabled, C ABI availability checked explicit refusal
for all three new entry points; worker Web and four MCP suites passed. All
four rewrite SDK behavior cases explicitly skipped because they require the
parser. With the entire Web backend disabled, both ABI cases and four MCP
suites passed; worker Web and the optional Claude MCP case skipped. Logs are
`/tmp/neverd-rewrite-nojs-{api,worker,mcp,sdk}.log` and
`/tmp/neverd-rewrite-disabled-{api,worker,mcp}.log`.

These checks establish local primitive substitutions, not whole-program or
source-reflection equivalence. Scope renaming, branch/control-flow transforms
and the other epics' pending capabilities still prevent completion of #714–#718.

## Selected Node source and package behavior — 2026-10-11

`node-22.15-static-behavior-v1` adds fixed process, filesystem, HTTP, native-load
and Buffer call syntax classes through C++ core, session, C API, CLI, worker
and MCP. Shared source bindings, module origins and primitive values own
semantic admission. Rules preserve exact import/member forms and overload
uncertainty; ordinary pages contain fixed classes and opaque anchors, not
commands, paths, URLs or payload values. The independent plan review required
exact default-import identity, separate promise method tables, ordinal-only
argument anchors, unknown package ownership and a 16-source aggregate that
does not depend on the four-result source cache. See the
[profile](web-node-behavior-profile.md).

The real standalone module 1,897 exposed a valid class name with two binding
identities on one syntax node. A minimal failing regression confirmed that
origins and paths incorrectly rejected the whole binding model. The shared
declaration projection now preserves this ambiguity consistently across
origins, paths, navigation and bundles. Full declaration occurrences are
unchanged. Exact ESM import-row evidence advances origins to profile v2.
The real module now yields a partial result with zero supported Node patterns
and 1,461 unattributed calls, not a clean or complete result.

NeverD's actual CLI extracted the two JavaScript members of the pinned Claude
Code npm wrapper, analyzed them as CommonJS and joined both to its manifest
inventory. It found eight call sites in five fixed API classes. No lifecycle
script or sample ran; PATH excluded external tools. Exact hashes, counts,
aggregate identities and limits are in the
[qualification record](web-claude-code-qualification.md#selected-node-source-behavior--2026-10-11).

The first SDK run caught two test mistakes: a native ELF fixture selected the
SEA preparation-blob profile, and the repeated CLI test did not truncate its
redirect files. Correcting the profile and redirects made all five new SDK
cases pass, including 16-source/4-cache independence, 19 declared entries
with 16 retained anchors, archive unknown ownership, unavailable sources,+Bun/SEA delivery-only relations, foreign selection and revision revocation.
The SDK failures did not require changing product extraction semantics.

Enabled qualification on macOS arm64 Release with prebuilt LLVM 23 r4:
490 registered owning Web cases, 483 passed and seven explicit skips (six
LLVM Zstd cases unavailable in this build and one ASAR-omission-only case).
The supplied six-platform Bun corpus, SEA corpus/images, npm/VSIX artifacts
and all 2,345 real Claude JavaScript recovery/reparse cases were enabled.
Worker Web, the four general MCP suites, the separate real Claude MCP process
case and enabled C ABI availability passed. A final review consolidated the
unchanged aggregate byte/node/ancestry/entry ceilings into the profile owner.
Logs: `/tmp/neverd-node-behavior-full-web.log`,
`/tmp/neverd-node-behavior-sdk-fixed-tests.log`,
`/tmp/neverd-node-behavior-shared-tests.log`,
`/tmp/neverd-node-behavior-{worker-tests,mcp-tests,mcp-claude,api-tests}.log`.

Parser omission passed explicit unavailability checks for all four new ABI
functions, worker Web and the four MCP suites; five parser-dependent SDK
cases skipped explicitly. Backend omission passed both ABI cases and four
MCP suites; worker Web and the optional Claude MCP case skipped. Logs:
`/tmp/neverd-node-behavior-nojs-{api,worker,mcp,sdk}.log` and
`/tmp/neverd-node-behavior-disabled-{api,worker,mcp}.log`.

Both feature options were restored to ON and the owning targets rebuilt.
All 49 focused source/package/shared-consumer cases (including both real
artifacts), five Node SDK cases, worker Web, four MCP suites and enabled ABI
availability passed again. The published ceilings match the shared profile.
Restoration logs use `/tmp/neverd-node-behavior-restored-*.log`.

This advances #715 source inspection and shared #714 semantics. Install-entry
propagation, source-behavior comparison, external advisory/provenance imports,
analyst dispositions, readable reports, other runtime/desktop adapters and
the rest of the acceptance matrix remain outstanding. The five epics are
not complete.

## Independent nexe resource extraction — 2026-10-11

`nexe-4.0.0-rc.7-eof-resources-v1` adds a separate C++ EOF envelope reader,
strict first-shim JSON index, portable private namespaces and immutable resource
occurrences. Reader, index and session ownership remain separate. SDK, CLI,
worker and MCP use the same core results; bootstrap and stored members feed
the existing source, package, HTML, storage-anchor and native consumers. The
native prefix, signatures, runtime activation and producer version remain
unverified. See the [profile](web-nexe-profile.md).

Independent plan review required exact first-sentinel window semantics,
deduplicated hashing of range aliases, nested nonzero storage coordinates and
namespace isolation for identical images. The C++ checks cover those cases,
all truncated prefixes, invalid binary64 sizes, malformed JSON, hostile paths,
overlaps, independent limits, revision revocation and failure atomicity. Two
boundary failures found during implementation were repaired in their owners:
the JSON node allowance omitted the resources key, and the stricter nexe
metadata subtraction could underflow after shared path-index accounting crossed
its threshold. Dedicated count, deep-path and aggregate-cache tests now cover
both boundaries without raising the published limits.

Controlled nexe 4.0.0-rc.7 producer outputs used the original Node 14.15.3 bases
for Linux x64, macOS x64 and Windows x64. NeverD extracted all three with an
unusable external-tool PATH and matched four independently preserved inputs
byte-for-byte and hash-for-hash; source/bootstrap parsing and package-manifest
consumers also passed. The manual fixture-input writer is C++; producer setup
and packaging are separate development activities, never analyzer or test
dependencies. No target was executed. Exact hashes and reproduction evidence
are recorded in the [fixture manifest](../unittests/web/fixtures/nexe/manifest.json).
Six inert ELF/PE/Mach-O x64/ARM64 prefixes additionally check preservation; they
do not qualify ARM64 producer outputs, native loading or runtime behavior.

The enabled owning Web run registered 511 cases: 504 passed and seven skipped
(six require unavailable LLVM Zstd; one applies only when the ASAR namespace
policy is absent). All supplied Bun, SEA, nexe, npm and VSIX artifacts were
enabled, including six-platform Bun containers and recovery/reparse of every
one of the 2,345 Claude Code JavaScript modules. The separate real Claude MCP
process case passed. Worker protocol/Web and eight shared worker suites, four
general MCP suites and enabled ABI availability also passed; the ABI case
specific to backend omission skipped as expected. Logs:
`/tmp/neverd-nexe-full-web.log`, `/tmp/neverd-nexe-mcp-claude.log`,
`/tmp/neverd-nexe-worker-{all,shared}.log`, `/tmp/neverd-nexe-mcp-tests.log` and
`/tmp/neverd-nexe-api-tests.log`.

Remote `dev` through `54c3fcff0` was integrated without changing its native
pipeline work. The capability manifest now includes both nexe APIs and the
previous Node-behavior and source-rewrite entry points; the existing manifest
checker passed with 23 capabilities. Public nexe member IDs were also changed
to use sorted ordinals and range identity, keeping private path text out of
independently enumerable member-identity inputs.

With the parser disabled, all 13 nexe core cases and five SDK cases passed;
three source-dependent SDK cases skipped explicitly. All three real producer
images were supplied to the core test. ABI availability, worker Web and the
four general MCP suites passed; the backend-omission-only ABI case skipped.
With the entire backend disabled, both ABI cases and four general MCP suites
passed; worker Web and optional Claude MCP explicitly skipped. Logs:
`/tmp/neverd-nexe-nojs-{sdk-core,api,worker,mcp}.log` and
`/tmp/neverd-nexe-disabled-{api,worker,mcp}.log`.

After restoring the backend and parser, the integrated build again passed
504 of 511 owning Web cases, with the same seven explicit skips and every
supplied real artifact enabled. Worker Web, all four general MCP suites and
enabled ABI availability passed. The separate real Claude MCP case also
passed. Logs:
`/tmp/neverd-nexe-restored-{web,worker,mcp,api,claude}.log`.

This advances #718 with a qualified, independent nexe profile. The separate
pkg adapter and the remaining acceptance items across #714–#718 are still
in progress; these results do not mark the five epics complete.

The later remote `dev` update through `8591189db` was integrated, including its
recorded Capstone revision. After rebuilding the affected native/SDK targets,
all 30 nexe/core/SDK/native cases passed with producer fixtures supplied, as did
worker Web, all five MCP suites including real Claude, and enabled ABI
availability. The backend-omission-only ABI case skipped. The capability checker
passed. Logs: `/tmp/neverd-nexe-latest-integration-{build,web,api,worker,mcp}.log`.

## Independent pkg virtual filesystem extraction — 2026-10-11

`pkg-5.8.1-uncompressed-vfs-v1` adds independent C++ locator, index and partition
owners. The bounded streaming locator accepts fixed-width LF/CRLF declarations;
the fixed prelude wrapper frames strict JSON VFS, entry and link declarations.
Compression, ambiguous locators, malformed ranges, unknown store types and
portable path conflicts refuse. Prefix, payload, prelude and trailer preserve
every byte; exact store aliases share hashing and stat parsing work. Source,
opaque V8 cache, directory-list and stat records have separate selection rules.
See the [profile](web-pkg-profile.md).

Independent plan review required metadata digest redaction, unresolved
source/cache equivalence, ancestor-link uncertainty, exactly one namespace root
and qualification against packaged outputs rather than unchanged native bases.
Consumer review additionally excludes content aliases of metadata from source
and package selection, including private metadata digests through region pages.
Stat sizes remain separate declarations: producer BOM/shebang stripping changes
stored source lengths without changing the original stat size. Cache-only entry
declarations report cache presence
without inventing source. Session, SDK, CLI, worker and MCP consume one core
model. The existing nexe and new pkg selections share checked nested storage
coordinate construction; package ancestry retains delivery-only semantics.

Six controlled pkg 5.8.1 outputs use the pinned pkg-fetch 3.4.2 Node 18.5.0
bases for Linux, macOS and Windows, each x64 and ARM64. Their inputs, base and
container hashes, npm producer identity and retained dependency lock are in the
[manifest](../unittests/web/fixtures/pkg/manifest.json). Four independent inputs
were packaged with bytecode/native building disabled. The C++ core matched all
six containers and every stored input byte/hash. Windows CRLF/snapshot paths
and macOS appended signing bytes are covered without claiming native identity,
signature validation or runtime activation. No target was executed. Producer
setup remains a separate development activity, never a test/analyzer dependency.

A supplemental producer output independently qualifies BOM/shebang processing:
original source lengths are 78/22 and stored lengths are 56/19. The C++ fixture
pins the complete container and checks separate original/stored byte and hash
oracles. See its [manifest](../unittests/web/fixtures/pkg/stripped-manifest.json).
All six canonical outputs also passed actual extraction and source CLI commands
with an unusable external-tool PATH. No target or embedded runtime was invoked.

Enabled verification on the nexe integration base passed 534 of 541 Web cases,
including 30 pkg cases, all supplied native producer corpora, npm/VSIX evidence
and the complete 2,345-unit Claude Code source recovery. Seven expected skips
were six unavailable LLVM Zstd map cases and the inapplicable ASAR-policy
omission case. The shared worker protocol/Web/transport checks passed, as did
seven additional worker regressions and all five MCP suites including real
Claude Code. The capability inventory passed after correcting the canonical
order of the two new public names. ABI capability discovery passed; its
backend-omission-only case was inapplicable. Logs are
`/tmp/neverd-pkg-full-web.log`, `/tmp/neverd-pkg-worker.log`,
`/tmp/neverd-pkg-worker-shared.log`, `/tmp/neverd-pkg-mcp.log`,
`/tmp/neverd-pkg-abi.log` and `/tmp/neverd-pkg-capabilities.log`.

With JavaScript parsing disabled, 45 of 51 pkg/nexe cases passed; six source-only
cases explicitly skipped. Core extraction still checked all supplied producer
images and the preprocessing variant. ABI discovery, actual Web worker and four
MCP suites passed; the source-dependent MCP Claude case explicitly skipped.
See `/tmp/neverd-pkg-nojs-{web,api,worker,mcp}.log`. With the entire backend
disabled, both C ABI tests passed, including all public stubs; four MCP suites
passed and the backend-dependent Web worker/Claude cases explicitly skipped.
See `/tmp/neverd-pkg-off-{api,worker,mcp}.log`.
These bounded profiles do not complete the remaining #714–#718 work.

The increment was rebased onto remote `dev` at `0f1accd2f`, preserving its
native graph/C projection updates. Both Web flags were restored to ON; the
235-step integrated rebuild completed. All 541 Web cases repeated with the
same 534 passes/seven explicit skips, including complete Claude Code recovery.
ABI discovery and ten worker suites passed. Logs:
`/tmp/neverd-pkg-restored-build.log` and
`/tmp/neverd-pkg-restored-{web,api,worker}.log`.

The first concurrent MCP run had four passes and one 15-second initialization
response timeout in the Claude case, before any sample-analysis tool call.
Other newly linked SDK/worker processes also had slow startup in that run;
the exact startup cause is not established. The unchanged isolated Claude case
passed in 3.55 seconds. A subsequent five-suite MCP run overlapped complete
Claude source recovery and the full Bun/pkg producer corpora: all eight selected
checks passed, with the MCP Claude case taking 3.61 seconds. The response limit
was not increased and no product change was made to mask the failure. Preserve
this intermittent startup limitation rather than treating retries as a fix.
Logs: `/tmp/neverd-pkg-restored-mcp.log`,
`/tmp/neverd-pkg-restored-mcp-claude-isolated.log`,
`/tmp/neverd-pkg-restored-mcp-repeat.log` and
`/tmp/neverd-pkg-restored-overlap-repeat.log`.

The later integration with remote `dev` at `3fbb86447` rebuilt the affected
native/SDK/transport targets and owning core tests. All 60 pkg/nexe/native
cases passed with producer corpora supplied, as did worker Web, all five MCP
suites including real Claude, ABI discovery and the 23-capability manifest
check. The backend-omission-only ABI case skipped as expected. Logs:
`/tmp/neverd-pkg-final-sync-{build,core-build,web,worker,mcp,api,capabilities}.log`.
The [combined matrix](web-native-javascript-matrix.md) now records independent
runtime/platform/map/cache contracts and explicit per-sample cross-track
statuses; it does not promote partial navigation or absent protocol captures
to completed acceptance.

## Reversible lexical renaming — 2026-10-11

`javascript-scope-rename-v1` adds an explicit `scope_renaming` pass using the
C++ parser and lexical binding model. Independent planning review excluded all
bindings owned by functions with non-simple parameters: their implicit
parameter-to-variable value copies are outside the current lexical graph.
Other exclusions cover dynamic scope, root bindings, function/class names,
inferred names, writes, shorthand and unsupported declaration forms. Accepted
bindings use deterministic collision-free names and complete identifier sets.
The candidate is reparsed; complete AST, scopes, declarations, bindings,
references and per-node scope ownership are compared before exact inversion.
See the [profile](web-scope-rename-profile.md).

Core planning and validation are separate rewrite components. Session, SDK,
CLI, worker and MCP use the existing shared cache and receipt-bound local
export. Rename metadata has binding evidence and no primitive-value claim;
private names stay out of previews. Fixed pre-change primitive receipts prove
that both older passes retain exact IDs, export tokens, bytes and SHA-256.
The initial CLI test exposed a reused test-output file retaining trailing
bytes; explicitly truncating the harness files fixed it without a product
change. The focused SDK and compatibility tests then passed.

Actual NeverD CLI extraction and rewriting of original Claude Code 2.1.296 Bun
module 1 accepted 46 identifier occurrences and refused 17 binding declarations.
The exported original exactly matched the earlier recovered member; the
1,908-byte candidate reparsed and restored its 1,362-byte original exactly.
Module 228 instead exceeded the unchanged 4,096-record ceiling and published
no candidate, partial records or export receipt. Both outcomes are exercised
from the original hash-pinned binary in the C++ core test. The
[qualification record](web-claude-code-qualification.md) pins the hashes and
local output; no target or runtime was executed and no sample source is
committed.

Enabled Web verification passed 550 of 557 cases with every existing producer
corpus and the complete 2,345-unit Claude recovery supplied. Seven explicit
skips cover six unavailable LLVM Zstd map cases and the ASAR-policy omission
case. All ten worker regressions, all five MCP suites including real Claude,
and enabled ABI discovery passed. Logs:
`/tmp/neverd-rename-full-web.log`, `/tmp/neverd-rename-worker-all.log`,
`/tmp/neverd-rename-mcp.log` and
`/tmp/neverd-rename-availability-enabled.log`.

With JS parsing disabled, ABI discovery, worker Web and four MCP suites passed;
the source-dependent Claude MCP test skipped. With the whole Web backend
disabled, both ABI discovery/stub cases and four MCP suites passed; Web worker
and Claude MCP cases explicitly skipped. The new pass reports
`capability_unavailable` through the existing API when its owner is absent.
Logs: `/tmp/neverd-rename-nojs-{api,worker,mcp}.log` and
`/tmp/neverd-rename-off-{api,worker,mcp}.log`.

Both Web flags were restored to ON. The owning rebuild and all 31 rewrite
core/SDK tests, ten worker suites, five MCP suites, ABI discovery and the
23-capability manifest check passed on `1d9652797`. Logs:
`/tmp/neverd-rename-restored-{build,web,worker,mcp,api}.log` and
`/tmp/neverd-rename-capabilities.log`. This increment does not complete general
control-flow simplification or the remaining four tracks.

## Reversible Boolean branch expressions — 2026-10-11

`javascript-boolean-branches-v1` adds explicit `boolean_branches` selection
through the shared C++ binding/value/effect models. The independently reviewed
plan required two corrections: retain module asynchrony by refusing every
candidate containing await, even in an unreachable branch; and use
`(0,(<selected bytes>))` so a chosen sequence remains one operand and GetValue
semantics survive calls, typeof, delete and indirect eval. No statement
transformation or duplicate truthiness/binding analysis was introduced.

`rewrite/Branches.cpp` owns bounded candidate selection and syntax barriers;
`BranchValidate.cpp` owns exact GetValue scaffolding and retained AST/reference
projection. Complete scope/declaration ownership survives. Chosen side effects
and possible exceptions are retained, never assumed absent. Exact inversion
and the existing private export transaction precede publication. Session,
SDK/CLI, worker and MCP share the new profile and two-result cache. The older
primitive goldens and a pre-change rename golden retain their entire serialized
receipt bytes, hashes, IDs and export tokens.

The actual NeverD Bun CLI selected Claude module 1047, accepting one branch and
refusing six; its exported original exactly matches the earlier recovered
member and its candidate passes reparse, graph projection and inverse checks.
Module 1897 retains all 1,007 candidates unchanged. Module 1613 refuses the
source-byte ceiling before rewriting. C++ tests extract all three from the
original hash-pinned executable. See the
[qualification](web-claude-code-qualification.md) and
[profile](web-boolean-branches-profile.md). No target was executed or sample
source committed; budgets were unchanged.

All 50 focused core/SDK tests passed, including work exhaustion, old raw
receipts, private canaries and unusable-PATH CLI export. Enabled ABI discovery,
the actual worker Web transport and all five MCP suites passed. After adding
the three-member real test, complete Web regression passed 570 of 577 cases,
including all supplied runtime/package corpora and all 2,345 Claude JS units.
Seven explicit skips remain the six unavailable LLVM Zstd map cases and the
ASAR-policy omission case. All ten worker regressions and the 23-capability
manifest check passed. Logs: `/tmp/neverd-branch-{focused,full-web,api,mcp,worker-all,capabilities}.log`.

With JS parsing disabled, ABI discovery, worker Web and four MCP suites passed;
the source-dependent Claude MCP case skipped. With the whole backend disabled,
both ABI/stub cases and four MCP suites passed; worker Web and Claude MCP
explicitly skipped. Both flags were restored to ON, then all 51 focused rewrite
core/SDK cases, ten worker suites, five MCP suites and ABI discovery passed.
The backend-omission-only ABI case skipped in the enabled profile. Logs:
`/tmp/neverd-branch-nojs-{api,worker,mcp}.log`,
`/tmp/neverd-branch-off-{api,worker,mcp}.log` and
`/tmp/neverd-branch-restored-{build,web,api,worker,mcp}.log`.

## Offline installation entry traces — 2026-10-11

The independently reviewed `node-install-entry-traces-v1` plan corrected three
counterexamples before implementation: manifest/lock-only evidence must work
without a fake source selection; Node's bare `inspect` is a reserved command;
and readable archive namespaces cannot distinguish missing members from omitted
symlinks or encrypted/unsupported content. Commands, entries, source coverage,
requests and associations preserve these boundaries in C++ metadata.

`packages/install/Command`, `Trace` and `Publication` separate literal admission,
bounded evidence joins and metadata serialization. The package owner retains
its exact namespace, and entry validation shares `packages/Paths`. A separate
Node adapter in the module-link owner consumes authoritative source origins
without changing older ESM behavior or upgrading external/caller-selected
CommonJS evidence. Result identities bind the shared semantic profiles as well
as package, namespace and selected behavior. The session, SDK, CLI, worker and
MCP retain the same paged, revision-bound result with no runtime invocation.
See the [profile](web-package-install-profile.md).

Actual NeverD CLI analysis of the original hash-pinned Claude Code 2.1.296 npm
wrapper selected both JS members and retained two lifecycle declarations. One
admitted entry associates six existing behavior records in its own file; the
other command refuses. The remaining two selected source behavior records are
not attached to that entry. All 13 module requests retain unresolved boundaries.
The [qualification record](web-claude-code-qualification.md) pins the original
archive, result identity and saved metadata report hash. No package script,
sample JS or external analyzer ran.

The first core run passed all nine cases. Expanded owner regression passed
41 cases, including the original-archive entry/record checks. The first paging
fixture incorrectly repeated a const declaration and was correctly rejected by
the parser; replacing it with independent call statements produced the intended
300 records and 600 associations. All eight then-existing Node SDK/CLI cases,
worker Web, five MCP suites and enabled ABI discovery passed. Initial complete
Web regression passed 588 of 595 cases, with six unavailable LLVM Zstd map cases
and the ASAR-policy omission case explicitly skipped. A subsequent independent
1,000-call/nine-hook fixture passed the 8,192-association failure check: no result
or cached chain is published and a source-free trace remains usable.

Logs: `/tmp/neverd-install-{core,sdk,atomic,worker,mcp,api,full-web}.log`.
The surface manifest was updated for both new C APIs and the 23-capability
manifest check passed. The optional build-identity audit could not run because
this existing build lacks a CMake File API index; it is not reported as a
successful audit. Logs: `/tmp/neverd-install-capabilities-manifest.log` and
`/tmp/neverd-install-capabilities.log`.

JS-disabled ABI discovery, worker Web and four MCP suites passed; the
source-dependent Claude MCP case skipped. With the backend disabled, both
ABI/stub cases and four MCP suites passed, with worker Web and Claude MCP
explicitly skipped. After restoring both flags, complete Web regression passed
589 of 596 cases with the same seven expected skips; all ten worker suites,
five MCP suites and enabled ABI discovery passed. Logs:
`/tmp/neverd-install-{nojs,off,restored}-{build,api,worker,mcp}.log` and
`/tmp/neverd-install-restored-web.log`.

Installation associations additionally preserve the original fixed API/kind,
origin class and node/byte-span anchors. The CLI report remains readable when
package behavior was created from temporary Node models and the independent
Node behavior cache is empty; the SDK regression resolves each copied anchor
against the retained package behavior records.
All 23 affected core/SDK cases passed after this publication addition, including
the association-budget failure. The actual wrapper rerun retained six
associations and the same analysis identity. Logs:
`/tmp/neverd-install-anchors-{build,tests}.log` and
`/tmp/neverd-install-claude-anchors.jsonl`.

## Selected package behavior comparison — 2026-10-11

The independently reviewed `node-selected-behavior-diff-v1` plan identified
three necessary counterexamples. A value-budget failure can change a Buffer
classification while Node behavior remains partial; comparison must retain
the original upstream coverage and refuse that source pair. Original records
must resolve through package behavior even when independent Node behavior was
never cached. Pattern groups must bind to source pairs so independent per-file
changes cannot cancel.

`packages/behavior/Package` retains source mode/hash/size and upstream coverage
from its original typed inputs without changing older publication or IDs.
`Diff` validates the retained selections, pairs exact inventory-relative files
and groups existing records. `DiffPublication`, `SessionPackageBehaviorDiff`
and thin C API/CLI/worker/MCP consumers preserve coverage and original anchors.
The [profile](web-package-behavior-diff-profile.md) defines unknown selection,
embedded delivery, incomparable context and resource ceilings.

The first core run exposed a negative fixture that used identical
content-addressed captures as if their owner IDs differed. An unrelated owner
ID corrects that fixture. The combined run then passed all 41 affected core
cases and 15 of 16 SDK cases; the archive CLI fixture exposed an LLVM redirect
that preserved the tail of an earlier longer reply. Explicitly truncating the
test's output files fixed it, and all seven new SDK/CLI cases passed. The
3,000-call-per-side work-budget failure publishes no partial or cached result.
Logs: `/tmp/neverd-behavior-diff-{focused,sdk}-tests.log`.

The actual CLI compared both original Claude wrappers with an unusable
external-tool PATH: two comparable source pairs have identical supplied bytes,
six source-specific pattern groups retain equal counts and 16 original call
anchors remain resolvable. The original integrity owner independently verified
the 2.1.295 wrapper against its captured registry SHA-512 declaration. The
[qualification](web-claude-code-qualification.md) pins original archives,
metadata, result IDs and the saved report. No package or external analyzer ran.

After rebasing the installation increment onto dev and rebuilding the owning
targets, complete Web regression passed 607 of 614 cases, including all supplied
runtime/package corpora and all 2,345 Claude JS units. Seven explicit skips are
the six unavailable LLVM Zstd map cases and the ASAR-policy omission case.
All ten worker suites, five MCP suites and enabled ABI discovery passed. The
23-capability manifest check passed. Logs:
`/tmp/neverd-behavior-diff-{final-build,full-web,worker,mcp,api,capabilities}.log`.

With JavaScript parsing disabled, ABI discovery, worker Web and all five MCP
suites passed, including real Bun extraction, which does not need the parser.
With the backend disabled, both ABI/stub cases and four MCP suites passed;
worker Web and the actual Claude extraction case explicitly skipped. Logs:
`/tmp/neverd-behavior-diff-{nojs,off}-{config,build,api,worker,mcp}.log`.
After restoring both flags to ON, all 57 focused core/SDK cases, ten worker
suites, five MCP suites and enabled ABI discovery passed. Logs:
`/tmp/neverd-behavior-diff-restored-{config,build,focused,worker,mcp,api}.log`.
The subsequent dev synchronization preserves upstream archive-capability guards
and portable documentation paths. Its 777-step integration rebuild passed;
complete Web regression again passed 607 of 614 cases with the same seven
explicit skips. All ten worker suites, five MCP suites and enabled ABI discovery
passed. The 23-capability manifest and tracked-source provenance checks passed;
the latter also passed the 16-commit history scan after the unpublished work was
consolidated without machine-specific documentation paths. Logs:
`/tmp/neverd-behavior-diff-dev-{build,full-web,worker,mcp,api,capabilities,provenance}.log`
and `/tmp/neverd-package-triage-dev-history.log`.

## Captured npm advisory declarations — 2026-10-11

The independently reviewed offline-assessment plan requires source/time
descriptors, unknown-selector coverage and separately bounded uncomposed
imports. `packages/evidence/Descriptor` owns timestamp/source admission;
`packages/advisories/{Reader,SemVer,Match,Publication}` own declarations,
version semantics, comparison and fixed metadata. The session caches imports
and canonical selections atomically. Four C APIs and thin CLI/worker/MCP
consumers share these owners without JavaScript or runtime dependencies.
See the [profile](web-package-advisory-profile.md).

The first compilation found a use of `size()` on LLVM's JSON `ObjectKey`;
the explicit `StringRef` view corrects that boundary. All 15 core cases then
passed, including the pinned public OSV record. The first expanded run passed
23 of 24 cases; CLI output stopped after the existing inventory page because
that older API uses a null continuation without `page_complete`. The adapter
now consumes the common `next_offset` contract. All nine SDK/CLI cases passed
after repair, including 600-entry pages, uncomposed import capacity, shared
retained-byte and aggregate matching-work failures with unchanged caches.
Logs: `/tmp/neverd-advisory-{core-build,core-tests,focused-tests}.log` and
`/tmp/neverd-advisory-{cli-repair-build,sdk-repaired-tests}.log`.

Actual NeverD CLI commands compared the original public OSV declaration against
captured official npm metadata for lodash 4.17.18 and 4.17.19 with an unusable
external-tool PATH. The reports retain one declared match and one selected-range
non-match respectively, original anchors and explicit unverified exploitability.
The [qualification](web-package-advisory-qualification.md) pins input, descriptor
and report hashes. Originals and reports are preserved locally.

Complete Web regression passed 631 of 638 cases; the six unavailable LLVM Zstd
map cases and ASAR-policy omission case explicitly skipped. All ten worker
suites, five MCP suites and enabled ABI discovery passed. Logs:
`/tmp/neverd-advisory-{full-web,worker,mcp,api}.log`.
With JavaScript disabled, all 24 advisory core/SDK cases, worker Web, five MCP
suites and enabled ABI discovery passed. Replaying the preserved real capture
produced the byte-identical CLI report. Logs:
`/tmp/neverd-advisory-nojs-{config,build,focused,worker,mcp,api}.log`.
The 23-capability manifest and 7,267-file tracked-source provenance check,
including 16 history commits, passed. With the backend disabled, both ABI/stub
cases and four MCP suites passed; worker Web and actual Claude extraction
explicitly skipped. After restoring both options, all 24 advisory cases, ten
worker suites, five MCP suites and enabled ABI discovery passed. Logs:
`/tmp/neverd-advisory-{off,restored}-{config,build,api,worker,mcp}.log` and
`/tmp/neverd-advisory-restored-focused.log`.

## Captured signature and provenance metadata

The `offline-package-attestation-metadata-v1` increment adds explicitly
selected npm registry, in-toto Statement v1, DSSE and SLSA provenance v1
metadata subsets. Native bounded Base64 admission preserves exact decoded
DSSE bytes; signature declarations retain independent array positions even
when key IDs repeat. Unknown statement/predicate types, absent declarations,
invalid encodings and contradictory timestamps remain distinct states.
Names, key IDs, signature bytes and URIs stay private. No keys are fetched and
no signature, publisher, trust chain, transparency log or SLSA level is
authenticated. The [profile](web-package-attestation-profile.md) records the
precise boundary.

One explicitly selected in-toto SHA-256/SHA-512 declaration can be compared
against one captured original through the existing package integrity owner.
Canonical subject/digest ordinals and identity recipes prevent a foreign or
forged selection from becoming evidence. The receipt binds original and
declaration artifact/hash anchors; it does not bind an installed package or
authenticate another subject. The reader/publisher live under
`packages/attestations`; `packages/evidence/Publication` now owns the shared
descriptor/time output. Session, four C APIs, CLI, worker and MCP consume these
owners. Imports, parsed metadata and explicit comparisons have independent
limits and atomic refusal; descriptors and the 8 MiB retention ceiling are
shared with captured advisories.

The first focused run exposed test expectations that disagreed with existing
SRI and source-cache field names. Those expectations now use their owning
contracts. The CLI test also placed temporary storage in its input root,
correctly triggering `input_changed` when the spool was created and unlinked.
Its temporary directory now stays outside the capture, with child output
included in failure diagnostics. All 45 advisory/attestation core and SDK
cases pass, including shared metadata exhaustion, exact replay, malformed
inputs, paging, stale revisions and an unusable external-tool PATH. Logs:
`/tmp/neverd-attestation-{core-tests,focused-tests,sdk-diagnosis-tests,repaired-tests}.log`.

The actual CLI reads preserved official Claude Code 2.1.295 and 2.1.296 npm
registry captures. Each retains two unverified signatures and original hashes;
unknown capture timestamps remain absent. The
[qualification](web-package-attestation-qualification.md) pins inputs and
reports. The earlier advisory report remains byte-identical after the shared
publisher change. Complete enabled Web regression passes 652 of 659 cases,
including all 2,345 stored Claude Code JS units; six unavailable LLVM Zstd map
cases and the ASAR-policy omission case explicitly skip. Ten worker suites,
five MCP suites and enabled ABI discovery pass. Logs:
`/tmp/neverd-attestation-{full-web,worker,mcp,api}.log`.

With JavaScript disabled, all 45 focused cases, worker Web, all five MCP
suites and enabled ABI discovery pass; the real 2.1.296 report is byte-identical.
With the complete backend disabled, both ABI/stub cases and four MCP suites
pass; worker Web and actual Claude extraction explicitly skip. Both options
are restored to ON. The restored 45 unique focused cases, ten worker suites,
five MCP suites and enabled ABI discovery pass. Concurrent CTest discovery
initially duplicated generated SDK registrations; regenerating that local
index and rerunning the focused suite confirms the count without counting
replays as additional coverage. Logs:
`/tmp/neverd-attestation-{nojs,off,restored}-{config,build,worker,mcp,api}.log`,
`/tmp/neverd-attestation-nojs-focused.log` and
`/tmp/neverd-attestation-restored-focused-unique.log`.
LLVM formatting, the 23-capability manifest and source provenance across
7,301 tracked files and 16 history commits pass.

Composed assessments, analyst dispositions, readable reports and the broader
acceptance work remain; this increment does not close #715 or the other epics.

## Optional native Bun map decoder — 2026-10-12

`compression/Zstd` prefers the linked LLVM decoder and optionally uses the
exact native static Zstandard 1.5.7 SDK when LLVM omits Zstd. It writes into
the caller's bounded buffer; `BunSourceMap` remains the sole owner of frame,
window/output/blocks, size and UTF-8 admission. No tool, script or sample is
executed and no decoder is downloaded. The original BSD notice is staged in
binary and SDK license directories and the install tree. The
[profile](web-bun-profile.md#native-static-sdk-qualification--2026-10-12)
pins all twelve actual CLI report hashes.

The first corpus test found the historical Linux x64 manifest's
`full_elf_sha256` field, while the other five targets use
`full_container_sha256`. Selecting each fixture's actual field fixes the
test without changing recorded hashes. The six-target/twelve-image test now
passes, checking independently authored original sources and stored anchors.
All twelve CLI commands pass with an unusable external-tool PATH. Their Linux
x64 decoded module/map/source pages match the earlier LLVM-backed reports
exactly. Logs: `/tmp/neverd-zstd-{core-tests,six-platform-tests}.log`.

The 690-step full interface integration rebuild and the subsequent two-step
corpus-test rebuild pass. Complete Web regression passes 659 of 660 cases,
including all 2,345 stored Claude Code JS units; only the ASAR-policy omission
case skips. The six formerly unavailable decoder cases now run and pass.
All five MCP suites pass; enabled ABI discovery passes with the backend-off
case explicitly skipped. The full worker directory passes 21 of 24 tests,
with three absent native name/listing fixtures skipped. Logs:
`/tmp/neverd-zstd-{enabled-build,reconciled-build,full-web,worker,mcp,api}.log`.

With the SDK explicitly omitted and LLVM Zstd absent, all nine ordinary map
cases pass and seven decoder/corpus cases skip. All three focused SDK/CLI/anchor
cases pass, and the actual CLI returns `bun_source_map_zstd_unavailable` with
an unusable external-tool PATH. Logs:
`/tmp/neverd-zstd-omitted-{config,build,map-tests,sdk-tests}.log`.
With the SDK restored and JavaScript parsing disabled, all sixteen map cases
pass, including the twelve real images, along with the two compiled focused
SDK cases, worker Web, five MCP suites and enabled ABI discovery. The actual
Linux x64 asset-map CLI report is byte-identical to its parser-enabled report.
Logs: `/tmp/neverd-zstd-nojs-{config,build,map-tests,sdk-tests,worker,mcp,api}.log`.
The native license copies match the original BSD notice byte-for-byte and
neither the CLI nor SDK shared library requires a Zstd runtime dylib.

Both feature flags are restored to ON with the SDK enabled. All sixteen map
cases, three focused SDK cases, four selected worker suites, five MCP suites
and enabled ABI discovery pass after the 168-step restoration build. Logs:
`/tmp/neverd-zstd-restored-{config,build,map-tests,sdk-tests,worker,mcp,api}.log`.
LLVM formatting, the 23-capability manifest and the source-provenance check
over 7,310 tracked files and sixteen history commits pass. This increment
adds no C API symbols; complete-backend omission keeps the earlier qualified
stubs and is not separately requalified here. Composed package assessments,
reports and the broader #714–#718 acceptance work remain.

## Selected package assessment and analyst review — 2026-10-12

`offline-package-assessment-v1` composes exact retained inventory/namespace,
original-integrity, advisory, attestation, source behavior, installation and
comparison owners. The implementation separates selection/budgets, integrity
relationships, metadata, behavior, immutable dispositions, JSON publication and
Markdown under `lib/web/packages/assessment`. Session owns two immutable
assessments and one shared 256-event/256-KiB review allowance. Five additive C
APIs and native CLI/worker/MCP consumers use that same owner. Fixed dispositions
are caller assertions and do not change analyzer confidence or authorize source
disclosure/execution. Every finding/history/report page pins the current review
head as well as the revision. See the [profile](web-package-assessment-profile.md).

Cross-process parity exposed conditional `std::string` temporaries behind the
assessment identity's `string_view` preimage. The preimage now references
retained owner storage; independent root/archive recipe assertions and
direct/framed/MCP equality verify the fix. Other cases cover foreign registry
declarations despite a valid digest, immediate archive ancestry, canonical
selection order, unsupported advisory coverage, legacy SHA-1, retained metadata
refusal, nested subject receipt ownership, source/pattern/occurrence anchors,
610-finding pages, stale heads/revisions, exact last replay, foreign historical
findings, shared capacity, cache reset and private-field canaries. Fifteen core
and ten SDK/CLI cases pass. Logs:
`/tmp/neverd-assessment-{core,sdk,transports}-final.log`.

The real Claude Code 2.1.296 wrapper/registry capture produces 45 findings:
nine coverage rows, one inventory declaration, one SHA-512 original archive
match, three unverified registry/signature metadata rows, two selected source
rows plus eight recognized syntax records, and 21 installation rows including
13 unresolved requests and six static associations. JSON and Markdown agree on
assessment/root/head identities; the report charges 48,320 work units and
98,464 metadata bytes. Both run with an unusable external-tool PATH. The
separate public OSV sample preserves a positive declared version match for
lodash 4.17.18 and a selected negative for 4.17.19, without a safe-package claim.
Exact inputs and report hashes are in the
[actual qualification](web-package-assessment-qualification.md). Actual samples
have no automatic dispositions. Signatures, installed dependencies, runtime
behavior, reachability and feed freshness remain unverified.

Enabled full Web regression passes 684 of 685 cases, including all 2,345 stored
Claude JS units and the six-target Bun map corpus; the ASAR-policy omission
case explicitly skips. All 24 worker suites have no failures: 21 pass and three
missing independent native fixtures skip. All five MCP suites and enabled ABI
discovery pass. Logs:
`/tmp/neverd-assessment-web-regression.log`,
`/tmp/neverd-assessment-worker-regression.log` and
`/tmp/neverd-assessment-transports-final.log`.

With JavaScript omitted, 15 core and eight SDK cases pass; two source-dependent
SDK cases explicitly skip. Worker Web, five MCP suites and enabled ABI discovery
pass. The actual metadata-only Claude report is byte-identical to the enabled
report. With the complete Web backend omitted, both ABI/stub checks and four
MCP suites pass; worker Web and actual Claude extraction explicitly skip. Logs:
`/tmp/neverd-assessment-nojs-{configure,build,core,sdk,transports}.log`,
`/tmp/neverd-assessment-off-{configure,build,transports}.log` and
`/tmp/neverd-assessment-claude-nojs.jsonl`.

Both build flags are restored to ON. All 25 focused cases, worker Web, five
MCP suites and enabled ABI discovery pass after restoration; the full selected
Claude JSON report is byte-identical. Logs:
`/tmp/neverd-assessment-restored-{configure,build,core,sdk,transports}.log` and
`/tmp/neverd-assessment-claude-restored.jsonl`.
LLVM formatting and the 23-capability manifest pass, including all 115 manifest
checker tests. Source provenance passes across 7,327 tracked files and sixteen
history commits. The new archive-ancestry case also honors the optional archive
path policy: with ASAR omitted, fourteen core cases pass and that one case skips.
After restoring ASAR, all fifteen core and ten SDK cases, worker Web, five MCP
suites and enabled ABI discovery pass. The actual Claude report remains
byte-identical. Logs:
`/tmp/neverd-assessment-noasar-{configure,build,core}.log` and
`/tmp/neverd-assessment-asar-restored-{configure,build,core,sdk,transports}.log`.
After integrating `dev` at `a5f9115d3`, every feature implementation/header/test
file is byte-identical to the reviewed increment. The 570-step native/interface
rebuild and twelve-step source-test rebuild pass. All 25 assessment cases,
fifteen installation cases (including the preserved real wrapper), worker Web,
five MCP suites and enabled ABI discovery pass. The actual Claude assessment
is still byte-identical. Source provenance passes across 7,333 tracked files and
sixteen history commits. Logs:
`/tmp/neverd-assessment-upstream-{build,source-build,core,sdk,install,transports,provenance}.log`.
GUI/persistent reviews,
broader lifecycle/loader/host qualification and the other epic acceptance gaps
remain; this increment does not close #714–#718.

## Captured Claude npm platform packages — 2026-10-12

The C++ `archive-bun-export` CLI adapter selects one retained archive member and
delegates to the existing Bun extraction/export owner. It adds no extractor,
filesystem writer, C API symbol or automatic package-entry execution. The
manifest retains the executable's enclosing archive/member/hash and exact
original or expanded locator. Existing output directories, unavailable members
and non-Bun members are refused. C++ fixtures cover tar, gzip and stored/deflate
ZIP, exact original/source bytes, retained origins and metadata canaries with an
unusable external-tool PATH. All five focused SDK/CLI cases pass.

Eight independently captured Claude Code 2.1.296 npm platform packages qualify
real prelinked graphs on ELF/Linux, thin Mach-O/macOS and PE/Windows, each x64
and ARM64, including both supplied Linux libc variants. Every original archive
matches its separately captured SHA-512 declaration; this is not publisher
authentication. Eight C++ corpus cases pin archive/member hashes and lengths,
graph geometry, entry/startup indices and strict source counts/bytes. The
[qualification](web-claude-code-cross-platform-qualification.md) records their
exact hashes, capture layout and export manifest hashes. No installer, target
runtime or input code is executed. Private samples remain outside Git.

All eight actual CLI exports preserve and reread every output file. Decoded,
parsed and readable counts agree: 18,794 JS units and 358,539,894 decoded UTF-8
bytes summed per platform, not distinct source files across platforms. There
are no unavailable sources or source maps. Opaque JSC caches are preserved;
erased TypeScript, types/names and cache semantics are not recovered. Every
readable copy passes the same parser-tree comparison. Logs:
`/tmp/neverd-claude-cross-{build,core,sdk,regression-build}.log`;
the native/interface build takes 192 steps and the regression/interface resync
takes 39 steps.

The first local regression exposed duplicate generated CTest discovery entries
after concurrent root/subdirectory discovery. Its 1,116 records and two ASAR
policy skips are not additional coverage. The generated indices were preserved
in `/tmp/neverd-claude-cross-discovery-20261012/` and regenerated serially.
The resulting 695 unique Web cases have 694 passes and one expected ASAR-policy
skip (68.24 seconds). The selected ABI, worker and MCP checks have seven passes
and one expected backend-omission skip (7.15 seconds). Logs:
`/tmp/neverd-claude-cross-web-regression-unique.log`,
`/tmp/neverd-claude-cross-unique-discovery.log` and
`/tmp/neverd-claude-cross-transports.log`.

With JavaScript omitted, all eight corpus cases and three archive CLI cases
pass after the 129-step build. The actual macOS ARM64 archive still exports
2,364 exact sources and 45,035,899 decoded bytes, the same original hash and
archive ancestry, with zero unavailable sources. Parsed/readable counts are
zero and the readable profile is null. The omission export manifest SHA-256 is
`4992ccd5f2e3a04285aad6c8224102d4cdbb1701fc41bb228fd1d43b6e00ebc7`.
Logs: `/tmp/neverd-claude-cross-nojs-{configure,build,core,sdk}.log`.
JavaScript is restored to ON after the 129-step rebuild. All eight captured
corpus cases and five focused SDK/CLI cases pass again. Logs:
`/tmp/neverd-claude-cross-restored-{configure,build,core,sdk}.log`.
LLVM formatting and source provenance across 7,369 tracked files and sixteen
history commits pass. The capability checker reports sixteen pre-existing
unclaimed native source-patch declarations introduced by upstream integration;
the manifest is unchanged by this increment. That global check is not claimed
as passing. Logs: `/tmp/neverd-claude-cross-{capabilities,provenance}.log`.
After integrating `dev` at `89708f43f`, all twelve increment files are
byte-identical before the additional validation note. The 123-step
native/SDK/CLI/worker/MCP rebuild passes. Five focused SDK/CLI cases and the
seven enabled transport/ABI checks pass; the backend-omission ABI case skips as
expected. Source provenance passes across 7,370 tracked files and sixteen
history commits. Logs:
`/tmp/neverd-claude-cross-upstream-{build,sdk,transports,provenance}.log`.
Linux/Windows host qualification, additional container layouts and the other
epic acceptance gaps remain; this increment does not close #714–#718.

## Bounded client source syntax navigation — 2026-10-12

`javascript-client-syntax-features-v1` adds paged tool, configuration,
permission, MCP and hook candidates to the existing navigation operation.
Generic navigation remains the authoritative owner of containment and lexical
relationships. Feature admission/indexing, recognition and publication live in
`lib/web/features/`; `SessionNavigation` composes and caches the two results.
No C API symbol, evaluator, runtime or external analyzer is added. Every record
has `source_syntax_only` confidence. Available coverage stays partial, including
zero-match results; callback links describe source relationships, not runtime
targets. Model validation, indexing, parent walks and matching share explicit
work/record limits. Feature exhaustion preserves independent generic navigation.
See the [profile](web-client-feature-profile.md).

The captured Claude Code 2.1.296 macOS ARM64 package visits all 2,364 decoded
units and 45,035,899 UTF-8 bytes. The interactive parser admits 2,354 units;
three byte-limit, three parser-work and four syntax-inventory refusals remain
explicit. No deadline expires in the recorded receipt. All admitted units finish
the pattern inventory, producing 416 candidates: three tool, 88 configuration,
165 permission, 110 MCP and 50 hook records. Forty-three callback source links
and one permission-test containment link are retained. Those counts do not
authenticate implementations or establish configuration precedence, effective
policy, reachability or server behavior. Recovery has separately preserved and
reparsed all 2,364 units. Two actual CLI selections with an unusable external-tool
PATH qualify a positive permission-setting candidate and a zero-match result.
Exact inputs, identities and limits are in the
[qualification](web-client-feature-qualification.md). Logs:
`/tmp/neverd-client-features-corpus-receipt.log`,
`/tmp/neverd-client-features-corpus-receipt.xml` and
`/tmp/neverd-client-features-claude-{1676,0392}.jsonl`.

Eight new core cases plus five generic navigation cases pass; two SDK cases
cover private metadata, paging, anchors and stale revisions. Worker/MCP cases
compare the shared owner, confidence and availability contracts. Fixtures cover
escaped/static versus computed names, spread, accessors, duplicate fields,
reassignment, shadowing, dynamic scope, nested function boundaries, unrelated
event literals, record exhaustion and foreign/unavailable models. The final
92-step build and explicit four-step transport-test rebuild pass. The full
705 unique Web cases have 704 passes and one expected ASAR-policy skip
(90.79 seconds); selected transport/ABI checks have seven passes and one expected
backend-omission skip (10.60 seconds). Logs:
`/tmp/neverd-client-features-{build-final,transport-build,core-final,sdk,web-regression,transports}.log`.

With JavaScript omitted, the 135-step build passes. The SDK stub test explicitly
refuses the new `features` record query; seven transport/ABI checks pass and the
backend-omission case skips (6.71 seconds). JavaScript is restored to ON after
the 136-step build. All thirteen focused core cases, two SDK cases and seven
transport/ABI checks pass again; the backend-omission case skips as expected
(7.24 seconds). Logs:
`/tmp/neverd-client-features-nojs-{configure,build,sdk,transports}.log` and
`/tmp/neverd-client-features-restored-{configure,build,core,sdk,transports}.log`.
No public ABI is added, and a separate complete-backend-omission rebuild is not
claimed for this increment. LLVM formatting, diff whitespace and source
provenance across 7,381 tracked files pass. The existing sixteen unclaimed
native source-patch declarations still prevent claiming a global capability
checker pass. Import-rooted attribution, configuration/dataflow, property
dispatch, GUI/persistent reviews and the other epic acceptance gaps remain;
this increment does not close #714–#718.

After integrating `dev` at `0d2ae64ea`, all feature implementation and test files
are byte-identical. The 164-step native/SDK/CLI/worker/MCP rebuild passes, followed
by all thirteen focused core cases, two SDK cases and seven transport/ABI checks;
the backend-omission case skips as expected (9.39 seconds). Logs:
`/tmp/neverd-client-features-upstream-{build,core,sdk,transports}.log`.

## Atomic package review journals and the Qt workbench (2026-10-12)

`offline-package-review-journal-v1` adds persistent fixed analyst assertions
against an immutable assessment. `packages/assessment/Disposition` remains the
single owner of chain identities, finding membership, prefixes and shared
session charges. `SessionPackageReviewRestore` only admits the closed bounded
JSON schema and serializes the result before replacing state. A late invalid
decision refuses the entire restore. Existing history must be an exact prefix;
only the new suffix consumes capacity. Exact current-head replay is free even
at capacity. The always-exported additive C API and strict worker/MCP operation
delegate to that same owner. See the [journal profile](web-package-review-journal.md).

The C++ Qt `web` presentation library adds File → Offline package review. It
uses a private existing NeverD worker for explicit capture, bounded inventory
pages, tar/tgz/zip member selection, package-json/npm-lock analysis, optional
captured registry declarations and immutable assessment findings. Evidence,
confidence and analyst disposition remain separate. Journals contain opaque
identities and fixed assertions only, with no names, paths, source or notes.
`QSaveFile` preserves prior files on failed writes; loading previews metadata
and explicit restoration validates the complete chain in the backend. Escape,
assessment replacement, window closure and main-workbench quit preserve unsaved
decisions unless the user explicitly discards them. Stopping the worker leaves
a consistent local journal available to save. The controller never computes
review hashes or interprets targets.

Four new core restoration cases and two new SDK cases cover full-chain
validation, suffix charging, free replay, stale heads, changed evidence,
prefix refusal, duplicate/unknown fields and late tampering. The focused totals
are nineteen core and twelve SDK passes. The complete Web regression has 710
passes and one expected missing-ASAR-policy skip among 711 unique cases
(103.36 seconds). Worker/MCP/ABI qualification has seven passes and one expected
backend-omission skip (9.32 seconds). Logs:
`/tmp/neverd-review-journal-{core-final,sdk-final,web-regression,transports}.log`.

The Qt cases use C++ fixtures and the production worker, including two-page
inventories, registry/archive ancestry, restart restoration, unchanged history
on refusal, changed-input rejection and large-font/RTL controls. The actual
Claude Code 2.1.296 macOS ARM64 archive has SHA-256
`50fbf594d6526c9aae7d66b8b45a08d89ae6b863e2bc9777bfb277d6521a7458`.
Its captured root has four artifacts and the extracted tar has four members.
Selecting member one as package-json with the original/registry/descriptor
artifacts produces fourteen findings, a SHA-512 `match` and
`original_contains_declared_root_manifest` binding. Publisher authentication is
false and no analyst assertion is automatically added. Assessment identity:
`faaa00ef42df8caaad7e6a9e10750571dbbc0b003843b35cec6a8477e94a4dc2`.
Private input and the local PNG/receipt remain outside Git; no sample runs.

The final Qt controller and real-button selection checks have nineteen passes
and one expected backend-omission skip (20.49 seconds). With JavaScript omitted,
the 137-step rebuild passes: nineteen core cases, ten SDK cases, capabilities
and nineteen GUI cases pass. Two JS-dependent SDK cases and the unavailable-only
cases skip explicitly. All six worker/MCP transport cases pass (10.65 seconds).
With the whole Web backend omitted, its eight-step rebuild passes. Both additive
ABI availability cases pass, transport/availability has six passes and two
expected input/backend skips, and GUI has fifteen passes with five dependent
cases skipped. The unavailable-window case runs positively. Logs:
`/tmp/neverd-web-gui-{close-guards-tests,nojs-tests,noweb-tests}.log`,
`/tmp/neverd-review-journal-nojs-{build,core,sdk,availability,transports}.log`
and `/tmp/neverd-review-journal-noweb-{build,availability,transports}.log`.

Both feature flags are restored to ON. The 139-step enabled rebuild passes,
followed by all nineteen focused core cases, twelve SDK cases and seven
transport/availability cases with one expected backend-omission skip
(7.88 seconds). The final GUI suite has nineteen passes and one expected
unavailable-only skip (9.75 seconds), including actual-button Claude selection
and unsaved assertions surviving main-window quit cancellation. Final existing
widgets/smoke/language checks all pass (6.38 seconds). LLVM formatting and
diff whitespace checks pass. Logs:
`/tmp/neverd-review-journal-restored-{configure,build,core,sdk,transports}.log`,
`/tmp/neverd-web-gui-{restored-tests,main-close-test,final-smoke}.log` and
`/tmp/neverd-review-journal-{format,provenance-final}.log`.

Existing GUI widgets, smoke and language suites pass. The broader GUI regression
also reports two font-sensitive unit failures and a function-activation failure
followed by the controller timeout under Qt 6.11.1/offscreen. An independent
161-step GUI build from the exact unchanged parent `f650917bb` reproduces both
unit failures: unavailable Consolas changes the comment grid (3.73438 to
11.2813), and the input font is not fixed pitch. The unchanged-parent controller
also reproduces the exact function-activation mismatch (25.89 seconds) for
`source-primary-following`, with the same actual and expected addresses.
These failures are not counted as successful regression coverage. Baseline
source/build/logs remain in
`/tmp/neverd-js-gui-baseline-f650917` and
`/tmp/neverd-web-gui-baseline-{configure,build,units,controller}.log`.

Source provenance passes across 7,392 worktree files. The capability checker
reports the same sixteen pre-existing unclaimed native source-patch declarations;
the additive Web restore declaration is claimed in both surface lists. A global
capability-checker pass is not claimed.

This increment advances #715's review workflow. GUI advisory, installation,
source and comparison selectors, complete translated wording, native host
accessibility, full dependency resolution and the other #714–#718 acceptance
gaps remain. A metadata finding is not a safety verdict, analyst authentication,
publisher trust, runtime proof or recovery of erased source semantics.

Publication integration preserves the concurrent native CFG work from `dev`
at `ab2341267`. All Web/GUI implementation and test files are byte-identical
after rebase. Its 156-step native/SDK/tool rebuild passes, followed by nineteen
core cases, twelve SDK cases, seven transport/availability passes with one
expected backend-omission skip (10.76 seconds), and nineteen production-worker
GUI passes with one expected unavailable-only skip (8.54 seconds). Logs:
`/tmp/neverd-review-journal-upstream-{build,core,sdk,transports}.log` and
`/tmp/neverd-web-gui-upstream-tests.log`.

## Passive member-fetch syntax and spread boundaries (2026-10-12)

`fetch-websocket-xhr-syntax-v3` retains non-computed member-fetch call and
receiver anchors without assigning browser API identity, wrapper argument roles
or HAR eligibility. Direct fetch, WebSocket and member-fetch calls share an
explicit spread classification: a spread withholds all argument-role nodes,
private endpoint material, default methods and argument-absence claims. XHR's
existing conservative spread rule is unchanged. `SourceFetch` owns these syntax
rules; the session and transports only publish the same fixed metadata. See the
[interface profile](web-interface-profile.md).

The actual NeverD-exported Claude Code 2.1.296 macOS ARM64 module 928 is selected
by SHA-256 `a5e52526ae954aa74deb80cf1bb2372d1f4b77e1886bba69d55e30c51efa2cd2`
and declared module source type. The native CLI, with an unusable external-tool
PATH, retains one `.fetch(...args)` candidate at UTF-8 byte 150,734 for 36 bytes.
Its method and argument positions remain unknown; URL/options/header/body node
roles and private endpoint material remain absent. The old direct-only profile
returned zero candidates. Neither count proves runtime reachability or an
absent interface. No client executes and the original sample stays outside Git.

All twenty focused source cases and nine SDK cases pass, including the exact
Claude module, optional members/calls, computed/member-constructor exclusions,
spread placement, atomic bounds, explicit uncertainty, unchanged positive
direct-fetch joins and private-value canaries. The owning Web regression has
714 passes and one expected missing-ASAR-policy skip across 715 unique cases:
707 ordinary `Web` cases (44.78 seconds) plus all eight parameterized Claude
platform cases (18.43 seconds). Worker/MCP/ABI checks have seven passes and one
expected backend-omission skip (3.70 seconds). Logs:
`/tmp/neverd-interface-v3-{source-boundary,sdk-boundary,web-regression-final,cross-regression,transports-boundary-final}.log`
and `/tmp/neverd-interface-v3-claude-0928-final.jsonl`.

With JavaScript omitted, the 136-step rebuild passes: all five compiled
HAR/CLI SDK cases and seven transport/availability cases pass, with one
expected backend-omission skip. Source-specific SDK cases are omitted at
compile time. With the complete Web backend omitted, the nine-step rebuild
passes; both ABI availability cases run positively, and transport/availability
has six passes with two expected backend/input skips. Both flags are restored
to ON; its 320-step parser/backend/tool rebuild passes, followed by all twenty
source cases, nine SDK cases and seven transport/availability passes with one
expected unavailable-only skip (21.42 seconds). Logs:
`/tmp/neverd-interface-v3-nojs-{configure,build,sdk-final,transports}.log`,
`/tmp/neverd-interface-v3-noweb-{configure,build,transports}.log` and
`/tmp/neverd-interface-v3-restored-{configure,build,core,sdk,transports}.log`.

LLVM formatting, diff whitespace and source provenance across 7,392 worktree
files pass. The capability checker still reports the same sixteen pre-existing
unclaimed native source-patch declarations; no global checker pass is claimed.
No public C ABI entry point, dependency, target execution or network path is
added by this increment.

Wrapper implementation/argument flow, response/transformation tracing, dynamic
URL resolution, source/stream joins and the remaining #714–#718 acceptance gaps
remain. This increment does not close the five epics.

## Independently bounded request source relationships (2026-10-12)

`request-source-relations-v1` consumes selected request evidence and the
authoritative navigation and declaration models. Separate `RelationContext`,
`RequestLinks` and `ResultLinks` components retain function/direct-return,
awaited const-origin, fixed member-use and then-handler syntax. No runtime
response identity, caller target, wrapper argument role or callback parameter
flow is inferred. Relation failure is atomic and leaves requests available.
The profile has independent 4,096-record/2,000,000-step bounds; cached and
fresh navigation have identical identities and work charges. Explicit session,
C ABI, CLI and strict worker/MCP operations all delegate to that owner. See
the [relationship profile](web-interface-relations-profile.md).

The native CLI with an unusable external-tool PATH analyzes the same exact
NeverD-exported Claude Code 2.1.296 macOS ARM64 module 928. It retains two
relationships: the member-fetch call at byte 150,734 for 36 bytes belongs to
one function, and its direct return starts at byte 150,728 for 42 bytes. The
relation identity is
`d9cea58d2be6c7cf4b17f2e1071e31b05bc0978a2b09c04d5c08071e283e5ded`,
with 553,772 charged steps. The request retains its existing v3 identity,
unresolved spread and unknown method. The C++ pinned test independently checks
both exact ranges and their common function. No original client executes.

All twenty-seven focused core cases and twelve SDK cases pass. They include
aliases/shadowing/writes/computed exclusions, optional syntax, foreign/malformed
models, exact/short budgets, canary withholding, pagination and revision
revocation. A 2,050-request source exceeds the relation fanout cap and retains
128 valid request records while publishing zero relations and a fixed budget
reason. Fresh/cached navigation produces identical relation identity and work.
The 112-step SDK/CLI/transport build and the subsequent 33-step owning native,
artifact and source-map rebuild pass. After every owning binary is current,
the complete Web regression has 724 passes and one expected missing-ASAR-policy
skip among 725 unique cases (90.50 seconds), including all eight captured
Claude platforms. All nine native-handoff cases pass. Worker/MCP/ABI checks
have seven passes and one expected backend-omission skip (37.11 seconds).
The Qt production-worker review suite has nineteen passes and one expected
unavailable-only skip (14.39 seconds), including captured Claude selection.
No complete existing GUI regression pass is claimed; its previously reproduced
unchanged-parent font/focus failures remain documented above. Logs:
`/tmp/neverd-interface-relations-{build,owners-build,core-final,sdk,web-regression-final,native-final,transports,gui-build,gui-tests}.log`
and `/tmp/neverd-interface-relations-claude-0928.jsonl`.

With JavaScript omitted, the 137-step rebuild passes. All five compiled HAR/CLI
SDK cases and seven transport/availability cases pass, with one expected
backend-omission skip (21.08 seconds). Both new exported APIs return explicit
unavailability and their capability profile/operations disappear. With the
complete Web backend omitted, the nine-step rebuild passes; both ABI
availability cases run positively and transport/availability has six passes
with two expected backend/input skips (3.19 seconds). Logs:
`/tmp/neverd-interface-relations-nojs-{configure,build,sdk,transports}.log`
and `/tmp/neverd-interface-relations-noweb-{configure,build,transports}.log`.

Both flags are restored to ON. The 139-step parser/backend/tool rebuild passes,
followed by all twenty-seven source cases, twelve SDK cases and seven
transport/availability passes with one expected unavailable-only skip
(11.53 seconds). LLVM formatting and diff whitespace pass. Source provenance
passes across 7,401 worktree files; the capability checker reports the same
sixteen pre-existing unclaimed native source-patch declarations. Both additive
Web relation declarations are claimed in both public surface lists; no global
checker pass is claimed. Logs:
`/tmp/neverd-interface-relations-restored-{configure,build,core,sdk,transports}.log`
and `/tmp/neverd-interface-relations-{format,provenance,capabilities}.log`.

This increment advances #717's source-navigation boundary. General wrapper
argument/dataflow, response transformations, dynamic URLs, source/stream joins,
reviewed endpoint presentations and the remaining five-epic acceptance gaps
remain. No new dependency, external analyzer/runtime, target execution or
network path is introduced.

Integration preserves upstream `dev` at
`a969ad55eb7fbd56a792b589de24baa2dd726a1c`, including its native symbolic
projection and HighC changes. The Web implementation, public SDK, tools, tests
and profile documents remain byte-identical after rebase. Its 761-step shared
native/SDK/tool rebuild passes, followed by all twenty-seven focused source
cases, twelve SDK cases and nine native-handoff cases. Transport/availability
has seven passes and one expected unavailable-only skip (6.95 seconds); the
production-worker Qt review suite again has nineteen passes and one expected
unavailable-only skip (12.41 seconds). Source provenance passes across 7,405
worktree files. Logs:
`/tmp/neverd-interface-relations-upstream-{build,core,sdk,native,transports,gui}.log`
and `/tmp/neverd-interface-relations-provenance-final.log`.

## Measured Wails stored assets and native handoff (2026-10-12)

`wails-2.11.0-go-1.25.7-linux-x64-elf-embedfs-v1` admits one measured ELF64
little-endian `ET_EXEC` layout. Separate C++ owners validate native mappings,
framed build declarations, complete ordered embed tables, private path policy,
content/metadata overlap and every stored compiler digest. The Go toolchain's
1,024/1,025-byte hash boundary is implemented explicitly; its stored 128-bit
value is not ordinary truncated SHA-256. Full plain SHA-256 remains the byte
identity. Declarations and structural compatibility do not authenticate Go,
Wails, a publisher, runtime filesystem selection or an asset-server mount.
Unknown producer versions, replaced framework declarations, PIE, ARM64, PE
and Mach-O remain explicit refusals for this profile. See the
[profile and primary format references](web-wails-profile.md).

Session extraction/pages and explicit local export add three always-declared
C APIs. Completed extractions are revision-bound and independently limited to
four caches / 8 MiB retained private metadata. Pages contain at most 128 records
and omit names and text. Each stored filesystem has its own namespace; another
table cannot satisfy an HTML/module reference. Shared occurrence selection
retains physical or expanded-stream coordinates through nested archives. The
worker and MCP expose only extraction/pages with closed argument schemas.
The raw export operation is a local SDK/CLI disclosure consumer and is excluded
from both transports.

The CLI uses these C APIs under an unusable external-tool PATH. Raw export
creates a new private directory with exclusive fixed filenames, preserves the
complete original container, writes every stored file, rereads all hashes and
only then publishes its manifest. Captured paths are manifest data rather than
host path components. Existing destinations and symlinks are refused. No Go,
Wails, package manager, target application or external analyzer is installed,
linked or launched.

The optional real capture is the bare Linux x64 Zentro v0.3.0 release, 28,832,896
bytes, SHA-256 `ed0932139a8735f403f8c237234d5fb958b4cf5f0a571499475809d330768145`.
Its GitHub release digest matches the captured bytes. NeverD verifies one table
at offset 19,580,128 with fourteen rows: three directories and eleven files,
4,700,610 payload bytes. All compiler digest fields validate. Member 7 preserves
the complete 1,552,742-byte JS bundle at offset 16,727,680, SHA-256
`550cdaae8e65f0cbd2a75132a0dc5073fc8a7883811d0d7abdd6a59f71bd0e1e`.
The raw export rereads thirteen files / 33,542,457 bytes including original and
manifest. Its manifest SHA is
`2d44bfa3c8b89e61310119497d26a1d623b948e0a0426ba850e4a30f4381fb06`.
Diagnostic outputs stay outside Git in
`/tmp/neverd-wails-qualification-zentro-v0.3.0/recovered`.

HTML member 5 contains one root-relative script reference. It remains
`unsupported_local_url` with a null linked artifact; embedded names do not
prove the application's runtime mount root. An initial optional SDK assertion
incorrectly expected that association and failed; the corrected test proves
the explicit unresolved result. Positive relative references are covered by
authored fixtures. Whole-bundle interactive source analysis returns
`source_byte_budget_exceeded` at the existing independent 1 MiB limit. Raw
export does not turn this into parse success. Original native handoff instead
opens the exact captured image and reports 36 sections / 32,475 function
records, with a valid independently owned session after Web-session destruction.
These are loader records, not complete native recovery or bridge semantics.

Eight core and eight SDK cases cover declaration/version refusal, unrelated
Go slice exclusion, native pointer ranges, directory ordering/ancestors, path
collisions, aliases/overlaps, empty/1,024/1,025-byte digest boundaries, exact and
one-short independent budgets, privacy, compressed origins, failed publication,
cache/revision invalidation, local export and native lifetime. The complete
owning Web directory passes 740 of 741 cases with one existing expected
ASAR-policy omission-only skip (166.58 seconds). All eight parameterized Claude
platform cases and supplied producer corpora remain enabled. Its nine native
handoff cases pass. Logs: `/tmp/neverd-wails-web-regression-fixed.log` and
`/tmp/neverd-wails-zentro-{cli-qualified,html,source,native,export}.jsonl`.

The first post-export transport run exposed a missing MCP test exclusion for
the new raw local export. The test now checks the documented exclusion, and
the affected MCP Web case passes (2.35 seconds). Worker, protocol, catalog,
process, availability and captured-Claude transport cases passed in the same
run; one backend-omission-only case skipped as expected. The production-worker
Qt review suite has nineteen passes and one expected unavailable-only skip
(9.62 seconds). No complete GUI-suite pass is claimed. Logs:
`/tmp/neverd-wails-{transports-final,mcp-fixed,gui-tests}.log`.

Parser omission rebuilds 145 owning SDK/tool steps. All six remaining Wails SDK
cases pass, including exact raw export, HTML and actual native handoff (8.81
seconds). Transport/availability has seven passes and one expected
backend-omission-only skip (25.25 seconds). Backend omission rebuilds eight
steps, preserves all three ABI declarations with explicit unavailability, and
has six transport/availability passes plus two expected unavailable-corpus
skips (2.73 seconds). Logs:
`/tmp/neverd-wails-nojs-{configure,build,sdk,transports}.log` and
`/tmp/neverd-wails-noweb-{configure,build,transports}.log`.

LLVM formatting and diff whitespace pass. Source provenance passes across
7,419 worktree files. The initial capability check found two new lexical-order
errors in the additive Web declaration lists; those are corrected. The checker
now reports only the same sixteen pre-existing unclaimed native source-patch
declarations. All three Wails declarations are claimed in both lists; no
global capability-check pass is claimed. Logs:
`/tmp/neverd-wails-{format-final,provenance-final,capabilities-corrected}.log`.

Integration preserves upstream `dev` at
`c3df0112296b75feed71917ef01c51342762a03b`, including its workbench appearance
changes and empty C-render-pass optimization. The Web implementation, public
SDK, CLI/transports, tests and Wails profile remain byte-identical after rebase.
The 149-step shared native/SDK/tool rebuild passes. With Web restored and the
parser still omitted, all nine native-handoff and six Wails SDK cases pass
(8.09 seconds); transport/availability has seven passes and one expected
backend-omission-only skip (15.21 seconds). Logs:
`/tmp/neverd-wails-upstream-{configure,build,native-sdk,transports}.log`.
The next shared readable-export increment restores the parser and qualifies
larger source selections; the raw extraction increment alone does not claim
that recovery.

This increment advances #716's stored-asset and native handoff boundary.
Wails bridges, other producer/container/architecture profiles, Tauri/custom
CEF, larger whole-source analysis and the remaining five-epic acceptance gaps
remain open.

## Shared sequential readable-source export (2026-10-12)

`source-local-readable-export-v1` adds a C++ local disclosure consumer over the
existing immutable `ArtifactView`, recovery parser and exclusive export writer.
It selects an exact revision/artifact and explicit source goal, verifies the
original hash through 64 KiB reads, and preserves original/expanded origins and
selected ranges. Its 32 MiB materialization/output limit is independent of
ordinary 1 MiB / 100,000-node source admission and the 200,000-node cache.
`SourceRecovery` remains the only formatter and same-parser-tree proof owner.
Recovery models never enter interactive caches or semantic consumers. The new
always-declared C API and direct/Wails CLI forms are thin selection layers;
worker/MCP exclude the raw operation. See the
[profile](web-readable-export-profile.md).

Every local export preserves exact `original.bin`. Only a verified same-tree
candidate produces `readable.js`. Invalid syntax/encoding and bounded parser
refusals instead preserve the original with explicit unsuccessful readability
status and no fake source. Unknown/stale/oversized selections and unsupported
goals refuse before writes. Fixed exclusive names and read-back hashes retain
the existing no-overwrite/symlink contract. No new dependency, runtime, formatter,
target execution or remote source path is introduced.

The actual pinned Zentro bundle succeeds under an unusable external-tool PATH:
1,552,742 original bytes / 449,790 syntax nodes become a complete 1,897,987-byte
readable file with a matching reparsed tree. Original SHA-256 is
`550cdaae8e65f0cbd2a75132a0dc5073fc8a7883811d0d7abdd6a59f71bd0e1e`;
readable SHA-256 is
`962afafa77d08adbc53e4f721456773a4bbaa5b16e86c7317bc1c6f1812dd8f7`.
The original physical offset remains 16,727,680. Three files / 3,452,862 bytes
are reread-verified, including manifest SHA
`99d98812f42db48455b10c20bf3db743c98989d0917bc22a409ecd0b90adb9bc`.
The complete result stays outside Git at
`/tmp/neverd-wails-qualification-zentro-v0.3.0/readable`.
The bundle also exceeds ordinary node/cache limits; simply raising a byte limit
would not provide an honest interactive analysis result.

Seven new SDK cases and all eight Wails SDK cases pass together (22.43 seconds),
covering exact input deletion/hash retention, comments/literals/ASI, goal and
encoding refusals, 9 MiB chunked reads, cache isolation, one-over 32 MiB refusal,
stale/unknown IDs, compressed origins, actual pinned output and CLI operation.
The initial test helper had an invalid optional/StringRef comparison; its
explicit value/type comparison fixes compilation. An enabled 174-step owning
build and the two-step pinned-test update pass. Logs:
`/tmp/neverd-readable-{enabled-build,pinned-sdk-build,enabled-focused-sdk}.log`
and `/tmp/neverd-wails-zentro-readable.{jsonl,stderr}`.

Parser omission passes all seven applicable readable/Wails SDK cases (9.14
seconds), plus seven transport/availability cases with one expected
backend-omission-only skip (20.77 seconds). Backend omission preserves the new
ABI and passes six transport/availability cases with two expected unavailable
skips (3.75 seconds). Both options are restored to ON. Logs:
`/tmp/neverd-readable-nojs-{build,build-fixed,build-final,sdk,transports}.log`
and `/tmp/neverd-readable-noweb-{configure,build,transports}.log`.

The complete owning Web directory passes 747 of 748 cases with the one expected
ASAR-policy omission-only skip (185.82 seconds). All supplied producer corpora
and all eight parameterized Claude platform cases remain enabled. The nine
native handoff cases also pass within that run. Transport/availability has
seven passes and one expected backend-omission-only skip (14.88 seconds). The
32-step production-worker Qt rebuild preserves the upstream workbench changes;
its review suite has nineteen passes and one expected unavailable-only skip
(24.56 seconds). No full GUI-suite pass is claimed. Logs:
`/tmp/neverd-readable-enabled-{web-regression,transports}.log` and
`/tmp/neverd-readable-gui-{build,tests}.log`.

LLVM formatting and diff whitespace pass. Provenance passes across 7,425
worktree files. The capability check reports the same sixteen existing
unclaimed native source-patch declarations; the new readable declaration is
claimed in both canonical Web surface lists. No global capability-check pass
is claimed. Logs: `/tmp/neverd-readable-{format,provenance,capabilities}.log`.

Independent future-profile captures now include the bare Windows x64 Zentro
v0.3.0 image, 29,747,136 bytes / SHA
`8c07c39c307c24d04be12019ef39210fc2360c0b40ffee8297deae9d1a0ee9e4`, and
the 26,123,021-byte macOS universal ZIP / SHA
`02a5cad79c66aea2bc5f87bf9999c773bc410d862cb27e478ee0da893e083c9b`.
Both match the release-page digests. NeverD itself inventories the Windows
bytes and opens a pathless native PE/x86_64 session with nine sections and
28,136 function records. It also expands the seven-member macOS ZIP with
verified member CRCs. These are input/container/native qualification results,
not Wails PE/Mach-O asset extraction or complete native semantics. No installer,
application, package manager or target runtime is launched. Capture logs stay
outside Git in `/tmp/neverd-wails-zentro-{windows,macos}-*.{jsonl,log,stderr}`.

This closes the measured bundle's sequential readable-export gap. It does not
claim erased TypeScript/types/names, complete JavaScript semantics, runtime
mounts/bridges or larger interactive dataflow. The five epics remain in progress.

Integration onto `41f5b263b69e202e1739bf63b9694a7ab9f2c0ee` preserves the
verified Web implementation, public entry point, CLI, tests and reader profiles
byte for byte. The upstream native/worker changes require a 252-step shared
rebuild, which passes. All fifteen readable/Wails SDK cases and nine native
handoff cases pass (17.21 seconds). Transport/availability passes seven cases
with the expected backend-omission-only skip (21.90 seconds). A fourteen-step
Qt worker/review rebuild passes; review has nineteen passes and one expected
unavailable-only skip (26.93 seconds). Provenance passes across 7,428 files;
the capability audit retains only the same sixteen existing source-patch
declaration gaps. Logs are `/tmp/neverd-readable-integrated-{build,
sdk-native,transports,gui-build,gui-tests,provenance,capabilities}.log`.

### Independently qualified Wails Windows PE images

The new explicit `wails-2.11.0-go-1.25.7-windows-x64-pe-embedfs-v1` profile
admits captured x64 PE32+ executables declaring Go 1.25.7 / Wails 2.11.0 /
GOOS=windows / GOARCH=amd64. `desktop/wails/Profiles` is the authoritative
layout registry, `PE` owns PE header/RVA/preferred-base translation, and
`Mappings` owns the shared permission, overlap and physical-pointer policy.
The original ELF format owner uses that same policy. Build-info, table/path
admission and compiler digest verification remain single shared owners.
There is no runtime/tool dependency, loader relocation, producer authentication,
implicit asset-server mount or bridge inference. ARM64, PE32, DLL, Mach-O and
other producer versions remain explicit refusals. Limits stay unchanged; the
PE header adds a fixed 96-section bound. See the [profiles](web-wails-profile.md).

Extraction caches bind artifact and profile, so a successful Windows capture
cannot answer a Linux request. Metadata pages, nested selections, raw manifests
and readable origins retain the actual selected profile/platform. Capabilities
keep the original Linux `profile` field and add the authoritative `profiles`
list. The same C APIs, worker/MCP schemas and direct C++ CLI consumers are used;
no separate formatter, byte reader or native loader is introduced.

Actual Zentro v0.3.0 Windows release bytes match SHA
`8c07c39c307c24d04be12019ef39210fc2360c0b40ffee8297deae9d1a0ee9e4` /
29,747,136 bytes. NeverD itself finds one table at 19,725,376, fourteen rows
(three directories / eleven files), and 4,700,647 payload bytes, verifying all
compiler digests and file hashes. Build information is at 28,108,288 / 5,570
bytes. The container has 3,276,250 reader steps, 18,603,080 scanned bytes and
39,147,988 hashed bytes under the existing limits. No release binary is run.

Windows member seven has physical offset 16,867,808, length 1,552,742 and SHA
`1b3a234dc7a96030d32d901baaf456771a0b63fc9c6f2cdfbb046980d618f79f`.
Its 449,790-node parse produces the complete 1,897,987-byte readable file with
matching reparsed structure and SHA
`d8ebf61ff86a156541432442d18cd82f5294dc104384e7887b1b7e9f3f94f013`.
Equal Linux/Windows sizes/node counts do not establish equal bytes; each has
independent hashes and origins. Readable export rereads three files / 3,452,863
bytes with manifest SHA
`3e843ec978b4bf4adb7f8b4ac89100b6274ef7fa32ae0e734a7cddb849caf7d3`.
Raw export rereads thirteen files / 34,456,736 bytes with manifest SHA
`6dce4fac4a7f769ed09af43eee92ca30b6fcb18114b5c8d97afaf6630ff0f504`.
The original native PE handoff reports nine sections / 28,136 function records
and survives Web-session destruction. The HTML's root-relative script stays
unbound without guessing the runtime mount. Logs and outputs remain outside
Git at `/tmp/neverd-wails-zentro-pe-{qualified,readable,export,native}.jsonl`
and `/tmp/neverd-wails-qualification-zentro-v0.3.0/windows-{readable,recovered}`.

Four authored PE core cases independently separate RVA/file offsets and cover
platform/ISA/header refusals, overlap/overflow/alignment, padding, virtual tails,
mutable pointers and corrupted hashes. Two additional SDK cases verify profile
cache isolation, capability lists, raw/readable provenance and all real PE
consumers; a pinned core case verifies every actual stored file. Existing CLI
and compressed-origin cases now exercise both profiles. The first new SDK
helper's optional/StringRef comparison failed compilation and was corrected
with explicit value extraction. The 60-step first build, two-step helper
repair, 18-step owning update, two-step final SDK build and fifteen-step
source/map/ZIP owning rebuild all pass.

Concurrent PRE_TEST CTest discovery initially duplicated generated SDK test
rows. Only disposable generated discovery files were regenerated serially;
the owning directory then has exactly 755 tests. The repeated initial run is
not reported as distinct coverage. The corrected focused run passes all 39
distinct cases: thirteen core, seventeen SDK and nine native (12.53 seconds).
Transport/availability passes seven cases with the expected omission-only skip
(15.67 seconds). Qt review passes nineteen cases with its expected unavailable
skip (20.64 seconds in the Qt receipt). Formatting/diff whitespace pass;
provenance passes across 7,431 files. Capability auditing retains the same
sixteen existing native source-patch declaration gaps. Logs are
`/tmp/neverd-wails-pe-{first-build,sdk-build,sdk-build-fixed,enabled-build,
sdk-final-build,owning-build,discovery,distinct-focused-tests,transports,
gui-tests,format,provenance,capabilities}.log`.

The complete owning Web run passes 754 of 755 distinct cases with only the
expected ASAR-policy omission-only skip (138.61 seconds). All eight actual
Claude platform cases and every supplied producer corpus remain enabled.
No file prefix filter or parser omission hides that coverage. Log:
`/tmp/neverd-wails-pe-full-web-tests.log`.

Parser omission passes the 151-step build, all eighteen applicable SDK/native
cases (2.95 seconds), and seven transport/availability cases with the expected
backend-omission-only skip (4.99 seconds). The real PE assets/raw export/native
handoff remain available; readable export refuses before writes. Backend
omission passes its eight-step build and six transport/availability cases with
two expected unavailable skips (2.93 seconds), retaining the existing ABI.
Both Web and JS options are restored to ON and the 156-step complete owning
rebuild passes. Logs: `/tmp/neverd-wails-pe-nojs-{configure,build,discovery,
sdk-native,transports}.log`, `/tmp/neverd-wails-pe-noweb-{configure,build,
transports}.log` and `/tmp/neverd-wails-pe-restored-{configure,build}.log`.

After restoration, all 39 distinct focused cases pass again (4.07 seconds),
transport/availability retains seven passes and the expected omission-only
skip (5.34 seconds), and Qt review has nineteen passes and its expected
unavailable-only skip (8.37 seconds). Logs:
`/tmp/neverd-wails-pe-restored-{discovery,focused-tests,transports,gui}.log`.
The latest fetched `dev` is the already-published readable-export baseline
`87668457804294d91aff8ca06eede343bf07fc27`; no upstream code is replaced.
The five epics are still in progress. Tauri assets, more general request/config
dataflow and the remaining GUI acceptance surfaces are separate work.

A normal push encountered concurrent `dev` updates. Integration onto
`1cc8cd52535e88559724277317ce011c0d166ed3` preserves all verified Web code,
entry points, tests and Wails profiles byte for byte. The 75-step shared
SDK/CLI/transport rebuild and ten-step Qt app/worker/review rebuild pass.
All 39 distinct focused cases pass again (6.36 seconds); transport/availability
retains seven passes and its expected omission-only skip (7.82 seconds).
Qt review has nineteen passes and its expected unavailable-only skip (13.01
seconds). Provenance passes across 7,445 worktree files; the capability audit
still reports only the sixteen existing source-patch declaration gaps.
Logs: `/tmp/neverd-wails-pe-{rejected-fetch,rebase,integrated-build,
integrated-gui-build,integrated-discovery,integrated-focused,
integrated-transports,integrated-gui,integrated-provenance,
integrated-capabilities}.log`. No force push or upstream replacement is used.

A second concurrent update advances `dev` to
`e8bdeeb69ba2c325415e45dce05ec4cf66cd3e8f`. It changes only the Qt design
skill/reference documentation. Rebase preserves the entire verified production
code and test tree byte for byte; no additional executable test boundary is
changed. Log: `/tmp/neverd-wails-pe-second-rebase.log`.

A third concurrent update advances `dev` to
`cdfdc1e120671c7ca50d64adc14ae96c4bc1a768`, changing native ARM32 call argument
recovery and a shared limits-header spelling. The Wails Web implementation and
tests remain byte-identical. The 571-step shared SDK/CLI/transport rebuild passes;
all 39 distinct focused cases pass (6.82 seconds), transport/availability keeps
seven passes and its expected omission-only skip (8.10 seconds), and the Qt
review suite passes (12.93 seconds). Provenance passes across 7,445 files; the
capability audit retains the same sixteen unrelated source-patch declarations.
The normal push publishes the verified Wails change as
`db000d136185c77fdc7247d5bffae5e9d82ac95e` on `dev`; no force push is used.
Logs: `/tmp/neverd-wails-pe-third-{rebase,build,discovery,focused,transports,
gui,provenance,capabilities}.log` and
`/tmp/neverd-wails-pe-verified-push.log`. This receipt covers that native baseline,
not any later upstream changes. The five epics remain in progress.

## Single-layer lexical request forwarding — 2026-10-12

This increment advances #717 through
`fetch-websocket-xhr-wrappers-syntax-v4` and
`direct-lexical-fetch-forwarding-v1`. `interfaces/SourceWrappers` uses the
authoritative declaration index to admit uniquely declared, unwritten function
declarations or directly initialized const functions/arrows with simple one/two
parameter signatures and exactly one returned external fetch call. The inner
URL/init operands must be exact lexical references to those parameters. Caller
positions can be reordered. The existing fetch-option and private-URL owners
interpret selected actual syntax; no duplicate method/value/parser semantics
are introduced. General alias, multi-layer, async, generator, transformed,
default/rest/destructuring and dynamic-member flow remains unsupported.

The shared SDK/CLI/worker/MCP metadata retains separate caller, function,
internal-request, binding and parameter anchors. Spread or absent arguments
withhold caller positions/role nodes; the wrapper definition remains navigable.
No raw URL, source/name, header or body value is published. These rows remain
static forwarding candidates and cannot join HAR or the separate result-link
grammar. Discovery consumes existing work/record/private budgets; failure clears
both records and definition counts. No runtime, external script or input sample
is executed. See the [interface profile](web-interface-profile.md).

Eight authored C++ core cases cover reordered anchors/stable IDs, shared fetch
rules, missing/spread arguments, unsupported grammar, writes/shadow/duplicates,
lexical separation, unchanged HAR eligibility and atomic record exhaustion.
The SDK case adds canaries, all public links, cache equality and revision
revocation. Worker/MCP cases compare the same backend contract. The first new
test translation unit had missing closing parentheses; those test-only syntax
errors were repaired. The first transport run retained v3/candidate-count and
old relation-page-index assertions; all affected assertions were updated for
the new fixture and shared v4 profile. No unrelated product semantics changed.

An additional optional C++ regression captures two exact NeverD-exported Claude
Code 2.1.296 macOS ARM64 units. Module 1905 is 54,991 bytes with SHA
`157c010c3056606ebf89818348df35a547fde95dde1b130d2e96e26e80c5e34d`;
module 1920 is 198,048 bytes with SHA
`0a1c2aa1424babd95eeec371f67c875b6382db0c891b3b3e90f710f2102b16df`.
NeverD preserves their four direct-fetch anchors at 2,693/47, 10,795/29,
11,101/36 and 79,329/53. All URLs/options/methods remain dynamic/unresolved;
zero wrapper definitions are admitted. This is a real refusal qualification,
not evidence that these complex requests were reconstructed. Manual CLI runs
use `PATH=/neverd-no-external-tools`. Six of eight selected units yield bounded
partial syntax reports; module 299 explicitly refuses its parser budget and
module 415 exceeds the interactive source byte limit. Logs:
`/tmp/neverd-wrapper-claude-m{00178,00299,00415,01892,01905,01908,01909,01920}.{jsonl,stderr}`.

Final serial target builds, the eight-step owning update and six-step transport
assertion repair pass. The first focused run passes all 48 then-selected cases
(6.57 seconds). After adding the exact real-module regression, the complete
owning Web suite passes 764 of 765 distinct cases with only its expected
ASAR-policy omission-only skip (86.97 seconds). Every supplied producer corpus
and all eight actual Claude platform cases remain enabled. Transport/availability
passes seven cases with its expected omission-only skip (6.69 seconds); Qt review
passes (14.26 seconds). Formatting passes; provenance initially passes across
7,447 files and capability checking retains the sixteen existing unrelated
source-patch declaration gaps. Logs:
`/tmp/neverd-wrapper-{first-build,fixed-build,final-serial-build,owning-build,
focused-tests,transport-fix-build,final-discovery,full-web-tests,
fixed-transports,gui,final-format,provenance,capabilities}.log`.

The [acceptance audit](web-epic-acceptance.md) now records per-issue remaining
requirements and per-platform pass/partial/not-attempted cross-track selections.
The five-issue objective remains incomplete, including the required Tauri asset
layout/native-handoff criterion and broader configuration/request flow.

JS omission passes its 151-step build, all 23 selected SDK/native cases
(6.70 seconds), and seven transport/availability cases with the expected
backend-omission-only skip (13.33 seconds). Backend omission uses the actual
`NEVERD_ENABLE_WEB_ANALYSIS=OFF` option: the eight-step build passes and six
transport/availability cases pass with two expected unavailable skips
(3.39 seconds). An initial unused `NEVERD_ENABLE_WEB` cache spelling was
removed; its no-op build is not counted as omission evidence. Logs:
`/tmp/neverd-wrapper-nojs-{configure,build,discovery,sdk-native,transports}.log`,
`/tmp/neverd-wrapper-noweb-{correct-configure,correct-build,transports}.log`.

Integration onto `e1841bc8b` preserves all verified Web production code and
tests byte for byte. Both actual Web/JS options are restored to ON and the
187-step complete owning/shared rebuild passes, including the updated native
C backend. All 88 distinct selected source/wrapper/interface/Wails/readable/
native cases pass (7.29 seconds); transport/availability keeps seven passes and
its expected omission-only skip (7.33 seconds). The Qt app/worker/review rebuild
and review test pass (11.90 seconds). Provenance passes across 7,453 files;
capability checking retains the same sixteen unrelated declarations.
Logs: `/tmp/neverd-wrapper-{rebase,restored-configure,restored-build,
restored-discovery,restored-focused,restored-transports,
integrated-gui-build,integrated-gui-tests,integrated-provenance,
integrated-capabilities}.log`. This is integrated regression evidence for that
native/GUI baseline, not a claim that the complete GUI or native test corpus ran.

A subsequent normal integration onto `a45105012` preserves the verified Web
code, entry points and tests byte for byte while retaining upstream native
parameter-printing changes. Its 77-step shared rebuild passes. All 88 selected
cases pass again (6.70 seconds); transport/availability has seven passes and
the expected omission-only skip (6.02 seconds), and Qt review passes
(9.66 seconds). Formatting/whitespace pass and both real Web/JS options remain
ON. Logs: `/tmp/neverd-wrapper-{second-rebase,second-integrated-build,
second-discovery,second-focused,second-transports,second-gui,publish-format}.log`.
The Tauri boundary in the acceptance audit now links pinned upstream encoding
and Rust-layout evidence; no Tauri binary profile or extraction success is
invented from that source review.

## Native Tauri PHF/Brotli qualification and old GNU tar — 2026-10-12

This increment closes the previously missing first Tauri asset-layout/native
handoff boundary with actual input bytes. The [Tauri profile](web-tauri-profile.md)
pins lencx/ChatGPT v1.1.0's original macOS x64 archive, expanded stream, selected
native member, initializer instructions and PHF tables. It qualifies a storage
grammar; exact compiled Tauri/Rust versions, type identity, initializer
reachability, runtime mutation/mounts, bridges and signatures are not verified.
No target/source bytes are committed or redistributed.

The real release first refused as an unsupported tar dialect. Uniform
`gnu-oldtar-basic-single-gzip-v1` now owns old GNU basic magic/timestamps and
refuses every sparse/multivolume extension, mixed headers and local PAX. Existing
checksum, portable paths, links-as-metadata, padding, complete gzip and budgets
remain authoritative. Capability `tar_profiles` lists both supported dialects;
profile selection participates in archive/member IDs. Native package/source/
Bun consumers and exclusive CLI exports also exercise authored GNU tar/tgz
fixtures with an unusable external-tool PATH.

The native program constructs its PHF header in instructions instead of storing
a complete header in data. `pipeline/evidence/ConstantStores` projects selected
full-width consecutive stores through the authoritative decoder/relocation
roles/LowIR. No web component decodes instructions or guesses pointer semantics.
`capi/TauriInitializer` joins the hash-bound native image and loader mappings;
`desktop/tauri` independently validates every PHF key/index, range/path and
explicit Brotli frame. SDK inputs cannot provide a descriptor. Chained fixups
remain unsupported under this first profile. Core readers stay independent of
sessions and adapters; all authored implementation/fixtures use LLVM-style C++
with project/file banners.

The optional pinned static Brotli 1.2.0 decoder/common SDK is called in process.
The C++ wrapper accounts allocations and bounds each input/output call, total
output/calls and a shared deadline. Missing codec/path support is unavailable,
never an external fallback. A separately compiled omission target checks the
owning codec and capabilities. Original Brotli, Rust Project, Frank Denis and
Steven Fackler notices/licenses/source revisions are preserved. The source
provenance checker has one narrowly reviewed allowance for the explicit PHF
adaptation notice, linked to those retained licenses.

The actual selected constructor is eight instructions/five stores; all fifteen
PHF rows validate. NeverD decodes **946,884 stored bytes into 2,700,272 bytes**.
Raw export preserves the original native image, fifteen frames and fifteen
decoded files plus a manifest, with reread hashes (32 files). All nine JS
members pass independent same-parser-tree readable export. A retained 910,730
byte JS member has 74,127 syntax nodes and a 993,874-byte readable copy with
SHA-256 `70f96cd6755e0e61064af380a565c789ddf00204773ad3304650118e53be7071`.
Deleted names/types/TypeScript and whole-program equivalence are not recovered.
`ArtifactView` keeps decoded-frame coordinates, exact compressed ranges and
nested archive origins; the original native member enters the shared native
handoff. Revision-bound SDK/CLI/worker/MCP selection uses the same owners and
metadata privacy; raw export remains local and absent from MCP.

Native five-field authored/actual regressions pass all five cases. The owning
core/GNU focused run passes six cases; GNU consumer/CLI plus Tauri SDK passes
fourteen cases before the additional chained-fixup refusal. Direct/framed
worker, MCP catalog/web/process pass four cases (6.35 seconds). The full owning
Web directory passes **747 of 775 cases**, with 28 explicit skips (44.63 seconds).
The retained eight Claude platform captures, official linux-x64 standalone/npm
and new Tauri capture are enabled. Old temporary producer captures, npm wrapper/
lock, advisory/attestation/diff, VSIX and Wails corpora are no longer present;
their 27 optional cases were not requalified. The remaining skip is the expected
ASAR-policy omission-only case. An initial full run used an unavailable native
path and a preexisting empty npm capture, was interrupted, and is not evidence;
the successful run selects the existing pinned standalone and platform archive.
No missing corpus was replaced by a fabricated input.

The CMake dependency repair clears only two stale temporary FetchContent
source overrides and restores the same repository-pinned Ada/Hermes revisions
and checksums. The rebuilt 287-step Web/CLI target succeeds. Initial C++ include/
error conversion failures and inert fixture PHF/function-entry mistakes are
fixed without weakening admission; inert Mach-O uses LC_FUNCTION_STARTS rather
than a guessed prologue. Subsequent core/consumer/parity builds pass. Logs:
`/tmp/neverd-{gnu-tar-fixed-build,tauri-{core-tests-build,consumer-fixed-build,
parity-build,gnu-omission-build,boundaries-build,native-final-tests,
gnu-consumer-tests,boundaries-core-tests,boundaries-sdk-tests,transports-tests,
no-codec-tests,full-web-fixed-env-tests}}.log`.

Parser omission passes its 156-step build, twelve selected SDK cases with one
expected parser-dependent skip, and all four transports (6.09 seconds). The
additional native/chained-fixup guard uses a five-step build and all four Tauri
SDK cases pass with the real archive enabled (0.69 seconds). Backend omission
passes its seven-step build and transport capability/refusal checks. Logs:
`/tmp/neverd-tauri-{nojs-{configure,build,sdk-tests,transports-tests,
guard-build,guard-tests},noweb-{configure,build,tests}}.log`.
The Web/JS options are restored before integration. Source provenance passes
across 7,481 worktree files. New Tauri C/JSON declarations are accounted for in
the shared capability exclusions; the checker retains the sixteen existing
unrelated source-patch declaration gaps on this pre-integration baseline.
The [five-epic audit](web-epic-acceptance.md) records the new bounded criterion
and remaining cross-track work; it does not declare the entire objective done.

Integration merges `origin/dev` at `7aabfa363` normally; every new Web reader,
SDK/native join and test remains byte-identical to the verified increment.
The 302-step shared rebuild passes, followed by **748 passes / 28 skips out of
776 distinct owning Web cases**. Five native-projection cases (including actual
input), native buffer boundary and four transports pass (9.72 seconds). Qt
app/worker/review builds and the selected review test pass (3.03 seconds).
All changed C++ files pass clang-format and whitespace checks; source provenance
passes across 7,485 files. Capability audit retains the same sixteen unrelated
source-patch gaps. Both Web/JS options are ON; submodule revisions and the
original checkout are unchanged.

An initial integration run regenerated GoogleTest inventory concurrently from
two CTest roots, causing duplicate discovery entries. Those 1,041 executions
are not counted as distinct coverage. Regenerating inventory once restores
exactly 265 source and 210 SDK cases; the clean owning run above is authoritative.
The initial integration build also requested a nonexistent `NeverDLoaderTests`
target; the correct existing `NeverDNativeBufferTests` target is built and run.
Logs: `/tmp/neverd-tauri-{dev-integration,integrated-fixed-build,clean-discovery,
integrated-clean-web-tests,integrated-native-transports-tests,
integrated-gui-{build,tests},published-format,integrated-provenance,
integrated-capabilities}.log`.

Two further normal integration merges retain the qualified Web changes while
including `origin/dev` at `c01e257c9` and `0b783ff55`. The second shared rebuild
passes 855 steps, followed by five native-projection cases, 53 selected Web/SDK
cases and all four transports (2.97 seconds). The upstream capability-declaration
repair resolves the earlier unrelated exclusions; the manifest now validates
all 23 capabilities. The third six-step rebuild, five native cases and four
Tauri SDK cases with the real archive enabled pass. Commit
`84af5be4ad75e5859ce229dd7c0e197e525439a4` is normally pushed to `dev` and its exact
remote head is checked. No force push or issue-state mutation is performed.
Logs: `/tmp/neverd-tauri-{second-{integrated-build,native-tests,sdk-tests,
transports-tests,capabilities},third-{integrated-build,native-tests,sdk-tests}}.log`.

### Direct desktop bridge syntax and preserved-source anchors

On the published `84af5be4a` baseline, the independent
`tauri-1-wails-2-direct-bridge-syntax-v1` owner adds redacted direct Tauri 1 IPC/
global invoke and Wails 2 target syntax to existing navigation pages. The
source/bindings/navigation owners retain authority for parsing, lexical roots
and generic call identities. Bridge publication does not authenticate APIs,
resolve native targets, infer runtime activation or publish command/argument
values. Record/work refusal clears only bridge candidates. Ordinary constructors,
tags and dynamic imports remain valid unrelated navigation entries; changed
callee/child agreement or call-kind models are refused explicitly.

The real decoded Tauri member 6 (85,197 bytes, hash pinned in the
[bridge profile](web-desktop-bridge-profile.md)) has one direct IPC call at
decoded byte offset 3,013/length 53. Its spread message remains unresolved.
The SDK source anchor links that span to native-container Brotli offset
5,069,463/length 24,569 and the retained old-GNU archive parent. All nine
readable Tauri JS exports still reparse with matching structure. A real CLI
`web navigate` run over this same preserved decoded member succeeds with an
unusable external-tool PATH and empty stderr; its metadata receipt is
`/tmp/neverd-desktop-bridge-actual-navigation.jsonl`.

The 62-step shared/CLI/transport build passes, five core bridge cases pass
(including actual preserved JS), six selected SDK cases pass, and all four
worker/MCP transport cases pass (4.78 seconds). The complete owning Web run is
**755 passes / 28 explicit skips out of 783 distinct cases** (73.58 seconds),
with all eight Claude platform captures and the preserved Tauri corpus enabled.
The same absent optional corpora and ASAR omission-only boundary account for
the skips; missing samples are not replaced. Parser omission passes its
155-step build, five selected SDK cases with real Tauri enabled, and all four
transports (14.60 seconds). The restored 161-step build and all fifteen focused
core/SDK/transport cases pass (11.70 seconds), with both Web/JS options ON.
Changed C++ passes LLVM formatting and whitespace checks; provenance scans
7,504 worktree files and the capability manifest validates all 23 capabilities.
Logs: `/tmp/neverd-desktop-bridge-{parity-build,final-{source,sdk}-tests,
transports-tests,owning-web-tests,nojs-{configure,build,sdk-tests,
transports-tests},restored-{configure,build,focused-tests},format,
provenance,capabilities}.log`.

### Issue-body re-audit correction — 2026-10-13

The user requested another comparison with the original five GitHub issues.
All remain open with zero comments; their bodies retain the October 9 scope.
The blanket completion conclusion above is superseded by the updated
[acceptance audit](web-epic-acceptance.md#historical-re-audit-findings-and-resolution).
The named implementations and earlier verification receipts remain valid.

Production CLI probes expose two #717 coverage gaps: unresolved URL templates
and fetch header/body nodes lack a component/parameter inventory, and observed
JSON type histograms cannot retain field types/nesting/presence. Two original
bodies with different field structure produce identical shape summaries while
retaining distinct observation IDs. Both checks used an unusable external-tool
PATH, no target execution and zero stderr. Focused source, package/runtime and
report/desktop checks passed 163 distinct cases, including a separate successful
preserved-Claude feature-corpus run (25.73 s). These tests qualify the current
limited contracts rather than proving the missing structures are implemented.

This correction changes documentation only. It records #717 work still required
and distinguishes recommended real CEF/WebView, minified-client navigation and
mandatory producer-corpus qualification from unsupported universal recovery.
It does not modify GitHub issue state or add new implementation claims.

Normal integration with `origin/dev` at `be72c8ee4` preserves every Web bridge
reader and test unchanged. The 64-step shared rebuild and integrated native
targets pass. The complete owning Web run remains **755 passes / 28 skips out
of 783 distinct cases** (90.24 seconds). Five native-projection cases, the
immutable native-buffer boundary and four transports all pass (4.05 seconds).
The correct `neverd-gui-web-tests` owner and Qt app rebuild successfully; the
offline package review test passes (4.27 seconds). The earlier GUI build request
used a nonexistent target name and is not evidence. Provenance again scans
7,504 files, capabilities validate all 23 declarations, and both Web/JS options
remain ON. Integration head is `6fd1505fa4e082a742ab35e1e1f98916f7ba2f81` before
this receipt-only documentation commit. Logs:
`/tmp/neverd-desktop-bridge-integrated-{build,native-build,owning-web-tests,
native-transports-tests,gui-{build,tests},provenance,capabilities}.log`.

### Configuration object order and lexical loading-source links

On the published `a4c6144f6` baseline, the independent
`javascript-configuration-object-order-v1` owner adds `config_merges` pages to
existing navigation. It consumes shared source, bindings, navigation and client
feature identities; shared declaration indexing owns unique binding admission.
Ordered property/spread terms keep exact spans and lexical initializer/loading
syntax links. Later unknown-key terms retain possible overwrite barriers;
runtime values, loader authentication and separate file-layer policy remain
unknown. Multiple declarations, including function/variable combinations,
do not gain a unique initializer. Object/term/work failures clear only this
owner's complete candidate list. All authored code and fixtures use C++ with
LLVM formatting and project/file banners; no external target tool is invoked.

The C++ archive/Bun/source corpus visits all 2,364 JS units and 45,035,899 bytes
of the pinned Claude Code 2.1.296 macOS ARM64 package. The interactive frontend
admits 2,354 and retains its ten explicit byte/work/node refusals. Order
analysis finishes for every admitted feature inventory: 273 candidate objects,
2,279 terms and 19 observed fields with later unknown terms. Named loading
links are zero; no actual internal loader identity or policy is invented.
Module 1676 has one object/four terms. Module 6 independently retains an
unknown later term at object offset 2,451/length 96. Their source hashes and
original release/channel/platform chain are pinned in the
[configuration profile](web-configuration-order-profile.md). A real offline
`bun-navigate` run selects module 6 directly from the retained original
executable, with an unusable external-tool PATH and empty stderr. The original
source is not redistributed or uploaded. Receipt:
`/tmp/neverd-configuration-order-actual-bun-navigation.jsonl`.

The first 51-step build passes all five initial core cases. The fourteen-step
SDK/transport build passes six core/corpus cases (48.748 seconds), seven selected
SDK cases and all four transports (5.31 seconds). After profile metadata and
identity admission are finalized, the 58-step boundary rebuild and complete
owning Web run pass **762 cases / 28 explicit skips out of 790 distinct cases**
(85.30 seconds), with all eight retained Claude platform captures and both
real desktop/Claude source selections enabled. The unchanged absent optional
corpora and ASAR-policy omission-only case account for the skips.
The final declaration-ambiguity guard passes its eleven-step rebuild and all
thirteen affected core/SDK/transport cases (4.40 seconds). Qt package review
passes (2.51 seconds). LLVM formatting and whitespace checks pass; provenance
scans 7,510 files and the native capability manifest validates 23 declarations.
Parser omission passes its 156-step build, all six selected SDK cases with the
real Tauri archive enabled, and all four transports (2.60 seconds). Logs:
`/tmp/neverd-configuration-order-{first-build,first-tests,parity-build,
source-corpus-tests,sdk-tests,transports-tests,final-build,owning-build,
boundary-build,owning-web-tests,declarations-{build,tests},gui-tests,
nojs-{configure,build,sdk-tests,transports-tests},format,provenance,capabilities}.log`.

Restoration completes all 160 build steps, followed by a two-step banner-only
test rebuild. A mistakenly early CTest run used a parser-off SDK binary during
parser-on relinking (its 744-case inventory and capability expectations were
stale); it is interrupted, preserved and **not counted as acceptance evidence**.
After completed linking, one explicit discovery produces exactly 790 cases.
The authoritative restored run passes **762 / 28 skips out of 790 distinct
cases** (118.84 seconds), including the final mixed-declaration refusal and
both pinned actual configuration units. The final original-Bun CLI receipt
again succeeds with empty stderr. Format, provenance (7,510 files) and capability
(23 declarations) checks pass. Both Web/JS options are ON. Logs:
`/tmp/neverd-configuration-order-{restored-{configure,build,clean-discovery,
ready-owning-web-tests},banner-build,final-{format,provenance,capabilities}}.log`;
actual receipt: `/tmp/neverd-configuration-order-final-actual-bun-navigation.jsonl`.

Normal integration with `origin/dev` at `eb295fdbc` preserves every Web source,
SDK join and owning test unchanged. The shared native/SDK/transport rebuild
passes all 102 steps; Qt app/worker/review builds pass. All 28 selected source,
SDK, actual Tauri/configuration, native-projection/buffer and transport cases
pass (25.28 seconds); Qt package review passes (12.59 seconds). Provenance scans
7,512 files and capabilities validate all 23 declarations. Both Web/JS options
remain ON. Integration head is `779893528b51c3ccb7d57e60f04bf8763c01829a`
before this receipt-only commit. Logs:
`/tmp/neverd-configuration-order-{dev-integration,integrated-{build,
focused-tests,gui-{build,tests},provenance,capabilities}}.log`.

The first normal push is rejected because `dev` advanced. A second normal
merge includes `5e08ef0eb` (two native HighIR-lowering owners and their tests),
with the Web/SDK/fixture surfaces still unchanged. The twelve-step rebuild
passes, followed by all nineteen affected core/SDK/native/transport cases
(5.65 seconds). Integration head is
`2537571a18225047ec1a484e9b7048575ad77ec9` before this receipt-only commit.
Logs: `/tmp/neverd-configuration-order-{second-integration,
second-integrated-build,second-focused-tests}.log`.

## Five-issue bounded acceptance completion — 2026-10-12

Base: `7e2309f7c249d69802cd4f13eec620edffd0bd68`. All authored implementation and
fixtures remain C++; no target or external analysis runtime/tool executes.

`request-source-relations-v2` adds independently bounded nearby JSON/base64/URI,
crypto.subtle and zlib call-spelling containment. Deferred body functions,
computed names and aliases do not gain value-flow or API-authentication claims.
`passive-evidence-shape-report-v1` composes selected source/committed HAR/stream
IDs, existing HTTP joins and a shared MCP method vocabulary. Recorded response
links stay owned by streams. Examples are observation IDs; JSON type summaries
withhold keys/values, and optionality/server constraints remain unknown.
Two additive report C APIs, CLI forms and closed worker/MCP operations share
this owner. Aggregate body work and report/cache/page bounds fail atomically.

`cef-webview-local-resource-descriptor-v1` supplies an explicit named local
resource index through existing namespace/path ownership. Directory and ZIP
fixtures preserve exact hashes/origins, missing/opaque resources and independent
CEF/WebView selection IDs. JS/HTML/native consumers and verified raw local export
are qualified separately. The additive local export C API is excluded from
worker/MCP; supplied declarations authenticate no framework or native version.

Verification on macOS ARM64 Release/prebuilt LLVM 23.0.0 r4:

- Enabled build completed. Focused checks passed 13 artifact cases, 10 source
  cases (one absent optional source initially skipped), and six SDK/CLI cases.
- Initial owning Web CTest: 805 distinct cases, 777 passed, 28 explicit skips,
  27.73 s. Restored enabled build: the same 805/777/28, 63.55 s. Real supplied
  Claude/Tauri inputs were selected; absent optional producer/npm/advisory/
  Wails/VSIX captures remain skips, not passed evidence.
- Native constant-store/buffer owners: six passed, including the pinned actual
  Tauri initializer. Worker and three MCP gates passed. An initial MCP discovery
  failure correctly found that the newly advertised local export needed the
  test's explicit exclusion; adding that exclusion restored the required closed
  surface. No raw MCP export was enabled.
- Qt `NeverDGuiWebReview` passed with the preserved Claude corpus, 6.30 s.
- Parser omission: 13 artifact, six SDK/CLI and four transport gates passed.
  Observed reports/resources remain available; selected source explicitly refuses.
- Whole-backend omission: three MCP gates passed, worker's unavailable-backend
  gate skipped its positive integration; a C++ probe verified all three new ABI
  functions return `capability_unavailable`. Both flags were restored to ON.
- Formatting, source provenance (7,528 files) and canonical capability ownership
  (23 capabilities) passed. The three new declarations were added to both C/JSON
  ownership lists in lexical order.
- Real Claude 2.1.296 macOS ARM64 module 928, SHA-256
  `a5e52526ae954aa74deb80cf1bb2372d1f4b77e1886bba69d55e30c51efa2cd2`,
  passed `report-source` with an unusable external-tool PATH and zero stderr.
  Its three static records retain the interface/function/return anchors.
  No real observation or transcript was supplied or manufactured.

Local diagnostic receipts use `/tmp/neverd-epic-completion-*`; no raw samples,
recovered source, build output or local screenshots are committed. The final
criterion mapping is [the acceptance audit](web-epic-acceptance.md), with the
[report](web-passive-report-profile.md) and
[resource](web-desktop-resource-profile.md) contracts. Historical incomplete
increment receipts remain historical; new runtime profiles, general dataflow,
erased TypeScript/names, opaque bytecode decoding and other host qualification
are explicit extensions/unsupported boundaries rather than universal promises.

### Final dev integration — 2026-10-13

The implementation commit is `e7ff8f2e9`. Normal integration first merged
`5e8593cb8` as `0190e5c8d`; its 646-target build completed, owning Web retained
805 distinct cases / 777 passed / 28 explicit skips (157.29 s), all ten selected
native/worker/MCP checks passed (4.48 s), and Qt review passed (34.12 s).
The actual Claude source report again completed with zero stderr and an unusable
external-tool PATH.

A later normal integration merged `a4f50d9cc` as `599393002`. Its changes concern
native x87/rendering/loader, PDB and driver entry work; Web algorithms, new source
profiles and Web fixtures were unchanged. The 160-target engine/SDK build and
GUI build completed. Affected source/report/resource/Tauri/native/worker/MCP
coverage passed 28 of 29 checks, with one absent optional full Bun compiler input
explicitly skipped (21.41 s). Qt review passed again (18.99 s). Source provenance
passed for 7,542 files and canonical ownership remained valid for 23 capabilities.
Web, JS and ASAR are ON. No submodule revision, original checkout, diagnostic
artifact or proprietary source was changed by this integration.

The criterion audit now covers the original #714–#718 acceptance items and
named supplement profiles. Future builds/hosts and unknown runtime behavior
remain unsupported or partial as documented; no GitHub issue was closed or
commented on by this task. Publication uses an ordinary fast-forward-compatible
push to `dev`, with no history rewrite.

The push encountered a further upstream advance to `fa7819708`. Normal merge
`4de9a2b05` retains the Web implementation while integrating native source patch,
loader data-in-code, SDK navigation/export-name and worker-listing changes.
The 466-step engine/SDK/test build and three-step Qt worker build completed.
The affected integration selection passed 86 of 88 checks (6.42 s), including
the preserved Tauri initializer/archive, resource exports, passive reports,
SDK navigation and worker/MCP. The absent optional full Bun compiler fixture
and Claude VSIX fixture are explicit skips. Qt review with the preserved Claude
package passed (6.09 s). Source provenance passed for 7,546 files and canonical
ownership remained valid for 23 capabilities. Web, JS and ASAR remain ON.
Receipts: `/tmp/neverd-epic-completion-final-{integrated-build,focused,
gui-build,gui-tests,provenance,capabilities}.log`. This integration does not
broaden the supported runtime profiles or change the acceptance boundaries.

A concurrent advance to `170e3ad4c` required normal merge `c913e314b` before
publication. Its native ABI, HighC and ELF patch changes completed a 228-step
engine/SDK build. The affected native/resource/report/CLI/worker/MCP selection
passed 61 of 63 checks (4.57 s); the same two optional inputs remain absent.
Qt Claude-package review passed (6.24 s), source provenance passed for 7,548
files, and all 23 capability declarations remained valid. Receipts use
`/tmp/neverd-epic-completion-publish-{integration,build,tests,gui-tests,
provenance,capabilities}.log`.

## Passive component and field evidence follow-up — 2026-10-13

Starting from published `a81518354`, the v2 passive report implements both
confirmed #717 audit gaps. All implementation and authored fixtures remain C++,
with LLVM formatting and NeverD file banners. No input execution, network
analysis, external runtime, new dependency or public C ABI entry point is added.

`SourceStructure` consumes admitted request roles and existing transform links;
`URLStructure` owns lexical component projection. They preserve original
template/expression anchors, cooked UTF-16 component positions, ordinary header
properties, body object/array/literal syntax and directly selected
JSON-stringify argument structure. Dynamic calls/variables, getters, spreads,
computed/duplicate properties and deferred functions stay explicit unknowns.
Interpolation cannot widen the existing HTTP candidate join.

`Shapes` retains body admission. `ObservedStructure` owns private field
dictionaries, per-example trees and capture-local aggregate types/presence.
Array examples keep actual positions; aggregate elements share a path. Object
presence counts only eligible observed object parents; missing/null, other
parent types and malformed/absent bodies stay distinct. Public field IDs encode
capture/profile/ordinals rather than private names. A 2 MiB private-dictionary
budget complements the existing body, node, record and metadata bounds.
`Compose`/`Publication` carry the same evidence through SDK/CLI/worker/MCP.
Server optionality, completeness and source execution remain unknown.

Verification receipts under `/tmp/neverd-passive-structure-*`:

- The full integrated engine/SDK/transport build completed. Initial focused
  checks passed 17 cases; route/SDK/worker/MCP coverage passed 46 cases (3.69 s).
- Owning Web regression passed **749 / 800**, with 51 explicit optional-input
  or feature skips (3.03 s). Skips are not qualified corpus successes.
- Parser OFF: rebuild succeeded, 28 report/SDK/transport checks passed (4.11 s).
  The original two-body HAR CLI report was byte-identical to parser ON.
- Whole Web backend OFF: rebuild succeeded; two availability and three MCP
  checks passed; worker's positive backend case explicitly skipped (2.13 s).
- Web/JS restored ON: rebuild succeeded and 48 source/report/SDK/transport and
  supplied Claude corpus checks passed (15.36 s). The full Claude source-feature
  corpus check took 13.00 s. ASAR remains ON.
- Qt web review passed (2.91 s). Source provenance and the 23-capability
  ownership checks passed.
- A later C++ real-template regression initially missed its private capture
  header; the compile failed and an early run of the old test binary is not
  counted. After adding the header and a successful rebuild, all eight request
  structure cases passed (1.08 s), including the pinned real templates.

The production CLI replays the original audit fixtures with
`PATH=/neverd-no-external-tools`: the request gives 23 static rows, including
21 structural rows, and the HAR gives 19 observed rows, including eight value
nodes and five aggregate nodes. Both old equal-histogram bodies now retain their
different nesting. Every selected command returned zero stderr; no observations
were invented.

Real Claude module 928 keeps its three existing static records. Twenty additional
fetch-spelling candidate modules were inspected through NeverD's CLI: sixteen
reports completed, module 141/299 hit the existing parser-work bound and module
415/1452 hit the existing source-byte bound. All diagnostic output stayed
redacted with empty stderr. In the successful selections, modules 1357 and 1740
provide ten/eight structure rows, respectively; each has three template segments
and two exact expression anchors. The regression and
[profile](web-passive-report-profile.md) pin their original hashes and lengths.
Unresolved header/body variables remain unknown. This is selected real-client
evidence, not full dynamic request reconstruction.

The [acceptance audit](web-epic-acceptance.md) marks the two confirmed #717 gaps
addressed. It retains the separate recommendations for real CEF/WebView
qualification, broader minified-client patterns and a mandatory producer-corpus
release lane. No issue state/comment or unrelated checkout is modified.

### Integrated v2 verification

Implementation `cd1458ed9` was normally merged with remote `01a640f7d` as
`dd0d8cc44`. The incoming changes concern native ABI and Qt code folding;
Web owners and fixtures are unchanged. The 14-step engine/SDK build and Qt web
test rebuild succeeded.

The broader availability check exposed a stale earlier test expectation for
`request-source-relations-v1` although the authoritative implementation is
already v2. The assertion is corrected, and passive-report v2 operations,
privacy limits and no-execution/network capabilities are explicitly checked.
After its successful rebuild, the complete owning Web/availability run with
the supplied cross-platform Claude corpus passed **764 / 804**, with **40
explicit skips**, in 20.99 seconds. All eight captured platform packages and
both new template witnesses passed; the source-feature corpus passed in
20.18 seconds. No production semantics were relaxed to fix the assertion.

All four final worker/MCP checks passed. Qt web review passed (5.50 s).
Post-merge HAR and real-template CLI reports, with an unusable external-tool
PATH, are byte-identical to the earlier reports and have zero stderr.
Provenance scans 7,565 files, capability ownership validates 23 declarations,
and new C++ files pass the LLVM formatter check. Web/JS/ASAR remain ON.
Final receipts use `/tmp/neverd-passive-structure-integrated-*`, with the
successful complete rerun in `integrated-web-tests-fixed.log`.

## Explicit large-source analysis (2026-10-13)

The additive `hermes-602befee-large-source-js-v1` profile addresses the four
real Claude request-source selections refused by interactive byte/parser
admission. `SourceLimits` owns input, AST, lexeme, decoded-string, model-storage,
parser-allocation and work bounds. Parsing, complete lexical binding, navigation,
features, configuration order and passive interfaces consume that profile
without partitioning source syntax. Interactive admission stays at 1 MiB and
100,000 nodes; readable recovery remains a separate non-semantic model.

Explicit admission permits 8 MiB and 1,500,000 nodes. The session retains one
large source independently of interactive counters and revokes both at a new
revision. Parser-profile-bound source/node identities, complete original
anchors, ordinary metadata pages and existing redaction apply throughout the
C API, CLI, worker and MCP. Independent record and other consumer limits remain
in force. Charged parser/model storage is not a whole-process RSS bound. See
the [selection contract](web-passive-report-profile.md#explicit-large-source-selections).

Pinned macOS ARM64 Claude Code 2.1.296 modules 141, 299, 415 and 1452 retain
180,783, 185,080, 1,165,367 and 286,849 AST nodes, respectively. Their exact
original hashes/lengths are in `LargeSourceTests.cpp`; passive reports retain
12, 33, 141 and 31 static rows. The complete retained package has 2,364 parsed,
navigated feature inventories with no parser/feature refusals, 418 configuration
objects, 3,702 terms and 32 possible later overwrites. Named configuration
loading links remain zero. These are syntax candidates, not runtime requests
or effective configuration policy.

Continuation verification against `8600156fe`:

- The Release engine/CLI/SDK/worker/MCP build succeeded. The complete owning Web
  directory passed **796 of 827** checks with **31 explicit skips** in 30.88 s,
  including the four pinned large modules, both whole-package source-feature
  profiles and all eight captured Claude platform packages.
- All five explicit large-source CLI forms passed with an unusable external-tool
  PATH, private canaries and no stderr. The initial continuation run exposed
  reused test logs: LLVM redirection preserves old file tails. Independent logs
  per command fix the harness; production output was unchanged.
- Worker/MCP and availability passed six cases, with the one backend-omission
  check explicitly skipped in the normal build. Qt Web review passed in 3.31 s.
- Parser OFF rebuilt successfully; **21 of 22** selected SDK/report/transport/
  availability checks passed with the same omission-only skip. The new C symbol
  remains exported and its operation explicitly reports `capability_unavailable`.
- Web backend OFF rebuilt successfully; six of seven availability/transport
  checks passed with worker's positive backend case explicitly skipped. The
  always-declared large-source C API returned the same unavailable contract.
- The new symbol is registered in the existing separate Web public-surface
  inventory. The 23-capability ownership check, source-provenance scan and LLVM
  formatter check pass.

Receipts use `/tmp/neverd-js-continuation-*`; the prior pinned full-client
property receipt is `/tmp/neverd-large-source-full-client.xml`. The original
temporary full Bun/SEA/pkg/nexe producer-container corpora are absent from this
host, so their optional tests remain coverage omissions. No proprietary source
or ignored `superpowers` document is included in the change.

### Final large-source integration

Implementation `cd62989ff` was normally merged with remote `a191a6167`, then
`e02da11c2`, as `9cf89bcb5`. Incoming native changes require the recorded
Capstone revision `f5fb7682d`; it was synchronized in this worktree. The Web
owners remain unchanged apart from an incoming SEA SDK test portability fix.
All seven owning Web test binaries, engine, CLI, worker and MCP rebuilt in
Release. Web/JavaScript/ASAR remain ON.

The final owning run passed **796 of 827 unique checks**, with **31 explicit
skips**, in 50.15 s. Four real large selections and all 2,364 macOS ARM64 feature
inventories pass, as do the eight fixed Claude platform-package selections.
Two earlier concurrent CTest invocations raced on GoogleTest discovery files,
producing duplicate registrations; that run is not counted as additional
coverage. The generated lists were preserved for diagnosis, regenerated and
checked for unique names before the final sequential runs. The testing guide
now records this orchestration constraint.

Final worker/MCP/availability checks passed six of seven cases with the normal
backend-omission-only skip. Qt Web review passed in 4.06 s. The final CLI report
for the 4.5 MiB module 415
retains 141 static rows and is byte-identical to its pre-merge report, with
unusable external-tool PATH and zero stderr. The 23-capability check, LLVM
formatter and source-provenance scan of 7,675 files pass. Final receipts use
`/tmp/neverd-js-continuation-final-*`; missing producer corpora remain the same
explicit qualification gaps.

The first push encountered newer remote `960e6a8eb`. Its only implementation
change is native object-patch ABI metadata; no Web owner changed. Normal merge
`c9782cfa5` and the seven-step SDK/CLI/transport rebuild succeeded. The final
focused run passed **20 of 22** large-source/SDK/native-handoff/availability/
transport cases in 5.50 s; the missing optional Bun producer image and the
normal backend-omission case are explicit skips. Capability ownership remains
valid. These receipts use `/tmp/neverd-js-continuation-push-retry-*`.

### 2026-10-13: actual CEF/WebView2 packs and required producer qualification

The follow-up closes the measured resource-format part of the #716 gap with
one independent C++ validator and two explicit layouts:
`chromium-datapack-v5-v1` (standard u16 IDs/counts) and
`msedge-webview2-datapack-v5-u32-v1` (the observed WebView2 wide table).
Both input headers mark v5; explicit selection prevents layout guessing.
Complete index/payload admission, sorted unique IDs, checked aliases, original
SHA-256 slices and revision-bound pages precede publication. Explicit bounded
native gzip produces separately identified bytes; failures remain unselectable.
Source/HTML/anchor consumers share `ArtifactView`, including nested ZIP and
compressed origins. Fixed-name exclusive local export verifies original and
already selected decoded files before completion; worker/MCP exclude raw export.

The fixed CEF 120.2.7 NuGet original is 128,957,697 bytes and SHA-256
`e7f2e0b6860d8cce3ab66d7cb50492b9e1ea601172c850e5c7595d2a45baff17`.
Its 8,187,343-byte pack contains 2,504 canonical resources and 16 aliases.
Resource 698's 23,108 gzip bytes at pack offset 24,660 decode to 102,823 bytes
and 11,830 parsed module nodes. Resource 693 yields a 976-byte HTML document
with one script candidate. Both decoded hashes independently match native
gzip reference reads. The original NuGet deflate frame remains separate from
the resource gzip frame in source origins.

The local Teams 26261.303.5165.1465 app supplies WebView2 154.0.4258.37's
51,851,676-byte pack, SHA-256
`b3b4fb23fa809ac33895d6f0a87d03fdb6a2f087d6a15824d39faf48644c7b8d`.
Its measured table contains 5,814 canonical resources and 633 aliases. Numeric
resource 24,521 retains its exact 535 bytes at offset 621,511 and parses into
56 script nodes with original anchors. No client/native runtime is run or
redistributed; resource tables establish no framework identity or runtime route.
The [profile](web-datapack-profile.md) pins sizes, hashes and limitations.

All 18 new resource/policy checks pass, including both original real inputs.
Owning Web Release regression passed **814 of 845** distinct cases with
**31 explicit optional-input/feature skips**, in 117.00 seconds. Worker/MCP/
availability passed six of seven with the normal backend-omission-only skip;
Qt Web review passed in 7.20 seconds. JS omission passed all 16 applicable
resource cases with the same real originals, plus six of seven transport/
availability cases. Web omission passed six of seven, with the dependent
positive worker case explicitly skipped; all four additive C symbols remain
available as omission stubs. Enabled flags are restored before integration.
LLVM formatting, the 23-capability ledger, source provenance and all 309
localized Markdown entries pass. Receipts use `/tmp/neverd-js-gap-*`.

For #718, eleven existing fixed full-image tests now have one required
qualification inventory. `NEVERD_REQUIRE_WEB_PRODUCER_CORPUS=1` converts absent
inputs and unavailable native/source consumers into failures; missing test
registrations and empty CTest selections also fail. The release target and
explicit/reusable workflow require an immutable corpus URL and SHA-256, then
retain the owning C++ artifact hashes and source witnesses. A separate fixed
CEF workflow requires its actual redistributable input.

The complete old producer images were stored under erased `/tmp` directories.
An absence probe selected all eleven required cases and failed all eleven with
zero skips; no green full-corpus receipt is claimed. Their original manifests
are preserved while fixed tools/inputs are prepared for reconstruction in a
persistent directory. [Required corpus qualification](web-producer-corpus-qualification.md)
records the release contract and remaining artifact-availability work. The
measured #714 minified-loader/configuration-file follow-up remains separate.

Integration commit `c22b3c756` and merge `46e35f050` retain upstream
`a07acfadd`'s native function-edit/call-context changes. The resulting Release
rebuild passed; owning Web passed **818 of 845** distinct cases with **27
explicit skips** in 146.60 seconds. All 25 full Bun containers were rebuilt
with the original input, compiler and template pins and individually matched
their existing complete-file hashes. The full Linux/cross-container, six-target
map and native-handoff cases now run from a persistent corpus. No old hash was
changed. Worker/MCP/availability passed six of seven with its normal omission
skip, and Qt Web review passed in 2.62 seconds. The 23-capability ledger and
7,695-file source-provenance scan pass after the native merge. These receipts
use `/tmp/neverd-js-gap-postmerge-*`.

### Persistent full producer corpus restoration (2026-10-13)

The fixed corpus has been rebuilt in a persistent local directory. All 25 Bun containers,
six injected Node SEA images, three nexe images, six canonical pkg images and
the BOM/shebang pkg witness match their original whole-file SHA-256 values.
The preprocessing witness requires the original `stripped-inputs` basename;
changing it changes the embedded virtual paths and is correctly rejected by
the fixed hash check. Fresh nexe/pkg dependency locks are recorded separately
instead of being passed off as the historical locks.

Node's snapshot builder did not reproduce the old opaque snapshot bytes even
with the original source, compiler path and fixed random seed. A separate
immutable `2026-10-13` witness therefore pins its 5,644,095 bytes and SHA-256
`9e94efe5aed0592ce0338a8c80f0e352cd154ff5849e1fe32f866a4841e2772a`.
The old witness remains the default and unchanged; only an explicit known
selection admits the new snapshot. Unknown selections and mismatched hashes
fail. Both witnesses stay opaque and have no invented source selection.

All eleven required producer cases pass with zero skips in 25.73 seconds;
the receipt is `/tmp/neverd-full-producer-qualified-fixed.xml`. The invalid
snapshot selection probe also fails as required. Production used only the
fixed trusted builders in separate fixture preparation; no generated image
was executed and the C++ qualification invokes no producer or runtime. Remote
release qualification still requires an accessible immutable archive and a
green workflow receipt. Real minified-loader/configuration navigation remains
the next implementation gap.

### Minified configuration file navigation (2026-10-13)

`javascript-configuration-file-navigation-v1` now retains fixed file/layer
literal sites, common switch-case anchors, local lexical call candidates and
ordered arrays under the existing source navigation operation. It uses
`SourceModel::indexSourceFunctions`, also shared by existing client callbacks,
for authoritative function declaration/initializer cardinality. Duplicate or
conflicting candidates, dynamic scope, imports and alias chains retain unknown
targets. Reassignment preserves a source relationship without proving the
runtime target. Array terms preserve unknown spreads/holes/calls and establish
source order only. Private paths/names/values remain withheld.

All 2,364 original Claude units complete the new inventory under explicit large
admission: 973 literal sites, 592 lexical call candidates, 69 layer arrays and
209 terms. The interactive profile keeps its ten refusals; all 2,354 admitted
units complete with 682 sites, 341 calls, 50 arrays and 156 terms. The existing
named loader grammar remains unchanged at zero. Direct CLI navigation of the
original `m00086.js` retains 30 sites, 19 call candidates and a two-term array
with an unusable external-tool PATH and empty stderr. The profiles do not
authenticate loaders or infer enabled layers, merge behavior or file precedence.

Seven new focused C++ checks pass. Owning Web regression passes 823 of 852
distinct cases with 29 explicit optional input/feature skips in 75.35 seconds;
all eleven strict producer cases run from the verified archive round trip.
Worker/MCP/availability passes seven of eight, including the actual fixed
Linux Claude MCP inventory, with only the backend-omission-only case skipped.
The full source counts use a separate GoogleTest property receipt because
CTest JUnit preserves labels rather than those custom measurements. Receipts
are `/tmp/neverd-configuration-file-*`.

The actual CEF distribution also passes its
[GitHub workflow](https://github.com/NeverSight/NeverD/actions/runs/38413710185)
at `9276ef94c`. The persistent local full-corpus archive is 1.2 GiB and has
SHA-256 `7372a4fb66605e2d62405ee1e5ef55da1fbde99bed8cd61c1f5c6790c1d53ce3`.
It preserves producer locks, manifests and receipts; full-corpus remote
qualification still needs an accessible immutable input and a successful run.

Parser omission passes six of eight transport/availability cases with the
unsupplied optional Claude input and backend-omission-only case skipped. Web
omission passes six of eight with the two dependent positive Web cases skipped;
the complete public C ABI omission check passes. Enabled Web/parser flags are
restored and all seven focused cases pass again. These checks establish honest
profile discovery and omission behavior, not Windows analysis-host support.
The restored worker/MCP/availability selection passes seven of eight again,
including the actual fixed Linux Claude inventory, and Qt Web review passes
in 4.11 seconds. Formatting, all 23 capability records, the 7,705-file source
provenance scan and all 309 localized Markdown entries pass. The primary
checkout's existing signature and diagnostic changes are preserved.

### Combined Web release qualification (2026-10-13)

The individual producer/CEF lanes now have a shared explicit/reusable release
entry point. `web-release-qualification.yml` calls both workflows at the selected
ref and exposes one final `Web release qualification gate`. The gate runs after
the input preflight and both dependencies finish, and accepts only three
successful results. Local execution of its exact shell condition verifies all
64 combinations of success, failure, cancellation and skip. Input preflight
rejects missing/invalid URL/hash/witness selections before reader builds.
The producer and wrapper preflights use explicit failure exits: local Bash 3.2
continued past a failing standalone `[[ ... ]]` despite `set -e`. Eighteen
valid/invalid input cases now verify both exact preflight snippets on that shell.
YAML parsing, documentation and provenance checks pass. No producer hash,
consumer selection or raw-source policy changes.

An immutable runner-accessible full-corpus URL remains unavailable. The local
archive and eleven-case strict receipt are retained; a green combined remote
release receipt cannot yet be claimed. The gate is ready to dispatch with the
fixed archive SHA-256 and explicit `2026-10-13` snapshot witness once that input
is supplied. This workflow does not publish fixtures or alter unrelated package
publication. See [the release contract](web-producer-corpus-qualification.md).

### Authenticated remote qualification (2026-10-14)

The producer lane and combined gate now accept the optional repository/call
Secret `NEVERD_WEB_PRODUCER_CORPUS_AUTHORIZATION`. Only the producer download
receives its bearer header; archive and independent artifact hashes still
govern admission. Public archive callers can omit it. Nonempty curl option
arrays preserve macOS Bash 3.2 behavior with `set -u`. Actionlint validates all
three workflows, and the actual download option construction passes an
authenticated HTTPS HEAD probe under Bash 3.2. Missing/wrong tokens are denied.

The prior [invalid-input probe](https://github.com/NeverSight/NeverD/actions/runs/38424287437)
finished with failed input validation, skipped producer/resource jobs and a
failed final gate. The [full combined run](https://github.com/NeverSight/NeverD/actions/runs/38500156926)
has been dispatched at `70f433ae49cc25841f0bda92bdcb649b0a847a3f` with the fixed
archive SHA-256 recorded above and the explicit `2026-10-13` snapshot witness.
The input preflight passes. A temporary authenticated HTTPS transfer supplies
only that archive during qualification.
The first producer job stops before consumer tests: its HTTP/2 stream resets
after approximately 405 MB, and curl exits 92 without using the existing retry
count. This is a failed remote receipt.
Its fixed CEF job succeeds, and the final combined gate fails because the
producer job failed; a successful resource job cannot mask that failure.

The download now retries all errors within the existing two-retry/time bounds.
A task-local C++ transfer probe serves the unchanged full archive and cuts the
first response after 1 MiB. Default retries stop with partial-file error 18;
the revised policy retries into its explicit output file and recovers all
1,251,173,226 bytes with SHA-256
`7372a4fb66605e2d62405ee1e5ef55da1fbde99bed8cd61c1f5c6790c1d53ce3`.
HTTPS restrictions, bearer authentication and both hash checks remain required.
The [corrected combined run](https://github.com/NeverSight/NeverD/actions/runs/38502323427)
now succeeds at `8c1f4ac03875cec8711568b692bc0c76843b40ed`. Download and envelope
verification complete before the native consumers build. Retained JUnit reports
confirm eleven producer cases (48 seconds) and fifteen CEF/resource cases
(6 seconds), both with zero failures and zero skips. Input preflight, both
called jobs and the final combined gate all report success. This is a full
fixed-corpus release qualification receipt on the macOS ARM64 reader.

After integration, the owning local selection discovers 852 cases: 822 pass,
29 retain explicit optional/omission skips, and the required CEF consumer fails
because its selected `/tmp` input is absent. The unchanged NuGet bytes are
downloaded to persistent storage and verified against the original hash; the
affected CEF case passes in 9.79 seconds. All eleven required producer cases
already pass without skips, and no unresolved test failure remains. The
worker/MCP/availability selection passes seven cases with one backend-omission
skip (14.16 seconds); Qt Web review passes (3.37 seconds). All 23 capabilities,
the 7,823-file provenance scan and 309 localized Markdown entries pass.
Machine-local receipts and both remote JUnit reports are retained beside the
persistent corpus. The qualified ref is explicit; later unrelated native
integration does not acquire a Web receipt from it automatically.
The task's fixture server and tunnel are stopped. Its temporary repository
Secret is confirmed absent, and the local token/header files are removed.
The immutable archive and qualification reports remain in persistent storage;
no permanent public fixture endpoint is claimed.

### Permanent corpus Release and automatic qualification (2026-10-14)

The [fixed corpus Release](https://github.com/NeverSight/NeverD/releases/tag/web-producer-corpus-2026-10-13)
now publishes the unchanged 1,251,173,226-byte archive. GitHub's uploaded asset
digest equals the qualified SHA-256
`7372a4fb66605e2d62405ee1e5ef55da1fbde99bed8cd61c1f5c6790c1d53ce3`.
Its matching source companion is 241,067,203 bytes with SHA-256
`48dd3c177dcae0fcdb9378d38beb70ecc1813b0dd3de7fb3126b2c69d6233048`.
It includes full Bun application source, the 7,990 original corresponding
JSCOnly library/build/license files, 21 native source archives, all 181 Cargo
registry archives checked against the original lock, ICU source and original
runtime/dependency/recorder notices. Source archive hashes, a rebuild guide,
the exact earlier qualified-ref receipt and both JUnit reports are independent
Release assets. Historical corpus bytes/manifests remain unchanged.

The combined lane defaults to this public URL, digest and fixed snapshot
witness. `web-qualification-ci.yml` calls the same complete gate automatically
for affected Web/transport/build changes on `dev` and pull requests. Explicit
invalid overrides still fail preflight; the public default requires no corpus
Secret. The producer lane retains the correct owning CTest `LastTest.log` path.
Corpus releases skip the unrelated Python SDK publication lane. Actionlint and
the SDK delivery-policy audit pass. This change establishes permanent public
fixture distribution and the automatic caller.

The [automatic public-download qualification](https://github.com/NeverSight/NeverD/actions/runs/38539252447)
passes at integrated ref `e669ac89e189285b6cfda77bf76fc38a0d90780a`:
eleven producer cases and fifteen CEF/resource cases have zero failures,
errors or skips. Input preflight and the final combined gate succeed. The
[explicit invalid HTTP override](https://github.com/NeverSight/NeverD/actions/runs/38541715306)
at `f17748cc4` fails preflight, schedules neither reader and fails the final
gate. The corpus Release's unrelated
[Python SDK run](https://github.com/NeverSight/NeverD/actions/runs/38537325835)
is skipped as intended. Both public source-companion download/envelope and
all 205 contained source archive hashes are verified. The new qualified-ref
receipt and both JUnit reports are retained as additional corpus Release
assets; earlier receipts remain unchanged.

### Native Windows host and extracted product qualification (2026-10-15)

The [Windows product contract](web-windows-product.md) now has actual Windows
Server 2025 x64 evidence. `platform/WindowsFile` owns fixed local NTFS traversal,
retained identities, reparse/stream/hard-link refusals, private current-user
creation, unlinked spools and no-clobber publication. Input capture, blob
storage and both export owners use that layer. Captured bytes survive source
deletion; sealing removes writable access and serializes shared range reads.

NTFS can retain stale directory timestamps in enumeration records. The native
regression binds the enumerated identity/type and compares authoritative
opened-handle revisions before and after capture. File revision checks remain
in place. A changed directory revision and replacement still refuse. Native
manifest publication uses retained directory handles and does not replace an
existing target; rewrite rollback preserves foreign files.

The [complete native/archive run](https://github.com/NeverSight/NeverD/actions/runs/38677375352)
at `5e126750ae7513591b6d79056c02358fe55da0f1` passes **110 native cases,
two adapter checks and six installed checks**, with zero failures or skips.
The selection includes sixteen Windows ownership cases and eight independent
native handoff cases. The 65 MiB + 1 byte original import retains separate
source/map parser bounds. The extracted archive's independent C++ consumer
loads its DLL/ICU and drives its CLI, framed worker and MCP with development
paths removed. It checks the worker's initial hello/schema and both CLI JSONL
records, plus immutable source export, ASAR and private stderr.

The product contains the CLI, public C ABI DLL/import library and headers,
worker, MCP, ICU 77.1, matching app-local CRT and notices. Native dependency
paths are normalized before generating install rules. The LLVM producer's
MSVC 14.51.36231 runtime boundary is enforced and a small LLVM consumer links
and runs before the full engine build. Native source/toolchain/dependency
pins, every required test and every archive member are bound by the receipt.
The formal release gate also requires all eleven producer and fifteen CEF
resource cases at the same source commit; it reaudits and publishes the
qualified ZIP without rebuilding or overwriting an existing Release. This same
run passes all eleven producer and fifteen CEF/resource cases on macOS arm64,
with zero failures or skips.

The [formal Windows Web product Release](https://github.com/NeverSight/NeverD/releases/tag/web-product-windows-x64-2026-10-15)
was published after both final gates succeeded at the same `5e126750ae75`
source. Independent public downloads verified all eight asset checksums and
matched the original qualified receipt exactly. The ZIP has 139 files and
60,769,171 bytes, with SHA-256
`4b0f0d2b082fce4762ec0b346fb408eec09c53e098dac03d69562d45435d4982`.
Every ZIP member's size/hash matched the receipt; the tag points to the
qualified commit, all 8,134 source archive files match that Git tree, and
`REBUILD.md` matches its product documentation. Later development commits do
not change this fixed release's qualification.

The local owning rebuild at `b2523a9f0` passes 807 of 852 Web cases with 45
explicit optional/omission skips and no failures. After the subsequent native
integration at `b86f9c4e5`, the rebuilt nine native cases, including full Bun,
and both worker/MCP adapters pass without skips. These local receipts do not
replace the separate Windows or formal same-source publication gates. Windows
ARM64/other host releases, additional package managers, general module/dataflow
resolution and wider framework coverage remain outside this delivery profile.

### Final issue audit and portable member-name repair (2026-10-15)

All five original GitHub issue bodies and comment histories were re-read.
The [acceptance audit](web-epic-acceptance.md) maps mandatory criteria to the
implemented profiles and distinguishes later adapters from required work.
The new [desktop matrix](web-desktop-adapter-matrix.md) independently states
detection, listing, extraction and analysis for each named version/layout.

The final #716 review found a real shared-boundary defect: directory capture
accepted superscript Windows device names and did not equate NFC/full-fold
aliases that archive readers already rejected. Commit `fe12a9815` moves component
validation and comparison into `PathPolicy`; both directory walkers and archive
namespaces use it. Original evidence names and IDs are not normalized. Fixed-name
export writers use the same component validator. Without pinned ICU, directory
member comparison admits only the exact ASCII subset and explicitly refuses
non-ASCII members; selected root paths are not member comparison keys.
Independent no-ICU compilation verifies that boundary without a Unicode fallback.

At integrated source `200eac54bf900d9f02844a20d1853296d1439d07`, the local
macOS arm64 run contains **856 distinct cases: 811 passed, 45 explicit optional
or omission skips, zero failures**. All eleven required producer cases and all
fifteen CEF/resource cases passed with fixed supplied inputs. Their subset XMLs
are derived from that full receipt, not additional independent runs. The worker,
MCP and enabled-backend API checks pass; the disabled-backend-only API case is
explicitly skipped. A fresh serialized CTest inventory removed a local concurrent
discovery race; the earlier duplicate-count receipt is retained as diagnostic
material and is not acceptance evidence.

The same repaired capture owner also passes all eight pinned Claude platform
packages plus the official matching Linux standalone input: nine cases, zero
failures or skips, with an unusable external-tool PATH. The eight packages retain
18,794 JS units and 358,539,894 UTF-8 bytes with their independent hashes. This
rerun covers capture, integrity, layout and decoding; earlier readable-export
and cross-track receipts retain their original scope. The focused passive
selection and supplied-source follow-ups also pass, including the exact-hash
Linux input after correcting a path that referred to a truncated download.

Local receipts are retained in `build-web-dev/issue-final-audit/`, including
`web-unique-inventory.json`, `web-full.xml`, `local-closure-receipt.json`,
`transport-api.xml` and `718-claude-capture-fix-receipt.json`. These receipts name
their source baselines and do not automatically qualify later unrelated merges.

After the loader/worker integration at
`8167d7cd396303bdf94bfa5bdd222811c520df1c`, the affected SDK/native selection is
rebuilt and passes 230 of 238 cases, with eight explicit optional/omission
skips. Worker/MCP and the enabled-backend API checks pass again; the one
backend-omission-only API case remains skipped. Their receipt is
`dev-integration-results.json`, with separate JUnit reports. The Web core owner
is unchanged by that upstream integration.

The independently downloaded [Windows run 38696359332](https://github.com/NeverSight/NeverD/actions/runs/38696359332)
qualifies repair commit `fe12a9815bec2492121490e5893ba4e8c52e686f`:
**113 native cases, two adapters and six installed checks, all passed with zero
failures or skips**. The native inventory includes both `WebPathPolicy` cases
and the captured-Unicode-alias regression as required entries. The receipt,
JUnit inventories, installed checks and all 139 packaged file hashes were
independently reverified. Its 60,793,059-byte ZIP has SHA-256
`e579fe8b5ed548305b38fe71bacb4c97c8bec7a52b08569be88b5fb07c5b32e5`.
This is a new qualified CI product artifact; the earlier formal Release remains
unchanged at its recorded `5e126750ae75` source and does not contain this repair.

The pinned Teams WebView2 154 resource SDK case additionally passes on the
integrated local build, preserving all 5,814 resource IDs, 633 aliases and the
selected JS source anchor. Prior Tauri/Wails/VSIX fixed captures were not found
in existing local storage for this rerun; their historical receipts remain
scoped to the original runs and are not counted as fresh passes. No replacement
sample was downloaded or invented. Self-authored adapter cases remain in the
full successful owning selection.

The separate [automatic integrated-ref producer/resource run](https://github.com/NeverSight/NeverD/actions/runs/38698074172)
was still pending when these closure receipts were assembled and is not counted
as a CI pass. The superseded manual producer/resource dispatch was cancelled
before its preflight started. The fixed eleven-plus-fifteen local cases above
passed without skips; the separately completed Windows run is the new native
host evidence. No new formal Release is claimed by this issue closure.

The user requested closure of completed issues. All five are now closed with
`state_reason: completed` and individual acceptance comments:
[#714](https://github.com/NeverSight/NeverD/issues/714#issuecomment-6186796347),
[#715](https://github.com/NeverSight/NeverD/issues/715#issuecomment-6186798946),
[#716](https://github.com/NeverSight/NeverD/issues/716#issuecomment-6187011700),
[#717](https://github.com/NeverSight/NeverD/issues/717#issuecomment-6186802702), and
[#718](https://github.com/NeverSight/NeverD/issues/718#issuecomment-6187013598).
Local copies of the verified Windows artifact and independent receipts are
retained separately from the earlier formal Release downloads. Issue completion
retains all documented profile limits; future adapters and unknown runtime
semantics are not newly claimed.
