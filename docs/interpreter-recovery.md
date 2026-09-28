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
select a bytecode cursor whose value is established by the entry stub. A runtime
input used as a counter must remain dynamic. Missing context separation can
cause recovery to stop when distinct cursor values meet; the engine must not
guess a dispatch target to compensate. For a spilled cursor,
`--vm-control-stack=-16:8` selects eight bytes at entry-RSP minus 16. The offset
is relative to the function entry, not the current adjusted stack pointer.

The C API is `neverd_devirtualize_source_v1()` in
`neverd/sdk/NeverDCAPIDevirtualize.h`. It runs a separate transaction without
modifying the session's ordinary decompilation cache. Failure returns no source
and can still return a JSON diagnostic. Both owned strings use
`neverd_free_string()`.

## Default execution contract

The binary adapter currently accepts linked x64 ELF and PE images at their
mapped addresses. Mappings, bytes, and permissions must remain fixed, with no
concurrent mutation. Strict on-demand lifting follows reachable machine code;
unsupported instructions, calls, opaque operations outside the flag forms
described below, ordered memory, unresolved control, and language exception
handling stop recovery.

PE recovery requires full image metadata, including image-wide relocations and
exception records. The CLI loads that metadata before applying `--func`; C API
callers must not first restrict the session with
`neverd_session_restrict_function()`. The binary adapter rejects images loaded
with a restricted function work-set because omitted metadata cannot prove the
absence of fixups or exceptional edges.

Only complete file-backed, read-only ranges without overlapping mappings or
loader fixups can supply constant image reads. Writable tables, unresolved
relocations, and a sampled runtime snapshot are not immutable-read evidence.
COPY relocations and structurally incomplete exception directories are refused.
A PE can contain an undecoded handler in an unrelated function when the
directory and function ranges are complete; reaching that handler still stops
recovery.
The ordinary source ABI reconstructs invocation-private frame storage. Every
external-origin LOAD/STORE range, including addresses computed from external
integers, must be disjoint from both the native private frame and its
reconstructed source storage. This is an explicit environment
precondition for source-frame relocation, not an alias fact inferred from a run.
The shared frame proof rejects escaping frame addresses, frame-dependent
scalar outputs and branches, and reads of uninitialized private bytes. The
explicit machine-state ABI retains original guest addresses and does not use
this private-frame precondition.

The source domain requires ordinary ABI returns: every external-origin store's
target range is disjoint from the entry return-address slot. This is an explicit
caller/environment precondition, including addresses computed from external
integers; absence of frame provenance does not prove numeric disjointness.
Frame-derived store addresses must prove disjointness from it, and the original
stack pointer must be restored at return. Origin information survives spills and
joins; losing an affine expression does not turn it into an external pointer.
Stack pivots, callee-pop returns, and RET-based dispatch are currently refused.
The binary adapter enforces x64 little-endian semantics.

This produces source and IR for analysis. It does not establish relocation,
unwind, asynchronous-exception, or binary replacement safety. Patch mode rejects
this option. There is no claim that every interpreter or protection
configuration is supported.

The exact x64 `PUSHFQ`/`POPFQ` forms remain in the residual program. Analysis treats each machine flag snapshot as an unknown runtime value; the lifter merges its separately modelled arithmetic flags. Restoring flags remains a runtime effect. A flag-derived address cannot borrow the external-pointer return-slot contract, and an unbounded flag-derived dispatch still fails.

Before a full flag snapshot, every modelled arithmetic or direction flag must have a definition within the recovered function. Any direct flag read also needs a definition on every reachable predecessor, even when symbolic simplification cancels its value. Otherwise recovery refuses instead of emitting an unknown-register trap in C.

LowIR temporaries are local to one lifted native instruction. Every byte read must have been defined earlier in that instruction; a reused offset from a prior instruction or an algebraically cancelled undefined value is not source evidence. Entry constants may bind physical registers only.

## Explicit machine-state recovery

```sh
neverd decompile program --func entry --devirtualize --vm-machine-state \
  --recovery-report machine-report.json -o machine.c
```

The separate C entry point is `neverd_devirtualize_machine_source_v1()`.
This opt-in source ABI takes a pointer to 17 aligned `uint64_t` words:
RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8 through R15, then RFLAGS.
Only architecturally defined flag observations are equivalence obligations;
undefined flags are not evidence of a particular processor's behavior.
Programs that feed undefined flags into control flow, addresses, or otherwise
defined outputs need a separate noninterference proof. The current recovery
report does not provide that proof or certify such processor-dependent behavior.
It returns an unsigned 64-bit status. Only zero certifies successful execution;
nonzero means the flag profile was violated and does not undo memory effects.
The output state is captured immediately before the final native RET pops its
return address. State storage must be disjoint from all guest memory. Guest
addresses use the original fixed mappings on a little-endian 64-bit host.
The entry return-address slot must remain disjoint from all external-origin
STORE ranges, including computed addresses, as in the default return contract.

The profile is 64-bit user mode at CPL3/IOPL0, shadow stacks disabled, normal
nonfaulting execution without asynchronous events. Entry flag images must be
canonical with TF, RF, VM, AC, VIF and VIP clear; executed POPFQ images must keep TF and
AC clear. Generated source checks these flag conditions. PUSHFQ/POPFQ use the
explicit guest state rather than the compiler's live flags. IF, IOPL and
reserved bits follow the user-mode preservation rules. RDSSP preserves its
destination when shadow stacks are disabled; reached INCSSP is unsupported.
These rules follow the [Intel instruction reference](https://cdrdv2-public.intel.com/671110/325383-sdm-vol-2abcd.pdf).

The adapter certifies direct near CALL and ordinary near RET semantics.
Internal calls retain the actual guest return-address store; internal returns
require a proved singleton target and retain the read and stack increment.
Restoring the entry stack can leave an internal call without returning to its
continuation. Callee-pop returns, unresolved return targets and arbitrary stack
pivots remain unsupported. Exact frame pointers survive complete spills, with
partial or potentially aliasing writes invalidating the facts. Active stack
positions and return-slot values separate contexts and are budgeted.

Exception handler metadata may be traversed only under this explicit normal
execution profile; exception dispatch and unwind equivalence are not certified.
The report records `sourceABI` and `executionProfile`. The default API retains
its stricter call, entry-flag and exception rejection. The machine-state wrapper
remaps guest registers and uses the existing LowIR/MedIR/HighC/LLVMC scalar
pipeline. It does not introduce a second instruction evaluator.

## Current limits

Input-dependent bytecode addresses and decoder-state relationships are supported
only when the required finite domains and correlations can be proved within the
configured limits. This does not establish support for arbitrary indirect
decoding schemes. Dynamic branches and loops may be recovered when every
dispatch target is proved; ordinary branch coverage is not evidence for all such
schemes. Unresolved control and exhausted required proof budgets are failures,
with no recovered source or partial replacement published. External calls, exception/reentry execution, mutable code, and other
architectures remain unsupported. The default source ABI also refuses native
helper calls; the explicit machine-state profile covers only the forms above.

## Shared implementation

`SpecializationProvider` supplies complete lifted instructions and immutable
read witnesses. `NeverDInterpreterSpecialization` uses the existing `SymExec`
semantics to partially evaluate integer and control operations. The binary
adapter owns mapping and instruction decoding; it does not implement a second
instruction evaluator. The narrow flag-snapshot rule in the specializer is an
overapproximation, not an evaluator for the machine's system flags.

For a finite symbolic read address, the built-in bitvector solver enumerates
candidate addresses under the current constraints. The set is accepted only
after a final UNSAT result proves that no other address is possible, and every
address must have a complete immutable, nonfaulting-read certificate. Such a
certified LOAD can be replaced in LowIR by capturing its address and selecting
the exact value with a SELECT chain; ordinary uncertified loads remain dynamic.
A sampled address is never a substitute for the full set. Selected control
registers and entry-frame slots can retain bounded joint tuples across nodes,
preserving relationships such as a cursor and its decoder key. Joins and
widening remain conservative. Partial SAT samples or unknown solver results are
not proofs of a complete address or target set. This mechanism does not require
the optional Z3 backend.

A node is identified by its native cursor, instruction mode, and selected
control-register and entry-frame-slot constants. Other byte-level facts meet by
intersection. When an incoming fact weakens, the node is evaluated again. This
keeps business loops as loops instead of expanding each observed iteration. All
reachable values of an indirect target must belong to a bounded, exhaustively
proved set; selected targets become explicit residual comparisons and CFG edges.

Dynamic operations and ordinary loads/stores stay in LowIR. Scalar constants,
affine entry-frame pointers, and proven constant frame bytes can cross nodes;
other expressions are discarded rather than expanded without bound. Frame memory
uses the existing symbolic state's conservative alias invalidation. A write
through an unknown potentially aliasing pointer invalidates conflicting facts.
This does not assume that a stack slot is private or erase it using an unproved
no-alias contract.

Unique synthetic instruction labels distinguish cloned contexts. The original
instruction boundaries remain in a separate origin map; original relocation,
exception, or jump-table certificates are not copied onto new occurrences. The
recovered LowIR enters the ordinary LowIR-to-MedIR conversion before the
HighC/LLVM route split, sharing register, stack, CFG, SSA, and ABI handling.
Recovery also requires successful MedIR verification on the HighC route.

Node, per-address context, operation, node-evaluation, and finite-target budgets
bound analysis. Budget exhaustion and unsupported semantics publish no residual
function. A complete control graph is distinct from successful source emission;
the public API checks both and reports the distinction.

Finite read-address sets, joint control tuples and control-field counts also
have explicit limits. Global solver-query limits and per-query limits on gates,
conflicts, propagations and watched-literal visits bound proof work;
symbolic-node limits bound expression growth. The JSON report includes these
budgets together with `solverQueries` and `relationalWidenings`.

## Evidence and tests

The optional local JSON report includes the input hash, selected controls,
budgets, status, work counters, residual block count, original instruction
locations, and immutable bytes used during recovery. It contains input-derived
information and is written only to the requested local path.

The public tests use original register-threaded and stack-dispatch machines,
each with arithmetic, branch/join, and runtime-loop programs. An independent
unsigned oracle checks returns, memory stores, carry/borrow results, and output
canaries. Recovered HighC and LLVMC are compiled at O0/O2 with
undefined-behavior traps and executed against that oracle. Negative cases
exercise unresolved and writable dispatch, incompatible byte order, exception
metadata, and budgets.

Additional original finite-address fixtures use input-selected read-only
records, related cursor/key control fields and a shared handler at different
virtual positions. They cover branch/join behavior and loops whose record choice
depends on live program state. Their independent native oracle uses both SysV
and Win64 calling conventions; both recovered C routes are checked at O0/O2 with
undefined-behavior traps and output canaries. Missing read certificates and
insufficient proof budgets must not publish a partial result.

See [testing.md](testing.md) for the focused targets.
