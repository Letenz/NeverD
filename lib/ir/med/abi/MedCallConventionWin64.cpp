//===- MedCallConventionWin64.cpp - Microsoft x64 call arguments ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Microsoft x64: RCX, RDX, R8 and R9.  A variadic callee spills its variadic
// registers to their home slots, a Control Flow Guard dispatcher
// (`_guard_dispatch_icall`) jumps to RAX with the caller's argument
// registers, and stack arguments follow the 32-byte home area.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/lift/X86Regs.h"

namespace neverd {

extern const CallArgumentConvention Win64CallArguments;
const CallArgumentConvention Win64CallArguments = {
    /*TheArch=*/Arch::X64,
    /*Format=*/BinaryFormat::COFF,
    /*SummaryListsNoParameters=*/nullptr,
    /*DispatcherTargetRegister=*/x86reg::RAX,
    /*VariadicFromSummary=*/true,
    /*StackArgumentSummary=*/true,
    /*IndirectCallsTakePrecedingSetup=*/false,
};

} // namespace neverd
