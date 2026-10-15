# Claude Code client reconstruction

The requested outcome is a readable, traceable recovery of the fixed Claude
Code 2.1.296 client that can be rebuilt and run. Extraction and issue closure
alone do not satisfy this goal. First qualify macOS ARM64 on the available
host, then Linux x64 and the remaining captured targets. Keep every original
and recovered private source file outside this repository.

The immutable macOS input SHA-256 is
`c9b5341637becbd423ddffc5b254afb645682a3868cb708bbc6cc0e7bb419937`;
the Linux x64 input is
`24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de`.
The macOS graph contains 2,364 JavaScript modules and 245 assets. Neither
sample has source maps. Original TypeScript names, types, comments and file
boundaries therefore remain unknown unless independent evidence establishes
them. Version 2.1.88 is a comparison aid, never replacement source.

## Boundaries and implementation

NeverD remains an offline C++ analyzer: no target execution, downloads,
package installation or external parser in the engine. Its existing Bun graph
and embedded Hermes parser own source evidence. A development-only C++ project
exporter produces derived files with fixed safe names and change receipts.
The separately invoked, pinned Bun compiler/runtime supplies build and runtime
acceptance. Build recipes may use JavaScript because that is the reconstructed
client language. Self-authored test fixtures remain C++ strings.

1. **Preserve rebuild metadata.** Extend `include/neverd/web/Bun.h`,
   `lib/web/Bun.cpp`, `lib/web/SessionBun.cpp` and
   `lib/web/SessionBunExport.cpp` to retain exact startup count, graph flags,
   prelinked file indices, runtime-option bytes and compile-argument bytes.
   Unknown runtime settings remain opaque. Extend `unittests/web/BunTests.cpp`
   to verify exact bytes, bounds, empty arguments and malformed refusal.

2. **Plan source relocation from syntax.** Add
   `lib/web/BunProjectSource.{h,cpp}` and
   `unittests/web/BunProjectSourceTests.cpp`. Rewrite only parser-confirmed
   import/re-export, dynamic import and `import.meta.require` literal arguments
   that resolve to an exact captured virtual module. Retain original byte
   spans, source/target hashes and mapping evidence. Reparse and compare all
   untouched syntax. Unknown requests and runtime path strings are explicit
   diagnostics; global string replacement is forbidden.

3. **Export a rebuild project.** Add `lib/web/BunProject.{h,cpp}` and a
   development driver under `unittests/web/fixtures/bun/`. Reuse the admitted
   extraction, shared text decoding and exclusive `ExportDirectory` writer.
   Export all modules/assets, including JavaScript-looking file assets, text
   loader data and native addons. Preserve loader roles, exact stored hashes
   and virtual-name mappings. Emit a build recipe and complete unresolved
   inventory; never label an incomplete export runnable. Keep original
   executable and cached bytecode out of the build inputs. Dependencies: 1–2.

4. **Prove isolation before target execution.** Use self-authored probes with
   an empty environment, fresh HOME/cwd, explicit runtime/project reads and
   denied external files, subprocesses and network. Record both successful
   allowed actions and refused IPv4/IPv6/file/process actions. If the platform
   cannot supply these constraints, retain the artifacts and identify the
   missing boundary. No real credentials or configuration enter validation.

5. **Build and compare actual behavior.** With a recorded compiler hash,
   compile the reconstructed modules without the original executable. Repeat
   the build in a fresh directory and record artifact differences. Compare
   original and reconstructed exit status, output and controlled side effects
   for version/help, option errors, configuration precedence, permissions,
   hooks and MCP/local protocol flows. Extend isolation only with tested,
   narrowly scoped capabilities required by a specific case. Tests involving
   credentials or live provider behavior remain outside offline acceptance.
   A version-only smoke test is a milestone, not final equivalence. Depends on
   3–4 and resolution of every exercised runtime-path/loader mismatch.

6. **Recover semantic structure.** Link captured module requests, imports,
   exports and lexical bindings using existing source owners. Produce a local
   navigable index for entry, tools, configuration, permissions, hooks and MCP.
   Keep exact retained names, evidence-backed semantic aliases, inferred type
   facts and older-version similarities separate. Every alias/type fact needs
   an original byte anchor and evidence category. Do not change executable
   bindings merely to improve labels. Quantify resolved and unresolved edges.

7. **Acceptance and handoff.** Publish generic implementation/tests/docs only.
   Deliver the private project, exact build command, compiler identity, source
   mapping, behavior receipts and an explicit remaining-gap list locally.
   Close this goal only after a clean rebuild and the agreed client behavior
   matrix pass; qualify every retained opaque native dependency separately
   from recovered JavaScript. Extend other platforms with their own inputs and
   receipts, never substitute macOS files by module index.

## Verification and review

Use the existing Release `build-web-dev` configuration. Build affected Web
targets, run `WebBun.*` and the new project source/export cases first, then the
owning Web suite and affected SDK/transport/omission checks for changes to
shared extraction. Run CTest discovery sequentially. No generated Claude
source, native binary or private runtime log belongs in a commit.

An independent agent reviews the relocation rules and plan against the real
graph. This is a new goal beyond issues 714–718; their bounded extraction and
analysis acceptance is preserved. The primary risks are embedded runtime
defaults, Bun-specific loader/prelinked behavior, runtime-computed virtual
paths, native ABI requirements and behavior sensitive to source text or
locations. All transformations are derived outputs; rollback means discarding
that newly created project directory while retaining original evidence.

## Current evidence

Metadata retention, syntax relocation, imported-require alias evidence,
project export and semantic navigation are implemented at `d32411dd2`.
The 50 focused reconstruction/shared-module checks pass. The Web suite has
898 cases: 841 pass and 57 optional fixture cases skip in that invocation;
separate real metadata receipts cover all eight captured distributions.
Available MCP, worker, SDK and feature-omission checks pass.

The private macOS ARM64 project has 2,364 JavaScript modules and 245 assets.
All JavaScript has a same-parser-tree readable view. The navigator retains
228,497 anchored evidence records; one direct-eval module has partial lexical
analysis. Erased TypeScript information remains unproven.

The independently compiled, cache-free client now preserves all 2,609 module
names, table order, loader/encoding/side metadata, the original entry, seven
startup modules, default autoload flags and the captured JIT-policy bytes.
The added bootstrap record is removed from the virtual filesystem. A separate
self-authored fixture qualifies entry identity, live bindings, module identity,
worker paths, embedded file listings and UTF-8/Latin-1/UTF-16 text behavior,
plus refusal of unsupported metadata. The native addons retain their captured
bytes and loader roles. A separately qualified exact-file mapping capability
now loads all three macOS addons under the independent Bun runtime and records
their export descriptors. No exported device-control function is invoked.
Embedded native-file materialization and hardware behavior remain unqualified.

Two independent compilation runs, followed by the qualified metadata adapter,
produce the same 124,511,568-byte client with SHA-256
`90868bc9180ed9240fe96974c4973bec9dd358ed83a2b5da69e52d67e65ca9fa`.
A relocatable private delivery directory with source, navigation, compiler and
build script has also reproduced that exact hash from a fresh output directory.
NeverD independently admits the rebuilt graph with the expected module count,
entry and runtime-option record. No original executable prefix, cached
bytecode, module-info cache or prelinked cache is used as a build input.

Version/help/unknown-option smoke observations match the fixed original.
All 29 current offline invocations pass and match original records: seven
configuration cases and 22 permission, hook, MCP, stream/error and session
cases. The comparison preserves semantic errors and decisions; only explicit
fresh paths, generated identifiers, process IDs and timing fields are
normalized. HTTP headers and generated state files remain available in raw
receipts but are not declared globally identical.

Earlier failures remain evidence. A raw-graph candidate once exceeded an
eight-second initialization deadline; later runs and the complete fidelity run
passed. Original SessionStart itself varied between cancelled and success.
The passing latest pair does not prove timing stability. Invalid hook text
continues as ordinary output in the original, whereas a valid JSON hook response
with an invalid schema blocks under the tested failure policy; both behaviors
are now represented explicitly in the matrix.

The independently pinned runtime is official Bun `1.4.3+c6da4a4d3`; the original
contains `1.4.3+eecfd55de`. Matching the version and policy bytes does not prove
runtime or performance equivalence. Native hardware behavior and embedded-file materialization, other platforms,
and erased source information remain outside completed acceptance.

Linux x64 now has an available local amd64 Docker environment. Its own sample
exports 2,345 readable JavaScript modules and 244 assets, with project manifest
SHA-256 `40a95d08a22c9baddd6801c39f2057c773c9cd176cad0f5f59187e02697fd46f`.
The Linux isolation and cross-container adapter fixtures pass, including text
encodings, worker paths, module identity and explicit external-network denial.
Two independent builds produce the same 140,675,040-byte ELF executable with
SHA-256 `51e64bded17171c28261a2452b8aa152371ec00f6164188297a9ebbab006e232`.
Version/help/unknown-option observations match the Linux original. NeverD
independently admits its 2,589 modules, entry and runtime-option record.
The full Linux behavior matrix and relocatable Linux delivery remain open.
The overall goal remains open.
