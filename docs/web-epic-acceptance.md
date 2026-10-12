# JavaScript epic acceptance audit

This audit records the implemented boundaries of #714–#718 on 2026-10-12.
The five-issue objective is **not complete**. A passed fixture or captured sample
qualifies its selected profile and operation; it does not establish general
framework support, source equivalence, publisher trust or runtime behavior.
All new implementation and authored fixtures use C++. Target samples are never
executed, and the analyzer uses no external scripts, runtimes or network tools.

## Issue evidence and remaining work

| Issue | Implemented, reviewable evidence | Remaining acceptance boundary |
| --- | --- | --- |
| [#714](https://github.com/NeverSight/NeverD/issues/714) | Immutable source/map origins, embedded bounded parser, lexical/value/effect/module/bundle/navigation models, separate reversible primitive/rename/Boolean-expression profiles, reviewed source views and verified same-parser-tree readable export. Client syntax candidates cover tool/config/permission/hook/MCP spellings with fixed confidence and unresolved fields. | Configuration precedence and broader dispatch/dataflow need explicit grammars and known-answer fixtures. Local transformations and formatting do not restore erased TypeScript, types, names or establish whole-program equivalence. |
| [#715](https://github.com/NeverSight/NeverD/issues/715) | npm graphs/diffs, tar/gzip inventory, original-byte SRI, bounded Node behavior/install associations and diffs, captured advisory/registry/provenance metadata, composed assessments, persistent dispositions and selected-evidence Qt review. All eight Claude platform packages have independent archive/payload integrity evidence. | Broader GUI evidence selectors, lifecycle/loader/host qualification and remaining delivery relationships. Signature declarations and matching hashes do not authenticate a publisher or establish benign behavior. |
| [#716](https://github.com/NeverSight/NeverD/issues/716) | ASAR/ZIP safety, Electron unpacked/member/manifest/entry/IPC boundaries, NW.js/VSIX/HTML/import-map candidates and native handoff. Real Wails Go 1.25.7 / Wails 2.11.0 Linux ELF and Windows PE x64 releases provide exact assets, raw/readable export and retained native binaries. The measured Tauri PHF/Mach-O x64 initializer profile extracts all 15 assets of a preserved public release, verifies 9 readable JS members and retains original native handoff through nested old-GNU archive evidence. Direct Tauri 1/Wails 2 bridge syntax now has redacted source/call/binding pages, with a real Tauri IPC call linked to its decoded range and compressed parent. | Broader Tauri Rust layouts/containers/ISAs and encoding selection, bridge aliases/imports/native-target links, broader Electron/NW.js distributions, desktop GUI selectors and runtime mount/window associations remain unqualified. The bounded Tauri-and-Wails asset/native-handoff criterion has concrete evidence; the complete desktop epic remains open. |
| [#717](https://github.com/NeverSight/NeverD/issues/717) | Redacted HAR preview/commit, direct fetch/WebSocket/XHR/member spellings, single-layer lexical fetch parameter forwarding, separate request/result syntax links and direct-fetch-only HAR joins. Versioned SSE/JSONL/JSON-RPC/MCP-shape/log imports preserve recorded directions/IDs and partial framing. | General request/config dataflow, nearby transformation chains, source/stream joins, remaining inventory/review/schema presentation. Real complex Claude dispatch remains unknown; no real transcript was supplied for qualification and none is invented. |
| [#718](https://github.com/NeverSight/NeverD/issues/718) | Independent pinned Bun ELF/thin Mach-O/PE x64/ARM64 profiles and Node SEA 22.15.0, nexe 4.0.0-rc.7 and pkg 5.8.1 profiles; producer fixtures, original/member/range/hash links, bounds, opaque-cache preservation and native/shared-source consumers. All eight Claude 2.1.296 platform deliveries have complete shipped-JS preservation/readable exports. | Additional runtime/layout/host profiles need their own measured qualification. Opaque JSC/V8 caches stay unsupported; all-platform extraction is separate from native lifting and complete cross-track analysis. |

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

## Next concrete acceptance work

1. Extend desktop extraction only through independently measured profiles and
   finish remaining desktop review/bridge relationships. The first
   [Tauri asset profile](web-tauri-profile.md) now qualifies a complete selected
   table from explicit native LowIR initializer evidence; it does not establish
   a generic Rust ABI, compiler identity, runtime binding or other architectures.
2. Extend request and configuration flow only through separately versioned,
   evidence-linked grammars. Use actual Claude refusals to select useful cases;
   keep computed dispatch, unknown effects and absent observations explicit.
3. Complete the remaining consumer/presentation boundaries using shared backend
   IDs and redaction, then rerun an issue-by-issue acceptance audit. Do not close
   the five epics based solely on extraction or parser success.

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
