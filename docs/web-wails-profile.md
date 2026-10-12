# Offline Wails stored assets

Two independent C++ profiles inspect stored Go `1.25.7` / Wails `2.11.0`
layouts. Both require `GOARCH=amd64`; the container and `GOOS` must match
the explicit selection:

| Profile | Container / required platform |
| --- | --- |
| `wails-2.11.0-go-1.25.7-linux-x64-elf-embedfs-v1` | ELF64 little-endian x64 `ET_EXEC` / `GOOS=linux` |
| `wails-2.11.0-go-1.25.7-windows-x64-pe-embedfs-v1` | PE32+ x64 executable / `GOOS=windows` |

Declarations select a stored layout; they do not authenticate a compiler,
framework, publisher or running application. Capabilities retain `profile`
as the original Linux selection and enumerate both in `profiles`. Cache
identity includes the explicit profile; a Linux request cannot reuse a
successful Windows extraction.

Detection, table extraction, HTML/source analysis and native loading are
separate results. This profile does not support every Wails executable.
Other Go/Wails versions, PIE, ELF/PE ARM64, PE32, DLL, Mach-O and universal images
are refused by this reader until their producer layouts are qualified.
Existing native loaders keep their independent format/architecture support.
Tauri assets and native bridges remain separate work.

## Ownership and evidence

`desktop/wails/Profiles` owns layout names, platforms and container selection.
`ELF` and `PE` own their respective native headers and address translation;
`Mappings` owns shared nonoverlapping file-backed permission/pointer checks.
`BuildInfo` owns aligned inline build-info framing and required
declarations. `Tables` owns structural recognition, ordered directory records
and portable namespaces. `Contents` owns payload overlap and stored digest
proof. `Reader` owns limits, identity and the atomic extraction transaction.
`SessionWails` only selects immutable inputs, caches completed extractions and
publishes bounded metadata. `ArtifactView` retains nested storage coordinates
and one namespace per table. Consumers do not reinterpret container records.

The PE profile requires an executable x64 PE32+ header with sixteen data
directories, a 64 KiB-aligned preferred base, standard power-of-two file/section
alignment and bounded headers/image size. Every section has a checked physical
span, RVA, virtual size and permissions. Raw spans and virtual mappings cannot
overlap; sections cannot substitute native headers for payloads. Only the
intersection of stored bytes and declared virtual size backs a pointer. File
padding, zero-filled virtual tails, writable content and writable executable
sections never become stored assets. Pointer values refer to the captured
preferred-image VAs; NeverD does not apply loader relocation or execute the
image. See the [PE specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
and the pinned [Go build-info reader](https://github.com/golang/go/blob/go1.25.7/src/debug/buildinfo/buildinfo.go).

A recognized table has an aligned self-referential slice header, nonzero
equal length/capacity, and a first name-string length of 1..4,096 bytes.
Unrelated Go type-data slices also use self-referential headers; incompatible
framing remains opaque in the original container. No symbol spelling or
framework guess selects a table. Once framing matches, every row must validate;
there is no partial table, best-effort skipping or truncated successful page.
An error in any recognized table publishes no extraction.

Rows are 48 bytes with two stored string headers and a 16-byte compiler
digest. Every pointer must resolve to exactly one admitted read-only physical
range. Directory rows have a trailing slash, no payload and a zero digest.
Names use Go's directory/base ordering and contain every declared ancestor.
The existing pinned native Unicode policy rejects unsafe components, device
names, traversal and portable case/normalization collisions. Names remain
private and are never opened as host paths. Missing path policy is an explicit
unavailable capability.

Payloads cannot overlap private names, table/build/native metadata or each
other partially. Exact payload aliases share hashing work. Empty files retain
their actual empty payload and its digest; directories never become empty
source selections. The full original image remains captured, including all
unrecognized or unreferenced bytes.

The compiler's 16-byte field is not ordinary SHA-256 truncation in this
release. For a file of at most 1,024 bytes, it derives from ordinary SHA-256
with the first digest byte XORed with `0xff`. Larger files use SHA-256 over
a leading byte `0x01` followed by the content. Both retain the first 16 digest
bytes. NeverD verifies this exact size-dependent contract and separately
computes ordinary full SHA-256 for every file. It does not try alternative
algorithms until one happens to match. See the pinned
[compiler file reader](https://github.com/golang/go/blob/go1.25.7/src/cmd/compile/internal/staticdata/data.go)
and [toolchain hash owner](https://github.com/golang/go/blob/go1.25.7/src/cmd/internal/hash/hash.go).

## Limits and publication

| Resource | Hard bound |
| --- | ---: |
| Captured input | 256 MiB |
| ELF program headers / admitted mappings | 1,024 / 128 |
| PE sections / admitted mappings | 96 / 128 |
| Tables / total rows | 32 / 4,096 |
| Aggregate declared file payload | 64 MiB |
| Charged private metadata per extraction | 2 MiB |
| Reader steps | 64,000,000 |
| Scanned bytes | 264 MiB |
| Cached extractions / aggregate charged private metadata | 4 / 8 MiB |
| Metadata page records | 1..128 |

Core qualification can lower limits, never raise them. Steps include native
mapping checks, discovery and admitted records. Hashing is bounded by captured
input and declared payload; exact aliases reuse their result. Publication
serializes the metadata summary before inserting a completed extraction.
Reimport invalidates tables, member selections and cached analysis.

Metadata returns opaque table/member identities, fixed suffix classifications,
byte lengths, container offsets, full file digests and extraction work. It
withholds paths, module/build-info bodies and raw content. Suffix classification
does not establish source encoding or valid JavaScript. Source consumers still
require an explicit script/module/commonjs choice and enforce their own bounds.

`table_coverage=matching_structural_framing_only` and
`filesystem_type_identity=not_verified` describe recognition separately from
`stored_digest_status=go_toolchain_hash128_match`.
`asset_server_binding=not_checked`, `native_bridge_analysis=not_analyzed`,
`runtime_activation=not_checked` and `producer_version_verified=false` remain
explicit. Multiple tables never satisfy each other's relative HTML/module
references. A native handoff uses original captured bytes; it does not restore
original Go source or prove backend equivalence.

## Interfaces and local disclosure

Always-exported additive C APIs are `neverd_web_wails_extract_json`,
`neverd_web_wails_records_json` and `neverd_web_wails_export_json`.
Extraction and pages are independent of the JavaScript parser. Backend omission
returns `capability_unavailable`. Worker/MCP expose the first two through closed
schemas; extra execution/path parameters and pages larger than 128 are refused.
Raw local export is excluded from worker/MCP.

```text
neverd web wails <root> <profile> [artifact-index]
neverd web wails-html <root> <profile> <artifact-index> <member-index>
neverd web wails-source <root> <profile> <artifact-index> <member-index> <source-type>
neverd web wails-native <root> <profile> [artifact-index]
neverd web wails-export <root> <profile> <new-output-directory> [artifact-index]
```

`wails-native` opens the existing native loader and reports counts; it does
not run the native pipeline or application. Native member selections remain
available through the independent handoff API where their bytes are supported.

Local export explicitly discloses raw bytes and embedded paths. It reuses
`ExportDirectory`: a fresh mode-0700 directory, exclusive no-follow mode-0600
files, fixed filenames and SHA-256 reread verification. It writes `original.bin`,
all stored file payloads and `manifest.json`. Names such as `m00007.js` come from
record indices and fixed classifications. Embedded paths are manifest data;
they never become output paths. Existing destinations and symlinks are refused.
The manifest records tables, original row/name/content offsets, file identities,
hashes and exact stored-byte status. Failed export can retain partial evidence;
it never publishes a successful manifest for an incomplete write.

## Measured capture

The optional corpus `NEVERD_WAILS_ZENTRO_030_ELF` names the bare Linux x64
[Zentro v0.3.0 release](https://github.com/alexnguyen03/zentro/releases/tag/v0.3.0).
Release bytes are not checked into NeverD and are never launched.

| Evidence | Value |
| --- | --- |
| Native bytes | 28,832,896 |
| SHA-256 | `ed0932139a8735f403f8c237234d5fb958b4cf5f0a571499475809d330768145` |
| Declared Go / Wails | `go1.25.7` / `v2.11.0` |
| Build-info offset | 27,987,728 |
| Table offset / rows | 19,580,128 / 14 |
| Directories / files | 3 / 11 |
| Stored payload bytes | 4,700,610 |
| JavaScript member / bytes | 7 / 1,552,742 |
| JavaScript offset | 16,727,680 |
| JavaScript SHA-256 | `550cdaae8e65f0cbd2a75132a0dc5073fc8a7883811d0d7abdd6a59f71bd0e1e` |

The pinned capture validates all directory records and stored digest fields.
The HTML member exposes one static script reference. Its root-relative
`/assets/...` URL remains `unsupported_local_url` with no linked member: the
captured table does not establish the application's asset-server mount root.
The native handoff opens the identical captured SHA and reports 36 sections
and 32,475 function records. Counts are loader evidence, not native recovery
or complete function semantics. The 1,552,742-byte JS asset is extracted intact
but whole-asset source analysis currently returns `source_byte_budget_exceeded`
under the independent 1 MiB source profile. Raw export does not erase that limit
or claim interactive parse success. The separate
[shared readable-source export](web-readable-export-profile.md) parses 449,790
nodes and publishes the complete 1,897,987-byte same-tree readable bundle
without entering the interactive caches. Authored fixtures independently exercise positive
source analysis, compressed origins and cross-table reference refusal.

Primary format references are the pinned
[Go embed layout](https://github.com/golang/go/blob/go1.25.7/src/embed/embed.go),
[compiler table writer](https://github.com/golang/go/blob/go1.25.7/src/cmd/compile/internal/staticdata/embed.go),
[build-info reader](https://github.com/golang/go/blob/go1.25.7/src/debug/buildinfo/buildinfo.go)
and [module framing](https://github.com/golang/go/blob/go1.25.7/src/cmd/go/internal/modload/build.go).
The C++ reader and authored fixtures are independent implementations; no Go or
Wails runtime/source dependency is installed or linked. The complete original
container and its stored license asset are preserved by local export.

## Independent Windows capture

`NEVERD_WAILS_ZENTRO_030_PE` selects the bare Windows x64 executable from the
same release. NeverD itself performs table extraction, raw/readable export and
native handoff with an unusable external-tool PATH. The application, installer,
Go runtime and Wails runtime are never launched.

| Evidence | Value |
| --- | --- |
| Native bytes | 29,747,136 |
| SHA-256 | `8c07c39c307c24d04be12019ef39210fc2360c0b40ffee8297deae9d1a0ee9e4` |
| Build-info offset / bytes | 28,108,288 / 5,570 |
| Table offset / rows | 19,725,376 / 14 |
| Directories / files | 3 / 11 |
| Stored payload bytes | 4,700,647 |
| JavaScript offset / bytes | 16,867,808 / 1,552,742 |
| JavaScript SHA-256 | `1b3a234dc7a96030d32d901baaf456771a0b63fc9c6f2cdfbb046980d618f79f` |
| Readable nodes / bytes | 449,790 / 1,897,987 |
| Readable SHA-256 | `d8ebf61ff86a156541432442d18cd82f5294dc104384e7887b1b7e9f3f94f013` |
| Native handoff sections / function records | 9 / 28,136 |

Windows and Linux assets are independently verified. Equal JS sizes/node
counts do not imply identical bytes or interchangeable evidence. The Windows
HTML's root-relative script remains unbound, as on Linux; the reader does not
infer a filesystem mount. The complete readable bundle has a matching reparsed
tree, not a claim of recovered erased types/names or complete runtime semantics.

Raw export rereads thirteen files / 34,456,736 bytes, with manifest SHA
`6dce4fac4a7f769ed09af43eee92ca30b6fcb18114b5c8d97afaf6630ff0f504`.
Separate readable export rereads three files / 3,452,863 bytes, with manifest SHA
`3e843ec978b4bf4adb7f8b4ac89100b6274ef7fa32ae0e734a7cddb849caf7d3`.
These outputs and public-release bytes stay outside Git. Matching architectures
on native Windows hosts, ARM64, Mach-O/universal Wails assets, additional
toolchain versions, asset-server bindings and native bridges remain unqualified.
