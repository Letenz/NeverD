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

Status: metadata retention, syntax relocation, imported-require alias evidence
and candidate export are implemented. All 22 project/source/alias focused cases
pass; real metadata checks pass for all eight captured Claude platform packages.
Self-authored isolation probes pass, including network/file/process denial.
Exact macOS original version/help/unknown-option baselines pass under that
isolation after allowing one ICU data file. The original contains runtime
revision `1.4.3+eecfd55de`; the independently pinned compiler is official Bun
`1.4.3+c6da4a4d3`. Matching version is not matching runtime revision.

The first recovered macOS project compiles to a 114,189,938-byte executable
without the original executable or cached bytecode. Its version output matches
the original, but help and unknown-option cases fail because the compiler did
not include a runtime-required JavaScript module. This is an unresolved build
closure failure. Self-authored loader experiments also find that compiling
ordinary `require` for a text asset changes its namespace shape, while direct
`import.meta.require` is not automatically bundled. Do not replace callees or
eagerly import modules to hide these differences. A fourth candidate retains
102 proven imported-require literal relocations, including three references
from the largest module after module analysis was made consistent with the
existing explicit Large work profile. It contains 2,364 verified readable
views, 245 assets and 440 unresolved runtime literals. The project-wide
metadata budgets and independent diagnostic totals pass.

Running the flat source project with the pinned runtime preserves version
output, but help fails at an unchanged absolute Bun virtual resource path.
Self-authored experiments show that keeping the entire JavaScript graph in
runtime loading preserves tested lazy evaluation, cycles and cache identity;
mixing bundled and raw assets duplicates module instances. Preserving safe
relative directory layouts in compiler assets retains original virtual resource
names, including nested workers. Text-module namespace identity remains a
separate loader qualification: runtime plugin wrappers change observable
identity. Runtime equivalence and the broader behavior matrix remain open.

The next private build preserves original virtual names and stages every
JavaScript module as retained source. A self-authored, cache-free graph adapter
restores captured text-loader encoding/side metadata after an independent Bun
compile; it verifies raw storage hashes and UTF-16 alignment before signing.
Native addon semantics, original JIT settings and bootstrap reflection remain
explicitly unqualified. Two clean builds produce the same 124,511,568-byte
executable with SHA-256
`f69d96fa1bd7975ed1e26e6b8b31bce47a9e947bed576ec4fecb8ad1f50b002c`.
Version/help/unknown-option observations match the original, including exits,
output and the tested filesystem side effects.

Original headless initialization required one additional macOS system timezone
dataset. Native stack sampling and a self-authored Intl fixture isolated the
boundary before adding only that exact read permission. Seven original and
candidate configuration cases then pass, covering precedence, disabled ambient
sources and malformed local settings. All initialize/get_settings responses
match after normalization of the fresh stage path and process ID. Other
generated UI events/state files are retained but are not declared identical.

The source navigator analyzes all 2,364 JavaScript modules and retains 228,497
anchored evidence records across entry, tool, configuration, permission, hook,
MCP, session, UI and module domains. One module retains partial lexical evidence
because of direct eval; this is an honest semantic boundary. Original TypeScript
names and erased types are not reconstructed by these navigation cues.
The current Web suite has 898 discovered cases: 841 pass and 57 optional fixture
cases are skipped in that invocation; the separate real metadata receipt
already covers the eight captured distributions. The 50 focused reconstruction
and shared-module cases pass, as do the available MCP, worker and availability
checks. Loopback protocol, tool, hook, MCP and session behavior acceptance is
still in progress; these milestones do not complete the goal.
