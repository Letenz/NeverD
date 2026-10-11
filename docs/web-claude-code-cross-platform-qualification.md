# Captured Claude Code 2.1.296 platform packages

The native npm distribution qualification on 2026-10-12 uses all eight pinned
platform-package declarations from the preserved
[@anthropic-ai npm scope](https://registry.npmjs.org/@anthropic-ai/claude-code/2.1.296).
The backend reads the selected archives, original-byte integrity declarations
and Bun members. No installer, package command, input code or target runtime is
executed. All native CLI runs use `PATH=/nonexistent` and `TMPDIR=/tmp`.

The local evidence collection retains separate
`2.1.296/cross-platform-evidence/2026-10-12/TARGET/input` scopes.
Each has `a.package.tgz`, `b.registry.json` and `c.descriptor.json`;
HTTP headers and reports remain outside the import scope. The source descriptor
does not invent missing retrieval/as-of timestamps. Private samples and recovered
source are not redistributed in this repository.

## Original archive and executable relationship

Every archive's native SHA-512 comparison is `match` against its own explicitly
captured registry declaration. This concerns selected original archive bytes.
Signatures, publisher identity, installed-tree content and safety remain
unverified. Each admitted archive has four available members; the selected
executable is member 0 at expanded offset 512. Its original compressed byte
offset remains unknown. The input scope's root/artifact indices are 0/1/2/3;
these indices belong to this exact capture, not an arbitrary package layout.

| Target | Original tarball SHA-256 |
| --- | --- |
| linux-x64 | `eef5a2e2b09a5e7d2ce784d1b3ba335ae7fc6c7f6256c802a0ebf2c21e4fd0e1` |
| linux-arm64 | `56abbe1cfc85e5b37afeef5ae8b4856ddc32bbe443b82689a3b5f99992045cd1` |
| darwin-x64 | `ec29d962f0f7c70cf8c44606ee7fbb3cd7936a33f4dc13d3c2e3664c5f0b3fc4` |
| darwin-arm64 | `50fbf594d6526c9aae7d66b8b45a08d89ae6b863e2bc9777bfb277d6521a7458` |
| win32-x64 | `f04bb1c18698dc6562912a573b031306225f5e202df4fe6bd2317351f5e492c5` |
| win32-arm64 | `7019eb71ba03dc70e76e7e909d5132192192948abe02bb18105ca34cae976547` |
| linux-x64-musl | `88fcfa54fdb0ea24594111378908d6488fb975da8b139f5fb5e38d8256ffba5b` |
| linux-arm64-musl | `7895ef5b714e675152a18058e83cbf9d5f01d758dee97c2e40969f71f2c884f3` |

The core C++ fixture pins original/executable lengths and SHA-256 hashes,
graph offsets/sizes, flags, startup/entry indices, module/region counts and
strictly decoded source counts/bytes. It compares the separate captured
SHA-512 declaration and selects the member from the archive owner's namespace.

| Target | Decoded JS units | Decoded UTF-8 bytes | Selected executable SHA-256 |
| --- | ---: | ---: | --- |
| linux-x64 | 2345 | 44,768,763 | `24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de` |
| linux-arm64 | 2345 | 44,760,184 | `f1f6e96e0d8342b9dbf41d7e88255397a6a52ce3d8736ad6a4c6b59c9b62fefa` |
| darwin-x64 | 2364 | 45,027,735 | `a6bf4f30be241053a923f3820ad23c5991d2f5ac2935b3a8dd64da218d9cc603` |
| darwin-arm64 | 2364 | 45,035,899 | `c9b5341637becbd423ddffc5b254afb645682a3868cb708bbc6cc0e7bb419937` |
| win32-x64 | 2343 | 44,717,469 | `76de74f7694a5bdc03378a5f837c9e132827bf004c98c276bc4e9094ff06769a` |
| win32-arm64 | 2343 | 44,709,481 | `a652a17f5a324d2d9ad9995cc047145f8176b9a4b4f28bbd3aa0708a22679e3b` |
| linux-x64-musl | 2345 | 44,760,179 | `94a1ccc6ffc9b0889c6b2b80e202885c8a575766775377398daa4db5fd89417f` |
| linux-arm64-musl | 2345 | 44,760,184 | `b0bbc936a82930933d7d5645081050d5f827a946cf6abc6575383b777a0feec0` |

The selected ELF, thin Mach-O and PE inputs cover x64 and ARM64 on Linux,
macOS and Windows, with both supplied Linux libc variants. Each uses its exact
`bun-71d0d439-prelinked-<platform>-<architecture>-<container>-v1` profile.
Compiler/producer version remains unauthenticated. Module counts include assets
and are distinct from JS counts. All eight inputs have zero source maps.

## Direct archive export

Use the normal [local export contract](web-bun-profile.md#local-evidence-export).
The `archive-bun-export` adapter selects an available member and delegates to the
same `bun_extract`/`bun_export` C APIs. It creates no filesystem extraction tree
from stored package paths. For one of these exact captured scopes:

```console
neverd web integrity ./TARGET/input 1 registry-dist 2
neverd web archive ./TARGET/input tgz 1
neverd web archive-bun-export ./TARGET/input tgz 1 0 ./TARGET/new-recovery-directory
```

`original.bin` is the selected executable, and `original_origin` in the manifest
retains the enclosing tarball/member/hash and expanded locator. The original
tarball remains in the input capture. Generated region, source and readable
filenames cannot follow stored paths. Existing destinations and unavailable or
non-Bun members are refused; no package main/bin declaration is executed.

The eight source totals are qualified independently per captured target.
Every source unit has an exact decoded file and a readable copy with status
`verified_same_parser_tree`: 18,794 units and 358,539,894 decoded UTF-8 bytes
summed across the eight platform inputs. This is not a count of distinct source
files across platforms. All eight exports have zero unavailable sources and
zero source maps; their reported parsed/readable counts equal decoded counts.
Every output file is reread and its SHA-256 checked by the C++ export owner.

| Target | Exported files | Exported bytes | Export manifest SHA-256 |
| --- | ---: | ---: | --- |
| linux-x64 | 14,698 | 615,295,322 | `d7d83f78cbc39c10d9ef61301f43bc81704865079a159fc2da664abeb9eb77de` |
| linux-arm64 | 14,698 | 614,010,802 | `5754fe32a78b9e9fe27bddc3fcbe8ea3d071c38479c3054b0a54c494efbb99a7` |
| darwin-x64 | 14,812 | 600,133,058 | `a8d46cde115360cf1e933901b285302f65a5a81f18ed6c7fe7d75f21128219c0` |
| darwin-arm64 | 14,812 | 583,088,484 | `6bc3d7df588ba439cd476c5e0934689d56625500b53ccf5d32f2f850e3ddd046` |
| win32-x64 | 14,686 | 622,024,329 | `d68c47cf5225ebd4410268cf27a834d706498a91389fe68b35ee1659da8db983` |
| win32-arm64 | 14,686 | 597,058,095 | `f132e8aff910e4a5d98c39d2d7b8b0badf0f4bb998d400721ef10440529dd8c0` |
| linux-x64-musl | 14,698 | 602,779,568 | `6e1f14d3434d9e3f88dd95d373508c9d389231712edb98fe082736533a627780` |
| linux-arm64-musl | 14,698 | 598,720,153 | `1ff73c17ecfbce672c35573e0c99065bd49414d7b9ffd8020a33dd1be50e0c5e` |

The `recovered` directories retain the manifests, executable originals, raw
regions, exact JS, readable copies and inert indexes. Their opaque JSC cache
counts are 2,343 per Linux input, 2,361 per macOS input and 2,342 per Windows
input. Cache preservation is separate from bytecode decoding.

An additional macOS ARM64 export with JavaScript parsing omitted preserves the
same 2,364 decoded sources, 45,035,899 bytes and original/archive identities.
It explicitly reports zero parsed/readable sources and a null readable profile.
The eight core corpus cases and three archive CLI cases also pass in that
configuration; the normal parser-enabled build is restored and rechecked.

Reparsing validates each readable copy's parser tree; it cannot restore erased
TypeScript, lost types/names, runtime behavior or opaque JSC cache semantics.
Container qualification on the macOS arm64 host is separate from native lifting
support and Linux/Windows host qualification.

The [implementation ledger](web-analysis-implementation.md) records export
summaries, regression counts and resource/host limitations. This increment
does not close #714–#718.
