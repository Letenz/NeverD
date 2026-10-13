# Offline Tauri PHF asset profile

`tauri-phf-0.10-macho-x64-five-store-brotli-v1` selects one explicit native
initializer address in an immutable thin Mach-O x64 image. It extracts the
complete selected PHF string-to-byte-slice table and decodes every Brotli
stream in process. It never launches the application, uses a script/runtime,
invokes a codec utility, reads a sidecar or contacts a network service.
This is a measured storage/initializer profile, **not** a framework or compiler
version detector. Other Rust representations, initializers, encodings,
containers and ISAs remain unsupported.
The first profile also refuses Mach-O chained fixups; its stored pointers are
ordinary preferred-address bytes, independently translated by loader mappings.

## Native evidence and ownership

The SDK joins the captured native bytes with the existing loader and
`pipeline/evidence/ConstantStores`. The latter uses the authoritative decoder,
instruction relocation roles and LowIR lift. It derives one to sixteen
consecutive full-width eight-byte constant stores through the same unchanged
symbolic register base. Only COPY/ADD/SUB/STORE effects are admitted; unknown
operations, loads, control flow, partial writes, atomics, non-default memory
spaces, unavailable values or nonconsecutive fields refuse atomically.
Numeric address-looking values never acquire pointer provenance.

The selected start must lie on an instruction boundary decoded from a
loader-recorded function entry. Heuristic prologue guesses are insufficient.
Native admission bounds are 65,536 symbols, 65,536 bytes from the recorded entry,
32 selected instructions / 256 bytes and one million logical work steps. The
packet cannot cross the next recorded entry. This proves selected local
instruction effects, not reachability, successful execution, a runtime object
type or final memory contents.

The Tauri profile selects exactly five ordered roles: displacement pointer,
displacement count, entry pointer, entry count and PHF hash seed. Only the two
pointers may carry exact native address provenance. The native adapter retains
the original image hash, instruction coordinates and file-backed mappings;
Web owns table/path/codec admission and does not depend on the native pipeline.
Transport requests cannot supply this internal descriptor.

## Table and stream admission

Displacements are little-endian `(u32, u32)` pairs. Entries are four
little-endian eight-byte fields: UTF-8 key pointer/length and compressed byte
pointer/length. All ranges must map unambiguously into readable, captured file
bytes. Virtual zero-fill and conflicting mappings refuse. This permits a
writable data mapping in the original file and does **not** assert runtime
immutability. Metadata, selected code and payload ranges may not overlap;
exact whole-payload aliases are permitted and charged per occurrence.

Every key must look up to its actual row with PHF 0.10's SipHash13-128 keys
`(0, seed)` and wrapping 32-bit displacement arithmetic. All rows must pass;
there is no string carving or partial table publication. The seed is a public
hash-table parameter, not publisher authentication or a secret credential.
Keys retain one asset-root `/` privately; their remaining names pass the
shared ICU 77.1 NFC/casefold portable namespace policy. Traversal, repeated
separators, device names, Unicode/case aliases, duplicate entries and file
ancestors refuse. Keys never become host filenames.

The explicitly selected codec is RFC 7932 Brotli; there is no compression guess
or per-entry codec inference. CMake optionally locates exact **static Brotli
1.2.0 decoder/common archives** using pkg-config. The wrapper checks the linked
decoder version, excludes the large-window extension, uses accounted private
allocations and feeds at most 4 KiB input/output per stream call. It checks
output, call, memory and shared deadline bounds. Truncated streams, decoder
errors, trailing bytes and no progress refuse. This deadline is checked between
native calls, not a preemptive interruption of a decoder call.
Without the pinned decoder or path policy, Tauri operations are unavailable;
there is no external fallback. Original authors/licenses are retained under
`LICENSES/brotli` and `LICENSES/phf`.

Limits: 128 MiB original image; 128 mappings; 4,096 entries/displacements;
4,096-byte keys; 8 MiB stored / 16 MiB decoded per member; 32 MiB stored and
decoded per extraction; 2 MiB private path metadata; one million reader steps;
65,536 codec calls; 64 MiB codec allocations; five seconds shared codec deadline.
Custom limits may only lower these ceilings. Complete output is sealed into
one private unlinked spool before publication. Two session extractions retain
at most 64 MiB decoded bytes. Import replacement revokes all selections.

## Shared consumers and local disclosure

`ArtifactView` provides exact decoded blobs to source, readable export, HTML,
package and native consumers. Each initializer table has its own namespace;
separate tables do not satisfy each other's imports. Decoded coordinates are
relative to each Brotli frame's expanded stream and retain the compressed
frame hash/range plus the full containing origin chain. No decoded character
is assigned a compressed byte position. Archive-to-native-to-asset selection
retains tar/gzip original/expanded coordinates independently.

SDK operations are `neverd_web_tauri_extract_json`, `..._records_json` and
`..._export_json`. Worker/MCP expose only extract and metadata pages, with closed
arguments: revision, artifact ID, listed profile and canonical decimal-string
`initializer_address`. Pages allow at most 128 records. Private keys and content
do not appear in ordinary metadata. Raw export remains explicit local SDK/CLI
disclosure through the shared exclusive writer: `original.bin`, fixed
`m00000.br`/decoded extension files and `manifest.json`, all reread by SHA-256.
Private keys are manifest data only. Existing destinations are refused.

```text
neverd web tauri <root> <profile> <initializer-address> [artifact-index]
neverd web tauri-export <root> <profile> <initializer-address> <new-directory> [artifact-index]
neverd web tauri-source <root> <profile> <initializer-address> <artifact-index> <member-index> <script|module|commonjs>
neverd web tauri-html <root> <profile> <initializer-address> <artifact-index> <member-index>
neverd web tauri-readable-export <root> <profile> <initializer-address> <artifact-index> <member-index> <script|module|commonjs> <new-directory>
```

CLI initializer addresses accept hexadecimal or decimal. Source mode is a
caller selection; filename suffixes are private kind candidates. Readable
export retains its independent parser/output budgets and same-parser-tree
qualification. It does not recover erased names, types or TypeScript.

## Preserved public qualification

The actual input is lencx/ChatGPT **v1.1.0**, published 2023-08-03, selected from
[its release](https://github.com/lencx/ChatGPT/releases/tag/v1.1.0). This is a
fixed third-party release, not the latest Tauri framework or an official OpenAI
application. Its tag declares a Tauri dependency range beginning at 1.3.0;
no lockfile or authenticated compiler/framework version is supplied.
[Tagged manifest](https://github.com/lencx/ChatGPT/blob/v1.1.0/src-tauri/Cargo.toml).
Release signatures are not verified and the release metadata has no SHA digest.

| Original input / measured boundary | Pin |
| --- | --- |
| `ChatGPT_1.1.0_macos_x86_64.app.tar.gz` | 5,074,405 bytes; SHA-256 `781d341bcc5f2ba8cc269c2f3fcbc5d4f0818af42c8dd99c5c00a3688311c009` |
| Old GNU basic tar expanded stream | 9,440,256 bytes; SHA-256 `e54d5731d3cdf5da98af18ee77cb48dc74f92d5106ff24f13ae5fbe52ce025e7` |
| Explicit archive member 2 | Expanded offset 1,536; 8,784,888 bytes; SHA-256 `351cffd71cc63205dda57990b1432d124150bd625ae886fc577a03e49e9bed92` |
| Selected native initializer | VA `0x1000135a8` through `0x1000135dc`; file offset 79,272; 8 instructions / 5 stores |
| PHF displacements | VA `0x10048e484`; file offset 4,777,092; 3 pairs |
| PHF entries | VA `0x100808058`; file offset 8,421,464; 15 rows |
| Exact compressed / decoded bytes | 946,884 / 2,700,272; all 15 lookups and streams validate |
| Stored kinds | 9 JS, 1 HTML and other CSS/image assets; suffix candidates only |

The entire selected table has exact frame/decoded exports. All nine shipped JS
members have independent same-parser-tree readable exports in the optional SDK
regression. The native image remains available through the same shared native
handoff. Tests never execute the program or replay its traffic, and no target
bytes are committed or redistributed. Other tables/CSP fields, bridge/window/
mount relationships, runtime mutations, type identity and signature authenticity
remain unqualified. Five-epic completion is tracked separately in the
[acceptance audit](web-epic-acceptance.md).

Relevant pinned format sources: [Tauri 1.3.0 asset object](https://github.com/tauri-apps/tauri/blob/tauri-v1.3.0/core/tauri-utils/src/assets.rs),
[generator](https://github.com/tauri-apps/tauri/blob/tauri-v1.3.0/core/tauri-codegen/src/embedded_assets.rs),
[PHF 0.10.1](https://github.com/sfackler/rust-phf/blob/v0.10.1/phf_shared/src/lib.rs).
These explain the selected compatibility grammar; they do not authenticate the
compiled producer. Rust's default representation supplies no declaration-order
guarantee. [Rust layout reference](https://doc.rust-lang.org/reference/type-layout.html#the-rust-representation).
