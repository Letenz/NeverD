//===- LowIRConcolic.h - Verified LowIR branch flips -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs one concrete LowIR trace, negates each exact conditional-decision
/// prefix, and publishes only entry-input seeds whose fresh replay reaches
/// the same occurrence with the opposite polarity.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_CONCOLIC_LOWIRCONCOLIC_H
#define NEVERD_CONCOLIC_LOWIRCONCOLIC_H

#include "neverd/concolic/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExplore.h"

#include "llvm/Support/Endian.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::concolic {

enum class ConcolicTraceReason : uint8_t {
  None,
  InvalidInitialSeed,
  NoTrace,
  AmbiguousTrace,
  IncompleteConcreteTrace,
  IncompleteLift,
  UnsupportedEffects,
  IncompleteOutcome,
  InvalidDecisionHistory,
  MemorySeedBudgetExceeded,
};

enum class ConcolicProjectionReason : uint8_t {
  None,
  InvalidQuery,
  FreshVariable,
  MissingInputOrigin,
  UnsupportedInputKind,
  NonzeroInputEpoch,
  InvalidInputWidth,
  MissingModelValue,
  OverlappingInputOrigins,
  MissingBaselineByte,
  CandidateDoesNotSatisfyQuery,
};

enum class ConcolicReplayReason : uint8_t {
  None,
  NoReplayTrace,
  EarlierDecisionMissing,
  EarlierOccurrenceMismatch,
  EarlierPolarityMismatch,
  EarlierConstraintPrefixMismatch,
  EarlierDecisionNotConcrete,
  TargetDecisionMissing,
  TargetOccurrenceMismatch,
  TargetDecisionNotConcrete,
  TargetConstraintPrefixMismatch,
  TargetPolarityNotFlipped,
};

enum class ConcolicFlipStatus : uint8_t {
  Verified,
  Unsat,
  SolverUnknown,
  InvalidQuery,
  ProjectionRejected,
  ReplayRejected,
  VerifiedDuplicate,
  AttemptBudgetExceeded,
  CandidateBudgetExceeded,
};

const char *concolicTraceReasonName(ConcolicTraceReason Reason);
const char *concolicProjectionReasonName(ConcolicProjectionReason Reason);
const char *concolicReplayReasonName(ConcolicReplayReason Reason);
const char *concolicFlipStatusName(ConcolicFlipStatus Status);

inline solver::SolverOptions defaultLowIRConcolicSolverOptions() {
  solver::SolverOptions Options;
  Options.Sat.MaxConflicts = ::neverd::limits::concolic::
      kLowIRConcolicDefaultLowIRConcolicSolverOptionsMaxConflicts;
  Options.Sat.MaxPropagations = ::neverd::limits::concolic::
      kLowIRConcolicDefaultLowIRConcolicSolverOptionsMaxPropagations;
  Options.Sat.MaxWatchVisits = ::neverd::limits::concolic::
      kLowIRConcolicDefaultLowIRConcolicSolverOptionsMaxWatchVisits;
  return Options;
}

struct LowIRConcolicOptions {
  std::vector<symbolic::SymConcreteRegister> InitialSeed;
  llvm::endianness ByteOrder = llvm::endianness::little;

  unsigned MaxSteps = 1u << 16;
  unsigned MaxBlockVisits = 3;
  unsigned MaxLoopIterations = 3;

  /// Number of solver checks. Zero deliberately attempts no flips.
  unsigned MaxFlipAttempts = 64;
  /// Number of distinct replay-verified seeds retained in the report.
  unsigned MaxCandidates = 64;
  solver::SolverOptions Solver = defaultLowIRConcolicSolverOptions();
  /// Concrete entry RAM in address order. These bytes do not constrain the
  /// primary symbolic state or establish allocation ownership/disjointness.
  std::vector<symbolic::SymConcreteMemory> InitialMemory;
  /// Total entry-memory bytes accepted before copying or running a trace.
  /// Zero accepts only an empty memory seed.
  size_t MaxMemorySeedBytes = 1u << 20;
};

/// Context-free copy of one decision from the original trace.
struct LowIRConcolicDecision {
  size_t Index = 0;
  symbolic::SymDecisionOccurrence Occurrence;
  bool Taken = false;
  size_t ConstraintPrefix = 0;
  bool Concrete = false;
};

struct LowIRConcolicCandidate {
  /// Sorted, non-overlapping ranges with the same shape as InitialSeed.
  std::vector<symbolic::SymConcreteRegister> Seed;
  /// Same sorted address ranges as InitialMemory, with projected input bytes.
  std::vector<symbolic::SymConcreteMemory> MemorySeed;
};

struct LowIRConcolicFlip {
  size_t DecisionIndex = 0;
  symbolic::SymDecisionOccurrence Occurrence;
  bool OriginalTaken = false;
  size_t ConstraintPrefix = 0;

  ConcolicFlipStatus Status = ConcolicFlipStatus::InvalidQuery;
  /// Absent when the attempt budget prevented a solver call.
  std::optional<solver::SatResult> SolverResult;
  solver::BlastError EncodingError = solver::BlastError::None;
  ConcolicProjectionReason ProjectionReason = ConcolicProjectionReason::None;
  ConcolicReplayReason ReplayReason = ConcolicReplayReason::None;
  std::optional<size_t> CandidateIndex;
};

/// An owned report. It contains neither SymRef values nor pointers into Func.
struct LowIRConcolicReport {
  uint32_t Version = 1;
  va_t FunctionEntry = 0;
  std::string FunctionName;
  std::vector<symbolic::SymConcreteRegister> InitialSeed;

  std::optional<symbolic::PathOutcome> TraceOutcome;
  bool LiftComplete = false;
  bool TraceComplete = false;
  bool TraceExact = false;
  /// A concolic run follows one trace and is never exhaustive.
  bool Exhaustive = false;
  ConcolicTraceReason TraceReason = ConcolicTraceReason::None;
  size_t ExecutedSteps = 0;
  unsigned UnmodelledOps = 0;
  unsigned OpaqueOps = 0;
  unsigned CallHavocs = 0;
  unsigned MemoryHavocs = 0;
  /// Block IDs entered by the original trace, in execution order.
  std::vector<int> Blocks;

  std::vector<LowIRConcolicDecision> Decisions;
  std::vector<LowIRConcolicFlip> Flips;
  std::vector<LowIRConcolicCandidate> Candidates;

  unsigned FlipAttempts = 0;
  bool FlipBudgetHit = false;
  bool CandidateBudgetHit = false;
  /// Version 2 reports retain memory seeds as well as register seeds. Reports
  /// without a requested memory seed retain the version 1 contract.
  std::vector<symbolic::SymConcreteMemory> InitialMemory;
};

LowIRConcolicReport
runLowIRConcolic(const LowFunc &Func,
                 const LowIRConcolicOptions &Options = LowIRConcolicOptions());

} // namespace neverd::concolic

#endif // NEVERD_CONCOLIC_LOWIRCONCOLIC_H
