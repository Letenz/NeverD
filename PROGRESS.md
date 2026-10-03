# NeverD Daily Progress

Last verified: **2026-10-03 09:12 Asia/Shanghai (UTC+08:00)** / **2026-10-03 01:12 UTC**

This is a point-in-time daily issue/PR and static-review tracker. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md) and [contribution guidance](CONTRIBUTING.md) remain authoritative. Priorities below are proposed acceptance work, not assigned deadlines or an overall completion percentage.

## Current snapshot

Source review is pinned to the observed dev commit. Issue/PR counts precede this documentation proposal.

| Measure | Verified state |
| --- | --- |
| Open issues | 17; unchanged |
| Open pull requests | 0; unchanged before this proposal |
| PRs merged since 2026-10-02 01:11 UTC | 35: #328–362 |
| PRs closed without merge in that window | 0 |
| Ordinary issues updated/closed in the window | 0 / 0 |
| Observed dev commit | [99340b58](https://github.com/NeverSight/NeverD/commit/99340b58657e3907588a33b363435f79b74b86fe) |
| Previous observed dev commit | [d87f27d2](https://github.com/NeverSight/NeverD/commit/d87f27d29ecb097d1fa8483006797dc4765c3972) |
| Change inventory | 262 commits; 567 changed file/submodule paths, including 175 added and zero deleted |
| New statically confirmed defects | 0 in the bounded scope below |
| Proposed code fixes today | None; documentation-only update |

The [comparison](https://github.com/NeverSight/NeverD/compare/d87f27d29ecb097d1fa8483006797dc4765c3972...99340b58657e3907588a33b363435f79b74b86fe) was read across 100 + 100 + 62 commit records and an empty fourth page. GitHub's comparison limits its file list to 300 paths; the 567-path inventory instead compares the complete recursive Git trees (5,313 and 5,496 entries, neither truncated), excluding directories. Enumeration is not a claim that every changed path was audited.

### Changes since the previous snapshot

- Yesterday's [PR #328](https://github.com/NeverSight/NeverD/pull/328) merged at 2026-10-02 01:29:42 UTC as [3c1f637f](https://github.com/NeverSight/NeverD/commit/3c1f637f514a9e6902a3ccc02690565d08408efb). The canonical WDK path assertion is present on the inspected dev tree. Its merged PROGRESS.md exactly matches the previous tracker preserved below.
- [#329](https://github.com/NeverSight/NeverD/pull/329) expanded complete WDK corpus/SEH acceptance. Later x64 bit-string, string transfer and string comparison changes landed in [#336](https://github.com/NeverSight/NeverD/pull/336), [#338](https://github.com/NeverSight/NeverD/pull/338) and [#342](https://github.com/NeverSight/NeverD/pull/342).
- Windows process memory, module graphs, lifetimes and exports landed through [#346](https://github.com/NeverSight/NeverD/pull/346), [#350](https://github.com/NeverSight/NeverD/pull/350), [#352](https://github.com/NeverSight/NeverD/pull/352), [#355](https://github.com/NeverSight/NeverD/pull/355) and [#357](https://github.com/NeverSight/NeverD/pull/357). These changes enlarge the current Windows native requirement to 404 outcomes.
- Native macOS HVF and Darwin environments were integrated. The latest [99340b58](https://github.com/NeverSight/NeverD/commit/99340b58657e3907588a33b363435f79b74b86fe) writes and captures Intel RIP/RFLAGS directly through VMCS after cancellation recovery. Its actual Intel full gate is still running.
- Interpreter recovery domains, bounded source generation, exported bytecode recovery, Swift/Objective-C binding and additional lifting changes were inventoried, not comprehensively source-reviewed today.

## Static review

**Mode:** Read-only source, diffs, caller/test contracts, configuration and existing GitHub CI evidence. No repository program, build, test, script, linter or formatter was executed. No workflow was manually dispatched or rerun. No new defect was established strongly enough to justify an automatic code change.

### HVF cancellation, register transfer and callers

HVF source coverage and findings are recorded below. Source invariants are not runtime acceptance.

The source/test audit covers **38 distinct artifacts** in total, including the native-evidence and historical-failure sections below; four workflow files were additionally inspected.

**Full HVF/adjacent source and test reads:**
- `lib/emulation/backends/hvf/{HvfExecutor.cpp,HvfExecutor.h,HvfX64Machine.cpp,HvfX64Registers.def}`
- `lib/emulation/backends/RunDeadline.h` and `lib/emulation/core/MachineRunControl.h`
- `lib/emulation/arch/x86_64/{X64Machine.cpp,X64Machine.h,X64MachineProbe.cpp,X64MachineProbe.def,X64OperandRegisters.def,CheckedX64Instructions.def}`
- `lib/emulation/os/windows/driver/WindowsX64ExecutionPolicy.cpp` and `include/neverd/emulation/X64Registers.def`
- `unittests/emulation/{HvfExecutorTests.cpp,HvfTests.cpp,HvfTestPolicy.h,X64StateTransitionTests.cpp,MachineRunControlTests.cpp,RunControlTests.cpp,NativeEntryTests.cpp}`
- `scripts/NativeHVFTests.def`, also included in the evidence inventory review below

**Focused adjacent sections:** `CheckedX64Backend.cpp` register validation, checked admission, native step/error handling and RAM/CPU publication (principally lines 191–204 and 265–590); `unittests/emulation/CMakeLists.txt` registrations for `NeverDHvfTests`, `NeverDX64ExceptionTests` and `NeverDRunControlTests`. The native-evidence script was also read in full below.

- Intel RIP/RFLAGS prepare and capture use the VMCS boundary on each step; their register-API entries are removed. Capture writes a staged next packet, so a failed field read does not publish partially captured caller state.
- Unsolicited Intel IRQ exits retry within one deadline invocation, preserving native state and the cancellation generation. Native errors return immediately. The watchdog is disarmed and acknowledged before cancelled vCPU teardown/recreation.
- Every Intel vCPU creation, including recovery, binds managed `IA32_KERNEL_GS_BASE`, denies guest MSR access and initializes the private value. CR8 completion accepts only authenticated MOV-from-CR8 qualifications and validates privilege, GPR, instruction length and value before modifying state.
- The checked caller discards speculative RAM on machine failure. Ordinary cancellation/capture failure does not publish staged CPU state; authenticated exceptions retain their precedence.
- The cancellation regression covers deadlines, stop tokens, unsolicited interrupt followed by stop, a real native return, retry at another RIP and completion-error precedence. After recreation it asserts RIP/RFLAGS/AX, not the complete state packet.
- The separate CR8 regression covers all 16 destination GPRs, priorities 0–15, supervisor success, user #GP(0), RF and complete unchanged-state comparisons. It belongs to `NeverDX64ExceptionTests`, outside the 10-case Intel transport-only target. Startup full-state probing occurs at machine creation, not after every cancelled vCPU recreation.

**Remaining uncertainty:** Only matching-host execution can establish that Apple's framework preserves the intended native state after recreation. A green transport subset cannot replace the full CPU/Darwin gate or establish complete post-cancellation state coverage.

### Native-evidence enforcement and current inventories

Read in full: [run_native_cpu_ci.py](scripts/run_native_cpu_ci.py), [test_run_native_cpu_ci.py](scripts/tests/test_run_native_cpu_ci.py), [NativeCPUTests.def](scripts/NativeCPUTests.def), [NativeDriverTests.def](scripts/NativeDriverTests.def), [NativeHVFTests.def](scripts/NativeHVFTests.def), [NativeDarwinTests.def](scripts/NativeDarwinTests.def), [WhpMemoryCases.def](unittests/emulation/WhpMemoryCases.def), [DriverBuiltinImages.def](unittests/emulation/DriverBuiltinImages.def), [DriverBackendParityCases.def](unittests/emulation/DriverBackendParityCases.def), and [test_build_wdk_driver_fixtures.py](scripts/tests/test_build_wdk_driver_fixtures.py).

Also reviewed the shared `TestRecord` / `parse_inventory` boundary in [audit_ci_test_inventory.py](scripts/audit_ci_test_inventory.py), and JUnit label, status, identity and count parsing in [audit_ci_test_results.py](scripts/audit_ci_test_results.py).

- Required host-specific test names are selected by explicit ARM64/x64 identity; an unknown architecture is rejected.
- Native profiles reject missing registrations and non-passing required outcomes. JUnit infrastructure not-run, explicit skips, failures and disabled cases remain distinct. Outcome reconciliation retains test name and owner-label identity.
- The transport-only HVF profile is an explicit subset of the full inventory; the Darwin profile expands every declared workload across each matching guest platform.
- Static text inventory counts reconcile: 160 explicit CPU names + 16 WHP mapping cases = 176 CPU outcomes; 26 built-in images + 46 WDK images + 40 scenarios at two bases = 224 driver outcomes; four SEH cases bring the current Windows total to **404**.
- These are declaration counts, not newly executed results. Existing regression source covers missing owners, missing results, skipped mandatory hardware cases, wrong owner identity, malformed host selections and deleted Darwin workload requirements.

### Historical failure reconciliation

Read both complete current shared-event headers: [AndroidNative.h](include/neverd/emulation/AndroidNative.h) and [ProcessCall.h](include/neverd/emulation/ProcessCall.h), the recovery-surface expectations in [test_check_capabilities.py](scripts/tests/test_check_capabilities.py), and the corresponding corrective commit patches.

- The two older main-CI failures in the “Verify Debug and Release target flags” step actually failed `test_repository_manifest_is_honest_and_executable`, owing to missing recovery-v4 expectations. [c4010c33](https://github.com/NeverSight/NeverD/commit/c4010c33e96851fc2ff170a0c06d6483ab9fad51) supplies those expectations; the inspected file retains them and later APIs. This was not evidence of incorrect Debug flags.
- The third older main-CI failure was a duplicate `NativeCallEvent` definition while compiling Linux services. [c3493d72](https://github.com/NeverSight/NeverD/commit/c3493d723135b0e4e7bfdbf3a810bfd562bb86c2) removes the Android duplicate and retains Library/Symbol in the shared header. The inspected source contains that correction.
- These already-delivered fixes were not duplicated. No claim is made that current full integration has passed.

Relevant architecture/testing/HVF guidance, the complete HVF workflow, and CI/mobile/style trigger and concurrency sections were also inspected. The rest of the 567 changed paths, broader Windows loader/export semantics, Darwin syscall semantics, Objective-C/Swift source recovery and interpreter/refinement work remain outside this bounded audit.

## Existing CI evidence

**Exact-head snapshot: 2026-10-03 01:12 UTC**, for `99340b58657e3907588a33b363435f79b74b86fe`.

| Workflow | Observed state | Evidence |
| --- | --- | --- |
| CI | In progress; all three platform jobs at their configuration/script-verification step; optional WHP job skipped | [37084475387](https://github.com/NeverSight/NeverD/actions/runs/37084475387) |
| HVF hosted-intel / full | In progress at the required transport step; full CPU and Darwin stages not reached | [37084520059](https://github.com/NeverSight/NeverD/actions/runs/37084520059) |
| Mobile Decompilation | Workflow API reports queued; Ubuntu job succeeded, macOS in progress, Windows queued | [37084475394](https://github.com/NeverSight/NeverD/actions/runs/37084475394) |
| Mobile Real Applications | Skipped | [37084500511](https://github.com/NeverSight/NeverD/actions/runs/37084500511) |
| LLVM Style | Success | [37084475343](https://github.com/NeverSight/NeverD/actions/runs/37084475343) |

Exact-head check runs: **18 total: 2 successful, 10 skipped, 5 in progress and 1 queued; zero failed at this snapshot**. Legacy statuses are empty; their combined `pending` state does not establish failure or success. The existing manually initiated Intel workflow was observed only; this review did not initiate it.

The dev Actions collection created from **2026-10-02 01:11 UTC through 2026-10-03 01:05 UTC** contains **597 runs**, across six non-empty pages (100 + 100 + 100 + 100 + 100 + 97) and an empty seventh page. Its **113 main CI runs comprise 109 cancelled, three failed and one in progress**, with no completed green main CI run in that query. The three failures above were diagnosed from their Linux logs and corresponding source corrections. Yesterday's tracked main CI and Mobile Real Applications later cancelled; yesterday's mobile-fixture success stays scoped to yesterday's commit.

### Native progress since yesterday

**Windows WDK/CPU:** The [native WHP job](https://github.com/NeverSight/NeverD/actions/runs/36981864458/job/110758081823) at `9d4c130c2f11d95a2f80f1055dfdbb34de07715c` reports **1,121 passed, 1,667 skipped, zero failed/disabled/not-run**, no missing/unexpected identities, and **all 359 required outcomes executed**. This confirms substantial progress beyond yesterday's 197-outcome blocked gate. It does not validate the newer 404-outcome inventory or today's head.

**Darwin on x64 KVM/WHP:** The [existing run](https://github.com/NeverSight/NeverD/actions/runs/37062839703) at `36e11ca8a3d80aecf585d3328018839ce7fdb989` succeeded on both hosts. Both inspected job logs record **51 passed, 235 skipped, zero failed**, with **all 26 required Darwin workloads executed**. This is Darwin guest-contract evidence on Linux/Windows native transports, not Intel macOS HVF acceptance. The separate [native macOS kernel reference](https://github.com/NeverSight/NeverD/actions/runs/37064795867) succeeded on both x86_64 and arm64 at `e727d3eab7086063bb392444bd55014ac48d43c3`; only its job/step metadata was checked here.

**Intel HVF:** The older [transport job](https://github.com/NeverSight/NeverD/actions/runs/37083061831/job/111087568902) at `48042a5e90e0977585114de092e423cd64b7f95f` had **9 passed / 1 failed / no skips**. The exact failure was the first `Prepare(RetryPC)` after cancellation in `NativeIntelCancellationAndCompletionFailureAllowRetry`, reporting an unexpected VM exit. Full CPU and Darwin stages were skipped. The new VMCS correction is on today's head; its existing full workflow remains incomplete.

**Apple Silicon HVF:** [macos-hvf.md](docs/macos-hvf.md) reports a clean-source full gate at `48042a5e` with 841 passed, 5,942 inapplicable skips and all 16 required outcomes, plus the 12-case transport subset. Those local results are maintainer-reported documentation, not logs independently accessed in this review. They must not be described as absent native ARM64 evidence, but also must not be transferred to Intel HVF or ARM64 KVM/WHP.

## Today's top priorities

### 1. Establish complete Intel HVF acceptance after recovery correction

**Status:** The previous gate failed after cancellation; the exact-head full workflow is still at transport validation.

**Next action:** Review its eventual transport, full CPU and Darwin outcomes against the same source identity. Include the all-GPR CR8/CPL3 regression and distinguish the retry test's limited RIP/RFLAGS/AX assertions from complete state coverage.

**Acceptance:** The identified commit passes every required transport case, the complete full-profile CPU gate and all matching Darwin workloads, with no missing/skipped required tests. Probe-only or transport-only success is insufficient. No manual execution is part of this static review.

### 2. Obtain uninterrupted three-platform integration and real-application evidence

**Status:** Current main CI is incomplete; the bounded dev window has 109 cancellations and no green main CI. Current mobile fixtures are incomplete and real-application qualification is skipped.

**Next action:** Reconcile existing terminal outcomes after the source fixes already present. Keep any superseding commit separate. Record real-application producer/consumer identity and actual execution instead of carrying forward a historical green fixture result.

**Acceptance:** Linux, macOS and Windows complete the intended integration profile for one identified commit, with audited test execution. Real-application qualification supplies actual evidence; a skipped consumer does not satisfy acceptance.

### 3. Reconcile expanded native gates and open issue criteria

**Status:** Historical Windows evidence passes 359 required outcomes; the current inventory requires 404. Seventeen issues remain open, including 14 epics; none has a milestone, and only [#12](https://github.com/NeverSight/NeverD/issues/12) is assigned.

**Next action:** Evaluate authorized existing/native evidence for the expanded Windows process/module/export requirements. Map delivered work to [#104](https://github.com/NeverSight/NeverD/issues/104), [#101](https://github.com/NeverSight/NeverD/issues/101) and [#4](https://github.com/NeverSight/NeverD/issues/4), separating implementation, verified platform scope and remaining gaps. The historical #12 report was not re-audited today.

**Acceptance:** Each selected criterion has an implementation/evidence link or an explicit gap; current Windows acceptance executes all 404 mandatory outcomes at an identified commit. Matching-host results remain distinct, and no issue is closed solely because related PRs merged.

## Daily log

### 2026-10-03 — HVF static audit and native-evidence reconciliation

- Inventoried 262 commits and 567 changed file/submodule paths since the previous source snapshot; reviewed the bounded HVF, native-evidence and historical-failure scope above.
- Found no new statically proven defect requiring a code change. Yesterday's WDK correction is merged.
- Reconciled 35 merged PRs, no unmerged closures, 17 open issues and no open PR before this proposal.
- Verified historical Windows 359-outcome success and x64 KVM/WHP Darwin 26-workload success, retaining the current 404-outcome and Intel HVF acceptance gaps.
- Diagnosed three older main-CI failures and verified their source corrections already exist. Current integration remains incomplete.
- Preserved the complete October 2 tracker, including October 1 and September 30 history, below.
- This English documentation-only proposal uses a topic branch and draft PR. No build, test, repository script, manual workflow trigger, merge, deployment, dependency revision or security-setting change was performed. The commit uses `[skip ci]`; independently managed automatic checks may still occur.

## Tracking conventions and limits

- Open issue/PR collection returned 17 ordinary issues and no PRs, followed by an empty second page; a separate open-PR collection was empty. Updated issue/PR records returned 35 PRs and an empty second page, with no ordinary issue.
- The first 100 most recently updated PRs extend past the tracking boundary and include all 35 in-window records. Older complete PR history was not enumerated.
- Exact-head workflow/check collections returned five/18 records and empty second pages. Main CI and current HVF job collections returned four/one records with empty second pages. Selected historical job collections were small; not every historic log was inspected.
- The separate dev manual-event collection returned 34 records and an empty second page. The historical WHP success was followed directly from repository evidence and is separate from the dev-bound Actions count.
- PRs #328, #329, #357 and #362 returned no submitted reviews, inline review threads or conversation comments. This is not independent approval.
- GitHub reports dev unprotected and an empty repository ruleset collection. The topic-branch/PR contribution workflow is still followed; no protection was changed.
- PROGRESS.md was compared with yesterday's merged version and re-read before writing; preserve this full history and future human edits.
- Pending, skipped, cancelled, failed, maintainer-reported and independently inspected results remain distinct. Static review does not establish compilation, runtime behavior, race freedom, complete ISA coverage or release readiness.

## Publication-time observation

At 2026-10-03 01:16 UTC, dev had advanced to [eee92640](https://github.com/NeverSight/NeverD/commit/eee926404c3a7ee91d46e9e84fa04d47f9543107), which separates and time-bounds Intel transport compilation/execution in the HVF workflow. Its one-file patch was read, and PROGRESS.md was unchanged. This later commit is outside the pinned 262-commit/567-path inventory and source snapshot above. The existing Intel run for `99340b58` was still in progress; no terminal acceptance is inferred.

## Previous snapshots (preserved)

<details>
<summary>2026-10-02 tracker, priorities, evidence and earlier history</summary>

# NeverD Daily Progress

Last verified: **2026-10-02 09:11 Asia/Shanghai (UTC+08:00)** / **2026-10-02 01:11 UTC**

This is a point-in-time daily issue/PR and static-review tracker. The [roadmap](docs/roadmap.md), [architecture](docs/architecture.md), [testing guide](docs/testing.md), and [contribution guidance](CONTRIBUTING.md) remain authoritative. Priorities are proposals, not assigned deadlines or a completion percentage.

## Current snapshot

The issue/PR snapshot precedes today's review PR. Source inspection is pinned to the observed dev commit, not to a moving branch.

| Measure | Verified state |
| --- | --- |
| Open issues | 17; unchanged |
| Open pull requests | 0; unchanged before this review PR |
| PRs merged since 2026-10-01 01:02 UTC | 41: #285–319 and #321, #323–327 |
| PRs closed without merge in that window | 2: #320 and #322 |
| Ordinary issues updated/closed in the window | 0 / 0 |
| Observed dev commit | [d87f27d2](https://github.com/NeverSight/NeverD/commit/d87f27d29ecb097d1fa8483006797dc4765c3972) |
| Previous observed dev commit | [6eb2e5c6](https://github.com/NeverSight/NeverD/commit/6eb2e5c6423f7b2977568fe070c7cd334f503572) |
| Change inventory | 266 commits; 521 changed file/submodule paths |
| Bounded source review | 31 source, test and build artifacts, with sections listed below |
| New confirmed defects | 1 test-portability defect; one-line correction committed |
| New production-code defects established | 0 in the sampled scope |

The [comparison](https://github.com/NeverSight/NeverD/compare/6eb2e5c6423f7b2977568fe070c7cd334f503572...d87f27d29ecb097d1fa8483006797dc4765c3972) was read across three commit pages (100 + 100 + 66), followed by an empty page. GitHub limits the comparison's changed-file list to 300 paths; the 521-path inventory therefore comes from comparing complete recursive Git trees (5,154 and 5,313 entries, neither truncated). Enumeration is not a claim that every changed path was code-reviewed.

### Changes since the previous snapshot

- Yesterday's [tracking PR #285](https://github.com/NeverSight/NeverD/pull/285) merged at 2026-10-01 03:53:57 UTC. Its PROGRESS.md content is identical to the version on the inspected dev tree; the complete previous tracker is preserved below.
- [#319](https://github.com/NeverSight/NeverD/pull/319) centralized width-aware absent-SIB-index handling across scalar address construction, metadata auditing and EVEX validation.
- [#308](https://github.com/NeverSight/NeverD/pull/308), [#311](https://github.com/NeverSight/NeverD/pull/311), [#313](https://github.com/NeverSight/NeverD/pull/313) and [#316](https://github.com/NeverSight/NeverD/pull/316) refined native x64 state ownership, XSAVE handling and physical x87 transition evidence.
- [#317](https://github.com/NeverSight/NeverD/pull/317) expanded the native driver gate from the older 97 required CPU/driver outcomes to 197, including the reproducible WDK corpus. Its PR description's queued run has since failed before native execution; today's correction addresses the observed test assertion.
- Other delivered work includes Android ARM64 native environments, external bytecode profiles, preferred-base PE evidence, loop refinement, bounded frame transfers and Objective-C/Swift recovery. These were inventoried, not comprehensively audited.

## Static review

**Mode:** Source, diff, caller, configuration and existing CI-log inspection only. No repository program, build, test, linter, formatter or script was executed. No workflow was manually dispatched or rerun.

### Confirmed defect and correction

**Test-portability defect:** [test_build_wdk_driver_fixtures.py](scripts/tests/test_build_wdk_driver_fixtures.py), `test_cmake_paths_preserve_spaces_and_reject_list_or_code_expansion`, compared the original path spelling with a cache entry that deliberately uses `Path.resolve()`.

The historical [Windows WHP job](https://github.com/NeverSight/NeverD/actions/runs/36899322120/job/110494224787), at commit `9944433cc4593a59fe899f90c4a532306a4f9ed6`, failed this exact assertion: the temporary path used the short Windows user-directory spelling while the emitted path used its resolved long spelling. The script suite reported 25 tests with one failure. The subsequent native build/verification step was skipped. This is not evidence of an XSAVE or WHP execution failure.

The same raw-path assertion was still present at today's pinned dev commit. [Fix 9b5b23ef](https://github.com/NeverSight/NeverD/commit/9b5b23efa35c577cdb22f6cd18eef7d73ac19e76) changes only the expected path to `image.resolve().as_posix()`. It retains the quoted-space assertion and the separate invalid-character rejection checks. Production canonicalization and validation are unchanged.

**Verification:** Independently checked the producer/test contract and historical log, then remotely read back the fixed file and commit diff. The commit changes one line in one test file. The fix has not been executed or validated by a new Windows run; it is proposed on today's topic branch and is not merged.

### Source coverage

The following 31 artifacts were inspected at `d87f27d2`. Whole-file reads are distinguished from selected sections.

**x86 SIB address semantics: 10 artifacts**

- [X86LiftDetail.h](lib/lift/X86/X86LiftDetail.h): shared `isNoSibIndex` declaration and relevant PR patch
- [X86Lifter.cpp](lib/lift/X86/X86Lifter.cpp): `isNoSibIndex`, `computeEA`, memory read/store callers, undefined-output memory audit, unmapped-register rejection and final sidecar publication
- [X86LiftSIMDMemory.cpp](lib/lift/X86/X86LiftSIMDMemory.cpp): ordinary memory validation, raw SIB/tail checks, EVEX/VEX3 adapters and masked memory-load construction
- [X86LiftSIMDMove.cpp](lib/lift/X86/X86LiftSIMDMove.cpp): masked memory-operand validation and full-vector move caller, including address construction and mask handling
- [X86Regs.cpp](lib/lift/X86/X86Regs.cpp): general-register mapping and invalid-register fallback
- [Decoder.cpp](lib/decode/Decoder.cpp): `liftToLow` dispatch and undefined-effects initialization
- [X86Lifter.h](include/neverd/lift/X86Lifter.h): shared memory-intrinsic/address helpers
- [X86_64_NoIndexAddressTests.cpp](unittests/lift/x86_64/X86_64_NoIndexAddressTests.cpp): all nine regression cases
- [X86_64_EVEXMemoryBroadcastTests.cpp](unittests/lift/x86_64/X86_64_EVEXMemoryBroadcastTests.cpp): absent-index masked-broadcast/move and contradictory-metadata cases, plus their helpers
- [unittests/lift/CMakeLists.txt](unittests/lift/CMakeLists.txt): dedicated no-index test-target registration

**XSAVE/WHP state transfer: 19 artifacts, read in full**

- [X64FPState.cpp](lib/emulation/arch/x86_64/X64FPState.cpp), [.h](lib/emulation/arch/x86_64/X64FPState.h), and [.def](lib/emulation/arch/x86_64/X64FPState.def)
- [X64Machine.h](lib/emulation/arch/x86_64/X64Machine.h), [X64MachineProbe.cpp](lib/emulation/arch/x86_64/X64MachineProbe.cpp), and [X64MachineProbe.def](lib/emulation/arch/x86_64/X64MachineProbe.def)
- [WhpXsaveState.h](lib/emulation/backends/whp/WhpXsaveState.h), [WhpXsaveRegisters.def](lib/emulation/backends/whp/WhpXsaveRegisters.def), [WhpProtocol.def](lib/emulation/backends/whp/WhpProtocol.def), and [WhpMachine.cpp](lib/emulation/backends/whp/WhpMachine.cpp)
- [X64XsaveTests.cpp](unittests/emulation/X64XsaveTests.cpp), [X64XsaveCases.def](unittests/emulation/X64XsaveCases.def), [WhpXsaveTests.cpp](unittests/emulation/WhpXsaveTests.cpp), and [WhpHostFailureCases.def](unittests/emulation/WhpHostFailureCases.def)
- [X64FPStateTests.cpp](unittests/emulation/X64FPStateTests.cpp), [X64FPCases.def](unittests/emulation/X64FPCases.def), and [X64MachineProbeTests.cpp](unittests/emulation/X64MachineProbeTests.cpp)
- [X64StateTransitionTests.cpp](unittests/emulation/X64StateTransitionTests.cpp) and [X64StateTransitionCases.def](unittests/emulation/X64StateTransitionCases.def)

**WDK assertion and producer: 2 artifacts, read in full**

- [build_wdk_driver_fixtures.py](scripts/build_wdk_driver_fixtures.py): especially `cache_entry`, its build caller and publication
- [test_build_wdk_driver_fixtures.py](scripts/tests/test_build_wdk_driver_fixtures.py): especially path spelling, quoted spaces and pre-resolution rejection tests

Repository guidance, relevant architecture/testing sections, roadmap hardening scope, CI/mobile trigger and concurrency definitions, and the native CI configuration step were also inspected.

### Findings in the production-code sample

No new production-code correctness defect was established strongly enough for an automatic fix.

- Absent SIB indices are width-specific; they do not contribute a scaled register term. Effective addresses retain 32-bit wrapping/zero-extension and separate FS/GS offsets from ordinary address provenance. Real R12 indices remain ordinary mapped registers. Raw EVEX tail validation checks the encoded index extension before accepting absent-index aliases.
- Invalid pseudo-register bases or wrong-width pseudo-indices become unmapped register operands and are rejected transactionally in strict lifting. Existing tests cover address widths, all redundant scale encodings, loads/stores, relocation ownership, undefined sidecars, masked accesses and malformed metadata. These are descriptions of test source, not new test results.
- Standard initial-SSE XSAVE packets retain and validate MXCSR; compacted initial-SSE packets reset it. Both clear XMM lanes. The codec stages the next state before publication and rejects unsupported layout bits, inconsistent lengths and truncated extension storage.
- WHP capture stages XSAVE decode, named metadata reads, consistency checks and supplemented-state validation before publishing. The machine caller stages complete state around the transfer. x87 transition/cancellation fixtures retain exact physical-state assertions.

**Limits:** The remaining changed paths, Objective-C/Swift pipeline, Android environment, general loop/refinement work, resource-cache concurrency, ARM64 transport and PE refactor were not source-audited in this pass. Static inspection does not establish runtime behavior, compilation/linking, formatting, race freedom, release readiness or complete ISA coverage.

## Existing CI evidence

**Exact-head CI snapshot: 2026-10-02 01:10 UTC**, for `d87f27d29ecb097d1fa8483006797dc4765c3972`.

| Workflow | Observed state | Evidence |
| --- | --- | --- |
| CI | In progress; Linux, macOS and Windows building; optional native WHP job skipped | [36947225176](https://github.com/NeverSight/NeverD/actions/runs/36947225176) |
| Mobile Decompilation | Success on Ubuntu, macOS and Windows | [36947225342](https://github.com/NeverSight/NeverD/actions/runs/36947225342) |
| Mobile Real Applications | Pending | [36949181960](https://github.com/NeverSight/NeverD/actions/runs/36949181960) |
| Push on dev | In progress | [36947225134](https://github.com/NeverSight/NeverD/actions/runs/36947225134) |
| LLVM Style | Success | [36947225307](https://github.com/NeverSight/NeverD/actions/runs/36947225307) |
| Code Quality: Push on dev | Success | [36947225199](https://github.com/NeverSight/NeverD/actions/runs/36947225199) |
| Prebuilt LLVM Audit | Success | [36947225314](https://github.com/NeverSight/NeverD/actions/runs/36947225314) |
| EVM Upstream Audit | Success | [36947225350](https://github.com/NeverSight/NeverD/actions/runs/36947225350) |

Exact-head check runs: **16 total: 11 successful, 1 skipped, 4 in progress, 0 failed**. Legacy commit statuses are empty; their combined `pending` state alone is not a failure or an all-checks pass.

The dev Actions collection created from **2026-10-01 01:02 UTC through 2026-10-02 01:03 UTC** contained **929 runs**, across ten pages (nine of 100, one of 29). Its **115 main CI runs comprise 114 cancelled and one in progress**; no completed green dev main CI run was observed in that window. This is an integration-evidence gap, not proof of a code failure. Yesterday's tracked [main CI](https://github.com/NeverSight/NeverD/actions/runs/36795886427) and [mobile run](https://github.com/NeverSight/NeverD/actions/runs/36795886438) both subsequently cancelled.

### Native evidence must retain its scope

The older [WHP run 36894495012](https://github.com/NeverSight/NeverD/actions/runs/36894495012), at `e7f205ab4ecb5a91646e8ea9ee8dd6b74ecb230a`, succeeded. Its [job log](https://github.com/NeverSight/NeverD/actions/runs/36894495012/job/110477966198) records **803 passed, 1,522 skipped, zero failed/missing/not-run**, and all **97 required native CPU/driver outcomes executed**. This is real earlier Windows evidence, not current-head or expanded-corpus acceptance.

The later [197-outcome WDK/native run](https://github.com/NeverSight/NeverD/actions/runs/36899322120) failed the path assertion before native verification. Today's one-line fix addresses that blocker but does not prove the expanded run will pass. The optional native job is skipped on today's ordinary dev push. Native ARM64 runtime evidence remains explicitly unavailable in the reviewed documentation.

## Today's top priorities

### 1. Obtain uninterrupted exact-head integration evidence

**Status:** Mobile fixtures now pass on the inspected head; the three-platform main CI is still building. The preceding 114 dev main CI runs in the collection were cancelled.

**Next action:** Inspect the existing main CI's terminal platform/test outcomes, retaining exact commit identities and all skip/missing-test distinctions. If development supersedes the run, record the new integration gap rather than transferring a previous success.

**Acceptance:** All three platform jobs finish for one identified integration commit, with required test execution audited. No manual dispatch/rerun is included in this static-only review.

### 2. Validate the expanded native WDK gate after the path-test correction

**Status:** The older 97-outcome native gate is green; the newer 197-outcome gate stopped before CPU verification. The canonical-path assertion correction is committed on today's review branch, unexecuted.

**Next action:** Review the one-line test fix and later evaluate an authorized Windows native CPU/driver run for a commit containing it. Preserve the original canonicalization and invalid-path protections.

**Acceptance:** Configuration passes, all 197 required native outcomes execute, and failed, missing or skipped required cases remain failures. The observed older 97-outcome run and portable XSAVE source coverage do not satisfy this larger gate.

### 3. Finish mobile real-application qualification and reconcile issue criteria

**Status:** Exact-head Mobile Decompilation is green; Mobile Real Applications is pending. The 17 open issues retain unchanged planning metadata: 14 epics, no milestones, and only [#12](https://github.com/NeverSight/NeverD/issues/12) assigned.

**Next action:** Record the real-application producer/consumer identities and actual result. Map merged implementation and current evidence to [#101](https://github.com/NeverSight/NeverD/issues/101), [#104](https://github.com/NeverSight/NeverD/issues/104) and [#4](https://github.com/NeverSight/NeverD/issues/4), distinguishing delivered code from outstanding acceptance. The historical Windows report in #12 was not re-audited today.

**Acceptance:** Selected criteria have an implementation/evidence link or an explicit gap and a bounded next owner/action. A merged PR, stale unchecked issue or skipped consumer alone does not establish completion.

## Daily log

### 2026-10-02 — Static review, Windows path assertion fix and evidence refresh

- Inventoried 266 commits and 521 changed paths; reviewed the bounded 31-artifact scope above.
- Corrected one statically confirmed test-portability defect in [9b5b23ef](https://github.com/NeverSight/NeverD/commit/9b5b23efa35c577cdb22f6cd18eef7d73ac19e76). No production-code change was justified.
- Recorded 41 PR merges, two unmerged closures, 17 open issues and no open PR before today's publication.
- Confirmed current-head three-platform mobile fixture success and the still-incomplete main CI/real-application result.
- Distinguished earlier 97-outcome WHP success from the later expanded 197-outcome configuration failure.
- Preserved the complete October 1 tracker, including the September 30 history, below. Yesterday's #285 is merged; today's focused topic-branch update requires its own draft PR.
- No repository code, builds, tests, scripts, linters or formatters ran; no manual CI trigger, merge, deployment, dependency revision or security-setting change was performed. Commits use English messages with `[skip ci]`; automatically triggered GitHub checks can still occur independently.

## Tracking conventions and limits

- Source, issue and CI states are point-in-time observations, not continuous monitoring. New review PRs are excluded from pre-publication counts.
- Open issues returned 17 records and an empty second page; the separate open-PR query was empty. Updated issue/PR records returned 43 PRs and an empty second page, with no ordinary issues.
- The recent PR collection's first 100 updated records spans past the tracking boundary. All 43 in-window PRs are present; the older complete PR history was not enumerated.
- Exact-head workflows returned eight records and an empty second page; check runs returned 16 and an empty second page. Main/mobile job collections returned four/three records, each followed by an empty page.
- The separate manual-event collection through 01:10 UTC returned 29 existing runs and an empty second page. This review read it only; no run was created. Only the two native jobs discussed above were examined in log detail.
- Selected PRs #285, #308, #311, #313, #316 and #319 returned no submitted reviews, inline review threads or conversation comments. This is not an independent approval.
- GitHub reports dev unprotected and an empty repository ruleset collection. No protection/security setting was changed. The contribution guide's topic-branch/PR workflow was followed.
- PROGRESS.md was checked against yesterday's merged content and re-read before replacement. Preserve history and any human edits on later refreshes.
- Remote file/diff verification proves publication of the proposed correction, not passing validation or permission to merge.

## Previous snapshots (preserved)

<details>
<summary>2026-10-01 tracker, priorities, evidence and earlier history</summary>

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

</details>

</details>
