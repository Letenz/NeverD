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

namespace neverd::emulation::kernel_version_query {
constexpr uint32_t Version = 1;
constexpr uint32_t BaseSize = 276;
constexpr uint32_t FieldsEnd = 20;
constexpr uint32_t MaxBuild = UINT16_MAX;
constexpr size_t MaxEvidenceSize = 256;
llvm::Error validate(const DriverVersionQuery &Query);
} // namespace neverd::emulation::kernel_version_query
#endif
