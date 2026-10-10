# Claude Code 2.1.296: offline NeverD qualification

Recorded on 2026-10-10 on macOS arm64, Release, using prebuilt NeverD LLVM
23.0.0 r4. This is a real artifact extraction and source-recovery qualification,
not a claim to reconstruct the publisher's original repository.

## Input and provenance

The official download channel and npm metadata both selected **2.1.296**.
The input was downloaded from the official
[Linux x64 release URL](https://downloads.claude.ai/claude-code-releases/2.1.296/linux-x64/claude),
using the coordinates from the
[installer](https://claude.ai/install.sh) and
[release manifest](https://downloads.claude.ai/claude-code-releases/2.1.296/manifest.json).
Neither the installer nor the target executable was run. Downloading the
artifact is separate from NeverD's offline analysis.

| Evidence | Value |
|---|---|
| Release | `2.1.296`, `linux-x64` |
| Manifest commit | `fdb6c17a97b6e0e3cf15e3566663fca58e7c43a9` |
| Manifest build date | `2026-10-09T14:46:35Z` |
| Original size | `257068216` bytes |
| Original SHA-256 | `24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de` |
| NeverD layout | `bun-71d0d439-prelinked-linux-x64-elf-v1` |
| Graph offset / length | `89120776` / `167890143` bytes |
| Graph flags | `7167` (`0x1bff`) |
| Entry / startup count | `6` / `7` |

NeverD independently computed the original SHA-256 and matched the official
manifest. Publisher signatures were not verified. The input, recovered target
code and old comparison source remain outside the NeverD repository; tests
do not download or redistribute them.

## Practical recovery

```console
PATH=/neverd-no-external-tools /absolute/path/to/neverd web bun-export /path/to/claude-linux-x64 /path/to/new-output-directory
PATH=/neverd-no-external-tools /absolute/path/to/neverd web bun-navigate /path/to/claude-linux-x64 6 module
PATH=/neverd-no-external-tools /absolute/path/to/neverd web bun-navigate /path/to/claude-linux-x64 228 module
```

These operations use NeverD's C++ C API/backend in process. There is no Bun,
Node, npm, Python, external extractor or formatter on the analysis path. The
target's `require`, `eval`, imports, configuration and executable bytes remain
data. Source recovery also ran successfully with an unusable external-tool PATH.

| Result | Count |
|---|---:|
| Declared modules | 2,589 |
| Exact exported regions | 10,005 |
| Strictly decoded JS modules | 2,345 |
| Decoded UTF-8 JS bytes | 44,768,763 |
| Unavailable source projections | 0 |
| Reparse-verified readable modules | 2,303 |
| Modules retained raw with parser rejection | 42 |
| Asset modules | 244 |
| Module JSC cache regions | 2,343 |
| Source-map regions | 0 |
| Output files | 14,656 |
| Verified output bytes | 603,440,501 |

The complete original, every declared region and all 2,345 decoded sources are
present. `index.html` maps generated filenames to captured virtual names and
identifies module 6 as the entry. Every exported file is reread and hashed;
`manifest.json` also carries original offsets, storage/source/readable hashes,
projection and parser status, and bounded diagnostic offsets.

The final repeated run produced the same manifest SHA-256:
`f8701e0a803e71abe27d166f9057caebc44ebeccec61dbaf60a26c17f3224b56`.
One final run took 128.32 seconds wall time, 40.73 seconds user CPU and
5.82 seconds system CPU, with 814,809,088 bytes maximum RSS as reported by
macOS `time -l`. Other builds/tests shared the host; this is an observation,
not a controlled throughput benchmark or resource guarantee.

The 42 remaining parser rejections retain exact decoded source. Inspection of
each first diagnostic found a `using` or `await using` declaration, which the
pinned parser does not implement. Subsequent syntax has not been qualified
past those failures. This does not establish that the publisher's JavaScript
is invalid. No guessed source or substituted empty module is emitted.

No source maps are shipped in this artifact. Original TypeScript, erased types,
deleted comments/names and pre-bundle file boundaries cannot be recovered from
these bytes with the same fidelity as the older source-map-based reference.
Native machine code and JSC caches are preserved, not decompiled. Readable
copies retain original bytes and insert whitespace, then compare retained
syntax trees; source-text reflection and positions change.

## Failures that drove implementation

1. The old reader refused flags 11/12. The new independently versioned layout
   validates prelinked graph tables and runtime-option storage against the
   pinned upstream writer, retaining linked-bytecode flag 13 as unsupported.
2. One 4,762,637-byte JS module exceeded the old storage materialization cap.
   The shared C++ Unicode decoder now reads in bounded chunks; explicit export
   uses a separate limit without raising interactive parser limits.
3. Nine sources exposed missing parser locations for async-arrow rest
   parameters. The pinned C++ parser conversion now preserves original ranges.
   A regression also exposed an illegal rest trailing comma being accepted;
   parser admission checks the parser-owned following token. Valid spread-call
   commas and comments remain admitted.
4. Whole-module readability needed a separate sequential parser budget and
   reparse comparison. Unsupported syntax remains explicit, with raw evidence
   and diagnostics retained.

Module 228 additionally passed NeverD's existing binding/navigation consumer:
55,522 syntax nodes, 1,169 functions, 4,468 calls and 10,980 references, with
`binding_status:ok` and `navigation_status:ok`. These are source relationships,
not proven runtime call targets. Old-source phrase matches supply navigation
hints only; no old source was substituted into recovered files.

## Repeatable checks

```console
cmake --build build-release --target neverd NeverDWebArtifactTests NeverDWebSourceTests NeverDWebSDKTests
NEVERD_CLAUDE_CODE_21296_ELF=/path/to/claude-linux-x64 build-release/bin/NeverDWebArtifactTests --gtest_filter=WebBun.ClaudeCode21296FullContainerWhenSupplied
build-release/bin/NeverDWebSourceTests --gtest_filter='WebSource.AsyncArrowRest*:WebSourceRecovery.*'
build-release/bin/NeverDWebSDKTests --gtest_filter='WebSDK.BunExport*:WebSDK.CLIBunExport*'
```

The full web suite registered 284 cases: 277 passed, seven explicitly skipped
(six require unavailable LLVM Zstd and one tests the ASAR-disabled branch).
Both the five pinned Bun 1.4.2 full images and this Claude artifact were supplied.
The native/source/backend boundaries retain their independent budgets and
qualification limits. This case advances #714 and #718; it does not close the
five JS epics or qualify other container platforms.

The five C++ worker suites passed. Broader mock-worker testing retains an
unresolved large-source SourceCache response timeout (it passed once, then
failed again on serial retry). That mock server does not link the web backend;
no worker code changed here. The macOS temporary-path alias fixture passed with
canonical `TMPDIR=/private/tmp`. Full worker-suite success is not claimed.
