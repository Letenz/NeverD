# Offline package review journals and desktop workbench

`offline-package-review-journal-v1` persists fixed analyst assertions against an
exact immutable `offline-package-assessment-v1` identity. The journal contains
only `schema_version: 1`, `profile`, `package_assessment_id`, `review_root_id`,
`review_head_id` and `decisions`. Each decision uses the existing seven fields:
`decision_id`, `package_assessment_id`, `finding_id`, `parent_head_id`,
`disposition`, `decision_index` and `assertion_source: caller`.
Names, paths, source, commands, notes, signatures and credentials are absent.
Hash-chain validation detects inconsistent evidence or history; it does not
authenticate an analyst or establish that their assertions are correct.

## Atomic restoration

The additive C API `neverd_web_package_review_restore_json` takes the current
revision, assessment ID, expected current review head and journal JSON.
`SessionPackageReviewRestore` admits at most 256 KiB, depth four, 4,096 JSON
nodes and 64-byte strings, with at most 256 decisions. Unknown/missing fields,
duplicate JSON keys, foreign identities, unsupported profiles and dispositions
are refused. No file, target runtime, remote service or key is read by this API.

`packages/assessment/Disposition` remains the single owner of review identities,
finding membership, ordinals, parent relationships, prefix matching and charges.
It validates the entire candidate before preparing a replacement. The current
chain must be an exact prefix: restoration cannot erase, reorder or fork accepted
history. A fresh capture/reconstructed assessment can restore from its root only
when the immutable assessment identity agrees. A changed package, evidence
selection or profile cannot inherit the saved assertions.

Only the suffix charges the existing shared 256-event/256-KiB session allowance.
An identical chain at the exact current head returns `replayed: true` and zero
`restored_events`, even at capacity. An old expected head refuses; unlike the
append operation, restoration has no old-parent replay exception. Parsing,
validation, capacity, allocation or response serialization failure leaves the
prior chain and counters unchanged. Publication occurs after response
serialization. Every subsequent finding, decision and report page still pins
the revision and current review head. The backend-omission build exports the
same C symbol with the ordinary explicit unavailable result.

Worker and MCP use `web_package_review_restore` with `schema_version: 1`,
`revision`, `package_assessment_id`, `expected_review_head` and bounded
`journal_json`. They delegate to that C API and accept no arbitrary note, path,
raw-export option or disposition extension. This operation changes only caller
assertions; it grants no input execution, disclosure, trust or safety verdict.

## Desktop workflow

**File → Offline package review** opens one modeless Qt Widgets workbench with
its own configured NeverD worker. Native project state and its analysis workers
keep their existing ownership. The new GUI library has no LLVM, interpreter,
external analyzer or target-launch dependency.

1. Preview one local file or directory, then capture the exact preview. The
   backend rereads and compares the snapshot. The preview lists counts, byte
   totals and the existing metadata-only disclosure policy.
2. Select a captured artifact and either a loose manifest/lock file or an
   explicit tar/tgz/zip archive. Select an available archive member before
   analysis. Changing the archive selection clears the prior member choice.
3. Select `package-json` or `npm-lock`. Optionally select all three captured
   registry inputs: original bytes, registry metadata and its descriptor. The
   shared owners produce the integrity/attestation results and compose the
   assessment. Missing registry selection is `not_selected` coverage, not a
   negative or safe-package result.
4. Filter/sort findings and inspect their opaque evidence/source anchors.
   Evidence, binding, confidence, disposition and disposition source have
   separate columns. Record a fixed disposition explicitly; none is automatic.
5. Save a local `.ndreview` file. Loading a file previews its decision count
   without applying it. Reconstruct the same assessment and explicitly restore
   the loaded review. The backend validates and commits the entire suffix.

The controller sends serial asynchronous requests, expires a request after
120 seconds and can stop its private worker. It publishes a complete bounded
page collection only after checking offsets, continuation, revision, owner and
review head. Archive pages use their existing `total`/`next_offset` contract;
finding/history pages use their existing `page_complete` contract. Inventories
admit at most 4,096 rows, findings 8,192 rows and aggregate metadata 16 MiB.
Pages contain at most 128 rows. A failed refresh disables review/save controls
until a consistent finding/history collection is available.

The journal writer uses [Qt QSaveFile](https://doc.qt.io/qt-6/qsavefile.html),
keeps direct-write fallback disabled and requests owner read/write permissions.
Failed validation or writes preserve an existing file. The reader admits only
the bounded canonical compact JSON form emitted by this writer, rejecting
duplicate-key ambiguity before restoration. SDK journals need not use this
Qt serialization form. File permission behavior is host-dependent; journal
contents are not signed, encrypted or a substitute for access control.
Unsaved assertions require save or an explicit discard before replacing their
visible assessment, pressing Escape, closing the window or quitting the main
workbench. Stopping its worker keeps the consistent local journal available
for saving. Restoring a loaded exact saved head
does not make that saved chain dirty.

The workbench inherits the existing desktop palette, uses scalable system
fonts, plain-text labels, keyboard-accessible widgets and a resizable table/detail
splitter. Registry selectors remain collapsed until requested. Confidence is
expressed in text, never color alone. New strings use Qt translation contexts;
translated wording for every shipped locale and native accessibility/host
qualification remain pending.

This increment presents manifest and selected registry evidence. GUI advisory,
source/installation/comparison selection, raw-source views, complete dependency
resolution, publisher authentication, installed-tree/runtime verification and
the remaining #714–#718 acceptance gaps are not claimed as completed.
