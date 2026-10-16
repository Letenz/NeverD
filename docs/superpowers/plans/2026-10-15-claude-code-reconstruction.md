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
their export descriptors. A separately authored embedded-graph probe also loads
them through their original virtual paths, verifies exact materialized bytes,
and checks repeated-require identity and matching export descriptors. No
exported device-control function is invoked. Original client feature call sites
and hardware behavior remain outside this loader qualification.

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
runtime or performance equivalence. Native hardware behavior, other platforms,
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
The full Linux matrix now passes: seven configuration and 22 permission, hook,
MCP, stream/error and session invocations match the Linux original. Ordered
protocol frames, complete API request bodies, stderr and exits match under the
same explicit identity/time normalization as macOS; side-effect oracles pass.
Two fixture-server startups exceeded their readiness deadline under concurrent
load before a client ran. The failed setup records remain available; sequential
continuations completed. Latest completed invocations were selected before the
comparison, with provenance and earlier completed attempts retained.

The persistent Linux behavior profile separately qualifies fresh host-backed
fixture/state directories and a dedicated container network namespace. It does
not claim noexec on those writable bind mounts. Root/input files remain
read-only, the process is non-root with no capabilities and no-new-privileges,
external network access is denied, and no real host configuration or credentials
are mounted. The stricter tmpfs smoke/build profile retains its separate tests.

A private relocatable Linux delivery now contains its own source, navigator,
pinned compiler, C++ adapters and build/verification commands. A clean build
from that directory reproduces the same ELF hash. Its build host is currently
qualified on macOS ARM64 with the pinned amd64 Docker environment, not every
possible Linux or Windows build host. Linux semantic navigation retains
226,839 anchored evidence records across all 2,345 JavaScript modules, with one
partial lexical analysis.

Both retained Linux native addons pass load-only ABI checks. The minimal Debian
image initially lacked libasound.so.2; an official Debian package was hash-pinned
and extracted without installation, and only the needed library was mapped
read-only for the audio check. Authored embedded-graph probes now qualify all
five macOS/Linux addons' original virtual paths, exact temporary-file bytes,
cached object identity and export descriptors. Device functions and original
native source reconstruction remain unproven.

macOS x64 now has separate source, graph, Rosetta and native-loader evidence.
Its 2,364 JavaScript modules and 245 assets produce a 132,347,408-byte client
with SHA-256
`ecb0c0ba3325b31d5347dd31b07e1ca7e18c92d4c5c7260afefa7a5861747942`.
Two independent builds and two package builds reproduce that hash. NeverD
independently reads back all 2,609 module names, storage hashes and loader
metadata, plus the expected entry, startup count and runtime options. Its own
seven configuration and 22 behavior cases pass and match the x64 original;
version/help/unknown-option observations also match. The navigator contains
228,498 anchored evidence records. All three x64 native addon records pass
standalone and embedded loading, exact temporary-file materialization, export
descriptor and repeated-require identity checks without calling device exports.

Direct execution of the packaged output exposed a delivery defect: macOS
builders had omitted the executable permission bit, which earlier smoke
staging supplied on its copied executable. Both macOS recipes now set mode
0500. A new ARM64 v2 delivery and the x64 delivery rebuild from fresh directories
and directly verify the produced files, without a permission-adjusting staging
step. Binary content hashes are unchanged; earlier packages and failed direct
verification records remain available.

Fresh x64 executable copies sometimes exceeded the 60-second Rosetta startup
ceiling before producing any output. A self-authored probe isolated the effect:
reusing the same pinned executable reduced a 51-second startup to 0.08 seconds
while a newly created denied-file canary remained unreadable. The x64 matrix
therefore shares only the read-only executable while retaining fresh state per
case. Package verification records up to two bounded version warmups separately
and retries only an output-free startup timeout; the three comparison cases
themselves are not retried. Cold-start failures are retained and performance
equivalence is not claimed. The Rosetta profile independently denies external
file reads, child execution and IPv4/IPv6 networking, but does not claim the
unobserved writable-file executable-mapping denial.

Linux ARM64 also completes the same acceptance with its own captured sample,
pinned independent compiler and architecture-specific Debian image digests.
Its 2,345 JavaScript modules and 244 assets rebuild to 140,036,904 bytes with
SHA-256 `728c0066f8c60b2cffd325ed21b7710ff357455b2a68da35df79121d90b01cc9`.
Two independent builds and the relocatable package's clean build match;
direct package smoke verification passes. NeverD independently reads back all
2,589 module records and their storage hashes. All seven configuration and
22 behavior cases pass and match the ARM64 original. Its two native addons
pass both standalone and embedded loading, matching export descriptors,
cached identity and exact materialized bytes, with a separately pinned ARM64
ALSA library and no device calls. Navigation has 226,839 anchored records.

The native ARM64 environment exposed a fixture assumption: opening the running
ELF for writing returns ETXTBSY before mount permissions are checked. A separate
non-executing read-only canary returns EROFS and now qualifies that property;
the original failed probe remains evidence. Existing amd64 images were retained
when Docker rejected assigning their multi-architecture digest to a second
architecture; the ARM64 compiler uses its verified architecture-specific digest.

Linux ARM64 musl and x64 musl now complete the same bounded acceptance. Each
retains 2,345 JavaScript modules and 244 assets, with 226,839 anchored navigation
records. Three independent builds, including each delivery's clean build,
produce matching hashes: ARM64 is 132,452,272 bytes with SHA-256
`9e0116f9da6a1b4c86de75ca08ab37d5d540b866e79db5800e08c8d0244f8565`;
x64 is 134,195,440 bytes with SHA-256
`45c3fba365552cd61b2aa2039a0fc59b05f6386d0a2a5a0449a1a3bfc2e13c2e`.
Both pass direct package smoke execution, all 29 original/rebuilt differential
cases, complete module readback and both native addon loading/materialization
checks without device calls.

The minimal Alpine image lacked the C++ runtime. Each architecture now uses a
pinned image with signed Alpine compiler, C++ runtime and ALSA packages,
installed offline with maintainer scripts disabled. The private deliveries
include an environment archive; loading that archive and rebuilding from the
package succeeds without downloading dependencies. x64 repository transport
failures were resolved through host HTTPS downloads while preserving TLS and
package-signature verification. Failed setup and stale platform-pin checks
remain recorded separately from successful target observations.

Windows x64 and ARM64 projects and navigators are exported, each with 2,343
JavaScript modules, 243 assets and 226,461 anchored evidence records. Official
independent runtimes are checksum-pinned. Three local cross-builds and two
native Windows builds produce identical outputs on each architecture: x64 is
145,140,736 bytes, SHA-256
`da3d6780681d86726737d3c4449fbbd20c1daa9a3d4a155fc7ff037626dbe204`;
ARM64 is 131,968,512 bytes, SHA-256
`e4d072017574a731a31e59cb639903060ef6fe6532155cc2c3bfe2046ad50cfa`.
All 2,586 reconstructed module rows match independent NeverD readback.

The user authorized native x64/ARM64 GitHub Actions environments in their
personal `gmh5225/test_mac_intel` repository. Both now pass four startup/CLI
cases and seven configuration differential cases against their own originals,
plus retained audio addon loading, cached identity, export reflection and exact
materialization checks. No retained device exports are invoked. Each process
uses a fresh, capability-free AppContainer and a bounded Job Object. Authored
positive and negative controls establish writable task state, denied host
canary reads and binary-directory writes, network denial and descendant
collection before any captured input executes.

Two fixture assumptions were corrected from measured controls: Windows Bun's
unmodified compiler also reports `import.meta.main=false`, and Windows redirects
AppContainer TEMP beneath the supplied disposable HOME. Neither observation is
silently normalized away. The standalone CI extractor was compared with every
NeverD module row before use. Original executables are fetched from a published,
hash-pinned package inside the runner; source and executables are not uploaded
as workflow artifacts. Both Windows architectures now also pass all 22 offline
behavior cases, completing the same 29-case configuration/behavior matrix as
the six macOS/Linux distributions. The final native run at
[`36fd64388c5e7fcc44276979d4e254d423b1501b`](https://github.com/gmh5225/test_mac_intel/actions/runs/38795636890)
passes both jobs. The reusable Windows environment is available in
[test_mac_intel PR #1](https://github.com/gmh5225/test_mac_intel/pull/1).

Authored child-process probes isolated Bun's ignored-stdio failure to the
AppContainer SID lacking access to the NUL device. A per-profile handle ACL
grant, removed after descendant collection, fixes that boundary without
granting internet capabilities. The loopback exception also belongs only to
the fresh profile and is removed after each invocation. Permission checks
account for the original client's Windows path canonicalization. Differential
comparison retains generated pipe identities with an audited bijection and
distinguishes missing JSON fields from null; all substantive protocol fields,
request bodies, exit codes and filesystem oracles remain checked.

The Windows v2 deliveries include the exact accepted CI sources, receipts,
source navigation and fixed build dependencies. A new clean build from each
delivery reproduces the accepted native hash. Every delivered file is covered
by a checked inventory; the six earlier platform inventories were rechecked
without changes. Wine startup failures and earlier fixture failures remain
recorded separately from passing native observations.

The bounded reconstruction matrix passes for all eight retained
distributions: readable anchored source evidence, independent repeatable
builds, 232 original/rebuilt configuration and behavior case pairs, 26 startup
case pairs, and loading/materialization checks for 16 retained native addons.
These bounded results do not recover erased TypeScript information, native
addon source or the original Bun revision, and do not establish live-service,
hardware or universal behavioral equivalence. No recovered private source or
executable is part of this repository change.

## Reopened overall goal

The earlier overall completion was premature. The user explicitly requires
continued complete recovery; the preceding acceptance is a milestone only.
The existing immutable delivery recipes compile hash-pinned captured source,
not edited readable files. A usable development project therefore still needs
an explicit, auditable source selection path: preserve the baseline, derive
readable source with the shared parser, record intentional edits, reject
malformed or ambiguous inputs, and prove that an edited module changes the
rebuilt program. Formatting equality must not be described as universal
behavioral equivalence because source text and locations are observable.

After qualifying that path, continue evidence-backed semantic naming and
types, classify unresolved runtime dependencies, broaden real feature
coverage, and investigate the independent Bun revision and opaque native
components. Keep erased information unknown unless evidence recovers it.
Issues 714–718 retain their completed bounded scope; they do not close this
larger goal. Preserve prior completion receipts as superseded history.

The macOS ARM64 development path now accepts actual source edits. The explicit
`neverd-web-source-recovery` development driver reuses the embedded parser and
readable-source owner; five focused source-recovery tests and authored driver
controls pass. A private workspace has all 2,364 original-path readable source
files. Its selector validates module ownership, immutable original hashes,
bounded syntax and runtime dependencies, with 19 positive/refusal controls.
The packaged parser runs with Homebrew reads denied using its bundled ICU.

A clean build from the self-contained development directory preserves the
qualified `90868bc9...` client hash when sources are unchanged. Actual edits
select edited source, while untouched views retain original byte layout.
Changing one CLI display literal in a separate workspace changes the rebuilt
program's observed output; the delivered source remains unchanged. An optional
all-readable build also passes three startup, seven configuration and 22
behavior comparisons. Configuration observations used the same 18-second
initialization limit for original and rebuilt clients; two earlier 8-second
timeouts remain recorded. New original behavior observations resolve an
old/new date-literal difference without broadening normalization. Startup
performance equivalence is not claimed.

This closes the macOS ARM64 editable-workspace gap, not the overall goal.
Across all eight distributions there are 16,676 distinct source-format/hash
pairs, so module indices or superficially similar source cannot substitute
for target-specific evidence.

## Editable development deliveries across platforms

All seven remaining distributions now have their own complete readable source
workspaces and explicit source-selection adapters around the previously
qualified target recipes. Every unchanged default build reproduces its
qualified baseline hash. A separate workspace changes one unique CLI version
literal in the original entry source, with all other source selections and
native assets preserved. Independent NeverD inventories verify every module
row, entry/startup/runtime metadata and empty cache bodies in each edited
binary. Seven corruption controls exercise the inventory comparator; the
source selector passes 19 positive/refusal controls with the exact parser,
ICU dependencies and snapshot harness pinned.

The five additional macOS/Linux targets pass three baseline smoke checks and
the changed-version execution probe. Their sealed development directories
retain the original immutable delivery and all source/qualification receipts;
every delivered file has been reread and hashed. Both Windows edited
cross-builds also match two independent native builds. Their native
[Actions acceptance](https://github.com/gmh5225/test_mac_intel/actions/runs/38844069619)
passes at `12eaa2ec3744bf2af9a76956b73678980be475a0`, including all 2,586
edited module rows and the changed version output in fresh capability-free
AppContainers. The previous four smoke and 29 configuration/behavior cases
pass again on both architectures. The Windows development deliveries retain
these diagnostic receipts and the exact accepted CI sources.

These checks qualify the default preserved-source layout and one explicit
edit. The unchanged binary identity carries the earlier 29-case baseline
evidence; it does not establish behavior equivalence for arbitrary edits.
All-readable execution beyond the original macOS ARM64 qualification remains
unverified. The Linux x64 legacy inventory uses a different schema; its first
assembly failure is retained, followed by schema-specific validation of every
baseline file. No baseline delivery was modified.

Dependency recovery now has a concrete expression-level match beyond version
metadata: the complete `formatDateDifference` function and export alias match
the retained Apache-2.0 gRPC 1.14.4 package in all eight distributions. The
narrow matcher preserves operator order, calls, numeric constants and the
string value; six authored changes must be refused. Shared-parser syntax
receipts and original byte ranges anchor the result. The upstream declaration
is a contract candidate only; erased original TypeScript annotations have not
been proven or inserted into executable source. Whole-package structure,
full feature coverage, native implementations and runtime revision differences
remain open under the active overall goal.

## Complete-function and upstream type correspondence

Two explicit development exporters now disclose full syntax/lexical models
and decoded source-map anchors to fresh local directories. They reuse the
existing parser, binding and offline map owners; there is no second parser,
production metadata change or target-code execution. Numeric attributes keep
binary64 bits and strings retain UTF-16 units. Each exporter reserves bounded
output before allocation and publishes hashes only after readback. The 19
binding/navigation and seven source-map owner tests pass, alongside eight
source-model and nine map-model driver controls.

A private comparator preserves complete function syntax, values, property
names, declaration/reference identities and external-name distinctions while
normalizing identified local bindings. Twenty-seven independently authored
controls cover alpha renaming, shadowing, captures, changed operators/values,
property keys, external bindings, strictness and dynamic-scope refusal. It
finds the same 208 one-to-one structural candidates in all eight platform
copies of the captured gRPC-containing module, against 67 top-level JavaScript
files from the integrity-verified Apache-2.0 `@grpc/grpc-js@1.14.4` package.
The 1,664 platform instances represent 208 candidate functions, not 1,664
distinct recovered functions. Forty-seven repeated structural groups remain
ambiguous. Uniqueness is local to the compared corpus, not proof of origin or
whole-program equivalence; dynamic and conflicting binding cases are retained
as refusals.

All 67 upstream source maps are decoded through the shared owner. A pinned
official TypeScript 5.3.3 package parses the corresponding upstream source
without executing it, resolving imports, emitting code or inferring types.
This analysis version is not asserted to be the original build compiler.
Exact function boundaries and parameter anchors connect 194 candidates to
upstream TypeScript, including 76 explicitly written parameter types and 83
return types. Fourteen candidates lack exact anchors and remain unlinked.
Absent annotations stay absent; original erased TypeScript types have not
been proven or added to execution source. Position, hash and altered-anchor
controls retain the mapping boundary rather than interpolating missing points.

Each platform also has 38 consistently anchored factory-location candidates
within 110 recognized top-level imported-helper calls. All matched functions
within each selected factory point to one upstream file; 32 factories have
multiple anchors and six have one. The resulting 304 private reading views
preserve original names and license headers, insert whitespace only, and
reparse to the same complete tree. Their outside bindings still belong to the
original containing module, so these views are not standalone build inputs or
a claim that whole modules have been recovered.

Factory admission follows captured module imports/exports and shared lexical
bindings, then matches the complete helper function against an authored
cache/exports factory. Its minified names differ (`T`, `C`, `R`) across
platforms; all match the same qualified structure. Eleven selection/helper
controls pass. An initial null-node handling failure for implicit bindings
and an earlier macOS-name assumption are preserved as failed runs. The final
locator uses structure and binding identities, without a platform-name table.

The active objective still includes original module/type evidence beyond this
dependency, full feature coverage, opaque native implementations, Bun revision
differences and all-readable execution on the remaining platforms. The eight
sealed development deliveries and their accepted build inputs are unchanged.

Linux x64 now also qualifies the all-readable layout. All 2,345 source modules
use readable text and all 2,589 module rows pass independent readback. The
result is 147,195,872 bytes, SHA-256
`d791d2b2fd6047966c05f4a92d80b371b7d0a9a8db6ce678f4d769559d82c00b`.
Three startup comparisons match the qualified baseline. Fresh original and
candidate runs pass the same seven configuration and 22 behavior comparisons,
without suppressing date text, missing fields, timeouts or protocol errors.
This extends layout qualification beyond macOS ARM64 while retaining the
same bounded feature scope; it does not add a universal equivalence claim.

macOS x64 and Linux ARM64 glibc now also pass complete all-readable module
readback, three startup checks and freshly paired seven configuration plus
22 behavior comparisons. The macOS x64 binary is 138,935,696 bytes, SHA-256
`263da018bd5b55e058d540f8a2e25e1126cf6d4831bc9fc2adc7642b1076d3c3`;
Linux ARM64 is 146,590,504 bytes, SHA-256
`167d6fa4c8b3a275b905029c64a1c7c40ba119f5d01b3a161c37b24830de82e7`.
The first macOS x64 version launch took 56.848 seconds under the qualified
Rosetta profile. This timing is retained rather than treated as performance
equivalence. Both musl layouts now also pass complete module readback, three
startup checks and fresh seven configuration plus 22 behavior comparisons.
The x64 musl binary is 140,716,272 bytes, SHA-256
`4615552b770636e03b8d7eba274550a477460536b463e975bbfbafca17be9682`;
ARM64 musl is 139,005,872 bytes, SHA-256
`71215ae8eea93f6247bb1456784c49ef6e8dcfbe2b3cd26813e47b2900eb1b6b`.

Both Windows all-readable cross-builds have also passed full 2,586-row
readback. The personal Actions workflow now builds the existing source
recovery owner from a pinned public NeverD commit, qualifies five owner tests,
and provides the tool with its matching CRT to each native runner. The runner
verifies the complete tool bundle, applies seven authored formatting controls,
and formats all 2,343 captured JavaScript modules. Native readable inventories
and two independent builds must match the local pinned results before four
startup and 29 offline comparisons. Windows ARM64 uses x64 emulation for the
analysis tool, while the client runtime and builds remain native ARM64.
All three jobs now pass in
[run 38852211205](https://github.com/gmh5225/test_mac_intel/actions/runs/38852211205)
at `0a49b20fcae098b7059d05b5a3d08269f682cb14`. The x64 readable binary is
151,655,936 bytes, SHA-256
`38da720c1a9d654236923b3c2fe8b6db828c2da22aec8ba35742606cc00645d1`;
ARM64 is 138,483,712 bytes, SHA-256
`8123783eeec8529b64465ddfba82f999d41f302385ed442e7b2dbe704bc752ca`.
Every original/readable source hash from the native formatter matches the
sealed local workspace. The complete baseline and editable probes pass again.
Captured source and client executables are not uploaded; a separate artifact
contains only the public formatter, licenses, owner-test result and runtime
dependencies. Native diagnostics and exact CI source are retained locally.

The early macOS ARM64 package combined readable runtime evidence with an
independent inventory of the edited probe. A new dedicated inventory now
checks the actual readable binary (`47e5d2aa...`) against all 2,609 selected
module rows, including 2,364 initial readable source hashes and every retained
asset. The existing successful runtime observations bind to that same binary
hash and are reused. A first finalizer attempt assumed a newer smoke-receipt
schema; it failed after the module comparison and remains recorded. Explicit
legacy-schema validation completes the receipt without changing the package.

All eight distributions now have bounded all-readable layout acceptance:
complete source/asset readback, 26 startup pairs and 232 configuration/behavior
pairs, separately from the earlier preserved-source layout results. Source
text, positions, performance, live services and universal equivalence remain
outside what these tests establish.

## Additional Read/Edit feature coverage

Fourteen authored Read/Edit cases now pass against the original and both the
preserved-source and readable-source rebuilds on Linux x64 and ARM64: 56
additional differential pairs across two architectures and two layouts. These
check UTF-8 line windows, missing files, directories, exact and global edits,
ambiguous matches, CRLF, combining characters and supplementary characters,
absent old strings, and the interaction of model selection and Read permission
with unread or ranged-read editing. Full protocol frames and API request
bodies retain the existing normalization rules; workspace contents have exact
byte oracles. No recovered implementation was changed to make a test pass.

A sequential-tool fixture is compiled and separately qualified with ten
authored controls on each architecture before captured clients run. Two
initial x64 expectations contradicted original behavior and are preserved as
failed tests. Source inspection and focused controls establish that the
tested newer model can edit an unread file when the relevant read permission
predicate permits it, while an older-model guard and explicit Read denial
reject the corresponding cases. An explicit offset/limit Read does not set
the token-cap truncation marker, so it does not imply the same edit refusal.
That truncation branch itself is not claimed as tested.

The final x64 evidence selects thirteen unchanged passing cases and one
focused rerun, retaining per-case provenance to the original failed runs.
ARM64 runs the corrected fourteen-case matrix in full for all three binaries.
Each ARM64 invocation shares only its pinned read-only executable across
cases; HOME, work, temporary state and network namespaces remain fresh.
Other platforms and tools such as Bash, Grep and Glob still require this
expanded coverage. The full recovery goal remains active.


## Eight-platform Read/Edit supplement complete

The same fourteen-case matrix now passes across all eight distributions and
both source layouts, for 224 additional differential pairs. The four Linux
packages, native macOS ARM64, Rosetta macOS x64, and native Windows x64/ARM64
have separately sealed diagnostics and full ordered protocol/API comparisons.
Exact workspace byte checks include CRLF and composed/decomposed Unicode.

[Windows run 38856660629](https://github.com/gmh5225/test_mac_intel/actions/runs/38856660629)
passes at `38d4a830b23b18ce3a36942c7b8f3313ceeb8908`. It revalidates the
independent native builds, source readback, previous configuration/behavior
matrix and new core-tool cases. Native source hashes use the exact Windows
CRLF checkout bytes; downloaded raw protocol records were independently
normalized and compared again without comparator changes.

The macOS x64 authored HTTP qualification initially exceeded its 30-second
bound. The retained retry adds a separate bounded independent Bun startup
before the server; all qualification controls and captured-client cases pass.
The existing pinned executable paths were reused with fresh per-case state;
a separate metadata addendum corrects an inherited receipt field that called
this a per-run copy. Original sealed evidence remains unchanged.

`progress-v20.json` is the active private checkpoint. Bash/Grep/Glob, token-cap
Read truncation, broader features, semantic/type/module recovery, native
implementations, Bun revision differences and source-text/performance
observability remain open. The complete recovery goal stays active.


## Confirmed native search gap and next repair

A new Linux x64 Grep content case finds `NEVERD_SEARCH_NEEDLE` in the original,
but both rebuilt source layouts report `No matches found`. Direct argv[0]
controls isolate the boundary: original `argv[0]=rg --version` reports
ripgrep 14.1.1 with PCRE2 10.45 and revision eecfd55deb. Independent rebuilds
instead report Claude Code, and reject `--no-config`. The recovered JavaScript
selects this multicall mode, so matching its source alone does not restore the
native runtime role. This is an open functional failure, not a passing case.

The first direct probe lacked execute permission on the immutable original;
its retry uses a hash-checked disposable executable copy. Both attempts and
the complete Grep wire observations are retained. The immediate repair path is
to independently build the measured ripgrep/PCRE2 versions, qualify authored
controls, and compare original embedded search with the client's existing
system-ripgrep selection. Embedded dispatch itself and the other native
runtime roles remain explicit gaps.

Seven additional token-cap Read cases pass on all six local distributions
and both layouts (84 pairs). They exercise real partial-cache and Unicode
boundary branches at an explicitly declared small cap; default-cap stress and
live counting are not claimed. Windows expansion is running in
[38862647678](https://github.com/gmh5225/test_mac_intel/actions/runs/38862647678).
`progress-v21.json` records these passing cases and the higher-priority search
failure together. Total goal status remains active.


## Native Windows token-cap supplement and managed Linux search repair

The seven token-cap cases now pass on all eight distributions (112 pairs).
Windows run 38862647678 succeeds at 3e032a1084e65f96f39407e8b51c96235e1cc591;
downloaded raw results were normalized again and sealed for both architectures.
This remains explicit small-cap coverage, not live counting or default-cap stress.

Linux x64 now has a runnable managed search supplement. The original embedded
search and both rebuilt layouts match thirteen Grep/Glob cases (26 pairs),
including malformed regex, Unicode, pagination and exclusion of external rg
configuration. An authored launcher selects the independently built ripgrep
14.1.1 / PCRE2 10.45 through the client's existing system-rg configuration.
The recovered JavaScript and previously sealed development packages remain
unchanged. Seven helper controls and six launcher controls pass before client
execution. Two native public-source builds produce byte-identical helpers.

The old Rust wrapper reported generated version constants instead of the
linked PCRE2 version. A recorded source overlay queries PCRE2_CONFIG_VERSION;
the native check still requires 10.45. An incomplete upload omitting four
hidden public metadata files was rejected and the upload fixed. A fixture
readiness timeout occurred before a preserved client ran; the unchanged
sequential retry passes. These failures remain in the evidence.

Two Glob results initially differ only in durationMs. The captured source
shows Date.now() subtraction at the result construction. A search-specific
comparator normalizes that field only at the exact six-field Glob protocol
result schema, retains the original numbers, and passes eleven mutation
controls for ordering, counts, truncation, schema and invalid timing values.
No file/content/error differences are excluded. Performance equivalence is
not claimed.

The managed runtime package has 615 verified files, both layouts, helper,
launcher sources, upstream source/crate archives, patch, and acceptance data.
Launchers rebuild byte-identically from the packaged source and both packaged
clients pass isolated version invocations. progress-v22.json records this
bounded repair and the still-open whole-recovery objective. Embedded rg
dispatch, apply-seccomp, other distributions and broader functionality remain
open. Native Linux ARM64 expansion is underway; its original thirteen-case
search baseline already passes.


## Both glibc Linux managed search packages checked

Linux ARM64 now repeats the same thirteen original-versus-managed search
cases for both layouts, adding 26 pairs (52 across both glibc platforms).
Before applying the adapter, both ARM64 rebuilds reproduce the actual false
no-match failure and their native argv[0]=rg probes enter the Claude CLI.
The original reports ripgrep 14.1.1 / PCRE2 10.45, NEON and JIT. The managed
component and launcher pass seven and six authored controls respectively.
The source-proven Glob timing rule is unchanged; a separate ARM64 source
anchor confirms the same Date.now subtraction.

The ARM64 public helper was rebuilt in a distinct native job on the same
commit in run 38868717218 attempt 2. Its binary and complete artifact file
contents match attempt 1. The runtime package has 640 verified files; both
launchers independently rebuild identically and both packaged clients pass
isolated version invocations. progress-v23.json references both checked
packages, all eight Read/Edit14 and token-cap7 supplements, and remaining
work; 172 referenced files were rehashed.

A literal callsite inventory distinguishes the next native boundary:
apply-seccomp uses ARGV0 in the shell environment and /proc/self/fd/3, not
necessarily the rg execve argv[0] path. Seven public sandbox-runtime 0.0.79
source/license files are retained at commit
0ded0c80bf8f44b64b30ad9e6bfc084cb9fc9dc5 with Git blob and SHA256 checks.
This is an unmatched upstream candidate, not a recovered implementation.
Musl/macOS/Windows managed search, native dispatch, syscall filtering and
wider functionality remain open. The overall recovery goal is incomplete.


## Four Linux managed search packages checked

Both musl distributions now pass the same thirteen Grep/Glob cases for both
source layouts, adding 52 pairs (104 across all four Linux distributions).
Their original baselines are fresh, and both unmanaged layouts reproduce the
actual false no-match failure before the repair. The comparator is unchanged;
separate captured-source anchors confirm the existing Glob timing rule.

The original x64 musl helper enables PCRE2 JIT, while ARM64 musl disables it.
The public build profile preserves this measured distinction. Pinned Rust
Alpine image manifests and SHA-256-pinned, signature-checked APKs supply the
toolchain; package installation and compilation run offline. Each helper has
no ELF interpreter or shared-library dependency and passes seven authored
controls in the corresponding musl boundary. The static launchers separately
pass six controls per distribution.

At c0c5a05d2609db30680e419e0cf66ced43f2a5fd, all four jobs succeed in
38872706376 attempt 2 and the independently dispatched 38874053349. The two
musl helpers reproduce byte-for-byte. Each public package has 85 identical
data files; only its receipt differs at the derived compiler image ID and the
three command references to that ID. All remaining receipt values match.
Both glibc helpers also retain their previously qualified binary hashes.

The first workflow rerun began before its artifacts were fully retained, and
the initial artifact list was replaced; that incomplete download is excluded
from repeatability evidence. The later x64 full transfer stalled, then one
range transfer failed. Complete ranges were retained, the missing range was
retried, and the assembled archive matched the GitHub API SHA-256 before
extraction and qualification. Transfer failures do not count as accepted data.

Each new musl runtime package has 649 verified files. Both launchers rebuild
byte-identically from packaged source and both packaged client layouts pass
isolated version invocations. progress-v24.json rehashes 200 referenced files
and keeps the recovery objective incomplete.

The next native boundary is now measured, not merely inferred from callsites.
On glibc x64 and ARM64, two authored environment controls precede no-command
ARGV0=apply-seccomp invocations. Originals enter helper usage; both rebuilt
layouts enter the ordinary Claude CLI. The first x64 readable probe times out
at 20 seconds; the retained focused retry with a 50-second bound completes in
19.71 seconds with the same CLI diagnostic. This proves role selection differs,
but does not qualify syscall policies, nested namespaces, command execution,
or correspondence with the public sandbox-runtime source candidate.

macOS/Windows managed search, embedded dispatch, apply-seccomp semantics,
broader tool paths, native extension internals and type/module recovery remain
open. No filtering bypass is used to turn the missing helper into a pass.


## Exact public seccomp filter correspondence

The public source history narrows the filter candidate to sandbox-runtime
9520430507ce3c354a71b1518e29e76121407ed1. With pinned libseccomp 2.5.4,
the unchanged generator produces 136-byte x64 and 120-byte ARM64 arrays
that occur exactly once, byte-for-byte, in each of the four Linux originals.
The newer candidate's filter differs and remains retained as a rejection.
All seventeen diagnostic literals in the selected public C helper occur in
all four originals, but this does not establish whole-function equivalence.

Both standalone static public helper candidates build and produce the
original no-command usage response in the previously qualified boundaries.
No positive command, namespace, supervision, optional observation, or kernel
filter behavior is accepted at this checkpoint. Native client dispatch is
still absent. The sealed source/data correspondence package retains public
sources and licenses, generator inputs, both candidates, the rejected filter
and original offsets/hashes. progress-v25.json keeps all prior evidence and
the whole recovery objective incomplete. Native Actions policy controls are
the next check; they cannot substitute for restoring client integration.


## Native public seccomp component controls

The pinned public helper now builds on native x64 and ARM64 runners and
reproduces the local helper binary hashes exactly. Two compilations within
each job also match. At 2d3891bfc1a4115a68efeddd244d0ae9c3e41583, run
38881785369 passes four jobs across Ubuntu 22.04 and 24.04. Each job records
eight syscall cases before filtering, after filtering, and after exec, plus
malformed-filter rejection. All five denied operations differ from their
native baseline, while three unaffected operations preserve their behavior.

Both Ubuntu 22.04 jobs execute the public helper's command path and check
exit status 37, policy results, fresh PID/user/mount namespaces, exactly two
visible processes, cleared effective/ambient capabilities, child signal exit
relay and missing-executable rejection. Ubuntu 24.04 denies setgroups in the
nested user namespace; that limitation is retained separately. No host policy
changes are applied. The 419-file seccomp-native-policy-v1 evidence package
retains public code/licenses, exact inputs, diagnostics and an independent
artifact verifier. progress-v26.json rehashes 207 referenced files.

A subsequent original-role comparison is being qualified in a separate
empty-root bubblewrap boundary. Its first preparation lacked file mount
targets in a read-only root. The second lost the dynamic loader execute mode
while copying dependencies; its authored exec driver returned 126, so the
original entry did not run. The premature execution flag in that historical
receipt is explicitly corrected in a separate failure record. The recipe now
retains runtime modes and requires an authored dynamically linked boundary
control before the original, and distinguishes an attempt from confirmed
entry. Neither failed attempt counts as native-role acceptance.

The source callsite opens /proc/self/exe with O_PATH, then invokes fd 3 with
ARGV0=apply-seccomp and closes fd 3 before the payload. A future managed
adapter can route that descriptor to a verified helper while preserving the
existing invocation and closure. This is a recorded integration candidate,
not an implemented or qualified client repair. The overall goal remains open.


## Managed helper descriptor integration candidates

Original-role acceptance now passes seven direct and four descriptor cases on
both native glibc architectures: 22 pairs. The caller contract uses O_PATH,
descriptor 3, ARGV0=apply-seccomp and /proc/self/fd/3; the controlled payload
confirms inheritance and successful close before work. At public helper commit
9b186a3737ddfa176f7c0650475e285ac7f9dc43, run 38910479754 passes all four
Ubuntu 22.04/24.04 jobs. Both static launcher variants and the helper reproduce
local hashes in every profile; each job builds them independently twice.

Separate x64 and ARM64 source workspaces now change only the descriptor
provider in module 236. The managed provider requires an absolute helper path;
the launcher selects its packaged helper and rejects unavailable, nonexecutable
or symlink dependencies. The original descriptor mapping, shell invocation
and close wrapper remain intact. Four client variants build and independent
NeverD readback matches all 2,589 modules/assets per variant. Preserved builds
change only module 236; all-readable builds match their actual source selection.

Ten authored launcher controls pass per architecture. Two instrumented CLI
experiments invoke the actual recovered provider: missing path rejects, the
selected descriptor resolves to the managed helper, the close wrapper matches,
and inherited fd 3 enters the real public helper usage path. The diagnostic
entry edit is kept separate from the delivered clients. Both ordinary layouts
start on each architecture, and all 52 original-versus-candidate Grep/Glob pairs
pass with the previously source-proven comparator unchanged.

Two separately sealed runtime-sandbox-linux-{x64,arm64}-candidate-v1 packages
contain both client variants, launcher/helper sources, source edit recipe,
public dependency inputs, independent readback, protocols and qualified
component observations. Their 1,109 and 1,160 files respectively rehash. Each
packaged preserved client rebuilds byte-identically from the previously sealed
editable development package. The base development packages remain untouched.

A ZIP transport stall is retained and recovered by checked 512 KiB ranges;
all ten current native artifacts match API SHA-256 and ZIP CRC before admission.
The evidence sealer initially selected local transport scratch directories with
a broad prefix; its pre-seal private-header guard rejected them. Those copies
were moved to a private diagnostic area and only exact artifact names were
admitted. No signed download header entered the sealed packages or a public
upload. An initial smoke probe likewise retained an artifact's missing execute
mode; a hash-checked owned executable copy resolves that test setup issue.

The actual complete Bash tool sandbox flow is still unqualified. External
signal forwarding, optional observation, full helper function correspondence,
original built-in engine dispatch and broader reconstruction work remain open.
progress-v28.json must preserve this distinction and must not complete the goal.
