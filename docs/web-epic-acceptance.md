# JavaScript epic acceptance audit

This audit was rechecked against all five GitHub issue bodies and comments on
2026-10-15. The required operations are implemented within the named profiles
below. The v2 passive report resolves the two earlier #717 findings; the final
#716 review found and repaired a shared input/archive filename-policy mismatch.
Final closure verification is recorded in the implementation ledger.

A passed fixture or captured sample qualifies its selected profile and
operation; it does not establish general framework support, source equivalence,
publisher trust or runtime behavior. Explicit future adapters and unsupported
semantics remain visible boundaries rather than implied implementation claims.
All new implementation and authored fixtures use C++. Target samples are never
executed, and the analyzer uses no external scripts, runtimes or network tools.

## Required issue coverage and supported boundaries

| Issue | Implemented, reviewable evidence | Explicit supported boundary |
| --- | --- | --- |
| [#714](https://github.com/NeverSight/NeverD/issues/714) | Immutable source/map origins, bounded C++ parser, lexical/value/effect/module/bundle/navigation models, reversible primitive/rename/Boolean-expression profiles, reviewed source views and verified same-tree readable export. Known tool/config/permission/hook/MCP fixtures retain spans and uncertainty. Explicit large-source admission now parses and navigates all 2,364 pinned macOS ARM64 Claude units. Configuration order retains 418 objects, 3,702 terms and 32 possible later overwrites. Independent minified file navigation retains 973 literal sites, 592 local lexical call candidates and 69 layer arrays; interactive admission keeps its earlier bounds. | Enabled file layers, effective precedence, other merge forms and computed dispatch remain unresolved. The issue requires identifiable source navigation and explicit unknowns, not effective policy or universal dataflow. Erased TypeScript/types/names and whole-program equivalence are not promised. |
| [#715](https://github.com/NeverSight/NeverD/issues/715) | npm graphs/diffs, tar/gzip inventory, original-byte SRI, Node behavior/install associations and diffs, advisory/registry/provenance imports, assessments, persistent dispositions and Qt review. Eight Claude packages retain independent archive/payload integrity. | npm lockfile versions and the literal Node installation grammar are named; pnpm/Yarn, complete runtime resolution and other GUI selectors are further adapters/workbench extensions. The GUI preserves selected confidence/coverage/dispositions, with unselected evidence explicit. Hashes/signature declarations prove no publisher trust or benign behavior. |
| [#716](https://github.com/NeverSight/NeverD/issues/716) | Safe ASAR/ZIP, Electron unpacked/manifests/entries/IPC, NW.js/VSIX/HTML/native handoff. Real Wails Go 1.25.7 / Wails 2.11.0 ELF/PE x64 and measured Tauri PHF/Mach-O x64 assets have exact raw/readable exports and native links. Tauri yields 15 assets and 9 readable JS members. Direct Tauri/Wails bridge syntax retains private source anchors. Fixed DataPack v5/u16 and Microsoft v5/u32 readers qualify actual CEF 120.2.7 and Teams WebView2 resources with explicit gzip, JS/HTML consumers and exact origins. Supplied resource descriptors remain independently supported. | Other Rust/Go layouts, ISAs, codecs, bridge aliases and runtime mount/window relations need independent profiles. Resource tables and supplied descriptors do not authenticate frameworks or runtime behavior. The issue asks for supported versioned layouts and clear refusal of unsupported builds. |
| [#717](https://github.com/NeverSight/NeverD/issues/717) | Redacted HAR preview/commit, fetch/WebSocket/XHR/wrapper candidates, request/result and nearby JSON/base64/URI/crypto/zlib syntax links, direct-fetch HAR joins and versioned streams/logs. The v2 report adds template segments/expressions and lexical path/query roles, individual header and body syntax, per-example JSON field/array trees, capture-local type unions and observed presence. Existing source features, recorded links, same-MCP-method candidates and pagination remain. | Source components remain syntax; dynamic interpolations can change URL delimiters and cannot create new HAR matches. Names/values remain private. Observed absence is not inferred server optionality. Source body calls, unsupported object syntax, framework/API identity and server behavior remain unknown. No universal dataflow or optional OpenAPI completeness is claimed. |
| [#718](https://github.com/NeverSight/NeverD/issues/718) | Independent Bun ELF/thin Mach-O/PE x64/ARM64 and Node SEA 22.15.0, nexe 4.0.0-rc.7, pkg 5.8.1 profiles; producer fixtures, exact original/member/range hashes, bounds, opaque caches and shared consumers. All eight Claude 2.1.296 deliveries preserve every shipped JS unit and verified readable copy. | New runtime/layout/host profiles require independent measurement. Opaque JSC/V8 caches remain explicitly unsupported, as the issue permits. Extraction is separate from native lifting and complete cross-track runtime behavior. |

The [support matrix](web-support-matrix.md) is authoritative for individual
profiles. The [implementation ledger](web-analysis-implementation.md) records
build/test receipts and their exact baselines. The GitHub issue state records
closure; this audit supplies its acceptance evidence without replacing those
narrower contracts.

## Historical re-audit findings and resolution

Source review used implementation `03acf4d03` and compared the affected Web
owners with fetched `dev` at `587227e57`; those owners were unchanged. Issue
bodies and comment counts were read from GitHub on 2026-10-13. That earlier
review did not close, edit or comment on any issue.

1. **#717 request structure: original finding, resolved by v2.**
   `interfaces/SourceContext.cpp` records unresolved template expression counts;
   `interfaces/SourceFetch.cpp` retains headers/body node IDs. For an original
   fixture with a `/users/${id}?page=${page}` URL, two ordinary header properties
   and a `JSON.stringify` object body, the production CLI returns two template
   holes, no comparable endpoint, zero path/query counts and only headers/body
   anchors. The whole source is preserved, but the interface inventory does not
   represent static components, parameter shapes or individual fetch headers.
   The finding called for bounded component/field evidence with private or
   reviewed names, stable anchors and explicit unknown values. This does not
   require evaluating the request or changing HAR matching rules.
2. **#717 field-level schema evidence: original finding, resolved by v2.**
   `interfaces/report/Shapes.cpp::countTypes` discards parent/field relationships.
   The production `report-har` command gives both original fixture bodies
   `{"left":{"x":1},"right":true}` and
   `{"left":1,"right":{"x":true}}` the same shape summary: two objects, one
   number, one Boolean and two top-level members. Their observation IDs remain
   distinct, but their different field types/nesting cannot be inspected through
   the v1 report. The finding called for bounded observed field/array nodes,
   example links and presence evidence across explicitly selected observations;
   keep inferred
   optionality separate from observed absence and unknown server constraints.
   Redaction must preserve structural relationships without publishing secrets.
   OpenAPI remains optional; this finding concerns the issue's parameter-shape
   and exported-schema requirements.

The two probes used the production NeverD CLI with an unusable external-tool
PATH and returned zero stderr. Fixtures were authored by an inert C++ generator,
not executed as JavaScript. Local receipts are
`/tmp/neverd-issue-audit-{source,har-preview,report}.jsonl` and
`/tmp/neverd-issue-audit-probe.cpp`. Focused verification passed 13 report/desktop,
58 source and 92 package/runtime checks: 163 distinct checks in total. The
optional Claude feature-corpus selection initially skipped without its
environment input, then passed against the preserved macOS ARM64 package in
25.73 seconds. Its receipt is `/tmp/neverd-issue-audit-claude-corpus.log`.

### Follow-up implementation and evidence

The `passive-evidence-shape-report-v2` implementation addresses both findings.
The same source fixture now yields 23 static rows, including 21 structure rows:
three template segments, two expressions, eight lexical components, two header
fields and two body fields. The same two HAR bodies yield 19 observed rows,
including eight value nodes and five aggregate field nodes; their nesting is
distinct and shared fields retain capture-local identities.

The actual CLI used an unusable external-tool PATH and emitted no stderr.
With the JS parser disabled, the HAR report is byte-identical. C API one-row
pages, worker and MCP preserve the same parent/field evidence. All 46 initial
route checks passed; owning Web regression passed 749 cases with 51 explicit
optional-input/feature skips out of 800. Parser-off checks passed 28 cases.
Backend omission passed five checks and explicitly skipped worker's positive
backend case. Qt review passed. Restored source/SDK/transport checks plus the
provided Claude feature corpus passed 48 cases; the later eight request
structure checks include both pinned real templates and escaped-coordinate
boundaries. See the implementation ledger for receipts and integration.

Final normal integration with remote `01a640f7d` passed **764 of 804**
owning Web/availability cases with **40 explicit skips**, including all eight
Claude platform-package fixtures and the two new template witnesses. A stale
earlier relation-profile assertion was corrected before this complete rerun.
Four worker/MCP checks and Qt review also passed; merged CLI outputs are
byte-identical to the pre-merge results.

Real Claude 2.1.296 macOS ARM64 modules 1357 and 1740 retain respectively ten
and eight structure rows, each with three template segments and two exact
expression anchors. The new C++ regression pins original sizes/hashes and
checks actual interpolation delimiters. Headers/body variables remain unknown;
no traffic was supplied. A broader 20-module candidate probe returned 16
bounded reports and four existing byte/parser-budget refusals, all with empty
stderr. It does not establish full client network reconstruction.

The explicit `hermes-602befee-large-source-js-v1` follow-up removes the four
earlier selected byte/parser refusals without changing interactive admission.
Pinned modules 141/299/415/1452 retain 180,783/185,080/1,165,367/286,849 nodes,
4/3/3/6 interface candidates and 12/33/141/31 passive report rows. All four
production CLI commands use an unusable external-tool PATH and empty stderr.
The whole retained macOS ARM64 corpus now has 2,364 parsed, navigated feature
inventories with zero parser/feature refusals under explicit large admission.
This qualifies named syntax navigation and recorded original anchors, not
runtime requests, effective policy or full dynamic client reconstruction.

No additional blocking gap was established for the already named #714, #715
or #718 profiles in that review. Subsequent qualification addressed these
recommendations:

- **#714 real-client navigation:** the independent [file navigation profile](web-configuration-file-profile.md)
  now completes all 2,364 original units and retains 973 literal sites, 592 local
  lexical call candidates and 69 layer arrays through minified names. The old
  named-loader grammar still reports zero. Arbitrary alias/import propagation,
  enabled layers, merge semantics and effective file precedence remain unknown;
  recorded syntax relationships do not prove runtime loading.
- **#716 CEF/WebView:** the follow-up [DataPack readers](web-datapack-profile.md)
  now qualify actual CEF 120.2.7 NuGet resources and Teams' WebView2
  154.0.4258.37 resource pack. Separate v5/u16 and v5/u32 profiles retain all
  original resources/aliases, explicit gzip selections and nested origins. Fixed
  JS/HTML consumers and source anchors pass. Framework authentication, native
  runtime activation and arbitrary custom client layouts remain unverified.
  [Fixed CEF GitHub qualification](https://github.com/NeverSight/NeverD/actions/runs/38413710185)
  also passes at integration commit `9276ef94c`.
- **#718 regression qualification:** the [required release lane](web-producer-corpus-qualification.md)
  now selects all eleven fixed full-image cases, requires immutable corpus
  URL/hash inputs and fails missing files/consumers instead of skipping. The
  combined Web release qualification calls both the producer and fixed CEF
  lanes, and its final gate requires both to succeed; cancellation/skips fail.
  The full-image corpus has been rebuilt in persistent storage: all original
  container hashes match, and eleven required local cases pass without skips.
  A separate fixed snapshot witness retains its opaque meaning. The
  [combined remote receipt](https://github.com/NeverSight/NeverD/actions/runs/38502323427)
  now passes at `8c1f4ac03`: eleven producer and fifteen CEF/resource cases have
  zero skips, and both called jobs plus the final gate succeed. The fixed corpus
  is now hosted with corresponding source/notices and the retained receipts in
  the [NeverD corpus Release](https://github.com/NeverSight/NeverD/releases/tag/web-producer-corpus-2026-10-13).
  The combined lane defaults to its exact archive URL/SHA-256, and the required
  automatic caller selects the same pin for affected `dev`/PR changes without
  credentials. The [automatic public-download receipt](https://github.com/NeverSight/NeverD/actions/runs/38539252447)
  at `e669ac89e` passes all eleven producer and fifteen CEF/resource cases
  with zero failures/skips. Explicit invalid HTTP selection still fails
  preflight and the final gate. Windows input formats are supported on
  the qualified macOS host. The [Windows product lane](web-windows-product.md)
  now qualifies the Windows Server 2025 x64 NTFS reader, private spool and
  export owners, with 110 native cases, two adapter checks and six extracted
  package checks at `5e126750ae75`, all without failures or skips. The same
  [qualification run](https://github.com/NeverSight/NeverD/actions/runs/38677375352)
  passes eleven full producer and fifteen CEF/resource cases on macOS arm64,
  also without failures or skips. Both final gates and publication succeeded;
  the [formal Windows Web product Release](https://github.com/NeverSight/NeverD/releases/tag/web-product-windows-x64-2026-10-15)
  contains the exact qualified archive. Independent public downloads verified
  its receipt, all asset checksums, all 139 ZIP members and the tagged source
  tree. [Product details and SHA-256](web-windows-product.md) record this
  Windows Server 2025 x64 delivery boundary.

Erased TypeScript information, general JSC/V8 bytecode decompilation, universal
framework support and fabricated live traffic are not missing acceptance work.

## Fixed Claude Code cross-track selections

The input release is **2.1.296**, captured from the npm platform-package
delivery channel. [The platform qualification](web-claude-code-cross-platform-qualification.md)
pins each original tarball and executable SHA-256, platform/container profile,
source totals and export manifest. It is a fixed observed release, not a claim
about the latest release. No sample or recovered source is redistributed.

`pass` below means the stated extraction operation passed; `partial` means the
selected analysis exposes narrower evidence and documented unknowns;
`not attempted` means that cross-track analysis was not qualified for this
particular input. These are per-input operation states, not issue completion.

| Selected platform package | #718 exact JS extraction/export | #714 source analysis | #715 provenance | #716 desktop wrapper | #717 passive mapping |
| --- | --- | --- | --- | --- | --- |
| linux-x64 | pass | partial | partial | not attempted | partial |
| linux-arm64 | pass | partial | partial | not attempted | not attempted |
| darwin-x64 | pass | partial | partial | not attempted | not attempted |
| darwin-arm64 | pass | partial | partial | not attempted | partial |
| win32-x64 | pass | partial | partial | not attempted | not attempted |
| win32-arm64 | pass | partial | partial | not attempted | not attempted |
| linux-x64-musl | pass | partial | partial | not attempted | not attempted |
| linux-arm64-musl | pass | partial | partial | not attempted | not attempted |

All 18,794 shipped JS units have exact decoded bytes and readable copies with
matching reparsed structure. Counts include duplicate content across platforms.
Source navigation and other semantic analyses remain bounded selections, not
full application recovery. All eight inputs have no source maps. JSC cache
bytes are preserved separately with unsupported decoding semantics.

All original archive SHA-512 declarations match their selected archive bytes.
Registry/signature/provenance declarations retain separate verification states;
publisher identity and installed dependency trees remain unverified. The npm
wrapper assessment and linux-x64 VSIX-to-Bun qualification concern separately
pinned inputs, not an inferred desktop wrapper around every platform package.

The passive selections include the pinned linux-x64 module 1897 and macOS ARM64
modules 928, 1905 and 1920. The new wrapper profile refuses to invent caller flow
for the complex dispatch in modules 1905/1920; their four fetch occurrences keep
exact anchors and dynamic URL/method/options limitations. No HAR or transcript
was supplied for these selections, so source/capture correlation is not attempted.

## Criterion coverage

The table maps implemented evidence to the original acceptance items; it is not
a claim that every framework/version is supported. The #717 gaps above are
addressed by the v2 profile. Additional builds,
runtime resolution and workbench workflows may extend the named profiles;
absent optional samples are omissions, never passed acceptance cases.

| Issue criteria | Owning checks and published contract |
| --- | --- |
| #714 syntax/bundler/map/transform matrix | [Support matrix](web-support-matrix.md), parser/source-map/bundle tests and each rewrite profile |
| #714 benign originals/mappings and negative semantics | `WebSource*`, `WebSourceValues`, effect/binding/rewrite/rename/branch and export tests cover original preservation, coercion/effects/getters/exceptions/eval/with, invalid maps and exact/short budgets |
| #714 deterministic IDs, limits, SDK/CLI/MCP and validation meaning | Source/navigation/anchor/view/SDK and worker/MCP tests; exact inverse and same-tree checks are separate from runtime/whole-program equivalence |
| #714 tool/config/permission/MCP/hooks and sensitive-field supplement | `WebSourceFeatures`, `WebConfigurationOrder`, client corpus and bridge/navigation tests; [client](web-client-feature-profile.md), [order](web-configuration-order-profile.md), shared preview/redaction policies |
| #715 reproducible versioned graph and conditional/unresolved edges | `WebPackages`/package SDK fixtures and [package profile](web-package-profile.md) |
| #715 legitimate/suspicious/synthetic behavior and exact version diff | Node behavior, package behavior/install/diff suites retain occurrences, source anchors, uncertainty and explicit version/platform selections |
| #715 advisory freshness, integrity/provenance contradictions and no execution | Advisory/attestation/integrity/archive/assessment tests and their independently versioned profiles; invalid, missing, unselected and unverified states remain distinct |
| #715 GUI/CLI/MCP confidence, incomplete coverage and dispositions | Assessment/review SDK/transport tests plus `NeverDGuiWebReview`; fixed evidence and caller assertions stay separate and journals restore atomically |
| #715 supplied native distribution supplement | `ClaudeDistributionTests`, installer/delivery/integrity tests and the pinned cross-platform manifest retain OS/ISA/libc, package/executable links, matching/different/missing hashes and independent signature states |
| #716 matrix, ASAR/unpacked/NW and Tauri/Wails supported/native handoff | Desktop/ASAR/Electron/Wails/Tauri suites and real fixed-profile releases; [resource descriptor](web-desktop-resource-profile.md) covers named supplied CEF/WebView assets |
| #716 hostile archives, reproducible origin/export, offline and opaque data | Archive/ZIP/ASAR/path/export suites; native/readable SDK checks preserve exact compressed or direct parent ranges and explicit unsupported members |
| #716 VSIX/helper/version/redaction supplement | ZIP/desktop SDK and pinned VSIX-to-Bun checks retain separate extension/helper versions; normal pages exclude private content |
| #717 literal/template anchors, static/HAR-only/correlated classes | `WebRequestStructure` covers template/URL/header/body syntax, private names, exact source anchors, unknown forms and unchanged HTTP matching. Existing HTTP equality remains authoritative |
| #717 canaries, malformed/missing/WS/budgets, offline and stable navigation | HAR/stream/source/SDK/CLI/worker/MCP checks; no raw values, implicit network, target runtime or manufactured offsets |
| #717 exported example/schema boundaries and stream/source supplement | `WebPassiveStructure` covers per-example field/array trees, capture-local type/presence aggregates, missing versus null, grouping, names and budgets. SDK/CLI/worker/MCP use the same [report](web-passive-report-profile.md); server optionality/constraints remain unknown |
| #718 matrix and primary pinned plain/minified/assets/maps/cache fixtures | Bun/SEA/nexe/pkg suites and redistributable producer graph/blob manifests; each format has independent version/platform/profile coverage |
| #718 reproducible ranges, hostile input, source frontend and opaque caches | Extraction/map/native/source/export suites refuse truncated/unknown/compression-bomb data and retain unsupported cache evidence |
| #718 fixed Claude manifest and per-track states | [Eight-platform qualification](web-claude-code-cross-platform-qualification.md) and the per-input table above; all 18,794 shipped JS units preserved, absent observations/maps and partial source analyses explicit |

Final build, omission, transport, GUI and integration receipts are recorded in
the [implementation ledger](web-analysis-implementation.md). This document
records reviewed coverage, not a GitHub issue-state mutation. Earlier increment
receipts remain historical evidence; the criterion and profile mapping above
supersedes their blanket issue-status conclusions.

The earlier Tauri source research remains relevant but separate from the
new binary qualification. At the pinned
`tauri-cli-v2.12.2` source tag, the framework crate is `2.12.3` and enables
compression by default. [Pinned crate manifest](https://raw.githubusercontent.com/tauri-apps/tauri/tauri-cli-v2.12.2/crates/tauri/Cargo.toml).
The generator emits included output bytes;
the asset object stores a PHF map from strings to byte slices. Its selected
compression feature controls Brotli decoding versus borrowed bytes, without a
per-entry codec field. [Generator](https://raw.githubusercontent.com/tauri-apps/tauri/tauri-cli-v2.12.2/crates/tauri-codegen/src/embedded_assets.rs),
[asset implementation](https://raw.githubusercontent.com/tauri-apps/tauri/tauri-cli-v2.12.2/crates/tauri-utils/src/assets.rs).
Rust's default representation also does not guarantee declaration-order field
offsets. [Rust layout reference](https://doc.rust-lang.org/reference/type-layout.html#the-rust-representation).
Therefore each additional binary profile must measure layout, stored pointer anchors,
relocations and encoding against a real preserved build. These source observations
are research evidence; the qualified reader above uses its own measured native
initializer, complete PHF relation and explicit Brotli profile. Neither is a
binary-version detector or permission to guess codecs and carve source strings.
