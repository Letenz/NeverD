//===- LowIRUndefinedIndependence.h - Bounded relational proof ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LOWIRUNDEFINEDINDEPENDENCE_H
#define NEVERD_ANALYSIS_LOWIRUNDEFINEDINDEPENDENCE_H

#include "neverd/ir/low/LowUndefinedEffects.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymState.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd::analysis {

/// Bind architecture evidence to one complete instruction, never just a PC.
struct LowIRUndefinedInstruction {
  int BlockId = -1;
  LowInstructionBoundary Boundary;
  LowInstructionUndefinedEffects Effects;
};

struct LowIRIndependenceConstant {
  NdVar Location;
  uint64_t Value = 0;
};

struct LowIRIndependenceFrame {
  /// The current exact-frame model uses a 64-bit entry address register.
  symbolic::SymRegisterRange RootRegister;
  /// Accessible, nonwrapping bytes relative to the entry root: [Begin, End).
  /// This is an explicit nonfaulting environment contract, not an alias fact
  /// inferred from executing one input. Only this mutable region is supported.
  int64_t Begin = 0;
  int64_t End = 0;
};

struct LowIRIndependenceContract {
  std::vector<LowIRIndependenceConstant> EntryConstants;
  std::optional<LowIRIndependenceFrame> Frame;
  std::vector<symbolic::SymRegisterRange> ReturnRegisters;
  bool ObserveWrittenFrameBytes = true;
  llvm::endianness ByteOrder = llvm::endianness::little;
};

struct LowIRIndependenceLimits {
  uint64_t MaxOperations = 65536;
  /// Bounds both input metadata and dynamically visited instructions, including
  /// instructions whose lifted operation spans are empty.
  uint64_t MaxInstructions = 65536;
  /// Total path states scheduled, including straight-line successor visits.
  uint32_t MaxPaths = 256;
  uint32_t MaxBlockVisits = 4096;
  uint32_t MaxProducers = 4096;
  uint32_t MaxFrameBytes = 4096;
  uint32_t MaxSolverQueries = 4096;
  uint64_t MaxObservations = 65536;
  uint64_t MaxSymbolicNodes = 262144;
  solver::SolverOptions Solver = [] {
    solver::SolverOptions Value;
    Value.Blast.MaxGates = 262144;
    Value.Sat.MaxConflicts = 10000;
    Value.Sat.MaxPropagations = 1000000;
    Value.Sat.MaxWatchVisits = 10000000;
    return Value;
  }();
};

enum class LowIRIndependenceStatus : uint8_t {
  Proved,
  Dependent,
  Unsupported,
  Invalid,
  BudgetExceeded,
  InfeasibleEntry,
};

enum class LowIRIndependenceScope : uint8_t { CompleteAcyclicLowIR };

struct LowIRIndependenceCertificate {
  LowIRIndependenceScope Scope = LowIRIndependenceScope::CompleteAcyclicLowIR;
  /// SHA256 of the checked operations, CFG, boundaries, sidecars, contract and
  /// proof limits. Recheck the supplied inputs before reusing a certificate;
  /// this digest detects stale inputs, not an untrusted producer of proofs.
  std::string InputDigest;
  std::vector<LowIRUndefinedInstruction> Instructions;
  LowIRIndependenceContract Contract;
  LowIRIndependenceLimits Limits;
};

struct LowIRIndependenceResult {
  LowIRIndependenceStatus Status = LowIRIndependenceStatus::Invalid;
  std::optional<LowIRIndependenceCertificate> Certificate;
  std::string Diagnostic;
  int BlockId = -1;
  va_t InstructionAddress = 0;
  int OpSeq = -1;
  uint64_t Operations = 0;
  uint64_t Instructions = 0;
  uint32_t Paths = 0;
  uint32_t BlockVisits = 0;
  uint32_t Producers = 0;
  uint32_t SolverQueries = 0;
  uint64_t Observations = 0;

  bool proved() const { return Status == LowIRIndependenceStatus::Proved; }
};

/// Prove that all control predicates, memory addresses, RETURN operands and
/// requested final observations are independent of architecture-arbitrary
/// choices. Ordinary entry inputs are shared; each effect creates independent
/// left/right choices that stay correlated with their own copies and spills.
///
/// Starts at Function.Entry; additional declared entry roots are unsupported.
/// The driver checks a branch before constraining its path. Calls, opaque
/// operations, reachable cycles, unknown aliases, unsupported modes and missing
/// architecture coverage refuse proof. Path exhaustion is never success.
/// This certifies the supplied LowIR/effects only, not native lifting, source
/// translation, termination of arbitrary loops, or a processor's undefined-bit
/// choice. It must not be applied only after uncertified control pruning.
LowIRIndependenceResult checkLowIRUndefinedIndependence(
    const LowFunc &Function,
    llvm::ArrayRef<LowIRUndefinedInstruction> Instructions,
    const LowIRIndependenceContract &Contract,
    const LowIRIndependenceLimits &Limits = {});

} // namespace neverd::analysis

#endif
