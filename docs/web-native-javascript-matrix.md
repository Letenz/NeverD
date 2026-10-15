# Native JavaScript extraction matrix

This matrix records the bounded #718 contracts and qualification evidence
through 2026-10-15.
All analysis code is C++; targets, embedded runtimes and package scripts are
never executed. SDK, CLI, worker and MCP use the same backend evidence.
Input target coverage and analyzer host qualification are separate.

| Adapter / versioned contract | Qualified input targets | Detection / inventory | Source and asset extraction | Maps | Bytecode / snapshots |
| --- | --- | --- | --- | --- | --- |
| [Bun 1.4.2](web-bun-profile.md) | Linux ELF64, macOS thin Mach-O64, Windows PE32+; x64 and ARM64 for each | Validated container and graph; producer version not authenticated | Yes, six real producer targets, original ranges/hashes retained | Raw preservation; twelve full producer images qualify bounded decoding with the native Zstandard 1.5.7 SDK; native LLVM Zstd is also supported | Raw JavaScriptCore caches preserved, decoding unsupported |
| [Bun `71d0d439` prelinked extension](web-bun-profile.md) | Eight captured Claude Code 2.1.296 npm platform packages: Linux, macOS and Windows x64/ARM64, including both supplied Linux libc variants | Flags 11/12, checked prelinked table and runtime-options record; producer version not authenticated | All 18,794 shipped JS units preserved across the eight inputs; independent archive/executable/member evidence | All eight inputs have no source maps; the baseline producer corpus separately qualifies map decoding | Prelinked cache opaque; flag 13 linked-bytecode layout refused |
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
tests. Ordinary local runs may explicitly skip unavailable large images.
The [required producer lane](web-producer-corpus-qualification.md) verifies the
fixed public corpus archive and requires all eleven registered producer cases;
missing inputs, consumers or test registrations fail. Tests never build or
execute a producer or target. The separate workflow supplies the pinned corpus.

The complete producer lane uses the macOS ARM64 Release reader, prebuilt LLVM
23.0.0 r4, native ICU/zlib and the pinned Zstandard 1.5.7 SDK. At
`5e126750ae7513591b6d79056c02358fe55da0f1`,
[run 38677375352](https://github.com/NeverSight/NeverD/actions/runs/38677375352)
passes all eleven producer cases with zero failures or skips. The separately
qualified [Windows Server 2025 x64 product](web-windows-product.md) passes 110
native cases, two adapter checks and six installed checks, including selected
Bun source/map/export consumers. That Windows selection does not qualify all
full producer images on Windows. Other analyzer hosts require separate evidence.
Raw source is excluded from worker/MCP replies.

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
The later [eight-platform qualification](web-claude-code-cross-platform-qualification.md)
and [per-input cross-track table](web-epic-acceptance.md#fixed-claude-code-cross-track-selections)
record all eight supplied npm packages independently. Missing desktop samples,
runtime traces and unselected analyses remain explicit limitations. Extraction
does not reconstruct original TypeScript, variable names, native Rust/Go
sources, server behavior or effective policy.
