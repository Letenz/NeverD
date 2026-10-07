# Preemptible Windows driver scheduling

An optional `scheduling` object enables deterministic preemption for x64 Windows drivers on CPU0 with Unicorn, KVM or WHP. Omitting it retains cooperative scheduling and idle-only virtual time. Both fields are positive integers; their product must fit `uint64_t`. An empty object selects the defaults shown below.

```json
{
  "scheduling": {
    "quantum_instructions": 1024,
    "instruction_time_100ns": 1
  }
}
```

`quantum_instructions` bounds a thread's admitted machine-instruction attempts. `instruction_time_100ns` assigns virtual time to each attempt; it is not a hardware speed estimate. Model APIs and instruction transactions complete before a switch. Event boundaries do not reset the remaining quantum. The report retains the selected policy in `configuration.scheduling`.

`KeSetPriorityThread` and `KeQueryPriorityThread` expose runtime priorities at `PASSIVE_LEVEL`. Setters accept 1..31 and return the previous priority; zero is reserved. The deterministic profile starts every modeled thread at 8. The highest-priority ready thread runs; equal priorities use ready order and quantum rotation. A higher-priority wake or priority change preempts before the next guest instruction below DISPATCH_LEVEL, preserving the interrupted quantum. Priority belongs to the thread through nested callbacks and SEH. Retained exited system-thread objects remain queryable until retirement; changing terminating/exited threads is unsupported. Borrowed callback thread objects retire with their owning thread, so stack reuse starts with fresh priority state.

Kernel mutex ownership follows the logical thread across nested callbacks and SEH. `KeWaitForSingleObject` captures that identity for deferred acquisition; `KeReleaseMutex` accepts the owning thread on any of its stacks. Normal APC suppression lasts until the final recursive release, and the outermost return rejects unreleased mutexes.

`KeWaitForMultipleObjects` supports `WaitAll` and `WaitAny` over 1..64 distinct initialized events, timers, semaphores, mutexes or referenced system threads, using nonalertable `KernelMode` and reason `Executive`. `WaitAll` commits every acquisition together; `WaitAny` returns the lowest ready array index and consumes only that object. More than three objects require writable nonpaged `KWAIT_BLOCK` storage. A deferred wait captures the object array, retains every object and protects caller wait blocks until success or timeout. Zero-timeout polling permits DISPATCH_LEVEL; blocking waits preserve an IRQL through APC_LEVEL. Duplicate objects, alertable/user-mode waits and mutex abandonment remain unsupported.

Runnable continuations, newly queued workers and system threads share a ready order. Nested calls and SEH retain their thread's quantum. Switching preserves the complete CPU context, logical thread identity, APC state, effective IRQL and process mappings. PASSIVE/APC execution is preemptible; DISPATCH or higher masks thread switching. Critical and guarded regions disable APCs without disabling thread preemption.

Running instructions advance timers and DMA through chronological deadlines. Cancellation waits for cancel-lock eligibility and, for WDM, dispatch return. Pageable provider completion, power policy and PoFx service wait for a passive boundary; original deadlines remain pending and observations record actual service time. Clock-produced independent callbacks retain separate ownership from blocking same-thread PoFx callbacks. Wait results become final at their deadline, before a later timer reset or event signal, even when one instruction attempt spans both deadlines.

This profile does not implement Windows priority classes/boosts, parallel OS-model CPUs, arbitrary interrupt nesting, APC delivery or Windows ring3 thread creation and scheduling. CPU parallel execution is a separate capability. The tests cover original compiled drivers, tiny quanta, floating-point/GS state, timed waits, interleaved process attachment, cancellation and PoFx continuations. Native ARM64 OS evidence remains separate.
