//===- MedCallConventionWin64.cpp - Microsoft x64 call arguments ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Microsoft x64: RCX, RDX, R8 and R9, or XMM0-XMM3 in the same positions.
// The caller reserves a 32-byte home area for them below the stack
// arguments.  A variadic callee spills its variadic registers to their home
// slots, and a Control Flow Guard dispatcher (`_guard_dispatch_icall`)
// jumps to RAX with the caller's argument registers.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/lift/X86Regs.h"

namespace neverd {

extern const CallArgumentConvention Win64CallArguments;
const CallArgumentConvention Win64CallArguments = {
    .TheArch = Arch::X64,
    .Format = BinaryFormat::COFF,
    .RegisterArgumentsFromCalleeSummary = true,
    .DispatcherTargetRegister = x86reg::RAX,
    .VariadicFromSummary = true,
    .StackArgumentSummary = true,
    .PositionalArgumentSlots = true,
    .ReservedOutgoingArea = true,
};

} // namespace neverd
