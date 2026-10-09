//===- MedCallConventionSysV.cpp - System V x86-64 call arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// System V x86-64: RDI, RSI, RDX, RCX, R8 and R9.  A variadic callee tests
// AL (the vector registers its caller used) and spills every argument
// register to its register save area, so its summary lists no parameters.
// Its stack arguments start at the return address with no home area, which
// the stack-argument summary does not count.  RAX, R10 and R11 hold nothing
// at entry, so a function that only aligns the stack for a call with a push
// of RAX passes no stack argument.  A libc import with a fixed prototype
// reads exactly its parameters, through its PLT stub or its GOT slot alike.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/lift/X86Regs.h"

namespace neverd {

namespace {
/// Reading exactly AL at entry is the variadic prologue's `test al, al`: no
/// System V parameter is passed in RAX.  A wider read is no such test: an
/// alignment `push rax` before a call reads the whole register, and
/// `sete al; mov r14d, eax` copies bytes of RAX no caller set.
bool readsVectorCount(const GPRReadWidths &Reads) {
  return Reads[x86reg::RAX / 8] == 1;
}
} // namespace

extern const CallArgumentConvention SysVX64CallArguments;
const CallArgumentConvention SysVX64CallArguments = {
    .TheArch = Arch::X64,
    .Format = BinaryFormat::ELF,
    .RegisterArgumentsFromCalleeSummary = true,
    .VectorArgumentsFromCalleeSummary = true,
    .ImportArgumentsFromPrototype = true,
    .SummaryListsNoParameters = readsVectorCount,
    .UndefinedIncomingScratchRegisters = true,
    .IndirectCallsTakePrecedingSetup = true,
    .FormattedCallArguments = true,
};

} // namespace neverd
