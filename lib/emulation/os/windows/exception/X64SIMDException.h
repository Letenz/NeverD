//===- X64SIMDException.h - Windows status for an authenticated #XM -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_X64SIMDEXCEPTION_H
#define NEVERD_EMULATION_WINDOWS_X64SIMDEXCEPTION_H

#include <array>
#include <cstdint>
#include <optional>

namespace neverd::emulation::windows_exception {
namespace simd {
#define NEVERD_WINDOWS_SIMD_EXCEPTION_VALUE(Name, Value)                       \
  inline constexpr uint64_t Name = Value;
#include "X64SIMDException.def"
#undef NEVERD_WINDOWS_SIMD_EXCEPTION_VALUE
} // namespace simd
struct SIMDException {
  uint32_t Code;
  std::array<uint64_t, simd::ParameterCount> Parameters;
};
/// Classify retained MXCSR only after the CPU reports a real SIMD fault.
/// Sticky status alone never establishes that an instruction faulted.
/// Invalid controls or a state with no active exception remain unclassified.
std::optional<SIMDException> x64SIMDException(uint64_t MXCSR);
} // namespace neverd::emulation::windows_exception

#endif
