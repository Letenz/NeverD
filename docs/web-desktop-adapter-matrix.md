# Desktop container adapter matrix

The columns below are independent capabilities. Detection records evidence for
an explicitly selected profile; it does not authenticate a publisher, installed
runtime or framework version. Listing preserves unavailable members as evidence.
Extraction supplies immutable captured or decoded bytes; writing raw bytes to a
new local directory is an additional explicit operation. Analysis uses the
shared source, package, HTML or native consumer and retains that consumer's
limits and unknown states.

Input platforms below describe the captured application, not the analysis host.
Host/build qualification is recorded in the [support matrix](web-support-matrix.md)
and [Windows product profile](web-windows-product.md). A successful native loader
handoff does not restore original Rust/Go source or establish complete backend
semantics. No adapter launches a target, loads a native addon, executes JS,
fetches application resources or invokes an installer.

| Adapter and versioned input boundary | Input platform/layout | Detection | Listing | Extraction | Analysis |
| --- | --- | --- | --- | --- | --- |
| [ASAR](web-asar-profile.md), `asar-pickle-json-v1`; preserved writer commit `e4fb057678562b7b6170699a046d983ae6d31cb8` | Captured ASAR plus explicitly selected unpacked directory; platform-neutral container grammar | Validates the selected Pickle/JSON envelope; producer/Electron identity stays unverified | Paged member occurrences, hashes, ranges, integrity and unavailable/link states | Packed slices and exact captured unpacked occurrences; no automatic sidecar lookup or whole-tree disk export | Available IDs feed source/maps/packages/native consumers with original container or captured-file origins |
| [Electron](web-electron-profile.md), Electron 40 entry/API grammar and versioned static source/IPC profiles | Selected package manifest and source in a captured directory or ASAR namespace | Package declarations and source-visible Electron import/API evidence; runtime version and distribution authenticity remain unverified | Main/preload/renderer candidates, source boundaries and selected-scope IPC records | Uses the independent ASAR/directory bytes; the manifest/IPC consumers do not create another extractor | Exact entry-file links, private channel equality and static bridge/IPC anchors; runtime routing and complete application graph stay unknown |
| [NW.js](web-desktop-manifest-profile.md), `nwjs-manifest-file-candidates-v1` | Root package directory, `package.nw`/`app.nw`, macOS `Contents/Resources/app.nw`, or ZIP32 `package.nw`; no runtime release is inferred | Named layout and selected manifest declaration evidence | Main, node-main, background and injection declarations with exact/missing/unsupported states | Captured files or the independent stored/deflate ZIP32 reader; appended-executable ZIP is refused | Exact available entries feed JS/HTML/package/native consumers; command-line syntax and runtime fallback stay unresolved |
| [VSIX](web-desktop-manifest-profile.md), `vscode-extension-manifest-file-candidates-v1` over `zip32-local-central-v1` | `extension/package.json` in supported ZIP32, or captured extension root; fixed real extension capture 2.1.296 is separately pinned | Extension placement/manifest declarations; signatures, XML installer metadata and activation remain unverified | Main/browser entries and declaration statuses; ZIP lists all members including opaque helpers/assets | Bounded stored/deflate members with exact ZIP origins; no installation or whole-tree disk export | Authored fixtures connect JS entries to the source frontend and a distinct Bun helper to #718; extension and helper versions remain independent |
| [Wails](web-wails-profile.md), Wails 2.11.0 / Go 1.25.7 Linux profile | ELF64 little-endian x64 ET_EXEC, GOOS linux / GOARCH amd64 | Required inline build declarations and complete supported stored-table framing; compiler/framework authenticity remains unverified | Per-table file/directory occurrences with stored hashes and physical ranges | Stored embedfs bytes; verified fixed-name local raw export | Shared source/HTML/readable export and original ELF native handoff; table mount and runtime bridge binding remain unknown |
| [Wails](web-wails-profile.md), Wails 2.11.0 / Go 1.25.7 Windows profile | PE32+ x64 executable, GOOS windows / GOARCH amd64 | Independently validates PE mappings and the selected build/table declarations | Same evidence schema with explicit Windows/PE profile identity | Stored embedfs bytes and verified fixed-name local raw export; DLL/PE32/ARM64 and other layouts are refused | Shared source/HTML/readable export and original PE native handoff; real Zentro 0.3.0 captures retain 11 files each for ELF and PE |
| [Tauri](web-tauri-profile.md), `tauri-phf-0.10-macho-x64-five-store-brotli-v1` | Thin Mach-O x64 with selected loader-recorded initializer; real lencx/ChatGPT 1.1.0 input | Native LowIR proves the selected five-store initializer and all PHF 0.10 lookups; exact compiled Tauri/Rust versions are unverified | Complete selected table, frame/decoded hashes, ranges and member kinds | Bounded native Brotli 1.2.0 decoding and verified fixed-name raw export; other encodings/layouts/ISAs are refused | JS/HTML/package consumers and original Mach-O handoff; real input has 15 assets and nine same-tree readable JS exports; runtime mounts/bridge targets stay unknown |
| [CEF DataPack](web-datapack-profile.md), `chromium-datapack-v5-v1` | CEF 120.2.7 Windows x64 NuGet distribution, v5/u16 resource table | Validates complete selected resource-table framing; retains the pinned distribution declaration independently from runtime authenticity | 2,504 canonical resources and 16 aliases in the fixed input, with exact resource IDs/hashes/ranges | Stored resources, explicit bounded gzip selection and verified fixed-name raw export | Fixed actual JS/HTML consumers retain nested ZIP/gzip origins; arbitrary resource names/routes/codecs are not inferred |
| [WebView2 DataPack](web-datapack-profile.md), `msedge-webview2-datapack-v5-u32-v1` | Teams 26261.303.5165.1465, bundled WebView2 154.0.4258.37, macOS captured resource pack | Validates the measured v5/u32 grammar; not a universal Microsoft format/version detector | 5,814 canonical resources and 633 aliases in the fixed local input | Same explicit stored/gzip/export contracts under the separately selected wide-ID profile | Fixed original JS bytes and source anchors; runtime activation, route discovery and other WebView versions remain unqualified |
| [Supplied CEF/WebView resources](web-desktop-resource-profile.md), descriptor schema 1 / `cef-webview-local-resource-descriptor-v1` | Captured directory or supported ZIP namespace | Caller-supplied runtime/resource declarations only; framework/version claims stay unverified | Exact/missing/unsupported asset and native-helper links | Captured bytes and verified fixed-name resource export; `.pak` requires a separate DataPack selection | JS/HTML/maps/native consumers; compiled/unknown resources stay opaque unless a named downstream profile accepts them |

## Shared safety and evidence

Directory capture and archive admission use the same portable component rules
and NFC/full-fold comparison owner. They reject traversal, devices, unsafe
names and ambiguous namespaces while preserving original names privately.
Unicode comparison requires exact ICU 77.1 / Unicode 16.0; without it, captured
directory member names admit only the exact ASCII subset and refuse non-ASCII
names, while archive profiles remain unavailable. The selected root path is
not a member comparison key. Fixed-name exports use exclusive no-follow
writes, refuse existing destinations and verify every written file by SHA-256.
Container paths are manifest evidence rather than output filenames.

Each named reader bounds member counts, nesting, stored/expanded bytes and
format-specific offsets. Unsupported encryption/compression or compiled data
remains an explicit unavailable/opaque member where the envelope is supported;
malformed or unsupported envelopes refuse atomically. Original hashes and full
parent/member origins remain available after a refused analysis. Metadata pages
retain the common redaction policy; raw local exports are explicit disclosure
and are not exposed by worker/MCP.

## Qualification witnesses

- `WebAsar.*`, `WebZip.*` and the shared path tests cover unpacked association,
  malformed offsets, every ZIP truncation, duplicate/Unicode aliases, links,
  expansion limits and opaque members.
- `WebDesktopManifest.*` and `WebDesktopSDK.*` cover the documented NW.js layouts,
  authored VSIX JS/helper links, native/HTML consumers, redaction and exact scope.
- `WebWails.*` / `WebWailsSDK.*` and `WebTauri.*` / `WebTauriSDK.*` exercise named
  supported layouts, incompatible-build refusals, raw/readable exports and
  traceable native handoffs. Their profiles pin the independently supplied real
  inputs and distinguish missing optional corpus files from passing cases.
- `WebDataPack.*`, `WebDataPackSDK.*` and `WebDataPackReal.*` cover both resource
  widths, aliases, gzip admission, source/HTML consumers and actual fixed packs.
  The release CEF lane requires its immutable NuGet input without a skip fallback.

The [acceptance audit](web-epic-acceptance.md) and
[implementation ledger](web-analysis-implementation.md) retain executed receipts.
A new framework version or storage layout requires its own measured profile;
limited explicit compatibility does not imply general framework support.
