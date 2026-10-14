//===- RawEnvironment.h - Raw recovery integrity ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_CORE_RAWENVIRONMENT_H
#define NEVERD_ANALYSIS_CORE_RAWENVIRONMENT_H

#include "neverd/analysis/InterpreterSpecialization.h"

#include <array>

namespace neverd::analysis {

llvm::Expected<std::shared_ptr<const SpecializationRawEnvironment>>
prepareRawEnvironment(const SpecializationOptions &Options, va_t PrimaryBegin,
                      va_t PrimaryEnd, llvm::StringRef PrimaryDigest);
llvm::Error validatePreparedRawEnvironment(
    const std::shared_ptr<const SpecializationRawEnvironment> &Domain,
    const SpecializationOptions &Options);
std::array<uint64_t, 4>
rawEnvironmentDigestWords(const SpecializationRawEnvironment &Domain);
llvm::Error sealRawEnvironment(
    SpecializationResult &Result,
    const std::shared_ptr<const SpecializationRawEnvironment> &Domain,
    const SpecializationOptions &Options);

} // namespace neverd::analysis
#endif
