# Windows Web product

This product contains the NeverD command line, public C ABI DLL and headers,
analysis worker and offline MCP server for Windows x64. The named analysis-host
profile is `windows-ntfs-handle-v1`, with private
`windows-private-unlinked-spool-v1` storage. Local fixed NTFS volumes are the
admitted filesystem; UNC/network volumes, device paths, drive-relative paths,
reparse points (including ancestors), named data streams and hard-linked files
are refused explicitly. Captured input is never executed.

Extract the complete archive into a new directory. Keep `bin` together with
the included ICU 77.1 Unicode data and runtime dependencies. Run:

```powershell
.\bin\neverd.exe web capabilities
.\bin\neverd.exe web inspect C:\captured\app
.\bin\neverd-web-mcp.exe --stdio --input C:\captured\app.asar
```

Input capture walks retained handles and binds directory enumeration to file
identity. The opened handle's revision is checked before and after capture;
NTFS may retain stale directory timestamps in enumeration records even when
all input writers have closed. See Microsoft's description of
[NTFS directory metadata replication](https://devblogs.microsoft.com/oldnewthing/20111226-00/?p=8813).
Ordinary writers and renames conflict with retained
input handles. The spool name is removed while empty, before any input bytes
are copied. Sealing flushes and checks length, replaces the writable handle
with a non-inheritable read-only handle, and serializes independent range
reads. The original input can subsequently change or disappear without
changing captured bytes.

Exports create a new protected current-user directory, create only fixed
exclusive filenames, and verify lengths and hashes from their owned handles.
Manifest rename does not replace an existing manifest. Rewrite rollback uses
only created object handles; unrelated files survive. Windows locks prevent
rename/replacement of the retained output objects until their transaction
ends. POSIX hosts retain their existing descriptor behavior.

`.github/workflows/web-windows-qualification.yml` builds fixed dependencies,
runs native C++ contracts, packages once, then runs an independent C++ consumer
against the extracted archive. That consumer loads the installed DLL and ICU,
checks immutable source export and ASAR, and drives the actual CLI, worker and
MCP processes with development paths removed. Required tests may not skip.
`.github/workflows/web-product-release.yml` requires this qualification and
the existing complete producer/resource gate at the same source commit before
publishing those exact verified bytes. The receipt and checksum identify the
source, dependency pins, selected host profile and qualification run.

The [native and extracted archive qualification](https://github.com/NeverSight/NeverD/actions/runs/38677375352)
at `5e126750ae7513591b6d79056c02358fe55da0f1` passes 110 native cases,
two adapter checks and all six installed checks with zero failures/skips on
Windows Server 2025 x64. The native selection includes sixteen NTFS ownership
cases and eight self-contained native handoff cases. The installed consumer
uses only System32 in PATH, loads this archive's DLL/ICU, and exercises immutable
export, ASAR, CLI metadata/inventory JSONL, the worker handshake and MCP import
preview. The product receipt binds every archive member and its exact bytes.
The same run also passes all eleven full producer and fifteen CEF/resource
cases on the named macOS arm64 reader, with zero failures or skips.

The Windows x64 host qualification does not establish ARM64 host support,
universal framework coverage or general bytecode semantics. Existing named
input/profile limitations remain in the [support matrix](web-support-matrix.md).
Source and rebuild materials are identified in the product receipt; upstream
copyright and license notices are included in the archive.

To rebuild, check out the receipt's exact NeverD commit and initialize the
recorded submodules with `git submodule update --init --recursive`. The Release
source archive contains NeverD source; submodule revisions and downloaded
dependencies retain their own source repositories and licenses. Use the
Windows Server 2025 / Visual Studio 2026 x64 `clang-cl` frontend with the matching
MSVC runtime toolset and the exact
downloads and SHA-256 values in
`cmake/web-product-dependencies.json`. The full dependency build, CMake flags,
test selection and install commands are in
`.github/workflows/web-windows-qualification.yml` at that same commit. The
receipt also records the actual frontend, MSVC toolset and Windows SDK selected by the
runner. `NEVERD_BUILD_WEB_PRODUCT=ON` selects the archive's install component;
ordinary builds leave this optional packaging disabled.

The fixed LLVM package was built with MSVC 14.51.36231 and clang-cl 20.1.8.
Older MSVC libraries can miss its standard-library helper symbols. The product
lane enforces the recorded minimum toolset, links and runs a small consumer of
the actual LLVM Core/Object/Support libraries before building the engine, and
selects the x64 CRT from that same toolset. Native host qualification uses the
named Server 2025 image; other Windows releases need separate evidence.

Dispatch `web-product-release.yml` on `dev` with a new
`web-product-windows-x64-YYYY-MM-DD` tag (or a `-rN` revision). Both called
qualification workflows use that run's source commit. The final gate refuses
failed, skipped and cancelled prerequisites; the publish job rechecks reports,
source/run identity and archive SHA-256 and creates a new Release. Existing
published Releases are never overwritten. Optional readable-export cases for
an unsupplied Zentro image and an omitted parser are outside the required
enabled-product selection. The full Bun producer native-load case belongs to
the complete producer gate; the Windows lane requires its eight independent
native handoff contracts without that external corpus.
