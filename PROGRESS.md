# NeverD Daily Progress

Last verified: **2026-10-01 09:02 Asia/Shanghai (UTC+08:00)** / **2026-10-01 01:02 UTC**

This is a point-in-time daily issue/PR and static-review tracker. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md), and [contribution guidance](CONTRIBUTING.md) remain authoritative. Priorities are proposals, not assigned deadlines or a completion percentage.

## Current snapshot

Source snapshot is taken before opening the documentation-only daily-tracker draft PR.

| Measure | Verified state |
| --- | --- |
| Open issues | 17 |
| Open pull requests | 0 |
| PRs merged since 2026-09-30 03:51 UTC | 19: #220 and #267–284 |
| Issues closed in the same window | 0 |
| Observed dev commit | [6eb2e5c6](https://github.com/NeverSight/NeverD/commit/6eb2e5c6423f7b2977568fe070c7cd334f503572) |
| Previous observed dev commit | [4097148c](https://github.com/NeverSight/NeverD/commit/4097148c8de508f35bab1ad237ba0776c3a3dde6) |
| Change inventory | 160 commits; 410 changed file/submodule paths |
| New confirmed defects in today's sampled code | 0; no production-code change proposed |

The [comparison](https://github.com/NeverSight/NeverD/compare/4097148c8de508f35bab1ad237ba0776c3a3dde6...6eb2e5c6423f7b2977568fe070c7cd334f503572) was read across two commit pages (100 + 60). Its changed-file response stops at 300 paths, so the 410-path inventory instead comes from comparing the two complete recursive Git trees; neither tree was truncated. Commit enumeration and path inventory are not claims that every change was code-reviewed.

### Changes since the previous snapshot

- [#220](https://github.com/NeverSight/NeverD/pull/220) is now merged. Its former draft state and failed historical head are no longer current open-PR blockers.
- All 18 subsequently created PRs, [#267](https://github.com/NeverSight/NeverD/pull/267) through [#284](https://github.com/NeverSight/NeverD/pull/284), are also merged.
- [#283](https://github.com/NeverSight/NeverD/pull/283) consolidated failure-atomic ARM64 native integer-state capture.
- [#284](https://github.com/NeverSight/NeverD/pull/284) corrected MMIO test callback ownership across standard-library implementations.
- Recent work also includes KVM thread/state reuse, native execution profiles, loop refinement, mobile recovery and documentation fixes. Those broader features were inventoried, not comprehensively audited today.

## Static review

**Mode:** Static source and diff inspection only. No repository program, build, test, linter, formatter or script was executed. Existing GitHub Actions evidence was read without dispatching or rerunning workflows.

**Primary change:** [4f189d5e](https://github.com/NeverSight/NeverD/commit/4f189d5e630d207f6e4bfac803ff6c679a3c0973), inspected in the current dev tree, with related callers and rollback code. **Secondary change:** [bfb9a527](https://github.com/NeverSight/NeverD/commit/bfb9a527b0bbd154b81fbec92a489a664848ae0c), the MMIO fixture-ownership fix.

### Source coverage

The bounded review covered these 18 code, test and build artifacts. Relevant sections, rather than entire files, are identified explicitly.

- [AArch64GeneralState.cpp](lib/emulation/arch/aarch64/AArch64GeneralState.cpp), [.h](lib/emulation/arch/aarch64/AArch64GeneralState.h), and [.def](lib/emulation/arch/aarch64/AArch64GeneralState.def)
- [AArch64Machine.h](lib/emulation/arch/aarch64/AArch64Machine.h) and [AArch64Machine.def](lib/emulation/arch/aarch64/AArch64Machine.def)
- [Registers.h](include/neverd/emulation/Registers.h) and ARM64 inventory/ordering in [Registers.def](include/neverd/emulation/Registers.def)
- [KvmAArch64Machine.cpp](lib/emulation/backends/kvm/KvmAArch64Machine.cpp) and [WhpAArch64Machine.cpp](lib/emulation/backends/whp/WhpAArch64Machine.cpp)
- [CheckedAArch64Backend.cpp](lib/emulation/arch/aarch64/CheckedAArch64Backend.cpp)
- [RAMTransaction.h](lib/emulation/core/RAMTransaction.h) and [RAMTransaction.cpp](lib/emulation/core/RAMTransaction.cpp)
- [AArch64GeneralStateTests.cpp](unittests/emulation/AArch64GeneralStateTests.cpp) and [AArch64GeneralStateCases.def](unittests/emulation/AArch64GeneralStateCases.def)
- [lib/emulation/CMakeLists.txt](lib/emulation/CMakeLists.txt) and the ARM64 test-target stanza in [unittests/emulation/CMakeLists.txt](unittests/emulation/CMakeLists.txt)
- The `SharedDeviceRetirementReleasesCallbacksBeforeCPUsResume` case in [MemoryLifecycleTests.cpp](unittests/emulation/MemoryLifecycleTests.cpp) and `ExactUnmapRetiresCallbacksAndReturnsBudget` in [UnicornMMIOTests.cpp](unittests/emulation/UnicornMMIOTests.cpp)

Repository guidance, relevant architecture/testing sections, and workflow trigger/concurrency definitions were also inspected.

### Findings and evidence

No new correctness defect was established strongly enough to justify an automatic code fix in this scope.

- The ARM64 helper captures X0–X30, SP, PC, NZCV and TPIDR_EL0 into a copy, returns before publishing on any failed read, masks NZCV, then assigns the complete state. The register ordering and both native adapters agree with the 35-entry inventory.
- KVM raw reads and WHP captured-value indexes feed that same helper. The checked caller uses a separate next CPU state and publishes it only after the RAM transaction commits. The transaction destructor restores original RAM when native execution exits with an error before staging.
- The portable test source covers every register-read failure position, missing readers, retry, NZCV masking, both privilege modes, and preservation of vectors and untransferred registers. This describes test coverage, not a test result from this review.
- The MMIO regression fixtures now destroy caller-owned callback objects before testing mapping retirement. Their weak-reference assertions therefore do not depend on a moved-from callback container releasing its captures.

**Limitations:** The remaining changed paths, complete Objective-C/Swift pipeline, x64 state-reuse implementation and full issue backlog were not audited at source level. Static inspection does not establish native ARM64 KVM/WHP runtime behavior, cancellation interleavings, build/link success, formatting compliance or release readiness. Native ARM64 evidence remains explicitly outstanding in the project's documentation.

## Existing CI evidence

**CI snapshot:** 2026-10-01 01:01 UTC. Every row below is associated with observed dev head `6eb2e5c6423f7b2977568fe070c7cd334f503572`.

| Workflow | Observed state | Evidence |
| --- | --- | --- |
| CI | In progress; Linux, macOS and Windows jobs running | [36795886427](https://github.com/NeverSight/NeverD/actions/runs/36795886427) |
| Mobile Decompilation | Workflow API queued; Windows job succeeded, Ubuntu running, macOS queued | [36795886438](https://github.com/NeverSight/NeverD/actions/runs/36795886438) |
| EVM Upstream Audit | Queued | [36795886326](https://github.com/NeverSight/NeverD/actions/runs/36795886326) |
| Push on dev | Queued | [36795885475](https://github.com/NeverSight/NeverD/actions/runs/36795885475) |
| LLVM Style | Success | [36795886420](https://github.com/NeverSight/NeverD/actions/runs/36795886420) |
| Code Quality: Push on dev | Success | [36795885522](https://github.com/NeverSight/NeverD/actions/runs/36795885522) |
| Prebuilt LLVM Audit | Success | [36795886538](https://github.com/NeverSight/NeverD/actions/runs/36795886538) |
| Mobile Real Applications | Skipped | [36795891920](https://github.com/NeverSight/NeverD/actions/runs/36795891920) |

Exact-head check runs: **24 total: 6 successful, 9 skipped, 4 in progress, 5 queued, 0 failed**. Workflow and job/check states are reported separately because their APIs can show different aggregate states. Legacy commit statuses are empty; the combined status's `pending` value alone is neither a failure nor a full pass.

The complete dev Actions collection created since 2026-09-30 03:51 UTC contained **503 runs**, including **62 main CI runs: 61 cancelled and the current run in progress**. There was no completed green main CI run in that window. Cancellation is not a code failure, but leaves a substantial integration-evidence gap.

Mobile Decompilation in the same window: 54 cancelled, 5 failed, 2 successful and 1 queued. The latest older success was [36769822465](https://github.com/NeverSight/NeverD/actions/runs/36769822465), completed at 2026-09-30 20:19:17 UTC on `ac550f17d77d7e1f5e76fdf7b3509e8a7cea801d`. It does not establish current-head acceptance. Historical Objective-C x86_64 fixture failures and the old #220 failures must not be relabeled as failures of today's head.

## Today's top priorities

### 1. Establish an uninterrupted exact-head integration result

**Status:** Current main CI is still running; all other main CI runs in the tracking window were cancelled.

**Next action:** Inspect the existing run's final Linux/macOS/Windows results and preserve its commit identity. If subsequent development supersedes it, explicitly record that the replacement still needs full integration evidence. No workflow dispatch or rerun is part of this static-only review.

**Acceptance:** All three platform jobs reach terminal states for one identified current integration commit; failures and skipped suites are itemized. Pending or cancelled work is never counted as passing.

### 2. Reconcile mobile acceptance against the merged current tree

**Status:** #220 merged at 2026-09-30 04:39:17 UTC as [2bbd8019](https://github.com/NeverSight/NeverD/commit/2bbd80190a72d1749f5b64c3c19cf4eddec41813). Current Mobile Decompilation is not complete; Mobile Real Applications is skipped.

**Next action:** Use exact-head workflow evidence for Objective-C scalar/calls, Blocks and single-session qualification. Compare any remaining failures with the current source; do not reopen historical fixes merely because the previous tracker recorded a failed PR head. The PR's WMF recovery metrics remain author-reported evidence.

**Acceptance:** Required mobile fixture cases have explicit current-commit results, and real-application qualification has a recorded producer/consumer identity and actual result. A skipped consumer or an older green run does not satisfy this gate.

### 3. Reconcile native execution acceptance and issue planning

**Status:** Portable ARM64 capture tests are present, but native ARM64 KVM/WHP execution evidence is still outstanding. The 17 open issues have unchanged planning metadata; 14 are epics and only [#12](https://github.com/NeverSight/NeverD/issues/12) is assigned.

**Next action:** Map delivered work to [#104](https://github.com/NeverSight/NeverD/issues/104) and related acceptance criteria; separate portable static/test coverage from native transport evidence. Retain [#4](https://github.com/NeverSight/NeverD/issues/4) release readiness and #12's remaining Windows report as open until verified evidence resolves their specific criteria.

**Acceptance:** Each chosen criterion has an implementation/test link or an explicit gap, native transport claims name the actual platform and result, and the next bounded task has an owner and verification requirement. No issue is closed merely because a related implementation PR merged.

## Daily log

### 2026-10-01 — Static review and integration-evidence refresh

- Inventoried 160 commits and 410 changed paths since the previous observed dev commit; reviewed the bounded 18-artifact ARM64 capture/rollback and MMIO test scope above.
- Found no new statically proven defect in the sampled scope; no production-code fix is included.
- Reconciled #220 and #267–284 as 19 merges; 17 issues remain open and no PR was open at the source snapshot.
- Recorded current-head CI separately from historical failures and successes, including the 61 cancelled main CI runs and outstanding current integration result.
- Preserved the complete prior tracker text below. Followed the contribution guide's topic-branch/draft-PR workflow for this documentation-only update; no merge was performed.
- No builds, tests, repository scripts, linters, formatters, manual CI triggers, dependency revisions or security settings were changed. The documentation commit uses `[skip ci]`.

## Tracking conventions and limits

- Source data and CI are point-in-time observations, not continuous monitoring.
- Issues and PRs were paginated separately: 17 open issues and zero open PRs, followed by empty pages. The changed issue/PR collection returned 82 PR records and an empty second page; no ordinary issue updated in the window.
- Exact-head workflows returned 8 records and an empty second page; check runs returned 24 records and an empty second page; legacy statuses were empty. Main CI job pagination returned 3 jobs and then an empty page.
- The dev-window Actions collection was read across six pages (100 + 100 + 100 + 100 + 100 + 3). Every count above is bounded by that query and snapshot.
- The branch metadata reports `dev` unprotected and the repository ruleset collection is empty. The connector does not support the branch-rules endpoint; no protection setting was changed or bypassed.
- Newly created tracking PRs are excluded from the pre-publication source snapshot above. This update is proposed for `dev`; its presence in a draft branch does not mean it has been merged.
- Preserve history and human edits on subsequent refreshes; avoid treating an unchanged historical snapshot as current status.

## Previous snapshot (preserved)

<details>
<summary>2026-09-30 snapshot, priorities, evidence and initial daily log</summary>

# NeverD Daily Progress

Last verified: **2026-09-30 11:51 Asia/Shanghai (UTC+08:00)** / **2026-09-30 03:51 UTC**

This is the daily issue/PR execution tracker. The [roadmap](docs/roadmap.md)
remains the long-term product plan; [architecture](docs/architecture.md) and
[contribution guidance](CONTRIBUTING.md) remain authoritative for implementation.
Priorities below are proposed from current blockers and dependencies, not assigned
deadlines or an overall completion percentage.

## Current snapshot

| Measure | Verified state |
| --- | --- |
| Open issues | 17 |
| Open pull requests | 1: [#220](https://github.com/NeverSight/NeverD/pull/220), draft |
| PRs merged since 2026-09-29 00:00 UTC | 48 |
| Issues closed in the same window | 0 |
| Issue planning metadata | No milestones or explicit priority labels on the 17 open issues; 14 are labeled epic |
| Issue ownership | Only [#12](https://github.com/NeverSight/NeverD/issues/12) has an assignee: NeverSightAI |
| Observed dev commit | [4097148c](https://github.com/NeverSight/NeverD/commit/4097148c8de508f35bab1ad237ba0776c3a3dde6) |

Counts come from GitHub issue search with explicit issue/PR and state filters.
Each query returned `incomplete_results=false`, with all results within the
100-item page. The activity window is not a rolling 24-hour window.

### Active PR: #220

[Recover WMF Objective-C source with verified callbacks and Swift storage](https://github.com/NeverSight/NeverD/pull/220)

- Head: `c4d57f9ea95ad43da5382a9e03e4006d83416a30`; base branch: `dev`
- Open, draft, not merged; GitHub reports mergeable, which does not establish
  test readiness or permission to merge
- No submitted reviews, inline review threads, or requested reviewers were
  returned at this snapshot
- **Author-reported validation:** WMF recovery 4029/5046 methods, 47 gains and
  zero losses versus the PR base; latest focused suites report 692 Objective-C
  source tests and 19 metadata JSON tests passing
- Those reported results have not been independently rerun for this tracker

Verified Actions results associated with that PR head:

| Workflow | Result | Evidence |
| --- | --- | --- |
| CI | Failure | [Run 36662051074](https://github.com/NeverSight/NeverD/actions/runs/36662051074) |
| LLVM Style | Failure | [Run 36662051100](https://github.com/NeverSight/NeverD/actions/runs/36662051100) |
| Mobile Decompilation | Failure | [Run 36662051058](https://github.com/NeverSight/NeverD/actions/runs/36662051058) |
| Prebuilt LLVM Audit | Success | [Run 36662051118](https://github.com/NeverSight/NeverD/actions/runs/36662051118) |
| EVM Upstream Audit | Success | [Run 36662051077](https://github.com/NeverSight/NeverD/actions/runs/36662051077) |

### Default-branch status is different

At observed dev commit `4097148c`, LLVM Style and Python Plugin SDK succeeded,
but Mobile Decompilation failed. Main CI, Push on dev, and Mobile Real
Applications were still in progress. Do not transfer success or failure from
one commit to another.

- [dev LLVM Style](https://github.com/NeverSight/NeverD/actions/runs/36663627355):
  success
- [dev Python Plugin SDK](https://github.com/NeverSight/NeverD/actions/runs/36663627301):
  success
- [dev Mobile Decompilation](https://github.com/NeverSight/NeverD/actions/runs/36663627368):
  failure; its failure cause has not been compared with the PR run
- [dev CI](https://github.com/NeverSight/NeverD/actions/runs/36663627393):
  in progress
- [dev Mobile Real Applications](https://github.com/NeverSight/NeverD/actions/runs/36664902687):
  in progress

## Today's top priorities

### 1. Reconcile PR #220 with current dev and clear integration gates

**Status:** Blocked by failed checks on the observed PR head.

The three main CI platform jobs failed in the step named
"Verify Debug and Release target flags". The inspected Linux log identifies
the actual terminating failure as the Python ABI inventory missing
`neverd_devirtualize_source_v3` and
`neverd_devirtualize_machine_source_v3`; this is not evidence that Debug
compiler flags themselves are wrong. [Linux job](https://github.com/NeverSight/NeverD/actions/runs/36662051074/job/109718732885)

LLVM Style reports clang-format 22.1.2 violations. Its
[proposed-formatting artifact](https://github.com/NeverSight/NeverD/actions/runs/36662051100/artifacts/11074612923)
is available. A later dev commit already addressed several dev formatting
violations, and dev Python Plugin SDK is green, so compare against current dev
before duplicating fixes or attributing them to #220.

**Next action:** Determine which failures remain on an updated PR comparison;
address only the remaining ABI/formatting discrepancies.

**Acceptance:**
- Current PR head and corresponding workflow runs are recorded
- Python ABI inventory and LLVM Style pass on that exact head
- All three main CI platform jobs complete with explicit pass/skip evidence

### 2. Close the Objective-C cross-architecture acceptance gaps

**Status:** PR #220 Mobile Decompilation is failing.

The [macOS job](https://github.com/NeverSight/NeverD/actions/runs/36662051058/job/109718733004)
reports:
- Scalar fixture, x86_64-default: 20/22 methods recovered
- Calls fixture, x86_64-classic: 18/21 methods recovered
- Single-session qualification: 11/12
- Swift source acceptance passed in that job

**Next action:** Compare the failing fixture identities and diagnostics with
current dev, resolve remaining source/metadata gaps, and preserve strict
fail-closed behavior. Do not treat the larger WMF author-reported metric as
proof these independent acceptance gates pass.

**Dependencies:** Current-head integration evidence from priority 1 and access
to the supported macOS/x86_64 acceptance environment.

**Acceptance:**
- Requested scalar and calls fixture matrices meet their complete-recovery gates
- Single-session qualification completes all 12 cases
- Mobile workflow succeeds on the exact PR head, or a documented unsupported
  case is explicitly reviewed rather than counted as a pass
- Review readiness is assessed only after the draft's remaining scope is clear

### 3. Reconcile epics with delivered work and select the next bounded task

**Status:** Planning metadata needs evidence reconciliation.

[Apple analysis #101](https://github.com/NeverSight/NeverD/issues/101) and
[IR emulator #104](https://github.com/NeverSight/NeverD/issues/104) remain open,
while related implementation PRs have landed. Unchecked historical criteria
are not sufficient evidence that every feature is still absent.

**Next action:** Map each acceptance criterion to merged changes, current
documentation, and tests; distinguish completed, partial, and still-blocked
scope before changing issue status.

**Release candidate:** [#4 release pipeline](https://github.com/NeverSight/NeverD/issues/4)
depends on or complements [#1 CI](https://github.com/NeverSight/NeverD/issues/1)
and [#3 prebuilt LLVM](https://github.com/NeverSight/NeverD/issues/3), both closed
as completed. Closure alone does not establish current three-platform packaging
readiness; verify artifacts and build consumers before planning a dry run.

**Small-task candidate:** [#12's last Windows feedback](https://github.com/NeverSight/NeverD/issues/12#issuecomment-5295808194)
reports executable/library PDB filename collisions after an earlier Debug flag
fix. Verify whether that specific issue remains before implementing another fix.

**Acceptance:**
- Each selected epic criterion has a linked implementation/test or an explicit gap
- The next task has a bounded deliverable, dependency, and validation command
- No issue is marked complete solely because a related PR merged

## Recent delivered changes

These PRs were returned by the merged-PR query. Validation descriptions inside
them are author reports unless separately confirmed by linked workflow results.

- [#266](https://github.com/NeverSight/NeverD/pull/266): acknowledge cancellation
  of active KVM vCPU entries
- [#265](https://github.com/NeverSight/NeverD/pull/265): prevent sibling loops
  from starving reachable entry-prefix witnesses
- [#264](https://github.com/NeverSight/NeverD/pull/264): execute static PIE from
  original ELF bytes with explicit load bias
- [#263](https://github.com/NeverSight/NeverD/pull/263): expose interpreter
  recovery field and query budgets
- [#262](https://github.com/NeverSight/NeverD/pull/262): support compiler-generated
  static TLS across checked CPU backends

## Daily log

### 2026-09-30 — Initial baseline

- Recorded 17 open issues, one draft PR, and 48 merged PRs since September 29
  00:00 UTC; no issues closed in that window
- Identified three failed workflows on #220's observed head and recorded
  concrete ABI, formatting, and Objective-C acceptance evidence
- Distinguished later dev results: formatting and Python SDK are green;
  Mobile Decompilation is failing and other validation is still running
- Proposed three priorities with acceptance criteria; no code changes,
  workflow reruns, issue edits, or merge actions are part of this entry

## Tracking conventions and limits

- Refresh the snapshot and priorities, then append a dated daily-log entry;
  preserve prior entries and human edits
- Record exact commits and workflow URLs; pending, skipped, unavailable, and
  failed checks must remain distinct
- Compare changes against the previous recorded snapshot. Keep issue closure,
  merged implementation, and verified product acceptance separate
- PR workflow lookup covered the returned first page of PR-triggered runs.
  The separate dev Actions query returned nine runs for its exact SHA.
  This does not establish branch-protection requirements, every external check,
  or overall release readiness
- GitHub search and Actions may change after this timestamp. This document is a
  point-in-time record, not a claim of continuous monitoring or a committed
  delivery schedule

</details>
