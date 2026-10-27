//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_LIMITS_H
#define NEVERD_SOLVER_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::solver {

// include/neverd/solver/SymSynthVerifier.h
inline constexpr uint32_t kSymSynthVerifierTimeoutMs = 1000;

// lib/solver/bv/PermanentConjuncts.h
// Large path predicates can contain thousands of small Boolean wrappers.
// Bound this optional preparation independently of bit-blasting and SAT search;
// a smaller caller-supplied allowance still takes precedence.
inline constexpr unsigned kPermanentConjunctsMaxPermanentConjunctWork = 1U
                                                                        << 18;

// lib/solver/bv/SymSynthVerifier.cpp
inline constexpr size_t kSymSynthVerifierMaxTerms = 64;

// lib/solver/cnf/CnfEncoder.cpp
/// Beyond this many operands an exclusive-or is built as a chain of two-input
/// gates.  A single node would need one clause per assignment of its inputs,
/// which is fine at the two and three inputs an adder wants and is a mistake
/// at any width a caller might hand over.
inline constexpr size_t kCnfEncoderMaxXorFanIn = 3;

// lib/solver/sat/SatHeuristic.cpp
/// Activities are rescaled rather than allowed to overflow.  Scaling every
/// activity by the same factor leaves their order — which is all the heap
/// reads — untouched.
inline constexpr double kSatHeuristicVarActivityCeiling = 1e100;
inline constexpr float kSatHeuristicClauseActivityCeiling = 1e20f;

// lib/solver/bv/BitBlaster.cpp
inline constexpr int kBitBlasterEncodeReachableOrderSizeLimit = 1024;

// lib/solver/sat/SatSolver.cpp
inline constexpr double kSatSolverSatEngineSolveMaxLearntMaxArgument2 = 64.0;

} // namespace neverd::limits::solver

#endif // NEVERD_SOLVER_LIMITS_H
