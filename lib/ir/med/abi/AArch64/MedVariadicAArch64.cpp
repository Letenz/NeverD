//===- MedVariadicAArch64.cpp - AAPCS64 variadic prologue -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The AAPCS64 variadic prologue recognizer: the register save area that
/// spills most of both the GP and FP argument registers at entry.
///
//===----------------------------------------------------------------------===//

#include "../MedVariadicDetail.h"

#include <optional>

namespace neverd {
namespace med_variadic_detail {

/// Most of both argument register files spilled at entry.
bool hasAAPCS64VariadicPrologue(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  // AArch64 saves both the GP (x0-x7) and FP (q0-q7) argument registers to a
  // contiguous save area; spilling most of both register files at entry is
  // the variadic prologue's signature.  This is the AAPCS64 (Linux/ELF)
  // layout ONLY: Apple/Darwin arm64 passes EVERY variadic argument on the
  // stack and emits NO register save area (its va_start homes a single
  // overflow pointer, detected by MedVariadicDarwin.cpp).  On Mach-O this
  // save-area test would FALSE-POSITIVE on an ordinary -O0 function that merely
  // spills >=4 GP and
  // >=4 FP *named* parameters to its frame (e.g. a non-variadic
  // f(int,double,int,double,int,double,long,double)); the misclassification
  // skips detectXMMParams and silently drops every FP argument, so
  // detectVariadic uses this test only where variadic arguments travel in
  // registers; Darwin variadics use the home-slot test (MedVariadicDarwin.cpp).
  return countParamRegSpills(Func, TRI.IntParamRegs) >= kMinSaveAreaRegs &&
         countParamRegSpills(Func, TRI.FPParamRegs) >= kMinSaveAreaRegs;
}

/// The integer argument register the AAPCS64 register save area starts at:
/// va_start's prologue spills each argument register from the first unnamed
/// one through x7 to consecutive doublewords below __gr_top, wherever that is
/// in the frame.  A named parameter's own slot (-O0) lies elsewhere.
std::optional<int> aapcs64FirstVariadicRegister(VariadicScan &S) {
  return firstSavedArgumentRegister(S, std::nullopt);
}

} // namespace med_variadic_detail
} // namespace neverd
