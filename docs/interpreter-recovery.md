**Languages**: [English](interpreter-recovery.md) | [简体中文](zh-CN/interpreter-recovery.md) | [繁體中文](zh-TW/interpreter-recovery.md) | [日本語](ja/interpreter-recovery.md) | [한국어](ko/interpreter-recovery.md) | [Français](fr/interpreter-recovery.md) | [Deutsch](de/interpreter-recovery.md) | [Español](es/interpreter-recovery.md) | [Italiano](it/interpreter-recovery.md) | [Русский](ru/interpreter-recovery.md) | [العربية](ar/interpreter-recovery.md)

[← Documentation Index](README.md)

# Interpreter source recovery

The experimental interpreter specialization stage removes statically resolved
dispatch from a linked x64 function while retaining its runtime inputs, memory
effects, branches, and loops. It uses instruction semantics rather than handler
signatures or a particular protector's opcode table.

```sh
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --llvm -o recovered-llvm.c
```

`--vm-control` selects full general registers that distinguish interpreter
contexts. It can be repeated, and it supplies no concrete values. For example,
select a bytecode cursor whose value is established by the entry stub. A
runtime input used as a counter must remain dynamic. Missing context separation
can cause recovery to stop when distinct cursor values meet; the engine must
not guess a dispatch target to compensate. For a spilled cursor,
`--vm-control-stack=-16:8` selects eight bytes at entry-RSP minus 16. The offset
is relative to the function entry, not the current adjusted stack pointer.

The C API is `neverd_devirtualize_source_v1()` in
`neverd/sdk/NeverDCAPIDevirtualize.h`. It runs a separate transaction without
modifying the session's ordinary decompilation cache. Failure returns no source
and can still return a JSON diagnostic. Both owned strings use
`neverd_free_string()`.

## Execution contract

The binary adapter currently accepts linked x64 ELF and PE images at their
mapped addresses. Mappings, bytes, and permissions must remain fixed, with no
concurrent mutation. Strict on-demand lifting follows reachable machine code;
unsupported instructions, calls, opaque operations, ordered memory, unresolved
control, and language exception handling stop recovery.

Only complete file-backed, read-only ranges without overlapping mappings or
loader fixups can supply constant image reads. Writable tables, unresolved
relocations, and a sampled runtime snapshot are not immutable-read evidence.
COPY relocations and incomplete exception metadata are conservatively refused.
The source domain requires ordinary ABI returns: every external-origin store's
target range is disjoint from the entry return-address slot. This is an explicit
caller/environment precondition, including addresses computed from external
integers; absence of frame provenance does not prove numeric disjointness. Frame-derived store
addresses must prove disjointness from it, and the original stack pointer must
be restored at return. Origin information survives spills and joins; losing an
affine expression does not turn it into an external pointer. Stack pivots,
callee-pop returns, and RET-based dispatch are currently refused.
The binary adapter enforces x64 little-endian semantics.

This produces source and IR for analysis. It does not establish relocation,
unwind, asynchronous-exception, or binary replacement safety. Patch mode rejects
this option. There is no claim that every interpreter or protection configuration
is supported.

## Current limits

Input-dependent bytecode addresses and decoder-state relationships are not
generally resolved. A dynamic branch or loop can be recovered when its dispatch
targets are proved, but ordinary branch coverage does not establish support for
every indirect decoding scheme. Unresolved control and exhausted budgets are
failures, with no recovered source or partial replacement published. Native
helper calls, exception/reentry boundaries, mutable code, and other architectures
remain outside this initial adapter's execution contract.

## Shared implementation

`SpecializationProvider` supplies complete lifted instructions and immutable
read witnesses. `NeverDInterpreterSpecialization` uses the existing `SymExec`
semantics to partially evaluate integer and control operations. The binary
adapter owns mapping and instruction decoding; it does not implement a second
instruction evaluator.

A node is identified by its native cursor, instruction mode, and selected
control-register and entry-frame-slot constants. Other byte-level facts meet by
intersection. When an incoming fact weakens, the node is evaluated again. This
keeps business loops as loops instead of expanding each observed iteration.
All leaves of an indirect target must form a bounded, exact constant set;
selected targets become explicit residual comparisons and CFG edges.

Dynamic operations and ordinary loads/stores stay in LowIR. Only certified
immutable reads become constants. Scalar constants, affine entry-frame
pointers, and proven constant frame bytes can cross nodes; other expressions
are discarded rather than expanded without bound. Frame memory uses the
existing symbolic state's conservative alias invalidation. A write through
an unknown potentially aliasing pointer invalidates conflicting facts. This
does not assume that a stack slot is private or erase it using an unproved
no-alias contract.

Unique synthetic instruction labels distinguish cloned contexts. The original
instruction boundaries remain in a separate origin map; original relocation,
exception, or jump-table certificates are not copied onto new occurrences.
The recovered LowIR enters the ordinary LowIR-to-MedIR conversion before the
HighC/LLVM route split, sharing register, stack, CFG, SSA, and ABI handling.
Recovery also requires successful MedIR verification on the HighC route.

Node, per-address context, operation, node-evaluation, and finite-target budgets
bound analysis. Budget exhaustion and unsupported semantics publish no residual
function. A complete control graph is distinct from successful source emission;
the public API checks both and reports the distinction.

## Evidence and tests

The optional local JSON report includes the input hash, selected controls,
budgets, status, work counters, residual block count, original instruction
locations, and immutable bytes used during recovery. It contains input-derived
information and is written only to the requested local path.

The public tests use original register-threaded and stack-dispatch machines,
each with arithmetic, branch/join, and runtime-loop programs. An independent
unsigned oracle checks returns, memory stores, carry/borrow results, and output
canaries. Recovered HighC and LLVMC are compiled at O0/O2 with undefined-behavior
traps and executed against that oracle. Negative cases exercise unresolved and
writable dispatch, incompatible byte order, exception metadata, and budgets.

See [testing.md](testing.md) for the focused targets.
