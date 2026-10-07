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
// the stack-argument summary does not count.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/lift/X86Regs.h"

namespace neverd {

namespace {
/// Reading AL at entry is the variadic prologue's `test al, al`: no System V
/// parameter is passed in RAX.
bool readsVectorCount(const GPRReadWidths &Reads) {
  return Reads[x86reg::RAX / 8] != 0;
}
} // namespace

extern const CallArgumentConvention SysVX64CallArguments;
const CallArgumentConvention SysVX64CallArguments = {
    .TheArch = Arch::X64,
    .Format = BinaryFormat::ELF,
    .RegisterArgumentsFromCalleeSummary = true,
    .SummaryListsNoParameters = readsVectorCount,
    .IndirectCallsTakePrecedingSetup = true,
};

} // namespace neverd
