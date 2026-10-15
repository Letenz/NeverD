# Captured npm advisory declarations

`npm-osv-1.9.1-v1` imports one caller-selected OSV JSON artifact with an
`offline-package-metadata-descriptor-v1` artifact. Native C++ reads both captured
byte streams, verifies their capture hashes and retains bounded declarations.
No registry query, download, package installation, script execution or external
analyzer is part of this operation. JavaScript parsing is not required.

The reader implements an explicit subset of the
[OSV schema](https://ossf.github.io/osv-schema/), checked on 2026-10-11.
It admits release schema versions 1.0.0 through 1.9.1; absent schema version
means 1.0.0. Unsupported schema versions remain visible and prevent version
conclusions. Required `id` and `modified` fields, optional package identities,
selectors, severity declarations and chronology have one owner. Prose and
arbitrary custom objects are not retained in the analysis model.

## Identity and version boundary

The matcher consumes an existing `PackageAnalysis` and explicit retained
advisory IDs. Only its declared package name and full version can participate;
an installed alias, placement path or package-manager requirement cannot supply
the missing identity. Conflicting manifest or legacy declarations stay unknown.
Matching describes supplied declarations, not verified installed contents.

Names are case-sensitive; npm `*` is an ecosystem-wide selector. The optional
PURL subset is versionless `pkg:npm/name` or `pkg:npm/%40scope/name` with exact
name agreement and ASCII unreserved segments. Versions, qualifiers, fragments,
other percent encodings and conflicting PURLs retain unknown coverage. This is
not a general PURL resolver. See the official
[npm type definition](https://github.com/package-url/purl-spec/blob/main/docs/types/definitions/npm-definition.md).

Enumerated versions use exact declared string equality. `SEMVER` ranges use
strict [SemVer 2.0.0](https://semver.org/) precedence, including prereleases and
ignored build metadata; numeric components compare digit strings without native
integer overflow. No `v` prefix, shortened version, wildcard, caret, tilde or
package-manager coercion is admitted as a SemVer version.

Supported range events are sorted by version before evaluation. `introduced:0`
starts before all versions; fixed endpoints are exclusive and last-affected
endpoints inclusive. Limits form a separate union of upper bounds, with `*`
unbounded. Enumerated versions and ranges also form a union. Mixed fixed and
last-affected events, contradictory equal-precedence events, missing starts,
invalid versions and non-SEMVER ranges prevent negative coverage conclusions.
Reserved records and matching entries without selectors remain unknown.

| Match status | Meaning |
|---|---|
| `declared_version_match` | At least one admitted selector matches; other unsupported selectors may still make coverage incomplete |
| `version_not_in_admitted_selectors` | Matching package declaration exists, all applicable selectors were admitted, and none matches |
| `no_matching_package_declaration` | The selected advisory has no matching package declaration and no unresolved identity boundary |
| `coverage_unknown` | Identity, schema, version or selector coverage prevents a negative conclusion |

Every result remains `analysis_status:partial`. Feed completeness, freshness,
publisher identity, reachability, exploitability and package safety are not
established. A negative row applies only to the selected admitted declarations.

## Source, time and severity

A descriptor has `schema_version:1` and optional nonempty `source`,
`retrieved_at`, and `as_of` strings. The source label or URI is private and is
never contacted. Timestamps admit Gregorian years 0001..9999, uppercase `T`/`Z`,
seconds 00..59 and optional 1..9 fractional digits. Other forms have an explicit
unsupported/invalid state; raw invalid timestamp text is not published.
Chronology compares normalized keys without consulting the host clock.

Absent source/times, `as_of` after retrieval, contradictory advisory times,
withdrawal and an advisory newer than the supplied as-of all have explicit
states. Temporal state is separate from declared-version membership. The
analyzer invents no feed age policy or current-vulnerability assertion.

Severity rows retain their original advisory or affected-entry anchor. Scheme,
source presence and structure are metadata; score/vector validation and source
authentication are not performed. Simultaneous top-level and per-entry severity
has a conflicting-scope state. Raw scores, advisory IDs, names, prose, PURLs and
source labels remain private behind artifact/record anchors.

## Ownership, budgets and publication

`packages/evidence/Descriptor` owns source/time admission.
`packages/advisories/Reader`, `SemVer`, `Match` and `Publication` own declarations,
version semantics, bounded matching and fixed metadata respectively.
`SessionPackageAdvisories` owns revision checks and atomic cache publication.
The CLI, C API and shared worker/MCP backend contain no alternative matcher.

| Resource | Ceiling |
|---|---:|
| One advisory or descriptor JSON | 1 MiB; depth 32; 32,768 nodes |
| Affected entries / severity entries per advisory | 1,024 / 1,024 |
| Enumerated versions plus events / ranges per advisory | 8,192 / 8,192 |
| One version/event string | 1,024 bytes |
| Retained advisory imports / descriptors per session | 16 / 20 |
| Charged retained parsed metadata and anchors across imports | 8 MiB |
| Comparison rows / matched-entry anchors per result | 8,192 / 8,192 |
| Reader or matcher charged work | 8,000,000 units |
| Retained match selections / page rows | 2 / 1..512 |

Imports consume capacity even when never matched. Exact repeated imports reuse
their identity; shared descriptors are retained once in the descriptor cache,
and each advisory's private descriptor copy is independently charged. Retained
charges include fixed per-record overhead. Oversized or malformed input cannot
publish a partial import. Failed matching consumes no result slot. Reimporting
a project revokes imports, descriptors, matches and their retained-byte counter.

Advisory identity binds the profile, original artifact/hash and descriptor ID.
Affected/severity rows preserve original array indices. Match identity binds
the package analysis and canonical sorted advisory selection. Duplicate or
foreign IDs refuse; matched rows link original package and affected-entry IDs.
Existing inventory snapshots and their coverage fields remain unchanged.

## Public consumers

The four C APIs are `neverd_web_package_advisory_import_json`,
`neverd_web_package_advisory_records_json`,
`neverd_web_package_advisories_match_json`, and
`neverd_web_package_advisories_match_records_json`.
Import selects advisory and descriptor artifact IDs. Advisory pages select
`affected` or `severities`. Match options require `schema_version:1` and accept
an optional `advisory_ids` array of 0..16 IDs; absence means no supplied advisory.
Match pages use `package_advisory_matches_id`.

Worker/MCP operations have the corresponding names without `_json`, a required
revision and closed fields. No arbitrary path, source URI, fetch flag or raw
score can be supplied through those operation schemas. Backend omission retains
the C symbols with explicit unavailability; parser omission keeps these
metadata operations available.

```sh
neverd web package-advisories ./capture 3 package-json 1:2
neverd web package-advisories ./capture 3 npm-lock
```

Arguments select an inventory artifact and 0..16 `advisory-index:descriptor-index`
pairs from the same immutable capture. After successful matching, JSONL contains
the inventory summary and package pages, each advisory summary and its affected
and severity pages, followed by the match summary and pages. Exit success means
analysis completed, not that a package is safe or vulnerable at runtime.

The [qualification record](web-package-advisory-qualification.md) pins original
public metadata and actual NeverD CLI results. The separate
[attestation profile](web-package-attestation-profile.md) records provenance and
signature metadata; the [assessment profile](web-package-assessment-profile.md)
composes selected evidence, analyst dispositions and readable reports.
