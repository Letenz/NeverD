//===- FloatingPointContract.h - C floating-point rounding -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_FLOATINGPOINTCONTRACT_H
#define NEVERD_BACKEND_C_FLOATINGPOINTCONTRACT_H

#include "llvm/Support/raw_ostream.h"

namespace neverd::c_float {

/// Distinct source operations retain their distinct rounding. An explicit FMA
/// still uses its fused builtin. Both C projections publish the same policy;
/// it must also apply when the caller supplies its own includes.
// Consumers recognize this complete directive-only block without evaluating
// arbitrary preprocessor branches. Both emitters use the same spelling.
inline constexpr char ContractionPolicy[] =
    "#if defined(_MSC_VER) && !defined(__clang__)\n"
    "#pragma fp_contract(off)\n"
    "#elif defined(__GNUC__) && !defined(__clang__)\n"
    "#pragma GCC optimize (\"fp-contract=off\")\n"
    "#else\n"
    "#pragma STDC FP_CONTRACT OFF\n"
    "#endif\n\n";
inline void writeContractionPolicy(llvm::raw_ostream &OS) {
  OS << ContractionPolicy;
}

} // namespace neverd::c_float

#endif
