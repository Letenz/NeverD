# Supplied local CEF/WebView resources

`cef-webview-local-resource-descriptor-v1` is a named local asset index for
explicit `cef-assets`/`webview-assets` selections. It detects no universal native
CEF/WebView packaging. Supplied framework/version claims stay unverified;
another custom embedding requires its own named measured adapter.

Capture `neverd-resources.json` beside its indexed resources in a directory or
supported ZIP namespace:

```json
{
  "schema_version": 1,
  "profile": "cef-webview-local-resource-descriptor-v1",
  "runtime": {"file": "runtime/libcef.dll", "version": "supplied declaration"},
  "assets": ["web/index.html", "web/app.js", "web/app.js.map", "resources.pak"],
  "native_helpers": ["helpers/tool.exe"]
}
```

The existing portable path owner compares exact files relative to the captured
descriptor parent. Traversal, device/absolute paths, backslashes, URLs and
unsupported normalization cannot leave that namespace. Missing, unsafe,
unsupported and exact files have explicit states. Array occurrences and CEF/
WebView selections have distinct stable IDs. Unknown keys, wrong versions and
oversized lists refuse: 1 MiB descriptor, depth 32, 20,000 JSON nodes, at most
512 assets and 128 helpers.

Entry pages expose opaque IDs, hashes, sizes and fixed roles/ordinals/content
candidates. Paths/version values stay private. Existing source, HTML, map and
native operations select exact available artifacts. Suffixes authenticate no
syntax, native format or framework. `.pak`, `.dat`, WASM and bytecode remain
opaque; no compiled-resource or original-source recovery is invented.

Shared desktop-manifest SDK/CLI/worker/MCP accept these kinds. Raw local export
is separately selected and excluded from worker/MCP:

```sh
neverd web desktop-manifest root cef-assets DESCRIPTOR_INDEX
neverd web desktop-export root cef-assets DESCRIPTOR_INDEX NEW_DIRECTORY
```

`neverd_web_desktop_resources_export_json` uses the existing exclusive no-follow
writer, fixed `resourceN` filenames and read-back SHA-256. Its inert completion
manifest retains original artifacts/origins, roles, hashes and unavailable
entries. No target pathname becomes an output path. Existing destinations refuse;
failures can retain partial evidence without a completion manifest. Original
descriptor/resource files can contain sensitive values and are disclosed only
by this explicit local operation. The writer remains unavailable on unqualified
Windows hosts.

Self-authored C++ fixtures qualify directory/compressed ZIP scope, JS/HTML/native
handoff, opaque/missing members, stable identities, refusal paths and exact local
export. These fixtures do not authenticate a real application's framework or
packaging. Real Electron/Tauri/Wails/VSIX evidence keeps its independent profiles.
