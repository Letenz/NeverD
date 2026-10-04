//===- X64SIMDException.cpp - Windows x64 SIMD status and parameters ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64SIMDException.h"

#include "../../../arch/x86_64/X64FPState.h"

namespace neverd::emulation::windows_exception {
std::optional<SIMDException> x64SIMDException(uint64_t MXCSR) {
  if (MXCSR & ~x64::fp::ArchitecturalMXCSRMask)
    return std::nullopt;
  const auto Active = MXCSR & ~(MXCSR >> simd::MaskShift) & simd::StatusMask;
#define NEVERD_WINDOWS_SIMD_EXCEPTION_PRIORITY(Name, Bit, Status)              \
  if (Active & Bit)                                                            \
    return SIMDException{Status, {0, MXCSR}};
#include "X64SIMDException.def"
#undef NEVERD_WINDOWS_SIMD_EXCEPTION_PRIORITY
  return std::nullopt;
}
} // namespace neverd::emulation::windows_exception
