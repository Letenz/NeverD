//===- MedCallConventionWindowsAArch64.cpp - Windows ARM64 call arguments -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Windows on ARM64 passes a routine's fixed arguments as AAPCS64 does (X0-X7,
// V0-V7, then the stack).  The setup each caller writes gives a call its
// arguments, as for AAPCS64, but an import's prototype, as a header library
// declares it, counts the integer arguments its calls pass: no register a
// call happens to hold past them is one, and a register the caller passes on
// untouched is one of them.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"

namespace neverd {

extern const CallArgumentConvention WindowsAArch64CallArguments;
const CallArgumentConvention WindowsAArch64CallArguments = {
    .TheArch = Arch::AArch64,
    .Format = BinaryFormat::COFF,
    .ImportArgumentsFromPrototype = true,
    .ArgumentsFromCallSetup = true,
};

} // namespace neverd
