# Stack frame corrections

In a native x64 or AArch64 function, **Edit → Stack variables** (`Ctrl+K`)
lists observed stack accesses by their signed offset from SP at function entry.
Enter a name and, optionally, an exact-width integer or pointer C type. An empty
name leaves the slot automatic. The same record names its disassembly operand
and both C representations. `K` starts from accesses at the current instruction.

**Edit → Change stack pointer** (`Alt+K`) records the entry-relative SP after
the selected instruction. When SP before the instruction is known, the dialog
also accepts the net SP change made by that instruction. A correction can
therefore recover a known SP after a dynamic adjustment; unknown is never zero.
It does not move any machine instruction or change the loaded bytes.

Corrections are analyst interpretations for source analysis. LowIR, ordinary
MedIR/LLVM views, lifting and patching keep the original machine semantics.
The private source variant has a marker rejected by ordinary LLVM emission.
When a physical stack object is named, LLVM C keeps that function's frame
storage and pauses its LLVM optimizations so scalar replacement cannot discard
the selected object. Reset restores automatic source analysis and optimization.

SP assertions require a retained non-branch instruction and a single ordinary
entry. Independently rooted and exceptional graphs are refused rather than
given an invented dominating entry SP. Corrections currently support native
64-bit x86 and AArch64; i386 callee-pop integration is not qualified. All
offsets are bounded to one MiB and queries have block/operation/work limits.

Manual variables bind observed negative entry-SP offsets with the exact access
width. They cannot redirect an unknown address, redefine the return address or
change the parameter ABI. Overlapping accesses retain shared backing storage;
an escaped, ambiguous or optimized object without independent storage in both
C routes is refused. Larger aggregates and floating-point reinterpretation
are not supported by this operation. Existing source type actions remain
available for their separately supported cases.

The input image digest and graph snapshot bind saved policies to the original
analysis. Graph identity includes ordinary/exception roots, edges, instruction
modes and function temporary lifetimes. Obsolete policies can be inspected and
reset. Edits use project history, replicas and the existing database/sidecar
ownership; rejected changes publish no history or partial policy.

Regression coverage: `NeverDSourceStackFrameTests`, `NeverDWorkerStackFrame`
and `SourceVariableTypesTests::stackFrameDialogs`. The worker regression checks
both C routes, compiles and executes their C with an independent call-count
oracle, checks listing names, native isolation, rejected edits, undo/redo,
reopen and a read-only replica.
