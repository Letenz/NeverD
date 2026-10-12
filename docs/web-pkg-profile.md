# Offline pkg virtual filesystem extraction

See the [native JavaScript matrix](web-native-javascript-matrix.md) for separate
Bun, SEA, pkg and nexe coverage and fixed-version Claude cross-track status.

`pkg-5.8.1-uncompressed-vfs-v1` reads a fixed pkg loader/prelude envelope in C++.
It preserves original byte partitions and typed virtual stores, and exposes
captured source content to the shared consumers. It does not execute Node,
pkg, a target, bootstrap code or another analysis tool. This is an explicit
compatibility profile, not automatic native-CLI detection or version proof.

## Reader ownership

`pkg/Locator` scans immutable chunks for one exact four-field loader declaration.
Decimal slots retain the producer's widths and space padding. Both consistently
LF and consistently CRLF declaration layouts are supported. Duplicate candidate
locators, malformed slots, coercible expressions, mixed newlines and invalid
arithmetic refuse. A locator must precede the declared payload; payload and
prelude must be adjacent and fit the captured input. Every captured byte belongs
to `opaque_prefix`, `payload_storage`, `prelude_storage` or `opaque_trailer`.
The last region preserves bytes after the prelude, including the bytes appended
by the qualified macOS producer's signing step. Signatures are not validated.

`pkg/Index` reads the fixed prelude wrapper and its five final single-line JSON
arguments: VFS, entry declaration, symlinks, dictionary and compression enum.
The shared strict JSON reader rejects duplicate decoded keys, malformed Unicode
and excessive structure. Compression must be enum zero with an empty dictionary;
gzip/Brotli variants refuse explicitly. No general JavaScript parser or evaluator
participates in extraction. Preserving the prelude and its declarations does not
prove that custom prelude code executes those declarations.

`pkg/Reader` owns partition hashes, distinct store ranges, stable occurrences and
the captured namespace. `SessionPkg` owns atomic publication and revision caches;
`ArtifactView` supplies original/expanded storage coordinates. Transport adapters
consume these results without reproducing the format parser.

## Stores, paths and unresolved semantics

| Store | Meaning in this profile | Source selection |
|---|---|---|
| 0 | Opaque V8 cache bytes | Unavailable |
| 1 | Stored file content | Explicit parse goal; private metadata aliases excluded |
| 2 | Directory-list metadata | Unavailable |
| 3 | Stat metadata | Unavailable |

Each declared path requires bounded stat evidence with consistent file/directory
flags. Contradictory store roles, invalid ranges and partial overlaps refuse.
Exact aliases have distinct occurrence IDs and share one range/hash read. Empty
ranges and unreferenced payload bytes remain explicit. A file with only stat or
cache evidence never becomes a fabricated zero-length source file.
Stat size is a retained declaration, separate from the captured store length.
The producer can strip a BOM/shebang or patch source while preserving its old
stat size. Size differences therefore do not reject valid content; pages report
both lengths and their relation, without inferring preprocessing or original
source equivalence. Source anchors always address the actual captured bytes.

Store 0 and store 1 can coexist. The selected source records `cache_present`,
while `source_cache_equivalence` remains `not_checked`. The pinned upstream
loader prefers its cached-data branch. Source extraction therefore does not
establish which code the runtime executes or prove source/cache equivalence.

Virtual paths use one fixed `/snapshot` or `C:\\snapshot` root. The reader removes
that prefix and converts only the fixed Windows profile's separators before
applying the shared pinned ICU portable namespace policy. Mixed roots, path
escapes, devices, invalid Unicode, normalization/case aliases and file/directory
collisions refuse. Names never become host output paths. Namespace construction
creates exactly one empty-path root; a declared snapshot-root directory keeps
its separate store evidence without duplicating that namespace entry.

Symlink declarations are retained privately and never followed. A literal entry
path match can identify a stored member, but a matching symlink or ancestor
prefix retains `symlink_prefix_unresolved`. Every entry keeps runtime membership
unverified, including exact matches without declared links. Directory lists do
not authorize namespace additions, file discovery or host reads.

Default pages expose fixed categories, numeric ranges and opaque occurrence IDs.
Paths, entry values and link targets stay private. List/stat digests are internal
and appear as null with `hash_redacted:true`; this also applies when another store
aliases the same metadata range. Public IDs use extraction identity and ordinal,
store type and byte range rather than private names as independently guessable
identity inputs. Full input/partition hashes remain byte-preservation evidence.
Partition digests exactly aliasing private metadata ranges are also withheld.
Content aliases of private metadata have no source or package selection, so
downstream consumers cannot republish their per-store digest. Their page keeps
`selection_status:private_metadata_range`. An entry with only opaque cache
evidence still reports `entry_cache_present:true` and no selectable content match.
No raw pkg export is provided by this first profile.

## Admission bounds

| Resource | Limit |
|---|---|
| Captured input / prelude / final JSON arguments | 256 MiB / 8 MiB / 1 MiB |
| Paths / typed stores / symlink declarations | 4,096 / 16,384 / 4,096 |
| One stat document / aggregate JSON nodes / depth | 4 KiB / 100,000 / 5 |
| Structural/comparison steps | 4,000,000 |
| Private metadata per extraction / retained per session | 4 MiB / 8 MiB |
| Cached extractions / rows per page | 4 / 512 |
| Bytes read for scanning / bytes read for hashing | Each at most twice captured input length |

The count limits are independent of cumulative bytes, node and work budgets.
Alias ranges are hashed once; aliased stat documents are read/parsed once.
Locator scanning retains a fixed overlap rather than materializing native images.
Failures publish no members and preserve previously admitted extractions.
Reimport revokes derived selections. Extraction remains available without the
JavaScript parser when the native namespace policy is present.

## Shared entry points

The C API provides `neverd_web_pkg_extract_json` and
`neverd_web_pkg_records_json`. Both require the current revision. The record
operation takes `regions`, `members` or `links`, a zero-based offset and a limit
from 1 to 512. Worker operations are `web_pkg_extract` and `web_pkg_records`;
the capability-filtered MCP names add the `neverd_` prefix. Source, map, HTML,
package and native consumers accept only published non-null selections; each
keeps its own input contract and budgets.

CLI forms use the same C API:

```text
neverd web pkg <root> <profile> [artifact-index]
neverd web pkg-source <root> <profile> <artifact-index> <member-index> <script|module|commonjs>
neverd web archive-pkg <root> <profile> <tar|tgz|zip> <archive-index> <archive-member-index>
neverd web archive-pkg-source <root> <profile> <tar|tgz|zip> <archive-index> <archive-member-index> <member-index> <script|module|commonjs>
```

Use the exact profile above. `member-index` is the typed member page's index,
including opaque and metadata records; only a record with a non-null
`selection_id` is eligible. Source output is analysis metadata, not raw bytes.
Stored nested archives preserve byte identity through both offsets; compressed
archives retain their containing frame and separate expanded coordinates.

## Qualification and source evidence

Six original Node 18.5.0 bases from pkg-fetch `v3.4` were checked against its
pinned SHA-256 list and used with `pkg@5.8.1`: Linux, macOS and Windows, each x64
and ARM64. Four independently authored files were packaged with bytecode and
native-addon building disabled. The C++ core extracted all six and matched each
source/resource byte and digest to those independent inputs. macOS outputs
retain their additional trailing bytes. Producer labels do not authenticate an
arbitrary captured native prefix, target architecture, signature or runtime.

The [fixture manifest](../unittests/web/fixtures/pkg/manifest.json) records input,
container and base hashes, producer identity and the retained dependency lock.
The [recipe](../unittests/web/fixtures/pkg/README.md) separates producer creation
from tests and analysis. Tests never install/download/run a producer or target.
Synthetic C++ cases cover inconsistent source/cache pairs, which the no-bytecode
producer corpus cannot qualify. A supplementary Linux x64 producer artifact
in [`stripped-manifest.json`](../unittests/web/fixtures/pkg/stripped-manifest.json)
qualifies BOM/shebang preprocessing against separate original and stored byte
oracles: stat sizes remain 78/22 while stored source sizes are 56/19. This
prevents confusing original-file metadata with actual source coordinates.
No target runtime was executed.

Format evidence is pinned to pkg commit
[`5dc987b`](https://github.com/vercel/pkg/tree/5dc987b90ffd191263eb0202833dc382cea0d47d):
[`producer.ts`](https://github.com/vercel/pkg/blob/5dc987b90ffd191263eb0202833dc382cea0d47d/lib/producer.ts),
[`packer.ts`](https://github.com/vercel/pkg/blob/5dc987b90ffd191263eb0202833dc382cea0d47d/lib/packer.ts),
[`common.ts`](https://github.com/vercel/pkg/blob/5dc987b90ffd191263eb0202833dc382cea0d47d/lib/common.ts),
and [`bootstrap.js`](https://github.com/vercel/pkg/blob/5dc987b90ffd191263eb0202833dc382cea0d47d/prelude/bootstrap.js).
Source preprocessing and original stat capture are documented in the pinned
[`walker.ts`](https://github.com/vercel/pkg/blob/5dc987b90ffd191263eb0202833dc382cea0d47d/lib/walker.ts).
The locator and bases use pkg-fetch commit
[`0b2a037`](https://github.com/vercel/pkg-fetch/tree/0b2a03743e72ca11eacdb2acd90c6c5abdaa2734),
its [`Node patch`](https://github.com/vercel/pkg-fetch/blob/0b2a03743e72ca11eacdb2acd90c6c5abdaa2734/patches/node.v18.5.0.cpp.patch)
and [`expected hashes`](https://github.com/vercel/pkg-fetch/blob/0b2a03743e72ca11eacdb2acd90c6c5abdaa2734/lib/expected.ts).
No upstream implementation is copied or linked into this reader. Compressed
profiles, link resolution, V8 semantics, native validation, other producer
versions and the remaining #714–#718 acceptance work are separate requirements.
