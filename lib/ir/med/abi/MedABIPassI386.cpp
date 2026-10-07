//===- MedABIPassI386.cpp - i386 call-ABI recovery steps ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The i386 policy of recoverCallAbi.  cdecl passes every argument on the
/// stack, and clang constant-propagates an argument the callee never reads
/// into it without storing the argument's slot (a variadic `vfn(3, &G..)`
/// whose count is folded into the callee never stores arg0).  The real stack
/// arguments then start at slot 1 with slot 0 empty, and a strict first-gap
/// cutoff would drop them all.  An ordinary call, and a forwarder whose
/// first slot is always present, keeps the strict cutoff.
///
//===----------------------------------------------------------------------===//

#include "MedABIPassDetail.h"

namespace neverd {

extern const AbiCallPolicy I386AbiCallPolicy;
const AbiCallPolicy I386AbiCallPolicy = {
    .TheArch = Arch::X86,
    .LeadingStackGapIsUnusedArgument = true,
};

} // namespace neverd
