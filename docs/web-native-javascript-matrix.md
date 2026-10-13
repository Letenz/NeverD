# Native JavaScript extraction matrix

This matrix records the bounded #718 contracts qualified on 2026-10-11.
All analysis code is C++; targets, embedded runtimes and package scripts are
never executed. SDK, CLI, worker and MCP use the same backend evidence.
Input target coverage and analyzer host qualification are separate.

| Adapter / versioned contract | Qualified input targets | Detection / inventory | Source and asset extraction | Maps | Bytecode / snapshots |
| --- | --- | --- | --- | --- | --- |
| [Bun 1.4.2](web-bun-profile.md) | Linux ELF64, macOS thin Mach-O64, Windows PE32+; x64 and ARM64 for each | Validated container and graph; producer version not authenticated | Yes, six real producer targets, original ranges/hashes retained | Raw preservation; bounded decoding needs native LLVM Zstd | Raw JavaScriptCore caches preserved, decoding unsupported |
| [Bun `71d0d439` prelinked extension](web-bun-profile.md) | Real Claude Code Linux x64; other five container extensions have synthetic boundary tests only | Flags 11/12, checked prelinked table and runtime-options record | Yes for qualified Linux x64 sample; cross-target producer qualification remains absent | Raw preservation; same separate decoder boundary | Prelinked cache opaque; flag 13 linked-bytecode layout refused |
| [Node SEA 22.15.0 LE64](web-sea-profile.md) | Preparation blob plus real injected Linux ELF64, macOS thin Mach-O64 and Windows PE32+, each x64/ARM64 | Explicit blob or native-resource profile; runtime activation not inferred | Stored CommonJS and assets; snapshot-only resources expose no invented source | No SEA-specific map reconstruction contract | V8 cache and snapshot preserved as opaque artifacts |
| [pkg 5.8.1 uncompressed VFS](web-pkg-profile.md) | Real pkg-fetch Node 18.5.0 Linux/macOS/Windows bases, each x64/ARM64 | Fixed loader/prelude envelope; native prefix and target labels not authenticated by this reader | Typed content stores, files and native members; links never followed | Selected stored map content uses the shared map consumer | Cache store opaque; cache/source equivalence not checked |
| [nexe 4.0.0-rc.7 EOF resources](web-nexe-profile.md) | Real Node 14.15.3 Linux/macOS/Windows x64 bases; ARM64 producer builds unqualified | Fixed EOF/bootstrap/resource envelope; native prefix not authenticated | Bootstrap and resource bytes, with checked source/native/package selection | Selected resource maps use the shared map consumer | No bytecode decompilation or snapshot recovery claim |

All rows retain original container hashes and exact partition/member origins.
Unsupported compression, malformed or ambiguous tables, overflow, truncation,
unsafe paths and exhausted budgets refuse rather than becoming guessed source.
pkg and nexe do not inherit Bun format or platform validation. Universal
Mach-O, 32-bit, big-endian and other runtime layouts require named contracts.

The producer corpora are recorded independently of the production readers:
[Bun](../unittests/web/fixtures/bun/README.md),
[SEA](../unittests/web/fixtures/sea/README.md),
[pkg](../unittests/web/fixtures/pkg/README.md) and
[nexe](../unittests/web/fixtures/nexe/README.md).
They cover plain/minified input, stored resources and applicable cache/map
variants; pkg additionally pins separate original/stored BOM and shebang bytes.
Core C++ hostile fixtures and exact original-input oracles remain the primary
tests. Optional large producer artifacts are selected explicitly, never fetched
or built by tests. Missing artifacts produce explicit skips.

The qualified analyzer host is macOS ARM64 Release, with prebuilt LLVM 23.0.0
r4 and native ICU/zlib. This LLVM distribution lacks native Zstd: compressed
map decoding cases skip while raw map preservation and capability/refusal
checks still run. Native Linux/Windows host capture and distribution are not
qualified by cross-target input tests. Local export uses the documented POSIX
writer; raw source is excluded from worker/MCP replies.

## Fixed-version Claude Code cross-track status

The [qualification record](web-claude-code-qualification.md) contains exact
release/channel/download coordinates, sizes, SHA-256 values, native layout
evidence, toolchain details and local result locations. All selected product
artifacts below are version **2.1.296**; the two containing distributions and
their embedded helper remain separate identities. Status applies to the stated
operation, not an entire epic. No transcript or desktop artifact was invented.

| Selected artifact | #718 extraction | #714 JS navigation | #715 package provenance | #716 wrapper handling | #717 passive mapping |
| --- | --- | --- | --- | --- | --- |
| Official standalone `linux-x64`, SHA-256 `24972e3b…3370d0de` | **pass**: 2,589 modules, 10,005 preserved regions | **partial**: all 2,345 stored JS units have byte-preserving/reparse-verified recovery; selected lexical navigation, primitive rewrites, reversible renaming and Boolean branch selection; actual record/byte refusals retained; feature-specific understanding remains incomplete | **partial**: official manifest hash comparison; publisher signature and build provenance unverified | **not attempted**: this input is a standalone executable | **not attempted** for end-to-end protocol mapping; no supplied transcript/capture |
| npm wrapper, SHA-256 `f6c375d5…3bd670b5` | **not attempted**: wrapper has no selected native executable; platform payload is separate below | **partial**: both supplied JS members analyzed; unresolved calls retained | **partial**: original tgz SRI matches captured registry declaration; graph/scripts/platform constraints and eight Node-call patterns; one admitted lifecycle entry associates six calls, another command refuses and 13 module requests remain unresolved; selected comparison with 2.1.295 described below; no advisory or publisher authentication | **not attempted**: npm delivery is handled by the package track | **not attempted** |
| npm `linux-x64` payload, SHA-256 `eef5a2e2…1e4fd0e1` | **pass**: native member matches the standalone bytes and Bun inventory | **partial**: helper bytes link to the qualified standalone source; no claim of all package entry behavior | **partial**: original tgz SRI and native-member equality verified; signature/build provenance unverified | **not attempted**: npm delivery is handled by the package track | **not attempted** |
| Marketplace `linux-x64` VSIX, SHA-256 `31176c4a…99a3d11` | **pass** for selected embedded helper: exact standalone equality and Bun inventory | **not attempted** for extension-specific JS navigation; helper evidence is separate | **partial**: manifest/version and helper equality; publisher signature/build provenance unverified | **pass** for bounded ZIP/member extraction and the selected VSIX manifest/main-file relationship; extension activation unverified | **not attempted** |

Separately, NeverD compared the hash-pinned **2.1.295** npm wrapper against
**2.1.296** through its original archive readers and selected source behavior
owner. Both selected JS members have identical supplied bytes; six per-source
pattern groups have unchanged recognized counts and retain 16 original call
anchors. This comparison covers the wrapper sources, not either native
payload or runtime behavior. The qualification record pins both inputs and the
actual CLI report; no package script or external analyzer ran.

The standalone-to-container byte equality concerns one platform/release only. An SRI or
manifest match does not authenticate a publisher or establish benign behavior.
Missing desktop samples, runtime traces, feature-specific client navigation,
advisories and other platforms remain visible gaps. These extraction results
do not complete the other four epics or reconstruct original TypeScript,
variable names, native Rust/Go sources, server behavior or effective policy.
