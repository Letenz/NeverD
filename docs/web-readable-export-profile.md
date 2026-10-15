# Explicit local readable-source export

`source-local-readable-export-v1` selects immutable bytes through the shared
`ArtifactView` owner and applies the existing independently bounded
`hermes-602befee-recovery-js-v2` parser. It is a sequential local disclosure
consumer. It does not raise interactive source limits or retain recovery models
in source, binding, navigation or semantic caches.

## Selection and transformation

The caller supplies the published revision, exact artifact/member ID, explicit
`script`, `module` or `commonjs` parse goal, and a new output directory. Original
files, available stored archive/native members and captured HTML inline slices
use their existing byte selection. Names and suffixes do not choose a parse
goal. Encoded Bun modules and source-map content retain their existing dedicated
export/decoding consumers; an unavailable direct-byte selection is refused.

The selection may contain at most 32 MiB. It is read in 64 KiB chunks and its
complete plain SHA-256 must match the captured byte identity before recovery.
Nested origins preserve container offsets separately from original or expanded
storage coordinates. The local manifest and metadata receipt retain this
origin and selected range. The output never reopens the original input path.

`SourceRecovery` owns formatting and verification. It keeps every input byte
in order and inserts whitespace only at parser-owned token boundaries. It
reparses the candidate with the same source goal and compares the complete
retained syntax tree: node kinds, strictness, ordered child relations and
attribute values. Only `verified_same_parser_tree` produces `readable.js`.
This is a same-parser-tree proof rather than complete JavaScript semantics or
execution equivalence. It restores no erased identifiers, comments, TypeScript
types, original modules or opaque runtime caches.

The recovery profile retains its independent limits: 2,000,000 syntax nodes,
2,000,000 lexemes and 16 MiB of retained string units. Each parser invocation
has the existing 256 MiB allocation / 1,600,000 allocation-work / 30-second
deadline checks. Candidate output is bounded at 32 MiB. These are per-operation
limits, not a global process RSS quota or permission for interactive consumers
to accept the larger syntax model.

## Files and refusals

The existing exclusive export writer creates a fresh private directory with
fixed names. `original.bin` always contains the exact selected bytes.
`readable.js` exists only after the same-tree check. `manifest.json` records the
selected IDs, source goal, original/output hashes, byte counts, origins, fixed
diagnostic codes and recovery status. Every file is reread and hash checked;
the successful manifest is published last. Existing destinations and symlinks
are refused. Captured names never become host path components.

Invalid syntax, unsupported encoding, parser/resource refusal or a reparse
mismatch can still yield an exact original file and a successful evidence-export
receipt. Such a receipt has `status=ok`, an explicit unsuccessful
`readable_status`, null readable hash/size and no readable file. API errors such
as stale revision, unknown selection, unsupported goal or oversized input refuse
before directory creation. An I/O failure can leave partial local evidence; it
does not publish a successful manifest.

The ordinary SDK reply is metadata only. The local directory discloses the
selected source by explicit caller action and is outside the reviewed source
display/redaction flow. No worker or MCP operation exposes this raw export.
No target code, formatter, JavaScript runtime or external helper is executed.
The exclusive writer supports POSIX hosts and the qualified Windows Server
2025 x64 fixed-NTFS profile. The [Windows product contract](web-windows-product.md)
records its handle ownership, private creation and native export qualification;
other Windows hosts are not qualified by that receipt.

## SDK and CLI

The always-declared C API is `neverd_web_source_readable_export_json`. Its
`source_readable_export` capability is advertised only with the Web backend and
JS parser enabled. Parser/backend omission reports explicit unavailability.

```text
neverd web readable-export <root> <script|module|commonjs> <new-output-directory> [artifact-index]
neverd web wails-readable-export <root> <profile> <artifact-index> <member-index> <script|module|commonjs> <new-output-directory>
```

Both commands use the same C API. The Wails form only performs the explicit
container/member selection first; it does not contain another source reader or
formatter. They return zero only for a verified readable result. A parse refusal
returns one while retaining the exact original and refusal receipt; usage errors
return two. They neither install nor launch the selected application.

Authored C++ cases cover immutable input deletion, comments/literals/ASI,
parse/encoding refusal, explicit goals, large chunked selection, independent
interactive limits, stale/unknown selections, oversize refusal before writes,
compressed Wails origins, exclusive destinations and an unusable external-tool
PATH. `NEVERD_WAILS_ZENTRO_030_ELF` selects the pinned real bundle described in
the [Wails profile](web-wails-profile.md); it never downloads or executes it.

## Measured real bundle

NeverD's `wails-readable-export` command selects member 7 of the pinned Linux
x64 Zentro v0.3.0 capture with an explicit `module` goal. With an unusable
external-tool PATH, it preserves 1,552,742 original bytes, parses 449,790 syntax
nodes and publishes a 1,897,987-byte readable file after matching the complete
reparsed tree. Original SHA-256 is
`550cdaae8e65f0cbd2a75132a0dc5073fc8a7883811d0d7abdd6a59f71bd0e1e`;
readable SHA-256 is
`962afafa77d08adbc53e4f721456773a4bbaa5b16e86c7317bc1c6f1812dd8f7`.
The selected physical source starts at original byte 16,727,680. The export
contains three reread-verified files / 3,452,862 bytes, with manifest SHA-256
`99d98812f42db48455b10c20bf3db743c98989d0917bc22a409ecd0b90adb9bc`.
Local evidence is retained outside Git in
`/tmp/neverd-wails-qualification-zentro-v0.3.0/readable` and
`/tmp/neverd-wails-zentro-readable.jsonl`.

This bundle exceeds both the 1 MiB interactive byte limit and the 100,000-node
interactive model limit. Sequential recovery succeeds without admitting its
larger model into interactive consumers. Original source names/bytes that
survive the distribution become readable; deleted names, types, module
boundaries, browser mounts and native/JS bridge semantics remain unverified.
