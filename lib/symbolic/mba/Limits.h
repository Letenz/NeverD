//===- Limits.h - MBA refinement bounds -----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal bounds for MBA refinement, polynomial recovery and result reuse.
/// These refinements share the caller's existing work and storage budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_MBA_LIMITS_H
#define NEVERD_SYMBOLIC_MBA_LIMITS_H

#include <cstddef>

namespace neverd::symbolic::detail::limits {

inline constexpr std::size_t kMaxLinearTailTerms = 128;
inline constexpr std::size_t kMaxLinearTailNodes = 32;
inline constexpr unsigned kMinLinearTailSummands = 2;
inline constexpr unsigned kMinLinearTailProducts = 1;
inline constexpr unsigned kMinLinearTailProductDegree = 8;
inline constexpr unsigned kMaxLinearTailInputs = 3;

// Shorter products retain their original spelling for local identities.
inline constexpr std::size_t kMinCompletedProductWalkDegree = 16;
inline constexpr std::size_t kMaxCompletedProductTerms = 128;

inline constexpr std::size_t kMinDelayedCommonAtoms = 4;
inline constexpr unsigned kMinDelayedPolynomialOperands = 2;

inline constexpr std::size_t kMaxPolynomialCofactorTerms = 128;
inline constexpr std::size_t kMaxPolynomialCofactorEdges = 4096;
inline constexpr unsigned kMaxCofactorSearchDegree = 3;
inline constexpr unsigned kMaxCubicCofactorSearchAtoms = 3;
inline constexpr unsigned kMinQuadraticRowAtoms = 4;
inline constexpr unsigned kMaxQuadraticRowAtoms = 8;
inline constexpr std::size_t kMaxQuadraticRowTerms = 8;
inline constexpr std::size_t kMinCompletedRegionCacheWork = 256;

} // namespace neverd::symbolic::detail::limits

#endif // NEVERD_SYMBOLIC_MBA_LIMITS_H
