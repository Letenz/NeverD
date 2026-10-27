//===- KernelVersionQuery.h - Declared base version query
//-------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared bounds and validation for the version query contract.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELVERSIONQUERY_H
#define NEVERD_EMULATION_KERNELVERSIONQUERY_H

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/Limits.h"

namespace neverd::emulation::kernel_version_query {
constexpr uint32_t Version = 1;
constexpr uint32_t BaseSize = 276;
constexpr uint32_t FieldsEnd = 20;
constexpr uint32_t MaxBuild = UINT16_MAX;
llvm::Error validate(const DriverVersionQuery &Query);
} // namespace neverd::emulation::kernel_version_query
// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::emulation::kernel_version_query {
inline constexpr size_t MaxEvidenceSize =
    ::neverd::limits::emulation::kKernelVersionQueryMaxEvidenceSize;
}

#endif
