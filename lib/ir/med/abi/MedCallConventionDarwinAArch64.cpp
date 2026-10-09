//===- MedCallConventionDarwinAArch64.cpp - Apple arm64 call arguments ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Apple's arm64 convention follows AAPCS64 for fixed arguments (X0-X7,
// V0-V7), but passes every variadic argument on the stack, and packs stack
// arguments narrower than eight bytes at their natural alignment.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"

namespace neverd {

extern const CallArgumentConvention DarwinAArch64CallArguments;
const CallArgumentConvention DarwinAArch64CallArguments = {
    .TheArch = Arch::AArch64,
    .Format = BinaryFormat::MachO,
    .VariadicArgumentsOnStack = true,
    .ArgumentsFromCallSetup = true,
    .TargetSpillsSurviveLeafCalls = true,
};

} // namespace neverd
