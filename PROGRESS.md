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
