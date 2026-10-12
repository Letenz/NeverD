# JavaScript epic acceptance audit

This audit records the implemented boundaries of #714–#718 on 2026-10-12.
The five issues' bounded initial scope and named supplement profiles are
implemented. Completion is assessed against the original criteria below, not
against a universal analyzer or every future runtime build. A passed fixture or
captured sample qualifies its selected profile and operation; it does not establish general
framework support, source equivalence, publisher trust or runtime behavior.
All new implementation and authored fixtures use C++. Target samples are never
executed, and the analyzer uses no external scripts, runtimes or network tools.

## Issue evidence and remaining work

| Issue | Implemented, reviewable evidence | Explicit supported boundary |
| --- | --- | --- |
| [#714](https://github.com/NeverSight/NeverD/issues/714) | Immutable source/map origins, bounded C++ parser, lexical/value/effect/module/bundle/navigation models, reversible primitive/rename/Boolean-expression profiles, reviewed source views and verified same-tree readable export. Known tool/config/permission/hook/MCP fixtures retain spans and uncertainty. Configuration-object order includes loading initializer links and unknown-key overwrite barriers; real Claude units retain 273 objects, 2,279 terms and 19 possible later overwrites. | Separate file layers, other merge forms and computed dispatch are outside the named grammar and remain unresolved. The issue requires identifiable source navigation and explicit unknowns, not effective policy or universal dataflow. Erased TypeScript/types/names and whole-program equivalence are not promised. |
| [#715](https://github.com/NeverSight/NeverD/issues/715) | npm graphs/diffs, tar/gzip inventory, original-byte SRI, Node behavior/install associations and diffs, advisory/registry/provenance imports, assessments, persistent dispositions and Qt review. Eight Claude packages retain independent archive/payload integrity. | npm lockfile versions and the literal Node installation grammar are named; pnpm/Yarn, complete runtime resolution and other GUI selectors are further adapters/workbench extensions. The GUI preserves selected confidence/coverage/dispositions, with unselected evidence explicit. Hashes/signature declarations prove no publisher trust or benign behavior. |
| [#716](https://github.com/NeverSight/NeverD/issues/716) | Safe ASAR/ZIP, Electron unpacked/manifests/entries/IPC, NW.js/VSIX/HTML/native handoff. Real Wails Go 1.25.7 / Wails 2.11.0 ELF/PE x64 and measured Tauri PHF/Mach-O x64 assets have exact raw/readable exports and native links. Tauri yields 15 assets and 9 readable JS members. Direct Tauri/Wails bridge syntax retains private source anchors. A named supplied CEF/WebView resource descriptor links and exports local assets/helpers, including opaque resources. | Other Rust/Go layouts, ISAs, codecs, bridge aliases and runtime mount/window relations need independent profiles. The CEF/WebView descriptor is supplied local evidence, not automatic framework authentication or a `.pak` decoder. The issue asks for supported versioned layouts and clear refusal of unsupported builds. |
| [#717](https://github.com/NeverSight/NeverD/issues/717) | Redacted HAR preview/commit, fetch/WebSocket/XHR/wrapper candidates, request/result and nearby JSON/base64/URI/crypto/zlib syntax links, direct-fetch HAR joins and versioned streams/logs. Selected reports retain source features, recorded links, separate same-MCP-method candidates and observed JSON type summaries with example IDs, unknown optionality/server constraints and pagination. | General value propagation, aliases, dynamic transformations and authenticated API identity remain unknown. Same-method spelling does not establish a session, response or source execution. Complex Claude dispatch stays unresolved; no real transcript was supplied or invented. OpenAPI is optional and is not claimed by the metadata report. |
| [#718](https://github.com/NeverSight/NeverD/issues/718) | Independent Bun ELF/thin Mach-O/PE x64/ARM64 and Node SEA 22.15.0, nexe 4.0.0-rc.7, pkg 5.8.1 profiles; producer fixtures, exact original/member/range hashes, bounds, opaque caches and shared consumers. All eight Claude 2.1.296 deliveries preserve every shipped JS unit and verified readable copy. | New runtime/layout/host profiles require independent measurement. Opaque JSC/V8 caches remain explicitly unsupported, as the issue permits. Extraction is separate from native lifting and complete cross-track runtime behavior. |

The [support matrix](web-support-matrix.md) is authoritative for individual
profiles. The [implementation ledger](web-analysis-implementation.md) records
build/test receipts and their exact baselines. This audit does not change issue
state or replace those narrower contracts.

## Fixed Claude Code cross-track selections

The input release is **2.1.296**, captured from the npm platform-package
delivery channel. [The platform qualification](web-claude-code-cross-platform-qualification.md)
pins each original tarball and executable SHA-256, platform/container profile,
source totals and export manifest. It is a fixed observed release, not a claim
about the latest release. No sample or recovered source is redistributed.

`pass` below means the stated extraction operation passed; `partial` means the
selected analysis exposes narrower evidence and documented unknowns;
`not attempted` means that cross-track analysis was not qualified for this
particular input. These are per-input operation states, not issue completion.

| Selected platform package | #718 exact JS extraction/export | #714 source analysis | #715 provenance | #716 desktop wrapper | #717 passive mapping |
| --- | --- | --- | --- | --- | --- |
| linux-x64 | pass | partial | partial | not attempted | partial |
| linux-arm64 | pass | partial | partial | not attempted | not attempted |
| darwin-x64 | pass | partial | partial | not attempted | not attempted |
| darwin-arm64 | pass | partial | partial | not attempted | partial |
| win32-x64 | pass | partial | partial | not attempted | not attempted |
| win32-arm64 | pass | partial | partial | not attempted | not attempted |
| linux-x64-musl | pass | partial | partial | not attempted | not attempted |
| linux-arm64-musl | pass | partial | partial | not attempted | not attempted |

All 18,794 shipped JS units have exact decoded bytes and readable copies with
matching reparsed structure. Counts include duplicate content across platforms.
Source navigation and other semantic analyses remain bounded selections, not
full application recovery. All eight inputs have no source maps. JSC cache
bytes are preserved separately with unsupported decoding semantics.

All original archive SHA-512 declarations match their selected archive bytes.
Registry/signature/provenance declarations retain separate verification states;
publisher identity and installed dependency trees remain unverified. The npm
wrapper assessment and linux-x64 VSIX-to-Bun qualification concern separately
pinned inputs, not an inferred desktop wrapper around every platform package.

The passive selections include the pinned linux-x64 module 1897 and macOS ARM64
modules 928, 1905 and 1920. The new wrapper profile refuses to invent caller flow
for the complex dispatch in modules 1905/1920; their four fetch occurrences keep
exact anchors and dynamic URL/method/options limitations. No HAR or transcript
was supplied for these selections, so source/capture correlation is not attempted.

## Criterion coverage

The scope above implements each original acceptance item. Additional builds,
runtime resolution and workbench workflows are profile extensions; absent
optional samples are recorded omissions, never passed acceptance cases.

| Issue criteria | Owning checks and published contract |
| --- | --- |
| #714 syntax/bundler/map/transform matrix | [Support matrix](web-support-matrix.md), parser/source-map/bundle tests and each rewrite profile |
| #714 benign originals/mappings and negative semantics | `WebSource*`, `WebSourceValues`, effect/binding/rewrite/rename/branch and export tests cover original preservation, coercion/effects/getters/exceptions/eval/with, invalid maps and exact/short budgets |
| #714 deterministic IDs, limits, SDK/CLI/MCP and validation meaning | Source/navigation/anchor/view/SDK and worker/MCP tests; exact inverse and same-tree checks are separate from runtime/whole-program equivalence |
| #714 tool/config/permission/MCP/hooks and sensitive-field supplement | `WebSourceFeatures`, `WebConfigurationOrder`, client corpus and bridge/navigation tests; [client](web-client-feature-profile.md), [order](web-configuration-order-profile.md), shared preview/redaction policies |
| #715 reproducible versioned graph and conditional/unresolved edges | `WebPackages`/package SDK fixtures and [package profile](web-package-profile.md) |
| #715 legitimate/suspicious/synthetic behavior and exact version diff | Node behavior, package behavior/install/diff suites retain occurrences, source anchors, uncertainty and explicit version/platform selections |
| #715 advisory freshness, integrity/provenance contradictions and no execution | Advisory/attestation/integrity/archive/assessment tests and their independently versioned profiles; invalid, missing, unselected and unverified states remain distinct |
| #715 GUI/CLI/MCP confidence, incomplete coverage and dispositions | Assessment/review SDK/transport tests plus `NeverDGuiWebReview`; fixed evidence and caller assertions stay separate and journals restore atomically |
| #715 supplied native distribution supplement | `ClaudeDistributionTests`, installer/delivery/integrity tests and the pinned cross-platform manifest retain OS/ISA/libc, package/executable links, matching/different/missing hashes and independent signature states |
| #716 matrix, ASAR/unpacked/NW and Tauri/Wails supported/native handoff | Desktop/ASAR/Electron/Wails/Tauri suites and real fixed-profile releases; [resource descriptor](web-desktop-resource-profile.md) covers named supplied CEF/WebView assets |
| #716 hostile archives, reproducible origin/export, offline and opaque data | Archive/ZIP/ASAR/path/export suites; native/readable SDK checks preserve exact compressed or direct parent ranges and explicit unsupported members |
| #716 VSIX/helper/version/redaction supplement | ZIP/desktop SDK and pinned VSIX-to-Bun checks retain separate extension/helper versions; normal pages exclude private content |
| #717 literal/template anchors, static/HAR-only/correlated classes | Source/XHR/wrapper/interface/relationship and passive report SDK tests; existing HTTP equality rule remains authoritative |
| #717 canaries, malformed/missing/WS/budgets, offline and stable navigation | HAR/stream/source/SDK/CLI/worker/MCP checks; no raw values, implicit network, target runtime or manufactured offsets |
| #717 exported example/schema boundaries and stream/source supplement | `WebPassiveReport`/`WebPassiveSource`, stream framing/recorded-ID fixtures and [report](web-passive-report-profile.md) retain observed types, source domains, unknown constraints and only qualified candidate joins |
| #718 matrix and primary pinned plain/minified/assets/maps/cache fixtures | Bun/SEA/nexe/pkg suites and redistributable producer graph/blob manifests; each format has independent version/platform/profile coverage |
| #718 reproducible ranges, hostile input, source frontend and opaque caches | Extraction/map/native/source/export suites refuse truncated/unknown/compression-bomb data and retain unsupported cache evidence |
| #718 fixed Claude manifest and per-track states | [Eight-platform qualification](web-claude-code-cross-platform-qualification.md) and the per-input table above; all 18,794 shipped JS units preserved, absent observations/maps and partial source analyses explicit |

Final build, omission, transport, GUI and integration receipts are recorded in
the [implementation ledger](web-analysis-implementation.md). This document
records implementation acceptance, not a GitHub issue-state mutation. Historical
increment receipts that said the epics were incomplete remain historical evidence.

The earlier Tauri source research remains relevant but separate from the
new binary qualification. At the pinned
`tauri-cli-v2.12.2` source tag, the framework crate is `2.12.3` and enables
compression by default. [Pinned crate manifest](https://raw.githubusercontent.com/tauri-apps/tauri/tauri-cli-v2.12.2/crates/tauri/Cargo.toml).
The generator emits included output bytes;
the asset object stores a PHF map from strings to byte slices. Its selected
compression feature controls Brotli decoding versus borrowed bytes, without a
per-entry codec field. [Generator](https://raw.githubusercontent.com/tauri-apps/tauri/tauri-cli-v2.12.2/crates/tauri-codegen/src/embedded_assets.rs),
[asset implementation](https://raw.githubusercontent.com/tauri-apps/tauri/tauri-cli-v2.12.2/crates/tauri-utils/src/assets.rs).
Rust's default representation also does not guarantee declaration-order field
offsets. [Rust layout reference](https://doc.rust-lang.org/reference/type-layout.html#the-rust-representation).
Therefore each additional binary profile must measure layout, stored pointer anchors,
relocations and encoding against a real preserved build. These source observations
are research evidence; the qualified reader above uses its own measured native
initializer, complete PHF relation and explicit Brotli profile. Neither is a
binary-version detector or permission to guess codecs and carve source strings.
