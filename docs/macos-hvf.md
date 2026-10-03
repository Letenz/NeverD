**Languages**: [English](macos-hvf.md) | [简体中文](zh-CN/macos-hvf.md) | [繁體中文](zh-TW/macos-hvf.md) | [日本語](ja/macos-hvf.md) | [한국어](ko/macos-hvf.md) | [Français](fr/macos-hvf.md) | [Deutsch](de/macos-hvf.md) | [Español](es/macos-hvf.md) | [Italiano](it/macos-hvf.md) | [Русский](ru/macos-hvf.md) | [العربية](ar/macos-hvf.md)

[← Documentation index](README.md)

# macOS native CPU execution (HVF)

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

ARM64 explicitly single-steps the immutable TLB/I-cache maintenance sequence
before the admitted instruction. Debug exceptions routed to EL2 cannot be
masked using the guest's `PSTATE.D`. All scalar, TLS and FP/SIMD state uses the
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
`RunDeadline` acknowledges outstanding interrupts before returning. Cancelled
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
`.github/workflows/hvf.yml` exposes the same manual gate for dedicated native
`self-hosted, macOS, ARM64/X64, hvf` runners. Its `hosted-intel` selection tries
GitHub's `macos-15-intel` runner. Both choices first compile and sign
`scripts/probe_hvf_host.c` and require actual VM/vCPU creation and teardown
before preparing LLVM. This availability probe does not execute guest code.
A hosted runner that denies HVF fails at that boundary; its label does not
establish virtualization support. After the CPU gate, the workflow also
requires every matching Darwin workload. Dedicated runners are not assumed
to be provisioned.

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
On Intel, the workflow additionally builds `NeverDX64ExceptionTests` and runs
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
The transport profile requires 12 ARM64 or 10 Intel cases, and the full profile
requires 16 or 14 respectively. Intel compilation alone does not establish Intel
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

`sample-active-child=true` optionally preserves one sealed active snapshot after a method has run for five seconds: a one-second stack sample of its verified native child, up to 1 MiB of its current log tail, and host state. It defaults to `false`. Sampling accepts at most 166 methods per job to stay within the artifact limit; the sampling command has a five-second deadline and a 1 MiB report limit. Identity and collection failures are recorded. Active uploads use a separate immutable directory; an upload failure cancels the native child and fails the action. Sampling changes scheduling and is labeled instrumented partial evidence. It neither resets the original method timer nor replaces complete acceptance.
