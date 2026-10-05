**Languages**: [English](macos-hvf.md) | [简体中文](zh-CN/macos-hvf.md) | [繁體中文](zh-TW/macos-hvf.md) | [日本語](ja/macos-hvf.md) | [한국어](ko/macos-hvf.md) | [Français](fr/macos-hvf.md) | [Deutsch](de/macos-hvf.md) | [Español](es/macos-hvf.md) | [Italiano](it/macos-hvf.md) | [Русский](ru/macos-hvf.md) | [العربية](ar/macos-hvf.md)

[← Documentation index](README.md)

# macOS native CPU execution (HVF)

**Validation status (2026-10-05): Intel HVF has not been tested on a physical Intel Mac and remains unvalidated.** Neither the organization nor the personal Actions repository established complete Intel acceptance; crashes, lost runners and timeouts remain unresolved. Intel HVF Actions are suspended in both repositories, and the general HVF workflow now selects ARM64 only. Intel workflow instructions below are historical references, not a request to resume testing. Development prioritizes correctness and measured performance on native ARM64; applicable changes may then be ported to Intel with source review and available compile checks only. ARM64 results do not validate Intel runtime behavior. Existing Intel evidence is preserved; this pause does not claim the failures are fixed.

NeverD uses Apple's [Hypervisor.framework](https://developer.apple.com/documentation/hypervisor)
as the macOS counterpart to KVM and WHP. `--backend hvf` selects it explicitly;
`auto` selects HVF for a matching native host/guest ISA and native-capable
contract. The unrestricted `software-cpu-v1` profile continues to use Unicorn. An Apple Silicon build
executes ARM64, and an Intel build executes x86-64. Cross-ISA `auto` selection
continues to use Unicorn. HVF rejects a translated executable under Rosetta;
run the native arm64 NeverD build on Apple Silicon.

The transport requires macOS 11 or later and hardware virtualization. This API
baseline does not lower the minimum OS required by other build dependencies.
A selected native backend reports unavailable hardware or entitlement failures
without silently falling back. [Virtualization.framework](https://developer.apple.com/documentation/virtualization)
is a higher-level whole-VM API; NeverD needs the vCPU, register, mapping and
exception controls supplied by Hypervisor.framework.

## Build and signing

Enable `NEVERD_ENABLE_CPU_EMULATION=ON` (or driver emulation).
`NEVERD_EMULATION_BACKEND_HVF` defaults to `ON` and only links the framework on
macOS. `OFF` retains the `hvf` vocabulary and reports `build_disabled` through
the capability API.

Framework linking and hypervisor signing are restricted to the macOS build target (`CMAKE_SYSTEM_NAME=Darwin`). Apple mobile targets, including iOS, do not acquire this framework dependency or entitlement. An iOS guest profile can still use HVF when NeverD itself runs on a Mac with the matching ISA.

The **process executable** needs `com.apple.security.hypervisor`. Signing only
`libneverd.dylib` is insufficient. CMake signs the CLI, worker and emulation test
executables using `resources/macos/neverd-hypervisor.entitlements` after linking.
`NEVERD_HVF_SIGN_IDENTITY` defaults to ad-hoc (`-`) and can select an existing
signing identity. Packaging reapplies the entitlement to the worker after
Mach-O dependency repair, preserves it during recursive signing, and verifies
it in the final bundle. See Apple's [entitlement documentation](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.hypervisor).

An embedding application is responsible for its own executable's entitlement.
NeverD does not re-sign the caller's Python interpreter or another installed
application. Capability queries distinguish the build from live initialization;
use `cpu-capabilities --configuration=JSON --probe-host` to check the actual process.

Standalone worker/desktop builds also sign the worker by default because an
imported engine does not expose its build flags to CMake. Set
`NEVERD_WORKER_SIGN_HVF=OFF` only when that executable does not need HVF.

## Ownership and execution

`backends/hvf/HvfExecutor` owns one process VM and one native vCPU, created,
used and destroyed on a dedicated thread. Logical NeverD CPUs share that
executor and serialize entry. Switching CPUs unmaps the old physical backing
before mapping the new owner's two regions. An inactive CPU's destruction
cannot retire another CPU's mappings. Bindings detach synchronously before
releasing their projection and physical RAM. Failed partial registration is
rolled back; an unmap failure retires the VM before backing can be released.
An unrecoverable native teardown failure stops the process instead of retaining
dangling guest mappings.

Host mappings use the host page size (including 16 KiB on Apple Silicon).
NeverD's architectural page tables and guest budget remain 4 KiB. The ISA layer
owns instruction admission, page permissions, CPU state, memory transactions,
service traps and guest OS behavior. Native worker actions cannot call guest
observers or acquire the caller's core memory locks.

ARM64 executes the complete immutable TLB/I-cache maintenance sequence in one
native entry, with software stepping disabled. A distinct HVC #1 must match
its exact return PC, syndrome, PSTATE and unchanged ESR_EL1 before the
admitted guest instruction is single-stepped. `PSTATE.D` cannot mask debug
exceptions routed to EL2. All scalar, TLS and FP/SIMD state uses the
existing ARM64 capture boundary and startup probe. Intel uses negotiated VMCS
controls, monitor-trap stepping, TLB invalidation, complete FP/SSE XSAVE packets
and authenticated exception exits. RIP/RFLAGS are installed and captured
directly through VMCS, including after vCPU recreation. CR0/CR4 honor both framework writable masks
and hardware fixed bits; host-required bits are hidden by the guest read
shadows. CR8 uses explicit VMX access exits and an architecture-owned read
completion: the host TPR API can disagree with the guest's actual value.
Only authenticated MOV-from-CR8 exits are completed; other control-register
accesses fail. The checked instruction inventory is unchanged. Each Intel
vCPU creation, including cancellation recovery, binds and
initializes a private managed `IA32_KERNEL_GS_BASE`. Guest MSR access stays
trapped, and unsupported MSR/SWAPGS instructions remain outside the checked ISA. Both architectures run the existing complete
state startup probe before exposing a CPU.

Queue admission observes the borrowed stop token and original deadline. One
native-step allowance covers preparation, maintenance, entry and capture.
On ARM64, `RunDeadline` acknowledges outstanding interrupts before returning.
Intel polls cancellation on its owner thread through finite
`hv_vcpu_run_until` calls. Cancelled
native entry recreates the vCPU so a late kick cannot affect the next task.
Unsolicited Intel host-interrupt exits retry the same native state under the
same cancellation generation, without reporting an instruction completion.
Host/capture failures and authenticated x64 exceptions retain priority over a
concurrent stop; ordinary cancelled state is not published. This is cooperative
cancellation, not a hard real-time deadline.

## Validation

On a native Mac, configure a Release build and run the dedicated evidence gate:

The host needs CMake, Ninja, Python 3, Clang, `ld.lld`, `lld-link`, `ld64.lld`
and `codesign`.
The three LLVM linkers build the authored ELF, PE and Mach-O fixtures; Xcode's linker
does not replace them. Put these tools on `PATH` for the manual CI workflow.
Missing native fixtures fail required HVF runs instead of silently skipping.

```sh
cmake -S . -B build-hvf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DNEVERD_BUILD_SHARED=OFF -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF \
  -DNEVERD_ENABLE_CPU_EMULATION=ON \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=OFF \
  -DNEVERD_EMULATION_BACKEND_UNICORN=OFF
python3 scripts/run_native_cpu_ci.py --build build-hvf \
  --evidence build-hvf/native-evidence --require-hvf
```

Apple Silicon can additionally use `-DNEVERD_LLVM_PREBUILT=ON`; Intel uses the
repository's pinned LLVM source build. The gate records the CTest inventory,
every test result, skipped foreign transports, source revision and a summary.
Required HVF tests must execute successfully; an all-skipped run cannot pass.
The required inventory includes three native Darwin startup/service fixtures on
ARM64 (macOS, iOS device and Simulator), or two on Intel (macOS and Simulator).
Missing registrations and skips fail the gate. Darwin CTest names retain their
exact GoogleTest identities instead of address-bearing parameter dumps.
[The ARM64 HVF workflow](../.github/workflows/hvf.yml) runs only on dedicated
`self-hosted, macOS, ARM64, hvf` runners. It rejects any non-native-ARM64 host,
then compiles and signs `scripts/probe_hvf_host.c` and requires real VM/vCPU
creation and teardown before preparing LLVM. The availability probe does not
execute guest code. `full` requires transport, complete CPU and Darwin gates;
`transport`, `darwin` and `probe` retain their narrower meanings below. No
Intel runner or image selection remains. Dedicated runners are not assumed
to be provisioned. Intel-specific workflow procedures below are historical.

GitHub describes nested virtualization on hosted runners as experimental and
does not guarantee its stability, performance or compatibility. See its
[hosted-runner policy](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners).
Keep a dedicated native Mac path for repeatable acceptance. This platform
limitation does not identify the cause of an individual stalled run.

The workflow first builds `NeverDHvfTests`, whose dependency boundary is LLVM
Support and the decoder, and requires native instruction execution before the
full process/LLVM dependency build. `validation=transport` stops after this
diagnostic profile; `full` remains the default and also requires both complete
gates. `validation=darwin` builds the Darwin owner and requires every matching
native workload independently of the complete CPU gate. The transport profile
reuses the full inventory's HVF requirements and
records `hvf_transport_only=true`; it is not full CPU/process acceptance.
The suspended Intel workflow previously built `NeverDX64ExceptionTests` and ran
its CR8 state/privilege regression before the large dependency build. The full
CPU gate owns the complete state/exception suite, without a duplicate preflight.
It uploads transport results and the state inventory before execution, then
preserves the isolated CR8 result before the complete CPU gate.
Artifacts include the run attempt so reruns retain their own evidence.
Full-inventory compilation has its own step and log. `test_parallel` selects
four CTest processes by default or one for a serialized comparison. Hosted
Intel full/Darwin method execution is always serial; compilation remains
parallel. The result summary records actual execution concurrency,
host OS/kernel description and logical CPU count.
It can also run locally with `--require-hvf --hvf-transport-only` on
`scripts/run_native_cpu_ci.py`. `validation=probe` runs only the VM/vCPU
availability check without LLVM; it cannot establish instruction execution
or NeverD transport correctness.

Coverage includes complete register/FP state, both guest privilege levels,
page permissions and cross-page memory, aliases and saved contexts, live probes
alongside another CPU, multi-CPU isolation, owner-thread routing, partial map
rollback, queue cancellation, native loop interruption and retry. Both ISAs have
required raw loop interruption and completion-error fixtures. These must observe
an actual native return; cancellation before entry cannot satisfy the loop test.
The full profile also requires the Intel CR8 all-register and privilege regression.
The transport profile requires 15 ARM64 or 12 Intel cases, and the full profile
requires 27 or 20 respectively. Intel compilation alone does not establish Intel
runtime correctness; its native gate remains required.

Hardware-backed execution is not automatically faster for NeverD's checked
instruction-by-instruction contract. Startup, dispatch, state transfer and
maintenance cost must be included when comparing it with the same checked
Unicorn contract. No throughput improvement is promised by backend selection.

### Implementation validation, 2026-10-02 to 2026-10-03

Completed evidence (rows overlap and must not be added):

| Scope | Clean source | Result |
| --- | --- | --- |
| ARM64, complete CPU inventory, twenty owners | `defc93928` | 6,842 registered; 849 passed, 0 failed, 5,993 skipped; 16/16 required native checks |
| ARM64, all Darwin workloads | `defc93928` | 286 registered; 65 passed, 0 failed, 221 skipped; 39/39 required native workloads |
| Intel HVF, all Darwin workloads | `8dcc74c59` | 286 registered; 52 passed, 0 failed, 234 skipped; 26/26 required native workloads |
| Intel HVF, complete FP owner | `3e01cda5c` | 71 registered; 35 passed, 0 failed, 36 skipped; twelve native HVF cases passed |
| Intel HVF, native exception/state owner subset | `908a830e6` | All 253 native registrations passed: 120 raw exceptions, 128 public CPU division cases and five state transitions |

The ARM64 results use an Apple M4 Max, macOS 15.6.1, SDK 15.5, Release, with
Unicorn disabled. Evidence for the full CPU and independent Darwin gates is
retained in `build-hvf-native/hvf-launcher-clean-{full,darwin}-evidence/`.
Foreign architectures/backends and unavailable software transports account for
skips; every required native result was actually executed. The Intel inventory
contains 6,840 registrations across the same twenty owners. Its only count
difference is the ten-case Intel transport owner versus twelve on ARM64.

The [Intel Darwin run](https://github.com/NeverSight/NeverD/actions/runs/37106013999)
completed all 32 methods, including the original host-kernel reference. Its
aggregate artifact `11267489438` was downloaded and SHA-256 verified against
`cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`.
Every original XML identity and process status was independently reconciled.
Evidence is retained in `build-hvf/verification/hvf-intel-darwin-accepted/`.
The same run passed all ten transport cases, 100 interruption/recovery
repetitions and isolated CR8 state/privilege checks. It used macOS x86-64 with
four logical CPUs and Darwin 24.6.0. This establishes the bounded macOS and
iOS Simulator profiles; it does not establish iOS device-kernel behavior.

The complete Intel CPU inventory remains unverified. The
[earlier full run](https://github.com/NeverSight/NeverD/actions/runs/37106679688)
at `76a922088ceccb1e5fdaccbde7f0b3a680a4e3b8` built all twenty owners and passed
ten transport cases, 100 recovery repetitions and CR8. Those checkpoint
artifacts were downloaded and SHA-256 verified. The run ended on 2026-10-03
at 08:35 UTC with a GitHub annotation reporting lost runner communication;
no CPU artifact was produced and the job-log endpoint returned 404. The
repository had no self-hosted runners. A completed full result or an accessible
native Intel Mac is needed to close this acceptance gap. Earlier stalled or cancelled
runs without complete evidence do not count as passes and do not identify a
guest fault. One earlier [hosted run](https://github.com/NeverSight/NeverD/actions/runs/37097301977)
explicitly lost runner communication; its underlying cause was not established.
CPU and Darwin workflow steps have explicit 30- and 15-minute limits.

The [raw-fault](https://github.com/NeverSight/NeverD/actions/runs/37094333371)
and [division](https://github.com/NeverSight/NeverD/actions/runs/37094335126)
artifacts establish the 253 native exception/state cases above. They cover ten
fault types at both privilege levels, recovery, mapping changes, private gateway
integrity, terminal traps and observer stops. The five shared state-transition
checks are counted only once. The [FP run](https://github.com/NeverSight/NeverD/actions/runs/37101437752)
reconciled all 71 registrations, including physical FP state, every TOP,
logical CPU switching and live probes. Standard host XRSTOR passed; compacted
XRSTOR was unavailable on that runner. These independently verified subsets do
not replace the full CPU gate.

#### Evidence collection

Hosted Intel full and Darwin validation use `--execution-methods`. CTest still
supplies every registered command and identity; each GoogleTest method runs in
one process with all registered parameters, flags, environment and working
directory. Unknown CTest properties are rejected. Direct GoogleTest commands
and the plain native `GoogleTest/LaunchTest.cmake` wrapper are supported;
custom executors, extra arguments and preassigned output paths are refused.
The actual 6,840-case Intel inventory was parsed and reconciled before execution.

Each child has an aggregate deadline capped at 120 seconds, followed by bounded
process-group retirement. Individual parameter timeouts are not separately
enforced inside GoogleTest. Raw XML, logs, exit status and exact CTest-name
mappings are retained. A timeout or incomplete XML ends execution with a
partial failure; complete assertion results still allow later methods to run.
Missing, duplicate, unexecuted or skipped required-native results fail the gate.
The summary records `execution=gtest-methods` and serial execution rather than
claiming a CTest execution result. Self-hosted validation continues to use
CTest's per-case processes and timeouts. The collector, gate, CI configuration,
inventory and result-audit suites passed 124 checks. Earlier instruction probes
have been removed; the standalone method diagnostic below remains available
while Intel runner loss is investigated.

#### Correctness and integration

Three Intel defects were reproduced and corrected. An
[isolated MSR experiment](https://github.com/NeverSight/NeverD/actions/runs/37078094659)
identified the missing managed `IA32_KERNEL_GS_BASE` context; the other eleven
individual MSRs did not restore execution. VM-instruction error 12 was also
present on successful runs and was not treated as the cause. An
[independent CR8 comparison](https://github.com/NeverSight/NeverD/actions/runs/37082402190)
showed that guest priorities 0, 1, 3 and 15 disagreed with host TPR readback;
authenticated CR8 exits now preserve architectural ownership. Finally,
cancellation recovery exposed zero RFLAGS after vCPU recreation; direct VMCS
RIP/RFLAGS transfer restored all three interruption modes and retry. Their
regressions remain in the native transport/state inventories.

On ARM64, the raw cancellation fixture was corrected to use separate immutable
loop and retry programs after rewriting one address exposed a stale `HVC`.
It passed 1,000 repetitions with eight concurrent processes; production cache
maintenance remains enabled. The clean `9319c880d` transport run also passed
all twelve cases and 100 interruption/recovery repetitions without skips.

Build and public-integration checks established the following:

- A build with both testing and Unicorn disabled produced a signed CLI that
  initialized HVF and executed the ARM64 ELF fixture. HVF-disabled configuration
  checks reported `build_disabled`, with no framework dependency.
- Live probes verified native selection, explicit foreign-ISA rejection and
  software-contract selection. Removing the process entitlement produced
  `device_access`; required-native tests failed instead of silently skipping.
- The Qt 6.11.1 bundle passed dependency/signature checks for 186 Mach-O images.
  Its worker retained the entitlement; Cocoa startup and EVM loading passed.
  A signed probe loaded its engine and matched 18 CLI reports across the three
  ARM64 Darwin profiles. This bundle's dependencies require macOS 15.0.
- The standalone worker suite passed eight checks, including three real-engine
  integrations, and the fixture-backed Qt/IPC/MCP suite passed all nineteen.
  The [desktop GUI workflow](https://github.com/NeverSight/NeverD/actions/runs/37053518872)
  passed on macOS, Windows and Ubuntu at `e078b129c`.
- Darwin also passed independently on Linux KVM and Windows WHP with Unicorn
  disabled: each had 51 passes, zero failures and all 26 required x64 workloads.
  Exact sources and artifacts are in the [Darwin evidence](darwin-emulation.md#hosted-native-verification-2026-10-03).

#### Performance and remaining scope

Historical measurements below predate the 2026-10-04 optimization; see the current results at the end of this page.

An alternating seven-sample benchmark, after warmup and excluding CPU creation,
ran the same checked ARM64 loop on both backends: two setup instructions plus
1,000 `ADD/SUBS/B.NE` iterations, or 3,002 guest instructions. Final registers
and PC were checked. Median elapsed time was **73.9 ms for Unicorn** and
**95.1 ms for HVF** (about 29% longer). This provides no evidence of a speedup
and does not represent every workload.

An extended benchmark covered initialization, integer branches, ordinary RAM,
TLS/calls, alternating CPUs and a Linux process. Shared-host load was about 30,
so its large latency ranges are not a stable performance claim. Entry counting
confirmed six native entries per ordinary ARM64 instruction: five maintenance
steps and one guest step. Registers, PC, RAM and instruction counts matched;
the Linux process reports matched for normal exit, memory fault, unknown
service and instruction-budget stop. The [Chinese record](zh-CN/macos-hvf.md)
retains these measurements. Quiet-host performance tests and complete Intel
CPU acceptance remain outstanding. No dedicated self-hosted runner is currently
configured. The bounded Darwin profiles do not implement dyld, Mach IPC or
Apple application frameworks; see their [explicit contract](darwin-emulation.md).

#### Complete hosted Intel inventory in shards

The earlier full run `37106679688` ended on 2026-10-03 at 08:35 UTC with a GitHub annotation reporting lost runner communication. All four jobs of run `37116327329` also lost communication and produced no CPU XML. These observations do not identify a failing guest instruction. Hosted Intel `full` uses four jobs, with at most two running concurrently. Each job executes four sequential batches: sixteen method shards in total, numbered `job + 4 × batch`. This preserves each job’s original workload. Every batch first builds and checks the complete twenty-owner CTest inventory. `--hvf-shard INDEX/COUNT` keeps whole methods and every parameter together, even when execution properties differ.

Before CPU execution, `scripts/prepare_hvf_batches.py` saves the full inventory and each selected inventory and method plan; the workflow uploads these diagnostics separately. A local composite action executes four batches and uploads each batch’s original XML, identity mappings, process status and required environment-variable whitelist immediately. One outer 30-minute deadline covers all four batches and their uploads. A failed batch prevents subsequent execution. A separate two-minute diagnostic upload follows failure or timeout while the runner remains reachable; lost runners can only be diagnosed from previously uploaded evidence. Plans and unfinished diagnostic bundles cannot count as passing shards.

`scripts/audit_hvf_shards.py` runs in a separate Linux job and rederives owners and native requirements from the checked-out source. The workflow downloads only the current attempt’s CPU artifacts; the auditor requires all sixteen shards from one clean commit, the correct native macOS ISA, matching normalized full execution contracts, disjoint results whose union equals the full inventory, successful child retirement, and every required native result. Missing shards, changed filters, mismatched summaries, incomplete XML and required-native skips fail. Every native job also retains transport, recovery, CR8 and the independent Darwin gate. Self-hosted execution keeps the unsharded CTest path. Batching does not itself establish Intel acceptance. A retry must rerun all native jobs; artifacts from earlier attempts are not combined.

The first verified CPU batch from [run `37123148209`](https://github.com/NeverSight/NeverD/actions/runs/37123148209), clean source `f5f29a484`, is shard `1/16`: 476 registered results, 82 passed, 0 failed and 394 skipped across 31 method processes; its one required-native case passed. Artifact `11274755752` was SHA-256 verified, and the original XML, child exit status, inventory and method plan were reconciled against the pre-execution plan. This is partial Intel evidence and does not close complete CPU acceptance.

## Latest local native verification

Clean source `4ce0b8247`, 2026-10-03 UTC. The complete ARM64 inventory passed in 503 method processes, and every original XML result, execution contract and child retirement was independently reconciled. The separate Darwin gate also passed. The rows overlap and must not be added.

| Scope | Registered | Passed | Failed | Skipped | Required native |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU, complete inventory | 7,003 | 867 | 0 | 6,136 | 16/16 |
| Darwin | 286 | 65 | 0 | 221 | 39/39 |

`build-hvf-native/hvf-current-4ce0-full-evidence/summary.json` · `build-hvf-native/hvf-current-4ce0-darwin-evidence/summary.json`

## Standalone Intel diagnosis

The manual [Intel diagnostic workflow](../.github/workflows/hvf-intel-diagnostic.yml) checks out its controller and the tested source separately. `source-ref` requires a full commit SHA; `shards` selects original sixteen-way shards. `first-method` is zero-based, `method-count=0` selects the remaining methods, and `case-index` selects one original parameter only when `method-count=1`. The complete twenty-owner inventory is discovered before selection. The original Release build, commands, parameters and required native checks are retained.

`intel-image` selects `macos-15-intel` by default or `macos-26-intel` for a controlled comparison, just as in the complete workflow. The run title identifies the selected image, and the availability check still requires a native x86-64 host. An image change includes the OS, SDK and toolchain; it does not isolate a kernel change.

Compilation has a separate 120-minute budget within a 180-minute job: the first macOS 26 full build took 76 minutes. Native method execution remains limited to 120 seconds and the diagnostic action to 30 minutes.

Before each method, the action uploads its immutable execution plan and a host snapshot. After execution it preserves original XML, process retirement, controller status and a second snapshot. Snapshots include memory, swap, load, disk and process IDs/state/CPU/RSS/executable names, without process arguments or environments; collection errors remain visible. An execution or upload failure stops later methods. Each method retains its 120-second execution cap; an unreachable host may prevent cleanup and final upload. Only already uploaded artifacts survive that loss. These are partial diagnostic results and cannot satisfy the complete CPU or independent Darwin gate. A last start marker identifies an execution boundary, not the failing guest instruction or root cause.

The complete `Native macOS HVF` workflow also accepts optional `source-ref`. It defaults to the workflow commit and otherwise requires a full SHA. Native jobs and the aggregate auditor check out and verify that same source; the audit compares evidence against the tested source, even when the workflow controller has a different revision.

For `hosted-intel`, the full workflow accepts `intel-image=macos-15-intel` (default) or `macos-26-intel`, both listed in the [official runner images](https://github.com/actions/runner-images). This permits an explicit host-environment comparison with the same `source-ref`; the image also changes the OS, SDK and tools. VM/vCPU, native transport, CR8, full CPU and Darwin requirements are unchanged. An image choice alone is not evidence of stability or a runtime fix.

`sample-active-child=true` optionally preserves one sealed active snapshot five seconds after observing native child registration: a one-second stack sample of its verified native child, up to 1 MiB of its current log tail, and host state. It defaults to `false`. Sampling accepts at most 166 methods per job to stay within the artifact limit; the sampling command has a twenty-second deadline and a 1 MiB report limit. Identity and collection failures are recorded, including a zero exit status without a stack report. Active uploads use a separate immutable directory; an upload failure cancels the native child and fails the action. Sampling changes scheduling and is labeled instrumented partial evidence. It neither resets the original method timer nor replaces complete acceptance.

The full workflow also accepts `recovery-repetitions=1000` for a focused interruption/recovery investigation; the default remains `100`. This choice gives the repetition step a ten-minute budget instead of three minutes. Each native test keeps its original deadline, assertions and stop-on-failure behavior. The run title identifies the longer repetition setting. These repetitions do not replace the complete CPU or Darwin gates.

The separate [Intel recovery workflow](../.github/workflows/hvf-intel-recovery.yml) builds only `NeverDHvfTests` and runs the original `HvfExecutor.Native*` filter in one process with `repetitions=100` or `1000`. It requires an exact `source-ref`, a native Intel VM/vCPU probe, Release, HVF enabled and Unicorn disabled. Controller, tested source and official artifact uploader use separate checkouts.

The action uploads its plan before execution. During execution it preserves an initial process marker, progress after at least 25 additional completed repetitions, and at most one stalled-progress snapshot when new output has not yet been preserved. It coalesces progress during uploads, limits progress artifacts to 42 and each log copy to 1 MiB, and uploads only sealed directories. Uploads do not pause individual repetitions or reset their timers; they still affect host scheduling, so this is instrumented partial evidence.

Success requires consecutive iterations from 1 through the requested count, the exact native test with paired RUN/OK/PASSED records in every iteration, no failures or skips, exit status zero and confirmed process retirement. Overwritten repetition XML cannot prove this. Native execution has a three- or ten-minute total budget, with another 30 seconds for controller cleanup; the action has 20 minutes. An upload failure cancels execution, and cancellation retires both the native process group and uploader. Final evidence is uploaded when the host remains reachable. Incomplete or truncated snapshots never satisfy complete CPU or Darwin acceptance.

`runner=hosted-intel` remains the recovery workflow default. `runner=self-hosted` uses the existing `[self-hosted, macOS, X64, hvf]` labels for an Intel host comparison. Both choices require native x86-64 and the VM/vCPU probe. Hosted images install Ninja; self-hosted hosts must already provide `cmake`, `ninja`, `python3`, `clang` and `codesign`. Source isolation, repetition limits, evidence checks and failure behavior are identical. This route requires an available matching runner; it does not provision hardware.

On 2026-10-03, clean source `e4a8169e69eb668ed3795efe4bd5f42cf4f287c2` passed local native ARM64 Release validation with HVF enabled, Unicorn disabled and `NEVERD_LLVM_PREBUILT=ON`. The complete CPU profile reconciled 20 owners, 520 methods and 7,125 results: 882 passed, 6,243 skipped, zero failed, and all 16 required native cases passed. The independent Darwin profile reconciled 32 methods and 286 results: 65 passed, 221 skipped, zero failed, and all 39 required native cases passed. The original one-process recovery loop also completed 1,000 consecutive repetitions, followed by all 12 native transport cases. Original logs, XML, process status and source definitions were independently checked. CPU and Darwin totals overlap and must not be added. These results establish neither complete Intel acceptance nor an iOS SDK build.

The [standalone Intel recovery run](https://github.com/NeverSight/NeverD/actions/runs/37159724276), using source `bd284894c60427cf4e6a60e661a1fa0df8a070f5`, ended at 23:41:30 UTC on 2026-10-03 with a GitHub annotation reporting lost hosted-runner communication. Its plan and eleven progress artifacts survived and were downloaded with verified server SHA-256 digests. The last preserved snapshot proves 252 complete repetitions and the start of repetition 253; it does not locate the eventual failure. No final result or process-retirement record was available, and the full job-log endpoint returned 404. The requested 1,000 repetitions therefore remain unverified. The repository still had no self-hosted runners. This is preserved failure evidence, not a stability fix or complete Intel acceptance.

A [personal-repository harness](https://github.com/gmh5225/test_mac_intel) provides another hosted Intel route without local Intel hardware. It pins the NeverD diagnostic and tested-source revisions independently and records the workflow revision separately. Queue time and runtime stability must be evaluated separately. The recovery action now preserves each uploader child's PID, parent, executable, exit status, signal and termination reason before reporting failure. After a failed action, a bounded collector waits up to 20 seconds for macOS IPS crash reports that match that child's PID, parent, process name and execution time. Missing reports remain explicit; reports from unrelated processes are not copied. A failed upload still terminates the original native run and cannot establish a hypervisor failure or a passing loop.

On 2026-10-04, the first personal-repository [macOS 26](https://github.com/gmh5225/test_mac_intel/actions/runs/37175472452) and [macOS 15](https://github.com/gmh5225/test_mac_intel/actions/runs/37175511460) jobs started 8 and 5 seconds after creation. They failed after uploader subprocesses exited; the controller cancelled and retired the native children after 3 and 105 complete repetitions respectively. Their final bundles were preserved and independently checked. A separate [uploader-only control](https://github.com/gmh5225/test_mac_intel/actions/runs/37177383621) passed all 16 uploads, with 17 server-digest-verified artifacts and `native_execution=false`. These observations distinguish upload failure from a native assertion failure; they neither identify its cause nor validate the requested 1,000 repetitions. The earlier organization control also started in 5 seconds, so these samples do not establish a queue-speed improvement.

The subsequent [macOS 26](https://github.com/gmh5225/test_mac_intel/actions/runs/37176652027) and [macOS 15](https://github.com/gmh5225/test_mac_intel/actions/runs/37176990174) runs both ended in failure on 2026-10-04; GitHub explicitly reported lost hosted-runner communication. Their 10 and 16 artifacts were verified. The last preserved logs prove 175 and 326 consecutive complete repetitions respectively, followed by one started repetition; neither locates the eventual fault. Both lack final native results and retirement records, and both full job-log endpoints returned HTTP 404. No cancellation was requested. The 1,000-repetition gate remains unverified on Intel. [Retained original log prefixes and the run/digest manifest](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04) preserve these results beyond the Actions artifact retention period.

## ARM64 maintenance optimization (2026-10-04)

The five TLB/I-cache maintenance operations now run together in one immutable private stub, followed by a dedicated HVC #1. The transport verifies the complete syndrome, return PC, PSTATE and ESR_EL1 before executing exactly one admitted guest instruction. Software stepping is disabled only during maintenance; all barriers, full state capture and the shared cancellation deadline remain. ERET is deliberately absent because exception return makes ESR_EL1 architecturally UNKNOWN. [Arm](https://documentation-service.arm.com/static/649ae5b238511951cb799288).

On an M4 Max with macOS 15.6.1, Release/Apple Clang 17 and prebuilt LLVM, separate instrumentation counted 252600 native entries before and 84200 after for the same 42044 guest plus 56 startup-probe instructions: six entries became two. Instrumented timings are excluded. Three recovery/fault tests each passed 1000 consecutive iterations in one process; cancellation requires a native memory-store witness and successful retry with rewritten guest code.

The clean integration at [389bebfdd](https://github.com/NeverSight/NeverD/commit/389bebfdda31a0db19facc7ab8ca5461a8c8c1bc) passed the complete CPU inventory: 2546 passed, 4710 skipped, zero failed; all 23 required native cases passed. The separate Darwin inventory had 130 passed, 156 skipped, zero failed and all 39 required native cases passed. These inventories overlap; their totals must not be added. No iOS SDK/device oracle is claimed.

Fifteen alternating process pairs, with one warmup per workload and no build or tests from this task running during measurement. The shared host load was 26.7–33.0 (software comparison: 28.8–32.6). Cells show median [minimum–maximum] milliseconds. Speedup is the median of paired before/after time ratios; the 95% percentile bootstrap interval uses 10000 resamples with seed 20261004. Long tails and only fifteen pairs limit generalization.

`4b54908b9` → `056090929` / Release / Apple Clang 17 / LLVM 23 prebuilt / Unicorn `df88be772`.

| Workload | Before ms [min–max] | After ms [min–max] | Paired speedup | 95% interval | Faster pairs |
| --- | --- | --- | --- | --- | --- |
| `initialization` | 1.292 [0.804–6.793] | 1.118 [0.797–29.077] | 0.998× | 0.809–1.154 | 7/15 |
| `integer` | 125.426 [92.700–587.385] | 70.229 [54.067–895.051] | 1.595× | 1.373–1.884 | 13/15 |
| `branch` | 251.909 [168.279–974.846] | 155.678 [106.833–1566.522] | 1.495× | 1.055–1.687 | 12/15 |
| `memory` | 231.574 [140.137–1511.815] | 143.496 [96.508–1209.777] | 1.535× | 1.276–1.984 | 13/15 |
| `tls_call` | 347.850 [203.188–2536.731] | 212.467 [136.304–1441.830] | 1.552× | 1.428–2.912 | 14/15 |
| `two_cpu_switch` | 34.629 [25.019–416.105] | 26.684 [18.926–60.159] | 1.389× | 1.283–1.515 | 13/15 |

### Unicorn / optimized HVF (above 1 favors HVF)

| Workload | Paired speedup | 95% interval |
| --- | --- | --- |
| `initialization` | 0.232× | 0.195–0.325 |
| `integer` | 1.878× | 1.437–2.896 |
| `branch` | 2.681× | 1.824–3.317 |
| `memory` | 2.967× | 2.224–3.215 |
| `tls_call` | 2.339× | 0.977–3.257 |
| `two_cpu_switch` | 0.831× | 0.541–1.612 |

These are measurements of checked ARM64 workloads on one shared host, not a universal speed ranking. Intel runner disconnects and process crashes remain unresolved and require separate investigation. The bounded macOS/iOS CPU profiles do not become full Apple OS/device emulation.

[Reproduction: `neverd-cpu-bench`, `benchmark_cpu.py`](testing.md#reproduce-checked-arm64-cpu-measurements).

## Intel isolation results (2026-10-04)

The finite owner-deadline candidate `909672ca6` remains experimental. The original 1000-repetition recovery test lost runner communication on both macOS 15 and 26; its last preserved prefixes contain 277/278 and 250/251 completed/started iterations. Ordinary instruction execution with the same candidate and no explicit cancellation also lost communication on macOS 15 (576/577). GitHub confirmed all three losses. None has a final native result or retirement record; a saved prefix does not locate the eventual fault. [Original evidence](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-boundaries).

An independent, unchanged `hvf-edge-cases` program at `f150b38` completed its real-mode guest and 100000 random interrupt call attempts on both images, in approximately 362 and 398 seconds. Both exited zero and their children were reaped; artifact digests, source hashes, checkpoints and guest completion were independently checked. Earlier 300-second attempts reached their observer deadline and were retired, without losing the runner. The upstream program ignores random interrupt return codes, so attempt counts do not establish one-to-one delivery. This control is not NeverD acceptance or a performance comparison. [Source, logs and audit](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-upstream).

These results narrow the investigation but establish no production fix. Retaining the VM and executor threads is a diagnostic comparison, not an accepted lifecycle change. Intel still requires the original 1000-repetition recovery test, complete CPU inventory and independent Darwin gate on the same clean candidate.

Both lifecycle controls now passed 1000/1000 on both Intel images: vCPU recreation (37195529270, 37195554929) and VM plus vCPU recreation on one retained owner (37196504453, 37196535787). The latter has 8000 ordered native events and final generation 1001 retirement per run; all 24/27 artifact digests, zero native/controller exits and child retirement were verified. This narrows the comparison with full Executor turnover but neither identifies the cause nor proves a production fix. The next diagnostic will retain the VM while replacing the vCPU and owner thread; original recovery and full CPU/Darwin acceptance remain required.

Owner turnover did not complete its 1000-round experiment: the uploader crashed on macOS 15 with SIGTRAP in V8 string parsing (37198629082), and on macOS 26 with SIGSEGV in V8 scope lookup (37198630903). The controller then cancelled and reaped the native processes; the saved logs show 24/25 and 467/468 completed/started iterations, without a native assertion or final native result. Both runners stayed reachable and supplied matched crash reports. These are observer-triggered interruptions, not verified native passes or confirmed runner losses. The cause remains unknown; compare upload-only controls before changing the backend.

Both synthetic upload-only controls (37199672430, 37199674303) and exact failed-snapshot replays (37200549588, 37200551385) completed 16/16 uploads per run without a VM or guest. All 17 artifact digests per run, zero exits, absence of signals/cancellation, and replay bytes against pinned payload commit `e02e8c6` were independently verified. All four controls share Node 24.19.0, V8 13.6.233.17-node.51 and the same executable SHA256. Their Intel UUID matches the earlier crash reports, which did not record executable hashes. These controls did not reproduce the crash from those payloads alone; concurrent native execution, the crash cause and Intel acceptance remain unresolved. [Verified evidence](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-upload-controls).

Finite-deadline recovery with the whole Executor/VM/owner session retained passed 1000/1000 on macOS 15 (37203540596). macOS 26 (37203542459) completed 772 iterations, then asserted in iteration 773 because the caller's 50 ms deadline expired during admission, before the native callback began. Its native process exited with SIGTRAP under `--gtest_break_on_failure` and was reaped; the runner remained reachable. All 43/34 artifact digests were verified. This is a native test assertion, distinct from an uploader crash or runner loss, and does not isolate VM lifetime as the cause. The fixture correction gives admission its existing cooperative 2 s deadline and starts the native 50 ms deadline after owner preparation, clamped to the outer deadline. All interruption modes, real-return assertions, retry checks and the independent 600 s controller cap remain. Future controllers also seal the actual Node file's SHA256, native Mach-O UUID and Node/V8 versions before guest execution; this identifies the disk file at collection time, not process memory integrity or the missing hashes of older crashes.

The stable-owner-thread candidate `faad8299b` also failed the original fresh-Executor recovery1000 gate on both images: GitHub confirmed runner loss for macOS 15 (37202068724) and macOS 26 (37202070988). Their 15/12 verified artifacts preserve 302/303 and 225/226 completed/started iterations respectively, without final native results or process-retirement records. Keeping the owner thread alive was insufficient in these runs. The candidate was merged into `dev` through [PR #444](https://github.com/NeverSight/NeverD/pull/444) at 2026-10-04 13:16:49 UTC. That merge does not establish a verified runtime fix or complete CPU/Darwin acceptance; the saved prefixes do not identify a fault site.

The corrected fixture at `7dd7342ec` subsequently passed 1000/1000 consecutive retained-session recovery iterations on both macOS 15 (37204841332) and macOS 26 (37204843517). All 43 artifacts per run, zero native/controller exits, process retirement and identical pre/post Node runtime records were independently reconciled. This verifies the admission-budget correction in that diagnostic; it does not resolve fresh-Executor runner loss or establish complete Intel acceptance.

The corrected recovery with vCPU-only recreation (`7dd7342ec` source, `31afddad3` helper, `10bf50753` workflow) passed all 1000 rounds on macOS 15 ([37215096822](https://github.com/gmh5225/test_mac_intel/actions/runs/37215096822), 43 verified artifacts). Final native/controller exits were 0 and the child was reaped; the lifecycle shows one VM and 1001 vCPU boundary generations, while total/executed vCPU counts remain unknown. On macOS 26 ([37215098793](https://github.com/gmh5225/test_mac_intel/actions/runs/37215098793), 19 artifacts), the progress-016 uploader received SIGSEGV. Its IPS matches PID 32519, parent 29104, capture time and Node UUID; the saved native prefix proves 403 completed / 404 started rounds with no assertion. The controller then cancelled and reaped the native process (SIGKILL), leaving no final native result. This is incomplete native evidence, not a runner loss or a pass. Node 24.19.0 was recorded before execution with SHA-256 `1052eb9c7d6c60a79b968e09f75af55a73462b0f6dff0964336d63b5e13eb63c`; that establishes on-disk file identity, not unchanged process memory. These immutable-source results do not validate later merges into dev.

Recovery with per-round VM/vCPU recreation also ended in confirmed runner loss on both images: [macOS 15 / 37213675739](https://github.com/gmh5225/test_mac_intel/actions/runs/37213675739) and [macOS 26 / 37213681083](https://github.com/gmh5225/test_mac_intel/actions/runs/37213681083). Both used source `7dd7342ec`, helper `4c702d35a` and workflow `fad0eadf2`. The 23/26 verified artifacts preserve contiguous prefixes of 502/503 and 575/576 completed/started rounds. Neither has a final native result or process-retirement record. GitHub confirms loss of communication, not its cause. The instruction-only VM recreation passes do not extend to this recovery workload; the dependent whole-session reset experiment remains gated off.

The `jitless` uploader controls ([37217523688](https://github.com/gmh5225/test_mac_intel/actions/runs/37217523688) / [37217525863](https://github.com/gmh5225/test_mac_intel/actions/runs/37217525863), macOS 15/26) failed before starting recovery: both plan-upload children exited 1 without a signal because the official uploader’s HTTP parser requires WebAssembly, which `--jitless` hides. Each run has two verified artifacts; its plan is recovered from final evidence, not a successful independent plan upload. These failures do not test native recovery, and do not exclude the earlier workflow HVF capability probe. The replacement opt-in mode `js-interpreter` uses `--no-turbofan --no-maglev --no-sparkplug` only for controller plan/progress upload children, preserving WebAssembly and other code generation. Parent, native workload and deadlines stay unchanged; provenance/final uploads remain default. Local real-HTTP and credential-free official-uploader checks passed; this is observer compatibility evidence, not an HVF stability fix.

The `js-interpreter` vCPU-recreation recovery control used source `7dd7342ec`, helper `caeb594ad` and workflow `e6d054c45`. macOS 26 ([37218631679](https://github.com/gmh5225/test_mac_intel/actions/runs/37218631679)) passed 1000/1000: 43 artifacts, all 41 plan/progress upload invocations, native/controller exit0, child reaping and final boundary-generation 1001 retirement were independently verified. macOS 15 ([37218629672](https://github.com/gmh5225/test_mac_intel/actions/runs/37218629672)) instead ended after uploader progress-018 received SIGSEGV; its 21 artifacts preserve 460 completed / 461 started native rounds, followed by controller cancellation and native SIGKILL/reaping. The IPS matches PID 62812, parent 59666, time and Node UUID; its top frame is in V8 concurrent heap marking at invalid address `0x80000000`. All three JavaScript compiler-disable flags were present and both images used the same recorded Node file SHA. This mode therefore did not eliminate uploader crashes. The frame does not establish the cause, the macOS 15 native result remains unknown, and neither full CPU/Darwin acceptance nor the two-image gate for a cache candidate was met. A default persistent cache would also conflict with the existing temporary-probe and last-client VM-return contract; it was not implemented.

A reproducible supplementary audit of the unchanged output from run `37188627569` (source `392a9d171`) verified all 1,000 finite-probe iterations. The [auditor and 19 regression tests](https://github.com/gmh5225/test_mac_intel/tree/d203767/scripts) associate each begin/end/capture triplet with its call, fresh store witness and RIP, validate the fixed first-loop budget and the two separately controlled MTF observations, and retain legal no-entry observations without counting them as instruction progress. This replays saved evidence; it is not a new native run and does not change any prior outcome, resource-retirement requirement or Intel acceptance status.

The control that omitted the extra host kick also lost both runners: macOS 15 run `37221649736` and macOS 26 run `37221651593`, both with GitHub lost-communication annotations. Source `023a4a68d` retains the three recovery rounds, retries and per-iteration VM/vCPU recreation; its production `lib` tree is identical to the older diagnostic source `7dd7342ec`. All 34/15 artifact hashes were verified. Contiguous prefixes prove 778/779 and 300/301 completed/started iterations, with 779/300 terminated post-join omission markers; an omission marker can belong to an unfinished iteration. Neither run has a final native result or retirement record, and the last saved marker does not locate the fault. Omitting this call was insufficient to prevent these observed losses. This establishes neither their root cause nor acceptance of current `dev`. [Retained evidence](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-boundaries).

The finite-timer vCPU-recreation controls passed 1,000/1,000 on macOS 15 (`37226727488`) and macOS 26 (`37226729664`), using unchanged source `7dd7342ec`, helper `4e80b1394` and workflow `7f5d6fc89`. Each run retained one VM and owner, created 1,001 vCPU generations, and retired the final unused generation. All 28 artifact hashes per run, ordered raw-call captures, 1,000 fresh timer/store witnesses, both MTF observations per iteration, original budgets, zero native/controller/uploader exits and child reaping were reconciled. macOS 26 preserved two timer slices without guest progress, then obtained the required witness within the original budget. Controller durations were about 26/35 seconds; exposure differs from the earlier retained-session recovery controls at about 254/283 seconds. These results do not resolve runner loss, validate current `dev`, or establish complete Intel CPU/Darwin acceptance. [Verified evidence](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-boundaries).

The matched finite-timer VM-recreation controls also lost both hosted runners: macOS 15 [37250462574](https://github.com/gmh5225/test_mac_intel/actions/runs/37250462574) and macOS 26 [37250464732](https://github.com/gmh5225/test_mac_intel/actions/runs/37250464732), with explicit GitHub lost-communication annotations. Source `7dd7342ec`, helper `4e80b1394` and workflow `7f5d6fc89` match the successful vCPU-only controls. All 16/28 artifact hashes were verified. Contiguous prefixes prove 742/743 and 796/797 completed/started rounds; strict finite-call auditing covers only the 742/796 complete rounds, including their fresh timer/store witnesses and both MTF observations. Unfinished suffixes remain preserved and unverified. Neither run has a final native exit or retirement record, and the last saved marker cannot locate the failure. Loss recurred without the original deliberately cancelled recovery rounds; initialization and retries still use `Executor::run`. This does not identify the cause or validate current `dev` or complete Intel CPU/Darwin coverage. [Retained evidence](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-04-boundaries).

ARM64 revalidation on 2026-10-05 used clean source [90643c3d4](https://github.com/NeverSight/NeverD/commit/90643c3d47aec683953fe8a911397922816125d9), macOS 15.6.1 and an independent Release build with HVF enabled and Unicorn disabled. The complete CPU inventory ran 906 serial methods: 1,406 passed, 9,979 skipped, zero failed, with all 23 required native cases passed. The separate Darwin inventory had 65 passed, 221 skipped, zero failed and all 39 required native cases passed. Raw XML, inventory identities, zero child exits and reaping were independently reconciled. Three cancellation/fault/retry methods each passed 1,000 consecutive repetitions in one process. Skips are unexecuted coverage; overlapping inventories must not be added. Evidence is retained in `build-hvf-native/native-evidence-audit.json` and `build-hvf-native/recovery-stress/`. This establishes neither Intel acceptance, an iOS device oracle nor a new performance claim.

The new independent 16-bit real-mode API control removes NeverD's Executor, long-mode state and managed MSRs. All six attempts and 34 verified archives are retained: the first live pair produced a macOS 15 uploader SIGSEGV (native outcome unknown) and a macOS 26 pass; the first pair without live observation both failed their unchanged budget. Those earlier intervals included log writes, and the original parser rejected CRCRLF terminators. Protocol 2 at `ac717e4` corrects LF framing, disables PTY output processing for this control, rechecks the same deadline after the begin log, and captures state before end logs. The matched final-only vCPU pair then produced macOS 15 [37259797988](https://github.com/gmh5225/test_mac_intel/actions/runs/37259797988): 1,000 rounds, exit 0 and retirement; macOS 26 [37259800022](https://github.com/gmh5225/test_mac_intel/actions/runs/37259800022): 571 complete rounds, then a round-572 budget failure, exit 1 and verified cleanup/retirement. The failing pre-call-to-return interval was 2,461,380,098 ns; begin logging took 5,946 ns and state capture after return 4,191 ns. This excludes log writes from that interval, but not host scheduling; it does not prove VM entry time or an HVF kernel fault. No uploader failed or runner was lost in this pair. The 2-second budget was not relaxed. Both-platform vCPU acceptance, VM recreation, original NeverD recovery and full Intel CPU/Darwin acceptance remain open; these controls are not a performance comparison. [JSON / Actions](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-05-api-lifecycle).

The protocol-3 accounting pair at `fb7a9d2` also failed its original budget: macOS 15 [37262095705](https://github.com/gmh5225/test_mac_intel/actions/runs/37262095705) completed 781 rounds and failed round 782; macOS 26 [37262097389](https://github.com/gmh5225/test_mac_intel/actions/runs/37262097389) completed 396 and failed round 397. Both returned IRQ with the fresh store, exited 1, completed checked cleanup and were reaped; neither lost the runner or an uploader. The failing pre-call/return intervals were approximately 2.068/2.002 s. Over their separately bracketed sample windows, Intel HVF execution counters advanced approximately 2.063/1.996 s and Darwin thread CPU counters 2.068/2.001 s. The getters were outside the call/capture intervals, and their sampling widths and raw counters are retained. These nested-host reported clocks add evidence beyond wall time but cannot establish physical guest execution, scheduling duration or a kernel root cause. All eight attempts and 38 verified archives remain available; 5 ms slices and the 2 s iteration budget are unchanged. Both-image vCPU, VM recreation and full NeverD Intel acceptance remain open. [Raw evidence and accounting](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-05-api-lifecycle).

The protocol-4 HLT contrast at `ca8da7d` ended with both jobs cancelled: macOS 15 [37263893167](https://github.com/gmh5225/test_mac_intel/actions/runs/37263893167) and macOS 26 [37263895410](https://github.com/gmh5225/test_mac_intel/actions/runs/37263895410). Their matching GitHub check-runs report the 30-minute job limit, without an explicit lost-communication annotation. Each retains only its pre-execution artifact; both downloadable workflow-log ZIPs have zero members. Native startup, completed rounds, HLT exit, process exit and retirement remain unverified. The configured 5 ms slices, 2 s iteration and 600 s process budgets do not prove that timeout handling or retirement ran. This final-only control cannot identify a blocking operation or kernel cause, validate the original timer gate or complete Intel acceptance. All 10 attempts and 40 original artifact ZIPs are retained. The offline verifier checks archive digests, source pins, process/budget evidence and derived analyses; the 11-language index shows missing counts as unknown. [Evidence and replay instructions](https://github.com/gmh5225/test_mac_intel/tree/main/results/2026-10-05-api-lifecycle).

## ARM64 watchdog experiment (2026-10-05)

Removing the notification after watchdog disarm passed correctness checks but did not establish a reliable performance benefit. Fifteen alternating pairs used matching Release configurations and dependencies. Median baseline/candidate time ratios for `initialization`, `integer`, `branch`, `memory`, `tls_call` and `two_cpu_switch` were **0.902, 0.897, 0.856, 1.030, 0.912 and 0.969**; values above one favor the candidate. Other tasks were compiling and samples had long tails, so these observations do not establish a causal regression. The runtime change was withdrawn; the existing notification is retained. Three new regressions cover an earlier deadline after disarm, destruction after disarm and retirement of a scoped stop token.

The final retained source `6c4a5ef1f` independently passed 32 RunControl tests, 27/27 transport tests, the full CPU inventory (1429 passed, 10644 skipped; 27/27 required) and Darwin (65 passed, 221 skipped; 39/39 required), with zero failures. Each of the three native interruption/maintenance recovery methods also passed 1000 consecutive repetitions with confirmed child retirement. Inventories overlap; do not add their totals. Skips cover disabled or foreign transports/architectures. This establishes bounded native ARM64 coverage, not Intel execution, complete Apple OS emulation or a performance ranking. Intel checks were local x86-64 syntax compilation only.

[Raw paired samples](benchmarks/2026-10-05-arm64-hvf-watchdog.json) · [Source and configuration](benchmarks/2026-10-05-arm64-hvf-watchdog-metadata.json)
