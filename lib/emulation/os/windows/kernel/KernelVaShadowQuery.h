//===- KernelVaShadowQuery.h - Declared mitigation query validation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declared mitigation query validation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELVASHADOWQUERY_H
#define NEVERD_EMULATION_KERNELVASHADOWQUERY_H

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/Limits.h"

namespace neverd::emulation::kernel_va_shadow_query {
constexpr uint32_t Version = 1;
constexpr uint32_t InformationClass = 196;
constexpr uint32_t ResultSize = 4;
constexpr uint32_t ValidFlags = 0x3fff;
llvm::Error validate(const DriverKernelVaShadowQuery &Query);
} // namespace neverd::emulation::kernel_va_shadow_query
// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::emulation::kernel_va_shadow_query {
inline constexpr size_t MaxEvidenceSize =
    ::neverd::limits::emulation::kKernelVaShadowQueryMaxEvidenceSize;
}

#endif
