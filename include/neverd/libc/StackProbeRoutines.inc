//===- StackProbeRoutines.inc - Compiler stack-probe helpers ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// STACK_PROBE(Format, Arch, Symbol, Effect)
//
// Helpers a compiler calls before it allocates a large frame or an alloca,
// by binary format, architecture and symbol name.  The size travels in a
// register outside the calling convention (RAX/EAX, x15 on ARM64).
//
//   Probe     touches each page between the stack pointer and the new frame
//             bottom and returns with every general register and the stack
//             pointer as it found them; the caller then moves the stack
//             pointer itself.  The call changes nothing the program observes.
//   Allocate  also moves the stack pointer by the size before it returns.
//
//===----------------------------------------------------------------------===//

#ifndef STACK_PROBE
#error "define STACK_PROBE(Format, Arch, Symbol, Effect) before including"
#endif

// MSVC
STACK_PROBE(COFF, X64, "__chkstk", Probe)
STACK_PROBE(COFF, AArch64, "__chkstk", Probe)
STACK_PROBE(COFF, X86, "__chkstk", Allocate)
STACK_PROBE(COFF, X86, "__alloca_probe", Allocate)
STACK_PROBE(COFF, X86, "__alloca_probe_8", Allocate)
STACK_PROBE(COFF, X86, "__alloca_probe_16", Allocate)

// MinGW-w64 (libgcc)
STACK_PROBE(COFF, X64, "___chkstk_ms", Probe)
STACK_PROBE(COFF, X86, "___chkstk_ms", Probe)
STACK_PROBE(COFF, X64, "___chkstk", Allocate)
STACK_PROBE(COFF, X86, "___chkstk", Allocate)
STACK_PROBE(COFF, X64, "__alloca", Allocate)
STACK_PROBE(COFF, X86, "__alloca", Allocate)

#undef STACK_PROBE
