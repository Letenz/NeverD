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

Runnable continuations, newly queued workers and system threads share a ready order. Nested calls and SEH retain their thread's quantum. Switching preserves the complete CPU context, logical thread identity, APC state, effective IRQL and process mappings. PASSIVE/APC execution is preemptible; DISPATCH or higher masks thread switching. Critical and guarded regions disable APCs without disabling thread preemption.

Running instructions advance timers and DMA through chronological deadlines. Cancellation waits for cancel-lock eligibility and, for WDM, dispatch return. Pageable provider completion, power policy and PoFx service wait for a passive boundary; original deadlines remain pending and observations record actual service time. Clock-produced independent callbacks retain separate ownership from blocking same-thread PoFx callbacks. Wait results become final at their deadline, before a later timer reset or event signal, even when one instruction attempt spans both deadlines.

This profile does not implement Windows priority classes/boosts, parallel OS-model CPUs, arbitrary interrupt nesting, APC delivery or Windows ring3 thread creation and scheduling. CPU parallel execution is a separate capability. The tests cover original compiled drivers, tiny quanta, floating-point/GS state, timed waits, interleaved process attachment, cancellation and PoFx continuations. Native ARM64 OS evidence remains separate.
