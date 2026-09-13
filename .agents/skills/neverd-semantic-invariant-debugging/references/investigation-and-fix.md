# Investigation And Fix

## Failure Model

This class of bug commonly has the following shape:

1. Two or more IR values represent aliases of the same machine-level fact.
2. Different recovery paths choose between those values using different rules.
3. The observed result depends on PHI insertion order, block layout, traversal
   order, binary format, or a recent hardening check.
4. A local fix corrects one path while a sibling path retains the old rule.

Examples include:

- a narrow and a full-width register PHI for one ABI argument slot;
- entry-seeded data competing with a wider value that is undef on the first
  loop iteration;
- pointer provenance recovered differently by direct, cross-block, and
  predecessor scans;
- one loader exposing relocation metadata that another loader drops; and
- the same semantic decision being repeated in an emitter and a helper.

Treat a verifier or ambiguity diagnostic as evidence. Do not silence it until
the upstream semantic conflict has been understood.

## 1. Reproduce at the Smallest Faithful Boundary

Preserve the reporter's input and command line. Record:

- commit SHA;
- architecture and binary format;
- exact NeverD command and stage;
- complete diagnostic;
- relevant disassembly;
- MedIR around the failing block; and
- emitted LLVM IR when emission succeeds.

Use the narrowest stage that still exposes the defect:

```bash
build/bin/neverd lift --dump-med <binary> -o /tmp/repro.ll
build/bin/neverd lift --no-opt <binary> -o /tmp/repro.ll
```

Add decompile, patch, recompilation, or execution only when the failure crosses
those boundaries. Do not replace a faithful linked-binary reproducer with an
object file when imports, relocations, section permissions, or ABI metadata are
part of the behavior.

Before changing production code, state one falsifiable hypothesis. For example:

> Cross-block argument recovery selects the first PHI for an ABI slot, so a
> narrow alias wins only because it was inserted before the full-width alias.

## 2. Locate Every Owner of the Semantic Decision

Name the question independently of the current function. Examples:

- "Which PHI is authoritative for ABI argument slot N?"
- "Does this address have pointer provenance?"
- "Which relocation owns this stored value?"
- "Is this frame reload an address or a scalar?"

Search for every place that answers that question. Include:

- direct scans and CFG-backward scans;
- current-block, predecessor-block, and loop-header handling;
- optimized and unoptimized emitters;
- loader-specific metadata producers;
- architecture-specific helpers; and
- fallback or stale-address paths.

Use history to distinguish a new regression from an old inconsistency:

```bash
rg -n '<selector|walker|diagnostic|fallback>' lib include unittests
git log -S'<relevant rule or diagnostic>' -- <paths>
git blame -L <start>,<end> <file>
```

Build a small decision table. If equivalent callers use rules such as
"first match", "widest", and "entry-seeded then widest", the duplicated policy
is the root cause. Changing only the caller reached by the report is not enough.

## 3. Write RED Tests for the Policy, Not Only the Fixture

Before the fix, add a focused test that fails for the hypothesized reason.
Exercise both the reported ordering and its semantic counterexample.

For register or PHI alias selection, cover at least:

| Case | Expected choice |
|---|---|
| Narrow alias appears before an equally seeded wide alias | Wide alias |
| Wide alias appears before an equally seeded narrow alias | Wide alias |
| Wide alias is undef on an entry edge; narrow alias is seeded | Narrow alias |
| PHI predecessor is missing or invalid | Do not treat it as safely seeded |
| Constant incoming value on an entry edge | Treat it as defined |

The first two cases reject insertion-order behavior. The third prevents an
overcorrection such as "always choose widest".

Test the policy below file-format handling when the invariant is format-neutral.
Then retain an exact end-to-end fixture for the reported format so loader,
relocation, ABI, and emission integration remain covered.

## 4. Fix the Authoritative Layer

Create one named helper or model operation for the semantic decision and route
all equivalent callers through it. Remove duplicated local implementations.

For ABI register-PHI aliases, the intended precedence is:

1. reject candidates that do not represent the requested ABI slot;
2. prefer a candidate genuinely defined on every function-entry edge;
3. among equally safe candidates, prefer the widest register view; and
4. never use container order as semantic priority.

This precedence is specific to alias selection. Do not generalize it blindly to
unrelated PHIs or data-flow merges.

Preserve conservative behavior:

- a missing predecessor is not proof of a seeded value;
- a loop-backedge definition does not make the first iteration defined;
- a numeric address alone is not pointer provenance;
- a full-width value is not automatically safe when its entry value is undef;
  and
- an ambiguity diagnostic must not be replaced with a stale-address fallback.

If the decision is made after decoding into shared MedIR, prefer a
format-neutral fix. Use a loader-specific fix only when evidence shows that the
formats produce different or incomplete metadata before the shared boundary.

### Re-materialized immutable table bases

When a nested PHI combines an exact recurrent arm with a reset arm that
re-materializes an immutable table base, read
[rematerialized-table-base.md](rematerialized-table-base.md)
before changing recurrence or provenance logic. It defines the split proof,
initializer anchoring, fail-closed boundaries, and regression matrix.

### Keep discovery separate from semantic ownership

A broad DAG walk should collect candidate evidence; it should not make a
context-sensitive ownership decision that only the consuming operation can
prove. In particular, pointer-table discovery may report a unique segment and
whether every reachable leaf was understood, but the table-load owner must
still prove:

- that the recovered value is used in the expected base/index role;
- that every feasible PHI initializer and recurrent arm is accounted for;
- that widths and zero-extension behavior preserve the exact virtual address;
- that all accepted leaves use one compatible raw-or-symbolized model; and
- whether the result needs one raw-address rebase or can use the symbolized
  value directly.

Keep conservative discovery callers raw-only by default. Permit symbolized
evidence only for a caller that subsequently performs the complete role and
model proof. Otherwise an early "already symbolized" rejection can suppress
the only owner capable of validating a legitimate mixed-use induction PHI.

Use the emitter's actual value transformation as the width ground truth. A
direct narrow constant followed only by zero-extension may identify a
pointer-width table base when the address is exact, mapped, accepted by the
authoritative materializable-address classifier, and fits completely in the
narrow width. Reject truncation, arithmetic, sub-byte extraction, a second
width change, and narrow symbolization: a relocated narrow pointer can lose
high bits after relinking. Remember that PHI constants may bypass `getVar`
while operation operands pass through it, so equal numeric leaves can acquire
different provenance at different use sites.

### Make semantic caches transactional and re-entrancy-safe

A cache owner such as `CacheFor == CurFunc` proves only which function the
storage belongs to; it does not prove that construction finished. When cache
construction can call a helper that transitively consults the same cache, use
an explicit lifecycle such as `Empty -> Building -> Ready`.

- Build into private scratch containers and publish them only after the full
  computation completes.
- During `Building`, return a conservative non-cacheable result. Never memoize
  a terminal negative answer from partial state.
- If re-entry can affect decisions already made in the current build, set a
  sticky marker and rebuild from a conservative structural model before
  publication. Merely clearing a memo after publication cannot repair a graph
  that was already pruned from provisional evidence.
- Invalidate every transitive dependent memo at generation boundaries. Reset
  both contents and owner/generation fields; clearing only the container can
  turn later queries into permanent false negatives.
- Keep any predicate used while constructing reachability cycle-free. A
  control-folding predicate may be a conservative upper bound for
  relocatability, but it must not call PHI recurrence, indexed-base, induction,
  or feasibility analysis that depends on the graph being built.
- Assert that speculative/cache-building queries do not emit or clear fatal
  diagnostics. They may lose precision, never mutate the final safety state.

Test the cold first query and both call orders. A warm-cache test can miss the
entire defect. Lock down all three edge classes: a completed live edge is
proven feasible, a truly pruned structural edge is infeasible, and a malformed
non-edge remains unknown. Also poison a dependent memo during `Building` and
prove publication invalidates and recomputes it.

When a new semantic field is added to IR identity (for example occurrence-level
address provenance), include it in equality, every semantic memo key,
serialized or translation-cache hashes, and the relevant cache/schema version.
Otherwise old artifacts or same-value occurrences can silently reuse decisions
computed under a different semantic model.

Numeric equality is not address provenance. The same bit pattern can be a
scalar, an incomplete address-forming fragment, or a complete address in
different operands. Preserve occurrence provenance through bit-preserving
transports, prevent narrowing/widening from manufacturing a complete pointer,
and make observable escape checks fail closed for an unresolved fragment.

### Propagate fragment taint by semantic value flow

Propagate relocation taint along semantic value-flow edges, not every operand
use. A load address does not taint the loaded value, and a call target or
argument does not taint the return value. A `SELECT` condition likewise is not
value provenance for either arm, but it is observable control: reject an
unresolved fragment used as that condition, just as for a conditional branch.

Conversely, a fragment spilled into a canonical nonescaping frame slot must
taint a proven reload. The spill exemption is valid only when every potentially
aliasing read has a complete all-path reaching-store proof. An uninitialized
path, uncanonical frame root, partially overlapping read, or atomic read must
cancel the exemption.

Let the memory or code owner first try to complete a fragment in its operand
role. If that owner cannot prove reconstruction, reject the fragment immediately
before the raw fallback. This keeps legal PC-relative memory forms working
without allowing a page fragment to escape through a return, stored value, call
argument, branch or selection condition, or indirect target.

### Classify intrinsic operands by role

Classify intrinsic operands by role rather than treating the entire operation
uniformly. For atomic intrinsics, stored, expected, and desired values are
observable value sinks; the address belongs to the shared relocation-aware
memory owner; and width, ordering, and intrinsic-ID fields are metadata. Loaded
old-value and status outputs do not inherit address taint.

Keep one address-index policy shared by the taint audit and the emitter. Generic
and architecture-specific atomics must use the same global, writable, and raw
fallback resolver so equivalent memory operands cannot acquire different
relocation behavior.

### Keep exact addresses role-neutral until consumption

A role-neutral exact address may need different materialization at different
uses. Generic emission must not eagerly turn every executable address into a
`BlockAddress` merely because it lies in `.text`; switch and literal
arithmetic may need its numeric form. A data-memory owner applies the
corresponding data policy. A code sink such as `INDIR_CALL` owns the callable
role: converge every feasible PHI or `SELECT` arm to one normalized lifted
function entry and materialize that function there, otherwise fail closed.

This use-role split prevents both stale raw targets and premature code/data
reinterpretation. Do not infer a code-identity role from executable-segment
membership alone: architectural PC values used by switch lowering are
layout-dependent numerics, not automatically function pointers.

Do not infer code identity from a mixed symbol-name map either. Loader import
maps include data bind/IAT slots such as RTTI vtable fields and Objective-C
class references; a name at an address is not proof that the address denotes a
callable symbol. Require explicit code provenance or an actually materializable
lifted `Function`/`BlockAddress`. An exact format-native section must also beat
coarse segment permissions: Mach-O uses instruction attributes, while ELF/COFF
use the section executable flag. Thus an RX `__TEXT` header/alignment gap or
non-executable `.rodata` sharing an RX load map with `.text` is not code. Use
segment-level fallback only when the current mapped segment has no overlapping
mapped/readable section metadata. Keep this check segment-local: packed/partial images
may describe one data segment while leaving a different executable segment
section-less. Once the current segment has section ownership, an uncovered
header/gap has no section code owner.

Separate stable ownership from late materialization. The structural
upper-bound may trust a lifted function entry or a loader-authenticated import
veneer, but it must not depend on whether an earlier CALL happened to create an
LLVM declaration. Cache only that stable owner fact. The precise consumer may
then materialize the current `Function`, or fail closed if no declaration is
available; never cache a transient module-order-dependent `false`. Treat an
authenticated but unresolved entry as strong relocation evidence so an
unsupported transform cannot silently escape as raw arithmetic.

Re-authenticate the same owner after a patch pipeline reloads the input image.
Cached discovery sets such as `CodeRefTargets` are enrichment, not the only
proof that a synthetic original-VA symbol denotes a function. A fresh loader
may instead prove it through a typed function symbol, known-code range, export
with a section code owner, or explicit import veneer. This distinction is
observable on ARM/Thumb, where serializing a code pointer can change its low
mode bit even though x64/AArch64 address serialization is numerically inert.
In particular, when patch symbolization reuses an absolute-address symbol
spelling for a defined function's original identity, every Mach-O, ELF, COFF,
and in-place resolver must recover the typed function owner before deciding
whether to serialize that address as code.

Apply the same format-aware code classifier at every producer. Absolute
relocations and relocation-free RIP-relative LEA discovery must agree with the
consumer about Mach-O section instruction attributes; otherwise a header,
alignment gap, or `__cstring` address can re-enter the pipeline as a strong
code target even after the emitter-side classifier was fixed.

Heuristic post-load discovery is another producer, not independent proof.
Import-thunk byte patterns, padding/prologue scans, and data function-pointer
scans may only publish a function symbol or stub after the candidate entry has
one exact section code owner for every byte the recognizer consumes. Checking
only the first instruction lets a pattern start at the tail of `.text` and
finish in adjacent data. Otherwise executable-segment data can be promoted into
a typed function symbol, which later stages will reasonably—but incorrectly—
treat as authenticated identity.

Treat exports as names, not automatic function ownership. Mach-O export tries
and ELF/COFF symbol surfaces can name data in a coarse RX mapping, including
bytes that happen to decode as a valid return sequence. Function discovery and
patch-time source-function authentication must require the same format-aware
section code owner before trusting an export; otherwise detection can
manufacture a function from data and the patcher can overwrite it with a
trampoline.

An import's legacy `IATAddr` is likewise not structural stub proof: dyld bind
metadata can place a data pointer slot in an executable segment. Prefer exact
loader-recorded stub indices/ranges. If compatibility requires treating an
`IATAddr` as a veneer, limit that fallback to the legacy format that needs it
and require the same section-aware code classification; section-less images may
retain the documented segment fallback. PE/ELF IAT or GOT slots remain data
unless their loaders explicitly register a thunk/stub owner.

### Keep source replacement transactional

A lifted source definition is replaceable only when its original entry can
host the target trampoline without crossing a code-owner boundary or another
authenticated entry. If it cannot, externalize that definition before final
code generation so generated callers continue to resolve the original body
and no duplicate unwind record or second implementation is emitted. Merely
skipping its trampoline after compiling a copy creates split direct/indirect
call semantics. An exact replacement plan with zero eligible definitions is a
successful no-op; it must never fall back to legacy name-based trampoline
installation. For the replaceable subset, require one authenticated source
identity, one installed trampoline receipt, and one matching generated owner.

Preflight preserved definitions before calling `Function::deleteBody()`.
LLVM rewrites a still-live `blockaddress` whose BasicBlock is destroyed to an
`inttoptr(1)` sentinel, so a label address that escapes through another emitted
function, a global initializer, or metadata would otherwise become a silent
miscompile. Walk the complete constant-user DAG before mutating any body and
fail closed unless every terminal instruction belongs to another definition
being externalized in the same transaction.

Target-specific lowering exceptions used only by final-image rewriting need an
explicit backend-mode bit in addition to a tightly scoped IR marker and symbol
shape. Metadata alone is forgeable and cannot authorize bypassing the normal
object ABI. Verify address materialization after sliding the emitted bytes, not
only at the preferred base, so an absolute constant cannot masquerade as a
relocation-aware relation.

### Preserve observable code identity

Code identity is observable when carried as an integer value, not only when
called. Route return values, escaping stores, every call argument (including
integer-class and variadic arguments), atomic stored/expected/desired values,
and pointer-identity arithmetic or comparisons through the same
occurrence-aware code-identity owner.

An ordinary observable sink may preserve either an emitted
`llvm::Function` identity or an interior `BlockAddress` identity when the
origin proves that exact target. In contrast, `INDIR_CALL` must resolve to an
`llvm::Function` entry; an interior `BlockAddress` is valid for
computed-goto or pointer-table identity, but it is not callable.

For an explicitly supported bit or flag transform, relocate each genuine code
leaf in its own operand before applying the operation. Do not replace the whole
expression with one symbol, and do not leave any participating code leaf as a
raw original VA. A dynamic scalar offset, mask, flag, or comparison operand in
such a supported relation is not an alternative code identity. Distinguish it
from a reachable scalar arm of a PHI or `SELECT`: that merge can produce a
non-code value at runtime and must not be accepted as one exact code identity.
An unsupported transform that contains a code leaf must fail closed when its
result reaches an observable sink.

For a PHI or `SELECT` that combines an exact code identity with a supported
adjusted relation, materialize the exact arm at the merge edge and keep the
adjusted arm as its current SSA value. Do not collapse an arithmetic recurrence
such as `p = PHI(&f, p + stride)` back to `&f`; only a bit-preserving self-arm
may be treated as one unchanged identity. A nullable zero arm is a valid
ordinary code-value merge, while a nonzero scalar arm remains mixed and must
fail closed unless a more specific owner proves its semantics.

When proving a merged code identity, ignore proven-infeasible incoming edges and
proven recurrent self-arms, then require every remaining initialization arm to
converge to one emitted identity of the required kind.

### Keep all-address merges with their structural owner

An explicit-address summary is not permission to bypass a structural owner.
For an all-address PHI or `SELECT`, run the complete all-arm
raw-versus-symbolized model audit before the generic "already materialized"
guard. The guard prevents value-global reclassification; it must not suppress
the only owner capable of detecting mixed models or rebuilding a canonical
merged pointer.
