//===- AndroidResolver.h - Resolver input admission --------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded offline network contracts and their semantic checks.

#ifndef NEVERD_EMULATION_ANDROIDRESOLVER_H
#define NEVERD_EMULATION_ANDROIDRESOLVER_H
#include "neverd/emulation/AndroidResolverOptions.h"

#include "llvm/Support/Error.h"
namespace neverd::emulation::android_model {
llvm::Error
validateResolverQueries(const std::vector<AndroidResolverQuery> &Queries);
} // namespace neverd::emulation::android_model
#endif
