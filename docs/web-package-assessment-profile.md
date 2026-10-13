# Offline package assessment and review profile

`offline-package-assessment-v1` composes explicitly selected retained results.
It does not read target text, repeat owner analyses, execute JavaScript, invoke
tools, fetch feeds or keys, calculate a risk score, or declare a package safe.
Inventory, integrity, advisory and attestation findings also work when the
optional JavaScript frontend is omitted. Source and installation findings
require results retained by that frontend.

The public C API, CLI, worker and MCP use the same Session owner. The Qt workbench
and additive [review journal profile](web-package-review-journal.md) present
selected manifest/registry evidence and persist fixed assertions. Capturing a
new revision invalidates retained assessments/reviews; restoration requires
reconstructing the exact same immutable assessment.

## Explicit selection and relationships

`package_assessment_create` requires the exact `package_analysis_id` in the
current revision. Selection JSON has `schema_version: 1` and only these optional
fields:

| Field | Selection and limit |
| --- | --- |
| `integrity_ids` | At most 16 retained original-byte integrity receipts |
| `advisory_ids` | At most 16 retained advisory imports |
| `advisory_matches_id` | One match result from this exact package analysis |
| `attestation_ids` | At most four retained metadata imports |
| `attestation_digest_match_ids` | At most four retained subject-digest receipts |
| `package_behavior_id` | One result from this exact package analysis |
| `package_install_id` | One trace result from this analysis and namespace |
| `package_diff_id` | One comparison with this analysis as before or after |
| `package_behavior_diff_id` | One comparison from the selected package diff |

IDs are 64 lowercase hexadecimal characters. Unknown or duplicate selections,
foreign analyses, incompatible namespace relationships and unsupported fields
refuse the entire assessment. If an advisory match is selected and
`advisory_ids` is absent, the match owner's exact import selection is reused.
An explicit array must match that selection. Selection order is canonical and
does not change finding IDs. An empty selection is valid and publishes
`not_selected` coverage for omitted evidence.

The result has a stable `package_assessment_id`, `package_analysis_id`,
`namespace_id`, sorted `selection_ids`, `finding_count`, feed descriptors,
`review_root_id` and current `review_head_id`. Recreating the same recipe returns
the existing review head, preserving its decisions.

An integrity receipt's original artifact, original SHA-256, declaration and
selection are checked independently. A matching registry name/version in a
loose directory produces `declared_identity_original_inventory_unbound`.
Only a retained archive whose original ID/hash and available root manifest
membership agree can produce `original_contains_declared_root_manifest`.
Conflicting or foreign registry declarations refuse that relationship. Neither
case verifies an installed dependency tree. A selected lock instance binds only
its own retained declaration. An in-toto subject receipt produces
`selected_subject_original_only`; its nested integrity ID is not a separately
admitted ordinary integrity selection.

Advisory findings preserve `declared_version_match`,
`version_not_in_admitted_selectors`, `no_matching_package_declaration` and
`coverage_unknown`. Negative or unsupported rows do not prove feed completeness,
reachability, exploitability or safety. Signature and subject declarations stay
unverified, including when a subject digest matches selected original bytes.
Feed descriptors include admitted UTC times and fixed validation/chronology
states, with `freshness_assessed: false`. Supplied source labels remain private.

Source findings retain opaque artifact/source/node identities, original UTF-8
byte spans and fixed API/pattern/origin classifications. Installation findings
describe declarations, module request candidates and selected static associations,
not activation or execution. Selected behavior comparisons preserve pair
coverage, recognized pattern counts and occurrence anchors; missing evidence
does not become a zero count or a call correspondence.

Every finding has `finding_id`, `kind`, `evidence_status`, `binding`,
`confidence`, owner/record/related IDs, and nullable original/declaration/source
anchors. Digest findings retain `digest_algorithm` and `legacy_sha1`; raw SRI
strings and encoded signatures are absent. Pattern counts, when applicable,
are `recognized_count_before` and `recognized_count_after`.

## Analyst decisions and consistency

`package_review_append` requires `package_assessment_id`,
`expected_review_head`, `finding_id` and one fixed `disposition`:
`unreviewed`, `needs_review`, `accepted_risk` or `false_positive`.
It appends an immutable decision linked to its parent head. It never changes
the assessment, upgrades its evidence confidence, authorizes execution or export,
or accepts arbitrary notes. No decision is generated automatically.

An exact replay of the last accepted request returns `replayed: true` without
charging another event, including at capacity. A changed request with an old
head refuses with `stale_package_review_head`. Older historical requests do not
replay. The complete retained decision chain and finding membership are validated
before publication. Allocation/serialization or capacity refusal leaves the
prior state unchanged.

`package_assessment_findings`, `package_review_decisions` and
`package_assessment_report` all require the exact current `review_head` as well
as the revision. This prevents mixing pages from different review states.
Findings show the most recent assertion for each finding, with
`disposition_source: caller_assertion`. Findings with no decision show
`unreviewed` and `disposition_source: not_reviewed`. Explicitly returning a
finding to `unreviewed` still records the caller's assertion.

Reports contain `report_format: markdown` and `report_markdown`, with the same
opaque evidence anchors, fixed classifications, admitted times, current head
and coverage limits. They omit raw source, package names, paths, URLs, command
arguments, signature contents and arbitrary prose. CLI writes the complete
bounded result to stdout only, after every selected page succeeds.

## Resource limits

| Resource | Ceiling |
| --- | --- |
| Retained assessments per revision | 2 |
| Findings per assessment | 8,192 |
| Charged composition work per assessment | 8,000,000 |
| Retained metadata per assessment | 16 MiB |
| Assessment options | 8 KiB; depth 4; 128 JSON nodes; 64-byte strings |
| Review events across both assessments | 256 |
| Charged review metadata across both assessments | 256 KiB |
| Page size | 1..512 records |
| Markdown per page | 1 MiB |
| Buffered CLI output | 32 MiB |

Composition and review transitions publish only after successful validation and
response serialization. Existing owner budgets still apply; selecting a large
combination can refuse even when each individual result fits its own profile.
`page_complete` means only that the requested retained record set ended.

## CLI

```console
neverd web package-assessment ./capture 1 package-json -- --registry 2 3 4 --advisory 5 3 --markdown
neverd web archive-assessment ./capture tgz 1 3 package-json -- --registry 2 3 1 --source 0:commonjs --source 1:commonjs
```

The standard LLVM `--` terminates global option parsing before assessment
selections. `--registry` selects captured metadata, its descriptor and the
original artifact by three root inventory indices. `--advisory` selects a
captured advisory and descriptor; it may repeat up to 16 times. `--source`
selects an explicitly typed source up to 16 times. Source indices refer to root
artifacts for `package-assessment` and retained archive members for
`archive-assessment`. The manifest selection uses the same respective domain.
Registry/advisory indices always refer to root artifacts. Use the current
inventory to obtain ordinals; do not assume an example's numbers fit another
capture. Omitting `--markdown` emits the assessment summary and JSON finding
pages. Disposition updates use the persistent SDK, worker or MCP session.

## Qualification scope

Core and SDK cases cover foreign registry binding, immediate archive ancestry,
unsupported advisory selectors, canonical selection order, subject digest
ownership, shared review capacity, stale heads, replay, paged inventories,
private-field canaries and native CLI without an external `PATH`. Worker and
MCP cases compare all five operations against the shared API and reject
raw-export or arbitrary-note fields. Actual evidence and exact verification
results are recorded in [the implementation ledger](web-analysis-implementation.md).
See the [actual Claude/OSV qualification](web-package-assessment-qualification.md)
for pinned inputs, report hashes and the distinction between archive, signature
and source evidence.
This profile is one bounded increment of issue #715; the JavaScript epics
#714–#718 retain the gaps listed in the support matrix and implementation ledger.
