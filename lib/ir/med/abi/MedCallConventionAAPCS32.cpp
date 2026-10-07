//===- MedCallConventionAAPCS32.cpp - ARM AAPCS call arguments -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AAPCS passes R0-R3 and then the stack.  A 64-bit argument takes an
// even-odd register pair and an 8-byte aligned stack slot, wasting the
// register or the 4-byte lane before it.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"

namespace neverd {

extern const CallArgumentConvention AAPCS32CallArguments;
const CallArgumentConvention AAPCS32CallArguments = {
    .TheArch = Arch::ARM,
    .PairAlignedWideArguments = true,
};

} // namespace neverd
