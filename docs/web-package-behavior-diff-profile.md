# Selected Node package behavior comparison

`node-selected-behavior-diff-v1` compares recognized syntax patterns from two
explicit source selections. It consumes the existing package comparison and
Node behavior owners. It does not execute JavaScript, installation commands or
external tools, acquire packages, identify malware or prove a release safe.

## Evidence and coverage

The required base is a retained package diff. Either side may optionally select
one retained package behavior result belonging to that exact package analysis.
An absent selection remains `not_supplied`; zero selected rows cannot establish
zero behavior. Each side retains package ownership, caller-selected source mode
and upstream origin/value analysis status from the original analysis.

Direct sources pair only through exact inventory-relative paths. This is a file
candidate relation, not release alignment, provenance, rename detection or call
correspondence. Missing readable counterparts mean `not_captured_or_unavailable`;
unselected files and unavailable behavior remain separate coverage states.
Embedded sources remain unpaired, even when their outer delivery files have the
same path. No traversal invents ownership of embedded modules.

Changed declared root names, platform condition values or coverage, and metadata
contracts prevent pattern comparison. Missing declared names remain visibly
unverified. Different source modes also prevent comparison. An unavailable or
budget-exhausted upstream value/origin model makes the source pair incomparable:
the same `Buffer.from` call must not acquire a fake encoding change merely
because unrelated constants exhausted a value-analysis budget.

## Results

Three paged tables retain independent evidence:

- `pairs`: before/after source, artifact, file, package and behavior anchors,
  analysis mode/coverage, comparison status and supplied-file byte equality.
- `patterns`: counts before/after/delta for each source pair, fixed API class,
  kind, origin class, encoding class, optional/spread syntax and argument count.
- `occurrences`: every compared source's original record side, package behavior, source,
  artifact, file, node and byte span, plus its source-pair and pattern-group IDs.

Grouping is per source pair. A pattern disappearing from one selected file and
appearing in another produces separate count observations. It does not prove a
call moved. Equal counts do not imply equal behavior: arguments are unexamined,
and supplied-file byte changes remain independent. Every occurrence can be
resolved through the original package behavior records even when the independent
Node behavior cache was never populated.
Incomparable pairs retain their source/package-behavior anchors and coverage
states; their records are not counted or emitted as comparison occurrences.

Ordinary responses contain fixed vocabulary, counts and opaque evidence anchors.
They omit paths, package names, commands, argument values and fingerprints of
short private values. Runtime API identity, reachability, complete source
selection, release alignment and call correspondence remain unverified.

## API and limits

`neverd_web_package_behavior_diff_analyze_json` accepts revision, base package
diff ID and `{"schema_version":1}` with optional `before_package_behavior_id`
and `after_package_behavior_id`. IDs must select the corresponding retained
analysis. `neverd_web_package_behavior_diff_records_json` pages `pairs`,
`patterns` or `occurrences`, with 1–512 rows per page.

The core accepts at most 16 sources and 8,192 existing behavior records per side,
32 source-pair rows, 16,384 pattern groups/occurrences, 8,000,000 charged work
units and two cached comparisons. Existing package selection limits also apply.
Both sides share the session's existing 16-source and 200,000-node cache,
including its conservative per-source reservation; these core ceilings do not
increase session admission. Inventory traversal, key comparisons, record
validation, retained anchors and deterministic sorting consume the work budget.
Any failure publishes no partial comparison. Reimport revokes the cache.

CLI inputs are captured once. Artifact indices and parse modes are explicit:

```console
neverd web package-behavior-diff ./capture 6 3 package-json before:5:commonjs after:2:commonjs
neverd web archive-package-behavior-diff ./archives tgz 2 3 1 3 package-json before:0:commonjs before:1:commonjs after:0:commonjs after:1:commonjs
```

Omitting all source arguments reports missing selection without fabricating
source models. At most 16 source arguments are accepted across both sides.
Worker and MCP expose the same analysis/paging operations using retained IDs;
they do not accept source text, paths, execution directives or raw-value export.

This implements selected syntax comparison within #715. The separate
[assessment profile](web-package-assessment-profile.md) composes these results
with selected advisory/provenance evidence and analyst dispositions. The
[epic acceptance audit](web-epic-acceptance.md) records the combined coverage.
