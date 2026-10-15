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

namespace neverd::emulation::kernel_va_shadow_query {
constexpr uint32_t Version = 1;
constexpr uint32_t InformationClass = 196;
constexpr uint32_t ResultSize = 4;
constexpr uint32_t ValidFlags = 0x3fff;
constexpr size_t MaxEvidenceSize = 256;
llvm::Error validate(const DriverKernelVaShadowQuery &Query);
} // namespace neverd::emulation::kernel_va_shadow_query
#endif
