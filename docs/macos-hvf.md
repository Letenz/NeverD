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
and authenticated exception exits. Both architectures run the existing complete
state startup probe before exposing a CPU.

Queue admission observes the borrowed stop token and original deadline. One
native-step allowance covers preparation, maintenance, entry and capture.
`RunDeadline` acknowledges outstanding interrupts before returning. Cancelled
native entry recreates the vCPU so a late kick cannot affect the next task.
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
`self-hosted, macOS, ARM64/X64, hvf` runners. It does not assert those machines
are already provisioned or assume nested hosted runners support HVF.

Coverage includes complete register/FP state, both guest privilege levels,
page permissions and cross-page memory, aliases and saved contexts, live probes
alongside another CPU, multi-CPU isolation, owner-thread routing, partial map
rollback, queue cancellation, native loop interruption and retry. The raw loop
interrupt regression currently runs on ARM64. Intel compilation alone does not
establish Intel runtime correctness; its native gate remains required.

Hardware-backed execution is not automatically faster for NeverD's checked
instruction-by-instruction contract. Startup, dispatch, state transfer and
maintenance cost must be included when comparing it with the same checked
Unicorn contract. No throughput improvement is promised by backend selection.

### Implementation validation, 2026-10-02 to 2026-10-03

On an Apple M4 Max, macOS 15.6.1, SDK 15.5, Release: the focused CPU/process/C API
suite passed 2,304 tests with zero failures (4,205 inapplicable platform, ISA or
backend parameter cases skipped). The expanded no-Unicorn gate covers 19
owners and passed 753 tests, including all nine required HVF cases, with 5,652
inapplicable or disabled parameter cases skipped. The disabled-HVF configuration passed both configuration tests and
skipped its five hardware cases. Packaging and evidence-script unit suites
passed 10 and 17 tests respectively.

A separate build with both testing and Unicorn disabled built the signed CLI,
initialized HVF and executed the ARM64 ELF process fixture. Capability and
Python SDK audits passed, along with 92 native-evidence/CI audit unit tests.
The expanded gate also covers Linux/Windows processes, integer ABI, sessions
and budgets. Removing a process test executable's entitlement verified that
ordinary runs skip but required native runs fail.

The subsequent [Darwin process environment](darwin-emulation.md) extension
expands that no-Unicorn gate to 20 owners: 811 passed, zero failed, 5,842 skipped,
with all 12 required ARM64 cases executed (nine HVF checks and three Darwin
platform fixtures). This includes 57 Darwin checks for
macOS, iOS device and iOS Simulator guest contracts. Darwin's ARM64 OS pages
are 16 KiB even though the shared CPU mapping granule remains 4 KiB.

A final check found that the raw cancellation fixture could execute a stale
`HVC` when it rewrote its loop without guest instruction-cache maintenance.
Using separate immutable loop and retry programs passed 1,000 repetitions with
eight concurrent processes, followed by all 36 HVF/configuration/public API
smoke tests. The production ARM64 adapter retains guest cache maintenance.

Live CLI probes verified native selection, explicit foreign-ISA rejection,
the software-contract selection, and a `device_access` diagnostic when a copy
of the executable was signed without the entitlement. A standalone worker and
the complete Qt 6.11.1 bundle passed signature checks across 186 Mach-O images.
The packaged Cocoa GUI smoke and worker EVM load/disassembly passed. A signed
probe loaded the bundle's engine and executed HVF initialization and the ARM64
ELF fixture. This bundle's dependencies require macOS 15.0. Three backend translation units also
compiled against the macOS x86-64 SDK, but Intel hardware execution remains
unverified and requires the native gate above.

An alternating seven-sample microbenchmark, after warmup and excluding CPU
creation, executed the same checked ARM64 loop on both backends: two setup
instructions plus 1,000 `ADD/SUBS/B.NE` iterations, 3,002 guest instructions per
run. Final registers and PC were checked. Median elapsed time was 73.9 ms for
Unicorn and 95.1 ms for HVF (about 29% longer). This short integer loop provides
no evidence of a speedup and is not representative of every workload.

An extended seven-sample benchmark covered initialization, integer branches,
ordinary RAM, TLS/calls, alternating CPUs and a Linux process. It ran while
the shared host's load average was approximately 30 and observed large latency
ranges; these results are not a stable throughput claim. Direct entry counting
confirmed six native entries per ordinary ARM64 instruction (five maintenance
steps and one guest step). Registers, PC, RAM and instruction counts matched
the independent fixtures; the Linux process's normalized reports matched
Unicorn for normal exit, memory fault, unknown service and instruction-budget
stop. See the [detailed measurements](zh-CN/macos-hvf.md). Quiet-host performance
measurement and current-change Linux KVM/Windows WHP runtime regression remain
outstanding alongside the Intel gate; no self-hosted runner is currently
configured for the repository.

The subsequent integration pass repaired the test SDK's missing
`neverd_session_set_load_progress` and made the shared worker test client wait
for terminal responses while retaining progress for assertions. All eight
standalone worker checks passed, including three real-engine integrations.
The complete fixture-backed Qt/IPC/MCP suite passed all 19 checks on macOS.
