//===- LowIRUndefinedIndependence.cpp - Correlated arbitrary values
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LowIRUndefinedIndependence.h"

#include "../arch/x86_64/NativeStackControl.h"
#include "../arch/x86_64/X64Recovery.h"
#include "../arch/x86_64/X64UserFlags.h"
#include "FiniteQueryCache.h"
#include "FiniteValues.h"
#include "FrameEntryConstraints.h"
#include "FrameOffsets.h"
#include "LowIRLoopInference.h"
#include "NativeUndefinedIndependence.h"

#include "neverd/analysis/LowIRRefinement.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace neverd::analysis {
namespace {
using namespace symbolic;
using Status = LowIRIndependenceStatus;
using FlagProfile = detail::X64UserFlags;
constexpr uint64_t AddressTemporary = UINT64_MAX - 7;
constexpr uint64_t NativeStackTemporary = AddressTemporary - 8;

bool sameBoundary(const LowInstructionBoundary &A,
                  const LowInstructionBoundary &B) {
  return std::tie(A.Address, A.Size, A.FirstOp, A.OpCount, A.Mode, A.Control,
                  A.ControlFlags, A.TargetMode, A.Immediate) ==
         std::tie(B.Address, B.Size, B.FirstOp, B.OpCount, B.Mode, B.Control,
                  B.ControlFlags, B.TargetMode, B.Immediate);
}

bool scalar(const NdVar &V) {
  return (V.isReg() || V.isTemp() || V.isConst()) && V.Size && V.Size <= 8 &&
         (V.isConst() || V.Offset <= UINT64_MAX - (V.Size - 1)) &&
         (!V.isTemp() || (V.Offset < AddressTemporary &&
                          V.Size <= AddressTemporary - V.Offset));
}

/// Hash semantic input fields explicitly; neither padding nor pointer identity
/// may participate. Presentation names and stale predecessor lists are unused.
std::string
inputDigest(const LowFunc &F, llvm::ArrayRef<LowIRUndefinedInstruction> Records,
            const LowIRIndependenceContract &Contract,
            const LowIRIndependenceLimits &Limits,
            llvm::ArrayRef<LowIRNativeFlagTransition> Flags = {},
            llvm::ArrayRef<LowIRNativeProfileProjection> Projections = {},
            llvm::ArrayRef<LowIRNativeAuditBoundary> AuditBoundaries = {}) {
  llvm::SHA256 Hash;
  const auto Number = [&](uint64_t Value) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = static_cast<uint8_t>(Value >> (I * 8));
    Hash.update(llvm::ArrayRef<uint8_t>(Bytes));
  };
  const auto Variable = [&](const NdVar &V) {
    Number(static_cast<unsigned>(V.Space));
    Number(V.Offset);
    Number(V.Size);
    Number(static_cast<unsigned>(V.Provenance));
    Number(V.AddressOwnerVA);
  };
  const auto Boundary = [&](const LowInstructionBoundary &B) {
    Number(B.Address);
    Number(B.Size);
    Number(B.FirstOp);
    Number(B.OpCount);
    Number(static_cast<unsigned>(B.Mode));
    Number(static_cast<unsigned>(B.Control));
    Number(static_cast<unsigned>(B.ControlFlags));
    Number(static_cast<unsigned>(B.TargetMode));
    Number(B.Immediate.has_value());
    Number(B.Immediate.value_or(0));
  };
  const auto ExceptionalEdges = [&](llvm::ArrayRef<ExceptionalEdge> Edges) {
    Number(Edges.size());
    for (const auto &Edge : Edges) {
      Number(static_cast<uint64_t>(Edge.BlockId));
      Number(Edge.TargetVA);
      Number(static_cast<unsigned>(Edge.Kind));
      Number(Edge.RegionIndex);
      Number(static_cast<uint64_t>(Edge.State));
    }
  };
  Number(16); // Certificate semantic schema, independent of report formatting.
  Number(Contract.RetainUnauditedNativeBoundaries);
  Number(Contract.AllowOverlappingNativeInstructions);
  Number(Contract.DeferNativeConditionalEdges);
  Number(Contract.X64FlagsProfile.has_value());
  if (Contract.X64FlagsProfile) {
    Number(static_cast<unsigned>(*Contract.X64FlagsProfile));
    // Includes canonical entry and mandatory final system-state observation.
    Number(FlagProfile::SemanticsVersion);
  }
  Number(Flags.size());
  for (const auto &Transition : Flags) {
    Number(static_cast<uint64_t>(Transition.BlockId));
    Number(Transition.InstructionAddress);
    Number(static_cast<uint64_t>(Transition.OpSeq));
    Number(Transition.SemanticsVersion);
    Number(Transition.OperationDigest.size());
    Hash.update(Transition.OperationDigest);
  }
  Number(Projections.size());
  for (const auto &Projection : Projections) {
    Number(static_cast<uint64_t>(Projection.BlockId));
    Number(Projection.InstructionAddress);
    Number(static_cast<unsigned>(Projection.Kind));
  }
  Number(AuditBoundaries.size());
  for (const auto &Receipt : AuditBoundaries) {
    Number(static_cast<unsigned>(Receipt.Kind));
    Number(Receipt.SemanticsVersion);
    Boundary(Receipt.Boundary);
    Number(Receipt.NativeBytesDigest.size());
    Hash.update(Receipt.NativeBytesDigest);
    Number(Receipt.OperationDigest.size());
    Hash.update(Receipt.OperationDigest);
  }
  Number(F.Entry);
  Number(F.FunctionTemporaries.size());
  for (const auto &Range : F.FunctionTemporaries) {
    Number(Range.Offset);
    Number(Range.Bytes);
  }
  Number(F.ModuleAnalysisRoots.size());
  for (va_t Root : F.ModuleAnalysisRoots)
    Number(Root);
  Number(F.OrdinaryModuleAnalysisRoots.size());
  for (va_t Root : F.OrdinaryModuleAnalysisRoots)
    Number(Root);
  Number(F.Blocks.size());
  for (const auto &B : F.Blocks) {
    Number(static_cast<uint64_t>(B.Id));
    Number(B.StartAddr);
    Number(B.EndAddr);
    ExceptionalEdges(B.ExceptionalSuccs);
    ExceptionalEdges(B.ExceptionalPreds);
    Number(B.Ops.size());
    for (const auto &Op : B.Ops) {
      Number(static_cast<unsigned>(Op.Opcode));
      Number(static_cast<unsigned>(Op.MemoryOrdering));
      Number(static_cast<unsigned>(Op.MemoryAddressSpace));
      Variable(Op.Output);
      Number(Op.NumInputs);
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        Variable(Op.Inputs[I]);
      Number(Op.Addr);
      Number(static_cast<uint64_t>(Op.Seq));
    }
    Number(B.InstructionBoundaries.size());
    for (const auto &IB : B.InstructionBoundaries)
      Boundary(IB);
    Number(B.Succs.size());
    for (int S : B.Succs)
      Number(static_cast<uint64_t>(S));
  }
  Number(Records.size());
  for (const auto &R : Records) {
    Number(static_cast<uint64_t>(R.BlockId));
    Boundary(R.Boundary);
    Number(static_cast<unsigned>(R.Effects.Coverage));
    Number(R.Effects.OpCount);
    Number(R.Effects.OperationDigest.size());
    Hash.update(R.Effects.OperationDigest);
    Number(R.Effects.Effects.size());
    for (const auto &E : R.Effects.Effects) {
      Number(E.AfterOp);
      Variable(E.Output);
      Number(E.BitOffset);
      Number(E.BitCount);
      Number(E.When.has_value());
      if (E.When)
        Variable(*E.When);
    }
  }
  Number(Contract.EntryConstants.size());
  for (const auto &C : Contract.EntryConstants) {
    Variable(C.Location);
    Number(C.Value);
  }
  Number(Contract.Frame.has_value());
  if (Contract.Frame) {
    Number(Contract.Frame->RootRegister.Offset);
    Number(Contract.Frame->RootRegister.Bytes);
    Number(static_cast<uint64_t>(Contract.Frame->Begin));
    Number(static_cast<uint64_t>(Contract.Frame->End));
    Number(Contract.Frame->ExcludedAddressRanges.size());
    for (const auto &R : Contract.Frame->ExcludedAddressRanges) {
      Number(R.Begin);
      Number(R.End);
    }
    Number(Contract.Frame->EntryAlignment.has_value());
    if (Contract.Frame->EntryAlignment) {
      Number(Contract.Frame->EntryAlignment->Alignment);
      Number(Contract.Frame->EntryAlignment->Residue);
    }
  }
  Number(Contract.ReturnRegisters.size());
  for (const auto &R : Contract.ReturnRegisters) {
    Number(R.Offset);
    Number(R.Bytes);
  }
  Number(Contract.PreservedRegisters.size());
  for (const auto &R : Contract.PreservedRegisters) {
    Number(R.Offset);
    Number(R.Bytes);
  }
  Number(Contract.PreservedFrameRanges.size());
  for (const auto &R : Contract.PreservedFrameRanges) {
    Number(static_cast<uint64_t>(R.Offset));
    Number(R.Bytes);
  }
  Number(Contract.ObserveWrittenFrameBytes);
  Number(static_cast<unsigned>(Contract.ByteOrder));
  Number(Limits.MaxOperations);
  Number(Limits.MaxInstructions);
  Number(Limits.MaxPaths);
  Number(Limits.MaxBlockVisits);
  Number(Limits.MaxProducers);
  Number(Limits.MaxFrameBytes);
  Number(Limits.MaxSolverQueries);
  Number(Limits.MaxIndirectTargets);
  Number(Limits.MaxImmutableLoadAddresses);
  Number(Limits.MaxObservations);
  Number(Limits.MaxSymbolicNodes);
  Number(Limits.MaxNativeInstructionBytes);
  Number(Limits.Solver.Blast.MaxWidth);
  Number(Limits.Solver.Blast.MaxGates);
  Number(Limits.Solver.BuildModel);
  const auto &Sat = Limits.Solver.Sat;
  Number(std::bit_cast<uint64_t>(Sat.VarDecay));
  Number(std::bit_cast<uint64_t>(Sat.ClauseDecay));
  Number(Sat.RestartInterval);
  Number(std::bit_cast<uint64_t>(Sat.LearnedFraction));
  Number(std::bit_cast<uint64_t>(Sat.LearnedGrowth));
  Number(Sat.MaxConflicts);
  Number(Sat.MaxPropagations);
  Number(Sat.MaxWatchVisits);
  Number(Sat.MinimizeLearned);
  Number(Sat.PhaseSaving);
  Number(Sat.DefaultPhase);
  return llvm::toHex(Hash.final());
}

struct Stop {};

std::vector<LowIRNativeAuditBoundary> auditBoundaryReceipts(
    const std::map<va_t, LowIRNativeAuditBoundary> &Boundaries) {
  std::vector<LowIRNativeAuditBoundary> Receipts;
  for (const auto &[Address, Receipt] : Boundaries)
    Receipts.push_back(Receipt);
  return Receipts;
}

struct TerminalState {
  SymState State;
  SymRef Predicate, SystemFlags, ReturnOperand;
  std::set<uint64_t> Written;
  int Cutpoint = -1;
  std::set<uint64_t> DefinedFunctionTemporaries{};
};

struct LoopPrefix {
  TerminalState Original, Candidate;
  SymRef Predicate;
};

// Both relations use the same instruction, memory, profile and physical-stack
// executor. Only undefined-value selection and terminal obligations differ.
// Selected executions never publish an independence certificate.
struct RefinementSession {
  SymContext Context;
  LowIRIndependenceResult Statistics;
  const LowFunc &Candidate;
  LowIRRefinementWitness Witness;
  const LowIRRefinementLimits &Limits;
  std::optional<SymState> Initial;
  SymRef EntryRoot, MemoryRoot, EntrySystem, Predicate;
  uint64_t ScheduledPaths = 0;
  uint64_t TerminalPairs = 0;
  std::vector<TerminalState> OriginalReturns, CandidateReturns;
  std::vector<LowIRRefinementProducer> Producers;
  std::string OriginalDigest, CandidateDigest;
  std::vector<LowIRUndefinedInstruction> OriginalInstructions;
  std::vector<LowIRNativeFlagTransition> Flags;
  std::vector<LowIRNativeProfileProjection> Projections;
  std::map<va_t, uint8_t> ImmutableBytes;
  uint64_t ReadEvidenceBytes = 0;
  const LowIRLoopRefinementPlan *LoopPlan = nullptr;
  std::optional<TerminalState> OriginalStart, CandidateStart;
  int StartingCutpoint = -1;
  // An entry replay may seek one prefix behind earlier cuts. It still starts
  // from the shared real entry and finds a reachable prefix witness only.
  // Full entry/transition coverage is checked separately, and every arrival
  // must imply the witness predicate. No inductive state can seed this replay.
  int PrefixSearchCutpoint = -1;
  std::set<uint64_t> RegisterBytes;
  std::vector<SymRef> StartingRank;
  SymRef SegmentPredicate;
  int NextNativeBlock = 0;
  uint32_t OriginalPathCount = 0, CandidatePathCount = 0;
  uint64_t OriginalCutpoints = 0, CandidateCutpoints = 0;
  uint64_t LoopInitiations = 0, LoopTransitions = 0, RankingChecks = 0;
  std::vector<std::string> OriginalSegmentDigests, CandidateSegmentDigests;
  std::vector<std::optional<LoopPrefix>> LoopPrefixes;
  // One immutable native inventory across every segment. Re-fetching a cut
  // must not forget prior byte ownership, overlap checks or input budgets.
  std::map<va_t, SpecializationInstruction> NativeInstructions;
  std::map<va_t, va_t> NativeRanges;
  std::map<va_t, LowIRNativeAuditBoundary> NativeAuditBoundaries;
  bool NativeFinite = false;
  uint64_t NativeInputOperations = 0, NativeInputEffects = 0;
  uint64_t NativeInputBytes = 0;
  const LowFunc *Original = nullptr;
};

class Checker {
  friend class LoopPlanInference;
  const LowFunc *Function = nullptr;
  llvm::ArrayRef<LowIRUndefinedInstruction> Records;
  SpecializationProvider *Provider = nullptr;
  SpecializationCursor NativeEntry;
  detail::NativeUndefinedIndependenceResult *NativeResult = nullptr;
  std::map<va_t, SpecializationInstruction> OwnedNativeInstructions;
  std::map<va_t, SpecializationInstruction> &NativeInstructions =
      OwnedNativeInstructions;
  std::map<va_t, va_t> OwnedNativeRanges;
  std::map<va_t, va_t> &NativeRanges = OwnedNativeRanges;
  std::map<va_t, LowIRNativeAuditBoundary> OwnedNativeAuditBoundaries;
  std::map<va_t, LowIRNativeAuditBoundary> &NativeAuditBoundaries =
      OwnedNativeAuditBoundaries;
  std::map<va_t, uint8_t> OwnedImmutableBytes;
  std::map<va_t, uint8_t> &ImmutableBytes = OwnedImmutableBytes;
  LowFunc NativeTrace;
  std::vector<LowIRUndefinedInstruction> NativeRecords;
  std::vector<LowIRNativeFlagTransition> NativeFlagTransitions;
  std::vector<LowIRNativeProfileProjection> NativeProfileProjections;
  std::map<int, std::vector<int>> NativeTraceEdges;
  uint64_t OwnedNativeInputOperations = 0, OwnedNativeInputEffects = 0;
  uint64_t &NativeInputOperations = OwnedNativeInputOperations;
  uint64_t &NativeInputEffects = OwnedNativeInputEffects;
  uint64_t OwnedNativeInputBytes = 0;
  uint64_t &NativeInputBytes = OwnedNativeInputBytes;
  uint64_t OwnedReadEvidenceBytes = 0;
  uint64_t &NativeReadEvidenceBytes = OwnedReadEvidenceBytes;
  int NextNativeBlock = 0;
  const LowIRIndependenceContract &Contract;
  const LowIRIndependenceLimits &Limits;
  // The existing cache bounds key construction and retained complete domains
  // in words, using the same capacity convention as recovery.
  detail::FiniteQueryCache FrameProofs{Limits.MaxSymbolicNodes};
  LowIRIndependenceResult OwnedResult;
  LowIRIndependenceResult &Result = OwnedResult;
  SymContext OwnedContext;
  SymContext &Ctx = OwnedContext;
  // One completed model-free query in this immutable DAG and fixed solver
  // configuration. No model, incomplete answer or cross-context fact escapes.
  SymRef LastCompletedQuery;
  solver::SatResult LastCompletedAnswer = solver::SatResult::Invalid;
  RefinementSession *Refinement = nullptr;
  bool CandidateExecution = false;
  SpecializationProvider *ReadProvider = nullptr;
  SymRef EntryRoot, MemoryRoot;
  std::vector<SymRef> PreservedRegisterEntries;
  std::map<uint64_t, SymRef> PreservedFrameEntries;
  std::map<int, const LowBlock *> Blocks;
  std::map<va_t, int> Addresses;
  std::map<std::pair<int, va_t>, const LowInstructionBoundary *> Boundaries;
  std::map<std::pair<int, va_t>, const LowIRUndefinedInstruction *> Effects;
  std::unordered_map<uint32_t, SymRef> ZeroLeftChoices;
  uint64_t FrameBytes = 0;
  uint64_t OwnedScheduledPaths = 0;
  uint64_t &ScheduledPaths = OwnedScheduledPaths;
  bool Validated = false;

  struct Path {
    int BlockId;
    SymState Left, Right;
    SymRef Predicate;
    std::set<int> Ancestors;
    std::set<uint64_t> Written;
    va_t NativeAddress = 0;
    SymRef LeftSystemFlags, RightSystemFlags;
    bool SkipCutpoint = false;
    std::set<uint64_t> DefinedFunctionTemporaries;
  };
  std::deque<Path> Pending;

  bool inductive() const { return Refinement && Refinement->LoopPlan; }

  void trackRegister(const NdVar &V) {
    if (!inductive() || !V.isReg())
      return;
    if (!scalar(V))
      fail(Status::Invalid, "invalid loop register range");
    for (uint16_t I = 0; I != V.Size; ++I)
      Refinement->RegisterBytes.insert(V.Offset + I);
    if (Refinement->RegisterBytes.size() > Limits.MaxObservations)
      fail(Status::BudgetExceeded, "loop register metadata budget exhausted");
  }

  bool stopAtCutpoint(Path &P) {
    if (!inductive() || P.SkipCutpoint) {
      P.SkipCutpoint = false;
      return false;
    }
    const va_t Address =
        Provider ? P.NativeAddress : Blocks.at(P.BlockId)->StartAddr;
    const auto &Cuts = Refinement->LoopPlan->Cutpoints;
    for (size_t I = 0; I != Cuts.size(); ++I) {
      if (Refinement->PrefixSearchCutpoint >= 0 &&
          static_cast<size_t>(Refinement->PrefixSearchCutpoint) != I)
        continue;
      const auto Expected = CandidateExecution ? Cuts[I].CandidateAddress
                                               : Cuts[I].OriginalAddress;
      if (Address != Expected)
        continue;
      auto &Endpoints = CandidateExecution ? Refinement->CandidateReturns
                                           : Refinement->OriginalReturns;
      Endpoints.push_back({std::move(P.Left),
                           P.Predicate,
                           P.LeftSystemFlags,
                           {},
                           std::move(P.Written),
                           static_cast<int>(I),
                           std::move(P.DefinedFunctionTemporaries)});
      ++(CandidateExecution ? Refinement->CandidateCutpoints
                            : Refinement->OriginalCutpoints);
      if (Refinement->PrefixSearchCutpoint >= 0)
        Pending.clear();
      return true;
    }
    return false;
  }

  [[noreturn]] void fail(Status S, std::string Message) {
    Result.Status = S;
    Result.Diagnostic = std::move(Message);
    Result.Certificate.reset();
    throw Stop{};
  }

  void nodes() {
    if (Ctx.numNodes() > Limits.MaxSymbolicNodes)
      fail(Status::BudgetExceeded, "symbolic-node budget exhausted");
  }

  solver::SatResult query(SymRef Predicate) {
    nodes();
    if (Predicate && Predicate == LastCompletedQuery)
      return LastCompletedAnswer;
    if (Result.SolverQueries >= Limits.MaxSolverQueries)
      fail(Status::BudgetExceeded, "solver-query budget exhausted");
    ++Result.SolverQueries;
    const auto Answer =
        solver::checkSat(Ctx, Predicate, nullptr, Limits.Solver);
    if (Answer == solver::SatResult::Unknown)
      fail(Status::BudgetExceeded, "relational solver budget exhausted");
    if (Answer == solver::SatResult::Invalid)
      fail(Status::Invalid, "invalid relational solver query");
    LastCompletedQuery = Predicate;
    LastCompletedAnswer = Answer;
    return Answer;
  }

  void equal(SymRef Predicate, SymRef Left, SymRef Right,
             llvm::StringRef Observation, Status Failure = Status::Dependent) {
    nodes();
    if (++Result.Observations > Limits.MaxObservations)
      fail(Status::BudgetExceeded, "observation budget exhausted");
    if (!Left || !Right || Ctx.width(Left) != Ctx.width(Right))
      fail(Status::Invalid, "invalid observation: " + Observation.str());
    if (Left == Right)
      return;
    solver::SatResult Answer;
    try {
      Answer = query(Ctx.mkAnd(Predicate, Ctx.mkNe(Left, Right)));
    } catch (const Stop &) {
      Result.Diagnostic += " while checking " + Observation.str();
      throw;
    }
    if (Answer != solver::SatResult::Unsat)
      fail(Failure,
           Failure == Status::ContractViolation
               ? "return contract does not preserve " + Observation.str()
               : (Refinement ? "selected original and candidate differ at "
                             : "architecture-arbitrary value affects ") +
                     Observation.str());
  }

  void preservedReturn(Path &P) {
    for (size_t I = 0; I != Contract.PreservedRegisters.size(); ++I) {
      const auto &R = Contract.PreservedRegisters[I];
      equal(P.Predicate, P.Left.read(SymSpace::Register, R.Offset, R.Bytes),
            PreservedRegisterEntries[I], "entry register in left execution",
            Status::ContractViolation);
      equal(P.Predicate, P.Right.read(SymSpace::Register, R.Offset, R.Bytes),
            PreservedRegisterEntries[I], "entry register in right execution",
            Status::ContractViolation);
    }
    for (const auto &[Byte, Entry] : PreservedFrameEntries) {
      const auto Address = Ctx.mkAdd(MemoryRoot, Ctx.mkConst(64, Byte));
      equal(P.Predicate, P.Left.load(Address, 1), Entry,
            "entry frame byte in left execution", Status::ContractViolation);
      equal(P.Predicate, P.Right.load(Address, 1), Entry,
            "entry frame byte in right execution", Status::ContractViolation);
    }
  }

  SymRef ordinary(SymRef Value) {
    // Map keys are expression indices, not symbolic variable IDs.
    const auto Answer = Ctx.substitute(Value, ZeroLeftChoices);
    nodes();
    return Answer;
  }

  std::optional<llvm::APInt> frameDifference(SymRef Predicate, SymRef Value) {
    uint64_t Queries = Result.SolverQueries;
    const auto Offset = detail::proveFrameOffset(
        Ctx, Predicate, Value, EntryRoot, Limits.Solver,
        Limits.MaxSolverQueries, Limits.MaxSymbolicNodes, Queries,
        &FrameProofs);
    Result.SolverQueries = static_cast<uint32_t>(Queries);
    if (Offset.Status == detail::FrameOffsetStatus::Invalid)
      fail(Status::Invalid, "invalid frame-offset proof");
    if (Offset.Status == detail::FrameOffsetStatus::BudgetExceeded)
      fail(Status::BudgetExceeded,
           "frame-offset proof exceeded its solver or symbolic-node budget");
    if (Offset.Status == detail::FrameOffsetStatus::Exact)
      return llvm::APInt(64, Offset.Offset);
    return std::nullopt;
  }

  void checkTemporary(const NdVar &V, const std::set<uint64_t> &Defined) {
    if (V.isTemp())
      for (uint16_t I = 0; I != V.Size; ++I)
        if (!Defined.count(V.Offset + I))
          fail(Status::Invalid, "instruction reads an unbound temporary");
  }

  void checkScratch(const NdVar &V) {
    if (V.isTemp() && V.Size &&
        (V.Offset >= AddressTemporary || AddressTemporary - V.Offset < V.Size))
      fail(Status::Invalid, "input temporary overlaps proof memory scratch");
  }

  void define(const NdVar &V, std::set<uint64_t> &Defined, Path *P = nullptr) {
    if (V.isTemp())
      for (uint16_t I = 0; I != V.Size; ++I) {
        Defined.insert(V.Offset + I);
        if (P && Function && Function->isFunctionTemporaryByte(V.Offset + I))
          P->DefinedFunctionTemporaries.insert(V.Offset + I);
      }
  }

  bool flagIntrinsic(Path &P, const LowOp &Original,
                     const LowInstructionUndefinedEffects &Effects,
                     const std::set<uint64_t> &Defined) {
    if ((!Provider && !CandidateExecution) || !Contract.X64FlagsProfile ||
        Original.Opcode != NdOp::INTRINSIC)
      return false;
    // A separate state namespace prevents either original operands or public
    // register contracts from aliasing the implicit system-state identity.
    constexpr auto SystemOffset = uint64_t{0}, ImageOffset = uint64_t{8};
    LowOp Canonical = Original;
    if (Canonical.Output.Size)
      Canonical.Output.Offset = 0;
    if (Canonical.NumInputs == 2)
      Canonical.Inputs[1] = NdVar::reg(ImageOffset, Original.Inputs[1].Size);
    uint64_t NextTemporary = 8;
    auto Transition = detail::lowerX64UserFlags(
        Canonical, NdVar::reg(SystemOffset, 8), [&](uint16_t Size) {
          const auto Value = NdVar::tmp(NextTemporary, Size);
          NextTemporary += 8;
          return Value;
        });
    if (!Transition)
      fail(Status::Unsupported, "unsupported native flags intrinsic shape");
    if (!Effects.Effects.empty())
      fail(Status::Unsupported,
           "native flags transition requires an empty audited sidecar");
    if (Original.Output.Size && !scalar(Original.Output))
      fail(Status::Invalid, "invalid native flags output");
    for (unsigned I = 0; I != Original.NumInputs; ++I) {
      if (!scalar(Original.Inputs[I]))
        fail(Status::Invalid, "invalid native flags operand");
      checkTemporary(Original.Inputs[I], Defined);
    }
    if (Transition->Ops.size() > Limits.MaxOperations - Result.Operations)
      fail(Status::BudgetExceeded, "native flags transition budget exhausted");
    Result.Operations += Transition->Ops.size();
    const auto Execute = [&](SymState &State, SymRef System) {
      SymState Isolated(Ctx, Contract.ByteOrder);
      Isolated.write(SymSpace::Register, SystemOffset, System);
      if (Original.NumInputs == 2) {
        SymExec Reader(Ctx, State);
        Isolated.write(SymSpace::Register, ImageOffset,
                       Reader.operandValue(Original.Inputs[1]));
      }
      SymExec Exec(Ctx, Isolated);
      for (const auto &Op : Transition->Ops) {
        if (Exec.step(Op) != StepResult::Continue || Exec.unmodelledCount() ||
            Exec.opaqueOperationCount() || Exec.memoryHavocCount() ||
            Exec.callHavocCount())
          fail(Status::Unsupported, "native flags transition lost semantics");
        nodes();
      }
      return Isolated;
    };
    auto Left = Execute(P.Left, P.LeftSystemFlags);
    auto Right = Execute(P.Right, P.RightSystemFlags);
    if (Transition->Rejected) {
      const auto Bad = *Transition->Rejected;
      const auto L = Left.read(SymSpace::Temporary, Bad.Offset, Bad.Size);
      const auto R = Right.read(SymSpace::Temporary, Bad.Offset, Bad.Size);
      if (query(Ctx.mkAnd(P.Predicate, Ctx.mkOr(Ctx.mkNe(L, Ctx.mkZero(8)),
                                                Ctx.mkNe(R, Ctx.mkZero(8))))) !=
          solver::SatResult::Unsat)
        fail(Status::ContractViolation,
             "reachable POPFQ image violates the native flags profile");
    }
    // Never restrict Predicate by the profile guard: all feasible twins must
    // satisfy it. CALL/RET and branch scheduling copy these persistent fields.
    P.LeftSystemFlags = Left.read(SymSpace::Register, SystemOffset, 8);
    P.RightSystemFlags = Right.read(SymSpace::Register, SystemOffset, 8);
    if (Original.Output.Size) {
      P.Left.write(SymSpace::Temporary, Original.Output.Offset,
                   Left.read(SymSpace::Temporary, 0, 8));
      P.Right.write(SymSpace::Temporary, Original.Output.Offset,
                    Right.read(SymSpace::Temporary, 0, 8));
    }
    NativeFlagTransitions.push_back(
        {P.BlockId, Original.Addr, Original.Seq, FlagProfile::SemanticsVersion,
         lowUndefinedOperationDigest(Transition->Ops)});
    return true;
  }

  void arbitrary(Path &P, const LowUndefinedEffect &Effect,
                 uint64_t EffectIndex, std::set<uint64_t> &Defined) {
    trackRegister(Effect.Output);
    if (++Result.Producers > Limits.MaxProducers)
      fail(Status::BudgetExceeded, "arbitrary-producer budget exhausted");
    const unsigned Width = Effect.Output.Size * 8;
    const bool Whole = Effect.BitOffset == 0 && Effect.BitCount == Width;
    if (!Whole || Effect.When)
      checkTemporary(Effect.Output, Defined);
    SymRef LeftWhen = Ctx.mkTrue(), RightWhen = Ctx.mkTrue();
    if (Effect.When) {
      checkTemporary(*Effect.When, Defined);
      SymExec L(Ctx, P.Left), R(Ctx, P.Right);
      const auto LW = L.operandValue(*Effect.When);
      const auto RW = R.operandValue(*Effect.When);
      if (query(Ctx.mkAnd(P.Predicate,
                          Ctx.mkOr(Ctx.mkUlt(Ctx.mkConst(8, 1), LW),
                                   Ctx.mkUlt(Ctx.mkConst(8, 1), RW)))) !=
          solver::SatResult::Unsat)
        fail(Status::Invalid, "arbitrary-effect guard is not Boolean");
      LeftWhen = Ctx.mkNe(LW, Ctx.mkZero(8));
      RightWhen = Ctx.mkNe(RW, Ctx.mkZero(8));
    }
    if (Refinement) {
      Refinement->Producers.push_back({Result.Instructions, P.BlockId,
                                       Result.InstructionAddress, EffectIndex});
      if (Refinement->Witness == LowIRRefinementWitness::LiftedBits) {
        // The already computed bits form a constructive choice at this exact
        // producer occurrence. Copies and spills keep that one choice. The
        // guard was checked above even though retaining the bits is an
        // identity.
        checkTemporary(Effect.Output, Defined);
        return;
      }
    }
    const auto U = Refinement
                       ? Ctx.mkZero(Effect.BitCount)
                       : Ctx.mkFreshVar(Effect.BitCount, "undefined_left");
    const auto V =
        Refinement ? U : Ctx.mkFreshVar(Effect.BitCount, "undefined_right");
    if (!Refinement)
      ZeroLeftChoices.emplace(U.index(), Ctx.mkZero(Effect.BitCount));
    const auto Mask = llvm::APInt::getBitsSet(
        Width, Effect.BitOffset, Effect.BitOffset + Effect.BitCount);
    const auto Apply = [&](SymState &State, SymRef Fresh, SymRef When) {
      auto New = Ctx.mkShl(Ctx.mkZExtOrTrunc(Fresh, Width),
                           Ctx.mkConst(Width, Effect.BitOffset));
      if (!Whole || Effect.When) {
        const auto Space =
            Effect.Output.isReg() ? SymSpace::Register : SymSpace::Temporary;
        const auto Old =
            State.read(Space, Effect.Output.Offset, Effect.Output.Size);
        New = Ctx.mkOr(Ctx.mkAnd(Old, Ctx.mkConst(~Mask)), New);
        New = Ctx.mkIte(When, New, Old);
      }
      State.write(Effect.Output.isReg() ? SymSpace::Register
                                        : SymSpace::Temporary,
                  Effect.Output.Offset, New);
    };
    Apply(P.Left, U, LeftWhen);
    Apply(P.Right, V, RightWhen);
    define(Effect.Output, Defined, &P);
    nodes();
  }

  bool supportedShape(const LowOp &Op) {
    unsigned Count = 0;
    bool Output = true;
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::INT_NEGATE:
    case NdOp::INT_NOT:
    case NdOp::INT_NEG2:
    case NdOp::BOOL_NOT:
    case NdOp::POPCOUNT:
    case NdOp::LZCOUNT:
      Count = 1;
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::INT_CARRY:
    case NdOp::INT_SOVF:
    case NdOp::INT_SBOR:
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
    case NdOp::SUBBYTES:
    case NdOp::CONCAT:
      Count = 2;
      break;
    case NdOp::SELECT:
    case NdOp::EXTRACT:
      Count = 3;
      break;
    case NdOp::INSERT:
      Count = 4;
      break;
    case NdOp::LOAD:
      Count = Op.NumInputs;
      if (Count != 1 && Count != 2)
        return false;
      break;
    case NdOp::STORE:
      Output = false;
      Count = Op.NumInputs;
      if (Count != 2 && Count != 3)
        return false;
      break;
    case NdOp::BRANCH:
    case NdOp::INDIR_BR:
      Output = false;
      Count = 1;
      break;
    case NdOp::COND_BR:
      Output = false;
      Count = 2;
      break;
    case NdOp::RETURN:
      Output = false;
      Count = Op.NumInputs;
      if (Count > 1)
        return false;
      break;
    case NdOp::NOP:
      Output = false;
      break;
    default:
      fail(Status::Unsupported,
           "unsupported LowIR operation: " + std::string(ndOpName(Op.Opcode)));
    }
    return Op.NumInputs == Count &&
           (Output ? scalar(Op.Output) && !Op.Output.isConst()
                   : Op.Output.Size == 0);
  }

  void schedule(Path P, int Destination, SymRef Predicate) {
    if (query(Predicate) == solver::SatResult::Unsat)
      return;
    if (!Blocks.count(Destination))
      fail(Status::Invalid, "control edge names a missing block");
    if (!Refinement && P.Ancestors.count(Destination))
      fail(Status::Unsupported, "reachable cycle requires a loop invariant");
    if (++ScheduledPaths > Limits.MaxPaths)
      fail(Status::BudgetExceeded, "acyclic path budget exhausted");
    P.BlockId = Destination;
    P.Predicate = Predicate;
    Pending.push_back(std::move(P));
  }

  void validateEffects(const LowInstructionBoundary &B,
                       llvm::ArrayRef<LowOp> Ops,
                       const LowInstructionUndefinedEffects &D) {
    if (D.OpCount != B.OpCount ||
        (D.Coverage == LowUndefinedCoverage::Complete &&
         (D.OperationDigest.empty() ||
          D.OperationDigest != lowUndefinedOperationDigest(Ops))))
      fail(Status::Invalid,
           "undefined-effect operation digest is stale or missing");
    if (D.Effects.size() > Limits.MaxProducers)
      fail(Status::BudgetExceeded, "input arbitrary-effect budget exhausted");
    for (const auto &E : D.Effects)
      if (E.AfterOp > B.OpCount || !scalar(E.Output) || E.Output.isConst() ||
          !E.BitCount || E.BitOffset >= E.Output.Size * 8 ||
          E.BitCount > E.Output.Size * 8 - E.BitOffset ||
          (E.When &&
           (!scalar(*E.When) || E.When->Size != 1 || E.When->isReg())))
        fail(Status::Invalid, "malformed architecture-arbitrary effect");
    for (const auto &E : D.Effects) {
      checkScratch(E.Output);
      if (E.When)
        checkScratch(*E.When);
    }
  }

  // Code and immutable data share one byte inventory across the native
  // execution and its candidate. Candidate LowIR addresses are only labels.
  // Callers bound the whole range's evidence before comparing any bytes.
  void retainImmutableBytes(va_t Address, llvm::ArrayRef<uint8_t> Bytes) {
    if (Bytes.empty() || Bytes.size() > InvalidVA - Address)
      fail(Status::Invalid, "invalid immutable byte range");
    for (size_t I = 0; I != Bytes.size(); ++I) {
      const auto [It, Inserted] = ImmutableBytes.emplace(Address + I, Bytes[I]);
      if (!Inserted && It->second != Bytes[I])
        fail(Status::Invalid, "immutable provider bytes changed");
    }
  }

  void collectNative(va_t Entry) {
    if (NativeInstructions.count(Entry))
      return;
    std::vector<va_t> Work{Entry};
    std::set<va_t> Queued{Entry};
    const auto Enqueue = [&](va_t Address) {
      if (NativeInstructions.count(Address) || !Queued.insert(Address).second)
        return;
      if (NativeInstructions.size() + Work.size() >= Limits.MaxInstructions)
        fail(Status::BudgetExceeded,
             "original instruction graph budget exhausted");
      Work.push_back(Address);
    };
    while (!Work.empty()) {
      const va_t Address = Work.back();
      Work.pop_back();
      if (NativeInstructions.count(Address))
        continue;
      Result.InstructionAddress = Address;
      if (NativeInstructions.size() >= Limits.MaxInstructions ||
          NativeInstructions.size() >= Limits.MaxBlockVisits)
        fail(Status::BudgetExceeded,
             "original instruction graph budget exhausted");
      auto Fetched = Provider->instruction({Address, NativeEntry.Mode});
      if (!Fetched)
        fail(Status::Unsupported, llvm::toString(Fetched.takeError()));
      auto Insn = std::move(*Fetched);
      const auto &B = Insn.Origin;
      if (B.Address != Address || !B.Size || B.Size > InvalidVA - Address ||
          B.FirstOp || B.OpCount != Insn.Ops.size() ||
          Insn.NativeBytes.size() != B.Size ||
          Insn.Fallthrough.Address != Address + B.Size ||
          Insn.Fallthrough.Mode != NativeEntry.Mode)
        fail(Status::Invalid,
             "inconsistent original instruction boundary or bytes");
      if (B.Mode != InstructionMode::Default ||
          B.TargetMode != LowInstructionTargetMode::Preserve ||
          (B.Control != LowInstructionControl::None &&
           B.Control != LowInstructionControl::Branch &&
           B.Control != LowInstructionControl::Call &&
           B.Control != LowInstructionControl::Return &&
           B.Control != LowInstructionControl::Terminator) ||
          hasLowInstructionControlFlag(
              B.ControlFlags, LowInstructionControlFlag::InstructionGuard))
        fail(Status::Unsupported, "unsupported original instruction control");
      if (Contract.AllowOverlappingNativeInstructions) {
        if (B.Size > Limits.MaxNativeInstructionBytes - NativeInputBytes)
          fail(Status::BudgetExceeded,
               "original instruction byte budget exhausted");
        NativeInputBytes += B.Size;
        retainImmutableBytes(Address, Insn.NativeBytes);
      } else {
        auto Next = NativeRanges.lower_bound(Address);
        if ((Next != NativeRanges.end() && Next->first < Address + B.Size) ||
            (Next != NativeRanges.begin() && std::prev(Next)->second > Address))
          fail(Status::Unsupported, "overlapping original instruction ranges");
      }
      NativeRanges.emplace(Address, Address + B.Size);
      if (Insn.Ops.size() > Limits.MaxOperations - NativeInputOperations)
        fail(Status::BudgetExceeded, "original operation budget exhausted");
      NativeInputOperations += Insn.Ops.size();
      for (const auto &Op : Insn.Ops)
        if (Op.NumInputs > 6)
          fail(Status::Invalid, "operand capacity exceeded");
      for (const auto &Op : Insn.Ops) {
        checkScratch(Op.Output);
        for (unsigned I = 0; I != Op.NumInputs; ++I)
          checkScratch(Op.Inputs[I]);
      }
      LowBlock Raw;
      Raw.StartAddr = Address;
      Raw.EndAddr = Address + B.Size;
      Raw.Ops = Insn.Ops;
      Raw.InstructionBoundaries.push_back(B);
      if (auto Error = validateLowInstructionBoundaries(
              Raw, LowInstructionBoundaryRequirement::Required))
        fail(Status::Invalid, llvm::toString(std::move(Error)));
      validateEffects(B, Insn.Ops, Insn.UndefinedEffects);
      const bool Projection =
          Contract.X64FlagsProfile && x64::isCetDisabledProjection(Insn);
      const bool ProjectedTrap =
          Projection && Insn.ProfileProjection ==
                            InterpreterProfileProjection::
                                CetDisabledIncrementShadowStackTrapV1;
      const bool Trap = x64::isRetainedNativeTrap(Insn) || ProjectedTrap;
      if (Insn.ProfileProjection != InterpreterProfileProjection::None &&
          !Projection)
        fail(Status::Unsupported,
             "native projection lacks matching profile or exact evidence");
      if (B.Control == LowInstructionControl::Terminator && !Trap)
        fail(Status::Unsupported,
             "original trap lacks exact semantic evidence");
      if (Insn.UndefinedEffects.Effects.size() >
          Limits.MaxProducers - NativeInputEffects)
        fail(Status::BudgetExceeded, "input arbitrary-effect budget exhausted");
      NativeInputEffects += Insn.UndefinedEffects.Effects.size();
      const bool Unaudited =
          !Trap && !Projection &&
          Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Complete;
      if (Unaudited) {
        if (!Contract.RetainUnauditedNativeBoundaries ||
            Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Missing)
          fail(Status::Unsupported,
               "original instruction lacks complete undefined-output evidence");
        if (!Insn.UndefinedEffects.Effects.empty() ||
            Insn.UndefinedEffects.OperationDigest.empty() ||
            Insn.UndefinedEffects.OperationDigest !=
                lowUndefinedOperationDigest(Insn.Ops))
          fail(Status::Invalid,
               "unaudited native boundary has stale or partial evidence");
      }
      std::vector<va_t> Successors;
      bool Terminal = Trap;
      for (size_t I = 0; I != Insn.Ops.size(); ++I) {
        const auto &Op = Insn.Ops[I];
        const bool Transfer =
            Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::BRANCH ||
            Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::INDIR_BR;
        if (!Transfer)
          continue;
        if (Terminal || I + 1 != Insn.Ops.size())
          fail(Status::Unsupported, "original control is not terminal");
        Terminal = true;
        if (Op.Opcode == NdOp::RETURN) {
          if (Insn.NativeStackControl !=
              SpecializationNativeStackControl::Return)
            fail(Status::Unsupported, "missing physical near-return evidence");
        } else {
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
            if (Insn.NativeStackControl !=
                SpecializationNativeStackControl::Call)
              fail(Status::Unsupported, "missing physical near-call evidence");
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::BRANCH ||
              Op.Opcode == NdOp::COND_BR) {
            if (!Op.NumInputs || !Op.Inputs[0].isConst())
              fail(Status::Invalid, "direct control target is not constant");
            if (Op.Opcode != NdOp::COND_BR ||
                !Contract.DeferNativeConditionalEdges)
              Successors.push_back(Op.Inputs[0].Offset);
          }
          // A physical call only transfers to its callee. Its fallthrough is
          // the pushed return value, not a control edge: a callee may modify
          // or discard that slot. scheduleNative collects every actual RET
          // destination after complete target enumeration, under the same
          // evidence and resource checks as any other reached instruction.
          // The opt-in policy collects both conditional arms through
          // scheduleNative only after paired control equality and feasibility.
          // No bytes, boundary or semantics are asserted for a skipped arm.
          if (Op.Opcode == NdOp::COND_BR &&
              !Contract.DeferNativeConditionalEdges)
            Successors.push_back(Insn.Fallthrough.Address);
        }
      }
      if (!Terminal)
        Successors.push_back(Insn.Fallthrough.Address);
      if (ProjectedTrap)
        NativeProfileProjections.push_back(
            {-1, Address, Insn.ProfileProjection});
      if (Unaudited) {
        LowIRNativeAuditBoundary Receipt;
        Receipt.Boundary = B;
        Receipt.NativeBytesDigest =
            llvm::toHex(llvm::SHA256::hash(Insn.NativeBytes));
        Receipt.OperationDigest = Insn.UndefinedEffects.OperationDigest;
        NativeAuditBoundaries.emplace(Address, std::move(Receipt));
      }
      NativeInstructions.emplace(Address, std::move(Insn));
      // Only successor collection stops. Every structural check above still
      // applies, and another edge may independently enter the following bytes.
      if (!Unaudited)
        for (va_t Target : Successors)
          Enqueue(Target);
    }
    // Collection visits each original instruction once, including cyclic
    // arms. Execution creates a fresh trace block on every visit, preserving
    // state and fresh undefined choices. Direct and indirect cycles obey the
    // same path/visit/operation budgets; only exhaustion of all feasible paths
    // at ordinary returns can certify a finite unrolling, never a prefix.
  }

  // The caller first proves two-execution address equality. Enumerate its
  // ordinary projection without assuming a candidate, and require the final
  // no-more-values proof before reading any bytes. A shared ITE retains the
  // input-dependent selection; equal data never excuses a dependent address.
  SymRef immutableLoad(Path &P, SymRef Address, uint16_t Bytes,
                       va_t Instruction, int Sequence) {
    std::vector<uint64_t> Addresses;
    if (const auto Absolute = Ctx.asConst(Address)) {
      if (Absolute->getBitWidth() != 64)
        fail(Status::Invalid, "immutable load address must be 64 bits");
      Addresses.push_back(Absolute->getZExtValue());
    } else {
      uint64_t Queries = Result.SolverQueries;
      const auto Values = detail::enumerateFiniteValues(
          Ctx, P.Predicate, {Address}, Limits.MaxImmutableLoadAddresses,
          Limits.Solver, Limits.MaxSolverQueries, Limits.MaxSymbolicNodes,
          Queries);
      Result.SolverQueries = static_cast<uint32_t>(Queries);
      if (Values.Status != detail::FiniteValueStatus::Complete)
        fail(Values.Status == detail::FiniteValueStatus::Invalid
                 ? Status::Invalid
                 : Status::BudgetExceeded,
             "immutable load address enumeration is incomplete");
      if (Values.Tuples.empty())
        fail(Status::Invalid, "reachable immutable load has no address");
      for (const auto &Tuple : Values.Tuples) {
        if (Tuple.size() != 1)
          fail(Status::Invalid, "malformed immutable load address tuple");
        Addresses.push_back(Tuple.front());
      }
    }
    const auto &Frame = *Contract.Frame;
    const auto First = Ctx.mkAdd(
        EntryRoot, Ctx.mkConst(64, static_cast<uint64_t>(Frame.Begin)));
    const auto Last = Ctx.mkAdd(
        EntryRoot, Ctx.mkConst(64, static_cast<uint64_t>(Frame.End) - 1));
    SymRef Selected;
    for (uint64_t Candidate : Addresses) {
      if (Candidate > UINT64_MAX - Bytes)
        fail(Status::Unsupported, "immutable load address range wraps");
      const auto Disjoint =
          Ctx.mkOr(Ctx.mkUlt(Last, Ctx.mkConst(64, Candidate)),
                   Ctx.mkUle(Ctx.mkConst(64, Candidate + Bytes), First));
      if (query(Ctx.mkAnd(P.Predicate, Ctx.mkNot(Disjoint))) !=
          solver::SatResult::Unsat)
        fail(Status::Unsupported, "immutable read can alias the mutable frame");
      const auto Read = ReadProvider->immutableRead(Candidate, Bytes);
      if (!Read || Read->Bytes.size() != Bytes || Read->Evidence.empty())
        fail(Status::Unsupported, "missing immutable-read evidence");
      for (uint64_t Size : {Read->Bytes.size(), Read->Evidence.size()}) {
        if (Size > Limits.MaxOperations - NativeReadEvidenceBytes)
          fail(Status::BudgetExceeded,
               "immutable-read evidence budget exhausted");
        NativeReadEvidenceBytes += Size;
      }
      uint64_t Value = 0;
      retainImmutableBytes(Candidate, Read->Bytes);
      for (uint16_t I = 0; I != Bytes; ++I) {
        const unsigned Shift =
            Contract.ByteOrder == llvm::endianness::little ? I : Bytes - 1 - I;
        Value |= uint64_t{Read->Bytes[I]} << (Shift * 8);
      }
      NativeResult->Reads.push_back(
          {Instruction, Sequence, Candidate, Read->Bytes, Read->Evidence});
      const auto Constant = Ctx.mkConst(Bytes * 8, Value);
      // The first value is a default only outside the exhaustively proved
      // address set. That case is unreachable under this path's predicate.
      Selected = Selected
                     ? Ctx.mkIte(Ctx.mkEq(Address, Ctx.mkConst(64, Candidate)),
                                 Constant, Selected)
                     : Constant;
      nodes();
    }
    return Selected;
  }

  void scheduleNative(Path P, va_t Address, SymRef Predicate) {
    if (query(Predicate) == solver::SatResult::Unsat)
      return;
    collectNative(Address);
    if (++ScheduledPaths > Limits.MaxPaths || NextNativeBlock == INT_MAX)
      fail(Status::BudgetExceeded, "native path budget exhausted");
    const int Parent = P.BlockId;
    P.BlockId = NextNativeBlock++;
    if (Parent >= 0)
      NativeTraceEdges[Parent].push_back(P.BlockId);
    P.NativeAddress = Address;
    P.Predicate = Predicate;
    Pending.push_back(std::move(P));
  }

  void nativeTargets(Path P, SymRef Value, SymRef Predicate) {
    if (!Value || Ctx.width(Value) != 64)
      fail(Status::Invalid, "native control target must be a 64-bit address");
    const auto Target = ordinary(Value);
    // A structural constant has no other possible destination. Scheduling
    // still proves feasibility and audits the actual mapped instruction.
    if (const auto Constant = Ctx.asConst(Target)) {
      scheduleNative(std::move(P), Constant->getZExtValue(), Predicate);
      return;
    }
    uint64_t Queries = Result.SolverQueries;
    const auto Values = detail::enumerateFiniteValues(
        Ctx, Predicate, {Target}, Limits.MaxIndirectTargets, Limits.Solver,
        Limits.MaxSolverQueries, Limits.MaxSymbolicNodes, Queries);
    Result.SolverQueries = static_cast<uint32_t>(Queries);
    if (Values.Status != detail::FiniteValueStatus::Complete)
      fail(Values.Status == detail::FiniteValueStatus::Invalid
               ? Status::Invalid
               : Status::BudgetExceeded,
           "native control target enumeration is incomplete");
    for (const auto &Tuple : Values.Tuples) {
      if (Tuple.size() != 1)
        fail(Status::Invalid, "invalid native target tuple");
      // Complete singleton enumeration already proves this equality over the
      // entire incoming domain. Retaining it would make later feasibility and
      // terminal coverage queries prove the same fact again. Multiple targets
      // still need their separate guards, and Predicate retains branch guards.
      const auto TargetPredicate =
          Values.Tuples.size() == 1
              ? Predicate
              : Ctx.mkAnd(Predicate,
                          Ctx.mkEq(Target, Ctx.mkConst(64, Tuple[0])));
      scheduleNative(P, Tuple[0], TargetPredicate);
    }
  }

  LowBlock prepareNative(Path &P, LowInstructionUndefinedEffects &Effects) {
    const auto &Insn = NativeInstructions.at(P.NativeAddress);
    if (Insn.Origin.Control == LowInstructionControl::Terminator) {
      Result.BlockId = P.BlockId;
      Result.InstructionAddress = P.NativeAddress;
      Result.OpSeq = -1;
      fail(Status::ContractViolation,
           "feasible native trap violates the nonfaulting execution contract");
    }
    LowBlock B;
    B.Id = P.BlockId;
    B.StartAddr = P.NativeAddress;
    B.EndAddr = P.NativeAddress + Insn.Origin.Size;
    B.Ops = Insn.Ops;
    B.InstructionBoundaries.push_back(Insn.Origin);
    Effects = Insn.UndefinedEffects;
    if (Insn.ProfileProjection != InterpreterProfileProjection::None)
      NativeProfileProjections.push_back(
          {B.Id, B.StartAddr, Insn.ProfileProjection});
    if (Insn.NativeStackControl != SpecializationNativeStackControl::None ||
        Insn.IsNativeCall) {
      NativeReturnExpansion Mode = NativeReturnExpansion::OuterFunctionBoundary;
      if (Insn.NativeStackControl == SpecializationNativeStackControl::Return) {
        const auto &Root = Contract.Frame->RootRegister;
        const auto Left = P.Left.read(SymSpace::Register, Root.Offset, 8);
        const auto Right = P.Right.read(SymSpace::Register, Root.Offset, 8);
        equal(P.Predicate, Left, Right, "native return stack pointer");
        const auto Offset = frameDifference(P.Predicate, ordinary(Left));
        if (!Offset)
          fail(Status::Unsupported,
               "native return has no exact entry-relative stack pointer");
        if (!Offset->isZero())
          Mode = NativeReturnExpansion::InternalTransfer;
      }
      auto Expanded = expandNativeStackControl(
          Insn, NdVar::reg(Contract.Frame->RootRegister.Offset, 8),
          NdVar::tmp(NativeStackTemporary, 8), Mode,
          {NdVar::tmp(AddressTemporary, 8)});
      if (!Expanded)
        fail(Status::Invalid, llvm::toString(Expanded.takeError()));
      B.Ops = std::move(Expanded->Ops);
      B.InstructionBoundaries.front() = Expanded->Boundary;
      Effects = std::move(Expanded->UndefinedEffects);
    }
    if (auto Error = validateLowInstructionBoundaries(
            B, LowInstructionBoundaryRequirement::Required))
      fail(Status::Invalid, llvm::toString(std::move(Error)));
    validateEffects(B.InstructionBoundaries.front(), B.Ops, Effects);
    NativeRecords.push_back({B.Id, B.InstructionBoundaries.front(), Effects});
    NativeTrace.Blocks.push_back(B);
    return B;
  }

  int target(const LowBlock &B, SymRef Value) {
    const auto Number = Ctx.asConst(ordinary(Value));
    if (!Number || Number->getActiveBits() > 64)
      fail(Status::Unsupported, "control target is not a constant block entry");
    const auto Found = Addresses.find(Number->getZExtValue());
    if (Found == Addresses.end() || !B.hasSucc(Found->second))
      fail(Status::Invalid, "control target disagrees with the LowIR CFG");
    return Found->second;
  }

  void runPath(Path P) {
    // This gate precedes cutpoint handling, expansion, effects and execution.
    // Only a solver-proved-infeasible edge can avoid a retained boundary.
    if (Provider && NativeAuditBoundaries.count(P.NativeAddress)) {
      Result.BlockId = P.BlockId;
      Result.InstructionAddress = P.NativeAddress;
      Result.OpSeq = -1;
      fail(Status::Unsupported,
           "feasible path reaches an unaudited native boundary");
    }
    if (stopAtCutpoint(P))
      return;
    if (++Result.BlockVisits > Limits.MaxBlockVisits)
      fail(Status::BudgetExceeded, "block-visit budget exhausted");
    LowInstructionUndefinedEffects NativeEffects;
    std::optional<LowBlock> NativeBlock;
    if (Provider)
      NativeBlock = prepareNative(P, NativeEffects);
    const auto &B = NativeBlock ? *NativeBlock : *Blocks.at(P.BlockId);
    Result.BlockId = B.Id;
    // Only static independence uses ancestry to reject a reachable cycle.
    if (!Provider && !Refinement)
      P.Ancestors.insert(B.Id);
    if (!B.ExceptionalSuccs.empty() || !B.ExceptionalPreds.empty())
      fail(Status::Unsupported, "exceptional control flow is unsupported");
    SymExec Left(Ctx, P.Left), Right(Ctx, P.Right);
    for (const auto &Boundary : B.InstructionBoundaries) {
      if (++Result.Instructions > Limits.MaxInstructions)
        fail(Status::BudgetExceeded, "instruction-visit budget exhausted");
      Result.InstructionAddress = Boundary.Address;
      Result.OpSeq = -1;
      if (Boundary.Mode != InstructionMode::Default ||
          Boundary.TargetMode != LowInstructionTargetMode::Preserve ||
          (Boundary.Control != LowInstructionControl::None &&
           Boundary.Control != LowInstructionControl::Branch &&
           Boundary.Control != LowInstructionControl::Return) ||
          hasLowInstructionControlFlag(
              Boundary.ControlFlags,
              LowInstructionControlFlag::InstructionGuard))
        fail(Status::Unsupported,
             "instruction mode or local control guard is unsupported");
      const auto Record = Effects.find({B.Id, Boundary.Address});
      const auto *DescriptionPointer = Provider ? &NativeEffects
                                       : Record == Effects.end()
                                           ? nullptr
                                           : &Record->second->Effects;
      const bool ProfileProjection =
          Provider && Contract.X64FlagsProfile &&
          x64::isCetDisabledProjection(NativeInstructions.at(B.StartAddr));
      if (!DescriptionPointer ||
          (DescriptionPointer->Coverage == LowUndefinedCoverage::Missing &&
           !ProfileProjection))
        fail(Status::Unsupported,
             "missing architectural undefined-effect coverage");
      const auto &Description = *DescriptionPointer;
      if (Description.Coverage != LowUndefinedCoverage::Complete &&
          !ProfileProjection)
        fail(Status::Unsupported,
             "unsupported architectural undefined effects: " +
                 Description.Diagnostic);
      // Function-local storage starts unbound. Only bytes actually defined on
      // this path survive an instruction boundary; native lifter temporaries
      // retain their original instruction-local lifetime.
      std::set<uint64_t> Defined = P.DefinedFunctionTemporaries;
      std::multimap<uint64_t, const LowUndefinedEffect *> Events;
      for (const auto &Effect : Description.Effects)
        Events.emplace(Effect.AfterOp, &Effect);
      const auto Apply = [&](uint64_t Completed) {
        const auto Range = Events.equal_range(Completed);
        for (auto It = Range.first; It != Range.second; ++It)
          arbitrary(
              P, *It->second,
              static_cast<uint64_t>(It->second - Description.Effects.data()),
              Defined);
      };
      Apply(0);
      for (uint64_t I = 0; I != Boundary.OpCount; ++I) {
        const auto &Original = B.Ops[Boundary.FirstOp + I];
        trackRegister(Original.Output);
        for (unsigned J = 0; J != Original.NumInputs; ++J)
          trackRegister(Original.Inputs[J]);
        Result.OpSeq = Original.Seq;
        if (++Result.Operations > Limits.MaxOperations)
          fail(Status::BudgetExceeded, "LowIR operation budget exhausted");
        if (Original.MemoryOrdering != NdMemoryOrdering::None ||
            Original.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          fail(Status::Unsupported,
               "ordered or nondefault memory is unsupported");
        if (flagIntrinsic(P, Original, Description, Defined)) {
          define(Original.Output, Defined, &P);
          Apply(I + 1);
          nodes();
          continue;
        }
        if (!supportedShape(Original))
          fail(Status::Invalid, "malformed LowIR operation");
        for (unsigned J = 0; J != Original.NumInputs; ++J) {
          if (!scalar(Original.Inputs[J]))
            fail(Status::Invalid, "malformed scalar operand");
          checkTemporary(Original.Inputs[J], Defined);
        }
        LowOp Op = Original;
        const bool Memory = Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE;
        const bool Store = Op.Opcode == NdOp::STORE;
        if (Memory) {
          if (!Contract.Frame)
            fail(Status::Unsupported,
                 "memory access has no exact frame contract");
          const auto View = lowMemoryOperands(Original);
          if (!View.Complete || View.Address->Size != 8 || !View.AccessSize ||
              ((Op.NumInputs == (Store ? 3 : 2)) &&
               (!Op.Inputs[0].isConst() || Op.Inputs[0].Offset != 0)))
            fail(Status::Unsupported, "unsupported memory operand shape");
          const auto A = Left.operandValue(*View.Address);
          const auto Other = Right.operandValue(*View.Address);
          equal(P.Predicate, A, Other, "memory address");
          const auto Difference = frameDifference(P.Predicate, ordinary(A));
          const auto &Frame = *Contract.Frame;
          const bool InFrame =
              Difference && Difference->getBitWidth() == 64 &&
              Difference->getSExtValue() >= Frame.Begin &&
              Difference->getSExtValue() < Frame.End &&
              static_cast<uint64_t>(Frame.End) - Difference->getZExtValue() >=
                  View.AccessSize;
          if (!InFrame && ReadProvider && !Store &&
              (!Difference || Ctx.asConst(ordinary(A)))) {
            const auto Value = immutableLoad(P, ordinary(A), View.AccessSize,
                                             Boundary.Address, Original.Seq);
            P.Left.write(SymSpace::Temporary, AddressTemporary, Value);
            P.Right.write(SymSpace::Temporary, AddressTemporary, Value);
            Op.Opcode = NdOp::COPY;
            Op.NumInputs = 1;
            Op.Inputs[0] = NdVar::tmp(AddressTemporary, View.AccessSize);
          } else {
            if (!Difference || Difference->getBitWidth() != 64)
              fail(Status::Unsupported,
                   "memory address is not an exact entry-frame offset");
            const int64_t Offset = Difference->getSExtValue();
            if (Offset < Frame.Begin || Offset >= Frame.End ||
                static_cast<uint64_t>(Frame.End) -
                        static_cast<uint64_t>(Offset) <
                    View.AccessSize)
              fail(Status::Unsupported,
                   "memory access exceeds the certified frame");
            const uint64_t Index = static_cast<uint64_t>(Offset) -
                                   static_cast<uint64_t>(Frame.Begin);
            // Nonnegative proof-memory coordinates also handle a guest access
            // straddling entry-SP without overflowing SymState's offset bank.
            const auto Address = Ctx.mkAdd(MemoryRoot, Ctx.mkConst(64, Index));
            P.Left.write(SymSpace::Temporary, AddressTemporary, Address);
            P.Right.write(SymSpace::Temporary, AddressTemporary, Address);
            Op.Inputs[View.Address - Original.Inputs] =
                NdVar::tmp(AddressTemporary, 8);
            if (Store)
              for (uint16_t Byte = 0; Byte != View.AccessSize; ++Byte)
                P.Written.insert(Index + Byte);
          }
        }
        const auto LU = Left.unmodelledCount(), RU = Right.unmodelledCount();
        const auto LM = Left.memoryHavocCount(), RM = Right.memoryHavocCount();
        const auto LO = Left.opaqueOperationCount(),
                   RO = Right.opaqueOperationCount();
        const auto LF = Left.step(Op), RF = Right.step(Op);
        // A symbolic store reports conservative havoc of OTHER regions. All
        // accesses here were certified and normalized to one sole region, so
        // that specific increment loses no reachable proof-memory fact.
        const unsigned Expected = Store ? 1 : 0;
        if (LF == StepResult::Unmodelled || RF != LF ||
            Left.unmodelledCount() != LU + Expected ||
            Right.unmodelledCount() != RU + Expected ||
            Left.memoryHavocCount() != LM + Expected ||
            Right.memoryHavocCount() != RM + Expected ||
            Left.opaqueOperationCount() != LO ||
            Right.opaqueOperationCount() != RO || Left.callHavocCount() ||
            Right.callHavocCount())
          fail(Status::Unsupported, "symbolic execution lost exact semantics");
        define(Original.Output, Defined, &P);
        Apply(I + 1);
        nodes();
        if (LF == StepResult::Continue)
          continue;
        if (Boundary.FirstOp + I + 1 != B.Ops.size())
          fail(Status::Unsupported,
               "control transfer is not at the block boundary");
        if (LF == StepResult::Return) {
          if (!B.Succs.empty())
            fail(Status::Invalid, "return block has successors");
          preservedReturn(P);
          if (Contract.X64FlagsProfile)
            equal(P.Predicate, P.LeftSystemFlags, P.RightSystemFlags,
                  "final system flags");
          if (Original.NumInputs)
            equal(P.Predicate, Left.branchTarget(), Right.branchTarget(),
                  "RETURN operand");
          for (const auto &Register : Contract.ReturnRegisters)
            equal(P.Predicate,
                  P.Left.read(SymSpace::Register, Register.Offset,
                              Register.Bytes),
                  P.Right.read(SymSpace::Register, Register.Offset,
                               Register.Bytes),
                  "return register");
          if (Contract.ObserveWrittenFrameBytes)
            for (uint64_t Byte : P.Written) {
              const auto Address = Ctx.mkAdd(MemoryRoot, Ctx.mkConst(64, Byte));
              equal(P.Predicate, P.Left.load(Address, 1),
                    P.Right.load(Address, 1), "written frame byte");
            }
          ++Result.Paths;
          if (Refinement) {
            auto &Returns = CandidateExecution ? Refinement->CandidateReturns
                                               : Refinement->OriginalReturns;
            Returns.push_back(
                {std::move(P.Left), P.Predicate, P.LeftSystemFlags,
                 Original.NumInputs ? Left.branchTarget() : SymRef{},
                 std::move(P.Written), -1,
                 std::move(P.DefinedFunctionTemporaries)});
            ++(CandidateExecution ? Refinement->CandidatePathCount
                                  : Refinement->OriginalPathCount);
          }
          return;
        }
        equal(P.Predicate, Left.branchTarget(), Right.branchTarget(),
              "control target");
        if (Provider) {
          if (LF == StepResult::CondBranch) {
            equal(P.Predicate, Left.branchCondition(), Right.branchCondition(),
                  "branch predicate");
            const auto Condition = ordinary(Left.branchCondition());
            const auto Incoming = P.Predicate;
            const auto Taken = Ctx.mkAnd(Incoming, Condition);
            const auto Other = Ctx.mkAnd(Incoming, Ctx.mkNot(Condition));
            const auto Fallthrough =
                NativeInstructions.at(B.StartAddr).Fallthrough.Address;
            // A completed UNSAT proof for one edge proves that its complement
            // covers the incoming domain. Keep that domain without making
            // later queries reprove this branch fact. Unknown still refuses,
            // and two feasible edges retain their distinct predicates.
            if (query(Taken) == solver::SatResult::Unsat) {
              scheduleNative(std::move(P), Fallthrough, Incoming);
            } else if (query(Other) == solver::SatResult::Unsat) {
              nativeTargets(std::move(P), Left.branchTarget(), Incoming);
            } else {
              nativeTargets(P, Left.branchTarget(), Taken);
              scheduleNative(std::move(P), Fallthrough, Other);
            }
          } else {
            const auto Predicate = P.Predicate;
            nativeTargets(std::move(P), Left.branchTarget(), Predicate);
          }
          return;
        }
        const int Taken = target(B, Left.branchTarget());
        if (LF == StepResult::CondBranch) {
          // Never constrain this guard until its two-world equality is proved.
          equal(P.Predicate, Left.branchCondition(), Right.branchCondition(),
                "branch predicate");
          int Other = Taken;
          for (int S : B.Succs)
            if (S != Taken) {
              if (Other != Taken)
                fail(Status::Invalid,
                     "conditional branch has extra successors");
              Other = S;
            }
          const auto Condition = ordinary(Left.branchCondition());
          const auto TakenPredicate = Ctx.mkAnd(P.Predicate, Condition);
          const auto OtherPredicate =
              Ctx.mkAnd(P.Predicate, Ctx.mkNot(Condition));
          schedule(P, Taken, TakenPredicate);
          schedule(std::move(P), Other, OtherPredicate);
        } else {
          if (B.Succs.size() != 1)
            fail(Status::Invalid, "unconditional branch has extra successors");
          const auto Predicate = P.Predicate;
          schedule(std::move(P), Taken, Predicate);
        }
        return;
      }
    }
    if (Provider) {
      const auto Predicate = P.Predicate;
      scheduleNative(std::move(P),
                     NativeInstructions.at(B.StartAddr).Fallthrough.Address,
                     Predicate);
      return;
    }
    if (B.Succs.size() != 1)
      fail(Status::Unsupported, "fallthrough has no unique successor");
    const auto Predicate = P.Predicate;
    schedule(std::move(P), B.Succs.front(), Predicate);
  }

  void validate() {
    // Both inductive segments and finite executions use the same native
    // collector. Candidate inference needs an actual image reader as well;
    // semantic LowIR labels alone cannot authorize native collection policy.
    const bool Native = Provider || (CandidateExecution && ReadProvider);
    const bool FiniteNative =
        (Provider || (CandidateExecution && ReadProvider && Refinement &&
                      Refinement->NativeFinite)) &&
        (!Refinement || (Refinement->NativeFinite && !Refinement->LoopPlan &&
                         Refinement->PrefixSearchCutpoint < 0));
    if (Contract.RetainUnauditedNativeBoundaries && !Native)
      fail(Status::Unsupported,
           "unaudited boundaries require a native proof provider");
    if (Contract.AllowOverlappingNativeInstructions && !FiniteNative)
      fail(Status::Unsupported,
           "overlapping instructions require the finite native proof API");
    if (Contract.DeferNativeConditionalEdges && !Native)
      fail(Status::Unsupported,
           "deferred conditional edges require a native proof provider");
    if (Contract.X64FlagsProfile) {
      if (!Provider && !CandidateExecution)
        fail(Status::Unsupported, "flags profiles require the native API");
      if (*Contract.X64FlagsProfile !=
          InterpreterMachineStateProfile::UserX64NoFaultV1)
        fail(Status::Invalid, "unknown native flags profile");
      if (Contract.ByteOrder != llvm::endianness::little)
        fail(Status::Invalid, "native flags profile requires little endian");
    }
    if (!Limits.Solver.Blast.MaxGates || !Limits.Solver.Sat.MaxConflicts ||
        !Limits.Solver.Sat.MaxPropagations || !Limits.Solver.Sat.MaxWatchVisits)
      fail(Status::Invalid, "relational solver limits must be bounded");
    if (Contract.ByteOrder != llvm::endianness::little &&
        Contract.ByteOrder != llvm::endianness::big)
      fail(Status::Invalid, "invalid byte order");
    if (Provider &&
        (NativeEntry.Mode != InstructionMode::Default || !Contract.Frame ||
         Contract.Frame->RootRegister.Bytes != 8 || Contract.Frame->Begin > 0 ||
         Contract.Frame->End < 8 || !Limits.MaxIndirectTargets ||
         !Limits.MaxImmutableLoadAddresses))
      fail(Status::Invalid,
           "native proof requires a stack frame and bounded targets");
    uint64_t InputOperations = 0;
    uint64_t InputInstructions = 0;
    uint64_t InputEdges = 0;
    if ((Function &&
         (Function->Blocks.size() > Limits.MaxBlockVisits ||
          Function->FunctionTemporaries.size() > Limits.MaxInstructions ||
          Function->ModuleAnalysisRoots.size() > Limits.MaxBlockVisits ||
          Function->OrdinaryModuleAnalysisRoots.size() >
              Limits.MaxBlockVisits)) ||
        Records.size() > Limits.MaxInstructions ||
        Contract.EntryConstants.size() > Limits.MaxInstructions ||
        Contract.ReturnRegisters.size() > Limits.MaxInstructions ||
        Contract.PreservedRegisters.size() > Limits.MaxInstructions ||
        Contract.PreservedFrameRanges.size() > Limits.MaxInstructions ||
        (Contract.Frame &&
         Contract.Frame->ExcludedAddressRanges.size() > Limits.MaxInstructions))
      fail(Status::BudgetExceeded, "input metadata budget exhausted");
    if (Function) {
      for (const auto &B : Function->Blocks) {
        Result.BlockId = B.Id;
        if (!Blocks.emplace(B.Id, &B).second ||
            !Addresses.emplace(B.StartAddr, B.Id).second)
          fail(Status::Invalid, "duplicate LowIR block identity");
        if (B.Ops.size() > Limits.MaxOperations -
                               std::min(InputOperations, Limits.MaxOperations))
          fail(Status::BudgetExceeded, "input operation budget exhausted");
        InputOperations += B.Ops.size();
        if (B.InstructionBoundaries.size() >
            Limits.MaxInstructions - InputInstructions)
          fail(Status::BudgetExceeded, "input instruction budget exhausted");
        InputInstructions += B.InstructionBoundaries.size();
        const uint64_t MaxEdges = uint64_t{Limits.MaxBlockVisits} * 2;
        for (size_t Count : {B.Succs.size(), B.ExceptionalSuccs.size(),
                             B.ExceptionalPreds.size()}) {
          if (Count > MaxEdges - InputEdges)
            fail(Status::BudgetExceeded, "input CFG edge budget exhausted");
          InputEdges += Count;
        }
        for (const auto &Boundary : B.InstructionBoundaries)
          if (!Boundaries
                   .emplace(std::make_pair(B.Id, Boundary.Address), &Boundary)
                   .second)
            fail(Status::Invalid, "duplicate instruction boundary identity");
        std::set<int> Unique;
        for (int S : B.Succs)
          if (!Unique.insert(S).second)
            fail(Status::Invalid, "duplicate CFG successor");
        for (const auto &Op : B.Ops)
          if (Op.NumInputs > 6)
            fail(Status::Invalid, "operand capacity exceeded");
        for (const auto &Op : B.Ops) {
          checkScratch(Op.Output);
          for (unsigned I = 0; I != Op.NumInputs; ++I)
            checkScratch(Op.Inputs[I]);
        }
      }
      for (const auto &B : Function->Blocks)
        for (int Successor : B.Succs)
          if (!Blocks.count(Successor))
            fail(Status::Invalid, "CFG successor names a missing block");
      if (auto Error = validateLowInstructionBoundaries(
              *Function, LowInstructionBoundaryRequirement::Required))
        fail(Status::Invalid, llvm::toString(std::move(Error)));
      for (const auto &Range : Function->FunctionTemporaries)
        checkScratch(NdVar::tmp(Range.Offset, Range.Bytes));
      for (va_t Root : Function->ModuleAnalysisRoots)
        if (Root != Function->Entry)
          fail(Status::Unsupported,
               "additional function entry roots are unsupported");
      for (va_t Root : Function->OrdinaryModuleAnalysisRoots)
        if (Root != Function->Entry)
          fail(Status::Unsupported,
               "additional function entry roots are unsupported");
      if (!Addresses.count(Function->Entry))
        fail(Status::Invalid, "missing function entry block");
      if (inductive())
        for (const auto &Cut : Refinement->LoopPlan->Cutpoints)
          if (!Addresses.count(CandidateExecution ? Cut.CandidateAddress
                                                  : Cut.OriginalAddress))
            fail(Status::Invalid, "loop cutpoint is not a LowIR block entry");
      uint64_t InputEffects = 0;
      for (const auto &R : Records) {
        Result.BlockId = R.BlockId;
        Result.InstructionAddress = R.Boundary.Address;
        const auto B = Blocks.find(R.BlockId);
        const auto IB = Boundaries.find({R.BlockId, R.Boundary.Address});
        if (B == Blocks.end() || IB == Boundaries.end() ||
            !sameBoundary(*IB->second, R.Boundary) ||
            !Effects.emplace(std::make_pair(R.BlockId, R.Boundary.Address), &R)
                 .second ||
            R.Effects.OpCount != R.Boundary.OpCount)
          fail(Status::Invalid,
               "undefined-effect certificate does not bind its instruction");
        validateEffects(R.Boundary,
                        llvm::ArrayRef<LowOp>(B->second->Ops)
                            .slice(R.Boundary.FirstOp, R.Boundary.OpCount),
                        R.Effects);
        if (R.Effects.Effects.size() > Limits.MaxProducers - InputEffects)
          fail(Status::BudgetExceeded,
               "input arbitrary-effect budget exhausted");
        InputEffects += R.Effects.Effects.size();
      }
    }
    for (const auto &R : Contract.ReturnRegisters)
      if (!scalar(NdVar::reg(R.Offset, R.Bytes)))
        fail(Status::Invalid, "invalid observed register range");
    std::set<uint64_t> Bound;
    for (const auto &C : Contract.EntryConstants) {
      if (!C.Location.isReg() || !scalar(C.Location))
        fail(Status::Invalid,
             "entry constants must bind valid physical registers");
      if (Contract.X64FlagsProfile)
        for (const auto &[Offset, Bit] : FlagProfile::Flags) {
          (void)Bit;
          if (Offset >= C.Location.Offset &&
              Offset - C.Location.Offset < C.Location.Size &&
              (C.Location.Offset != Offset || C.Location.Size != 1 ||
               C.Value > 1))
            fail(Status::Invalid,
                 "native entry flag constants must be canonical one-byte bits");
        }
      for (uint16_t I = 0; I != C.Location.Size; ++I)
        if (!Bound.insert(C.Location.Offset + I).second)
          fail(Status::Invalid, "overlapping entry constants");
    }
    if (Contract.Frame) {
      const auto &F = *Contract.Frame;
      if (F.RootRegister.Bytes != 8 ||
          !scalar(NdVar::reg(F.RootRegister.Offset, 8)) || F.Begin >= F.End)
        fail(Status::Invalid, "invalid exact frame contract");
      if (F.EntryAlignment && !F.EntryAlignment->valid())
        fail(Status::Invalid, "invalid entry frame alignment");
      FrameBytes =
          static_cast<uint64_t>(F.End) - static_cast<uint64_t>(F.Begin);
      if (FrameBytes > Limits.MaxFrameBytes)
        fail(Status::BudgetExceeded, "frame-byte budget exhausted");
      for (const auto &R : F.ExcludedAddressRanges)
        if (R.Begin >= R.End)
          fail(Status::Invalid, "invalid excluded frame address range");
    }
    uint64_t PreservedBytes = 0;
    const auto ChargePreservedBytes = [&](uint16_t Bytes) {
      if (Bytes > Limits.MaxObservations - PreservedBytes)
        fail(Status::BudgetExceeded,
             "entry preservation snapshot budget exhausted");
      PreservedBytes += Bytes;
    };
    std::set<uint64_t> PreservedRegisters;
    for (const auto &R : Contract.PreservedRegisters) {
      if (!scalar(NdVar::reg(R.Offset, R.Bytes)))
        fail(Status::Invalid, "invalid preserved register range");
      ChargePreservedBytes(R.Bytes);
      for (uint16_t I = 0; I != R.Bytes; ++I)
        if (!PreservedRegisters.insert(R.Offset + I).second)
          fail(Status::Invalid, "overlapping preserved register ranges");
    }
    if (!Contract.Frame && !Contract.PreservedFrameRanges.empty())
      fail(Status::Invalid,
           "preserved frame ranges require an exact frame contract");
    std::set<uint64_t> PreservedFrameBytes;
    for (const auto &R : Contract.PreservedFrameRanges) {
      const auto &F = *Contract.Frame;
      if (!R.Bytes || R.Offset < F.Begin || R.Offset >= F.End ||
          static_cast<uint64_t>(F.End) - static_cast<uint64_t>(R.Offset) <
              R.Bytes)
        fail(Status::Invalid, "preserved range exceeds the certified frame");
      ChargePreservedBytes(R.Bytes);
      const uint64_t First =
          static_cast<uint64_t>(R.Offset) - static_cast<uint64_t>(F.Begin);
      for (uint16_t I = 0; I != R.Bytes; ++I)
        if (!PreservedFrameBytes.insert(First + I).second)
          fail(Status::Invalid, "overlapping preserved frame ranges");
    }
    if (Provider)
      for (uint64_t I = 0; I != 8; ++I) {
        if (!PreservedRegisters.count(Contract.Frame->RootRegister.Offset +
                                      I) ||
            !PreservedFrameBytes.count(
                I - static_cast<uint64_t>(Contract.Frame->Begin)))
          fail(Status::Invalid,
               "native proof must preserve entry stack and return slot");
      }
  }

public:
  bool validateLoopPlan() {
    try {
      const auto &Cuts = Refinement->LoopPlan->Cutpoints;
      if (Cuts.empty())
        fail(Status::Invalid, "loop proof requires a nonempty cutpoint plan");
      if (Cuts.size() > Limits.MaxBlockVisits || Cuts.size() > INT_MAX)
        fail(Status::BudgetExceeded, "loop cutpoint metadata budget exhausted");
      std::set<va_t> OriginalAddresses, CandidateAddresses;
      uint64_t Metadata = 0, Operations = 0;
      const auto Charge = [&](uint64_t Count) {
        if (Count > Limits.MaxInstructions - Metadata)
          fail(Status::BudgetExceeded,
               "loop template metadata budget exhausted");
        Metadata += Count;
      };
      const auto Location = [&](const LowIRLoopLocation &L, LowIRLoopSide Side,
                                bool UseEntryPrefix) {
        if (!L.Bytes || L.Bytes > 8)
          fail(Status::Invalid, "invalid loop location width");
        switch (L.Space) {
        case LowIRLoopSpace::Register:
          if (!scalar(NdVar::reg(L.Offset, L.Bytes)))
            fail(Status::Invalid, "invalid loop register location");
          trackRegister(NdVar::reg(L.Offset, L.Bytes));
          break;
        case LowIRLoopSpace::Frame:
          if (!Contract.Frame ||
              std::bit_cast<int64_t>(L.Offset) < Contract.Frame->Begin ||
              std::bit_cast<int64_t>(L.Offset) >= Contract.Frame->End ||
              static_cast<uint64_t>(Contract.Frame->End) - L.Offset < L.Bytes)
            fail(Status::Invalid, "loop location exceeds the certified frame");
          break;
        case LowIRLoopSpace::SystemFlags:
          if (!Contract.X64FlagsProfile || L.Offset || L.Bytes != 8)
            fail(Status::Invalid, "invalid loop system-flags location");
          break;
        case LowIRLoopSpace::FunctionTemporary: {
          if (!UseEntryPrefix || Side == LowIRLoopSide::Entry)
            fail(Status::Invalid,
                 "loop function-temporary state requires a checked prefix");
          if (!scalar(NdVar::tmp(L.Offset, L.Bytes)))
            fail(Status::Invalid, "invalid loop function-temporary location");
          const auto *F = Side == LowIRLoopSide::Candidate ||
                                  Side == LowIRLoopSide::CandidatePrefix
                              ? &Refinement->Candidate
                              : Refinement->Original;
          for (uint16_t I = 0; I != L.Bytes; ++I)
            if (!F || !F->isFunctionTemporaryByte(L.Offset + I))
              fail(Status::Invalid, "loop function-temporary location is not "
                                    "declared on its side");
          break;
        }
        default:
          fail(Status::Invalid, "unknown loop location space");
        }
      };
      for (const auto &C : Cuts) {
        if (C.GeneralizeEntryPrefix && !C.UseEntryPrefix)
          fail(Status::Invalid,
               "generalized loop template requires a checked prefix");
        if (!OriginalAddresses.insert(C.OriginalAddress).second ||
            !CandidateAddresses.insert(C.CandidateAddress).second)
          fail(Status::Invalid, "duplicate loop cutpoint address");
        Charge(C.Inputs.size());
        Charge(C.OriginalState.size());
        Charge(C.CandidateState.size());
        Charge(C.Rank.size());
        if (C.Expressions.size() > Limits.MaxOperations - Operations)
          fail(Status::BudgetExceeded,
               "loop expression metadata budget exhausted");
        Operations += C.Expressions.size();
        std::set<uint64_t> Defined;
        for (const auto &I : C.Inputs) {
          if (I.Side != LowIRLoopSide::Entry &&
              I.Side != LowIRLoopSide::Original &&
              I.Side != LowIRLoopSide::Candidate &&
              I.Side != LowIRLoopSide::OriginalPrefix &&
              I.Side != LowIRLoopSide::CandidatePrefix)
            fail(Status::Invalid, "unknown loop input side");
          if ((I.Side == LowIRLoopSide::OriginalPrefix ||
               I.Side == LowIRLoopSide::CandidatePrefix) &&
              !C.UseEntryPrefix)
            fail(Status::Invalid,
                 "loop prefix input requires a checked prefix");
          Location(I.Location, I.Side, C.UseEntryPrefix);
          if (!I.Temporary.isTemp() || !scalar(I.Temporary) ||
              I.Temporary.Size != I.Location.Bytes)
            fail(Status::Invalid, "invalid loop input binding");
          for (uint16_t J = 0; J != I.Temporary.Size; ++J)
            if (!Defined.insert(I.Temporary.Offset + J).second)
              fail(Status::Invalid, "overlapping loop input bindings");
        }
        const auto Operand = [&](const NdVar &V) {
          if (!scalar(V) || (!V.isTemp() && !V.isConst()))
            fail(Status::Invalid, "loop expression uses a nonlocal operand");
          checkTemporary(V, Defined);
        };
        for (const auto &Op : C.Expressions) {
          if (Op.NumInputs > 6 || !Op.Output.isTemp() || !scalar(Op.Output) ||
              Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
              Op.MemoryOrdering != NdMemoryOrdering::None ||
              Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
              !supportedShape(Op))
            fail(Status::Invalid, "loop expressions must be pure scalar LowIR");
          for (unsigned I = 0; I != Op.NumInputs; ++I)
            Operand(Op.Inputs[I]);
          define(Op.Output, Defined);
        }
        for (const auto *Assignments : {&C.OriginalState, &C.CandidateState}) {
          std::set<std::pair<LowIRLoopSpace, uint64_t>> Bound;
          for (const auto &A : *Assignments) {
            Location(A.Location,
                     Assignments == &C.OriginalState ? LowIRLoopSide::Original
                                                     : LowIRLoopSide::Candidate,
                     C.UseEntryPrefix);
            Operand(A.Value);
            if (A.Value.Size != A.Location.Bytes)
              fail(Status::Invalid, "loop assignment width mismatch");
            for (uint16_t I = 0; I != A.Location.Bytes; ++I)
              if (!Bound.emplace(A.Location.Space, A.Location.Offset + I)
                       .second)
                fail(Status::Invalid, "overlapping loop state assignments");
          }
        }
        Operand(C.Predicate);
        if (C.Predicate.Size != 1 || C.Rank.empty() ||
            C.Rank.size() != Cuts.front().Rank.size())
          fail(Status::Invalid, "invalid loop predicate or ranking tuple");
        for (size_t I = 0; I != C.Rank.size(); ++I) {
          Operand(C.Rank[I]);
          if (C.Rank[I].Size != Cuts.front().Rank[I].Size)
            fail(Status::Invalid, "loop ranking component widths differ");
        }
      }
      return true;
    } catch (const Stop &) {
      return false;
    }
  }

private:
  SymRef loopRead(TerminalState &State, const LowIRLoopLocation &L) {
    switch (L.Space) {
    case LowIRLoopSpace::Register:
      return State.State.read(SymSpace::Register, L.Offset, L.Bytes);
    case LowIRLoopSpace::Frame:
      return State.State.load(
          Ctx.mkAdd(Refinement->MemoryRoot,
                    Ctx.mkConst(64, L.Offset - static_cast<uint64_t>(
                                                   Contract.Frame->Begin))),
          L.Bytes);
    case LowIRLoopSpace::SystemFlags:
      return State.SystemFlags;
    case LowIRLoopSpace::FunctionTemporary:
      checkTemporary(NdVar::tmp(L.Offset, L.Bytes),
                     State.DefinedFunctionTemporaries);
      return State.State.read(SymSpace::Temporary, L.Offset, L.Bytes);
    }
    fail(Status::Invalid, "unknown loop location space");
  }

  struct LoopTemplate {
    TerminalState Original, Candidate;
    SymRef Predicate;
    std::vector<SymRef> Rank;
  };

  LoopTemplate loopTemplate(size_t Index, TerminalState *Original,
                            TerminalState *Candidate, SymRef Domain) {
    const auto &Cut = Refinement->LoopPlan->Cutpoints[Index];
    auto &Prefix = Refinement->LoopPrefixes[Index];
    if (Cut.UseEntryPrefix) {
      if (!Prefix && Original && Candidate && Refinement->StartingCutpoint < 0)
        Prefix = LoopPrefix{*Original, *Candidate, Domain};
      if (!Prefix)
        fail(Status::Unsupported,
             "loop template has no reachable entry prefix");
    }
    TerminalState Entry{*Refinement->Initial,
                        Refinement->Predicate,
                        Refinement->EntrySystem,
                        {},
                        {}};
    const auto InputState = [&](LowIRLoopSide Side, TerminalState *O,
                                TerminalState *C) -> TerminalState * {
      switch (Side) {
      case LowIRLoopSide::Entry:
        return &Entry;
      case LowIRLoopSide::Original:
        return O;
      case LowIRLoopSide::Candidate:
        return C;
      case LowIRLoopSide::OriginalPrefix:
        return &Prefix->Original;
      case LowIRLoopSide::CandidatePrefix:
        return &Prefix->Candidate;
      }
      fail(Status::Invalid, "unknown loop input side");
    };
    SymState Expressions(Ctx, Contract.ByteOrder);
    std::vector<SymRef> Inputs;
    for (const auto &I : Cut.Inputs) {
      auto *State = InputState(I.Side, Original, Candidate);
      const auto Value =
          State ? loopRead(*State, I.Location)
                : Ctx.mkFreshVar(I.Location.Bytes * 8, "loop_input");
      Expressions.write(SymSpace::Temporary, I.Temporary.Offset, Value);
      Inputs.push_back(Value);
      nodes();
    }
    SymExec Exec(Ctx, Expressions);
    for (const auto &Op : Cut.Expressions) {
      if (++Result.Operations > Limits.MaxOperations)
        fail(Status::BudgetExceeded,
             "loop expression execution budget exhausted");
      if (Exec.step(Op) != StepResult::Continue || Exec.unmodelledCount() ||
          Exec.opaqueOperationCount() || Exec.memoryHavocCount() ||
          Exec.callHavocCount())
        fail(Status::Unsupported,
             "loop expression lost exact scalar semantics");
      nodes();
    }
    const auto Boolean = Exec.operandValue(Cut.Predicate);
    if (query(Ctx.mkAnd(Domain, Ctx.mkUlt(Ctx.mkConst(8, 1), Boolean))) !=
        solver::SatResult::Unsat)
      fail(Status::Invalid, "loop predicate is not a canonical Boolean");
    LoopTemplate T{Entry, Entry, Ctx.mkEq(Boolean, Ctx.mkConst(8, 1)), {}};
    if (Cut.UseEntryPrefix) {
      T.Original = Prefix->Original;
      T.Candidate = Prefix->Candidate;
      if (!Cut.GeneralizeEntryPrefix)
        T.Predicate = Ctx.mkAnd(T.Predicate, Prefix->Predicate);
    }
    for (const auto &R : Cut.Rank)
      T.Rank.push_back(Exec.operandValue(R));
    const auto Apply =
        [&](TerminalState &State,
            const std::vector<LowIRLoopAssignment> &Assignments) {
          for (const auto &A : Assignments) {
            const auto Value = Exec.operandValue(A.Value);
            switch (A.Location.Space) {
            case LowIRLoopSpace::Register:
              State.State.write(SymSpace::Register, A.Location.Offset, Value);
              break;
            case LowIRLoopSpace::Frame:
              State.State.store(
                  Ctx.mkAdd(Refinement->MemoryRoot,
                            Ctx.mkConst(64, A.Location.Offset -
                                                static_cast<uint64_t>(
                                                    Contract.Frame->Begin))),
                  Value);
              break;
            case LowIRLoopSpace::SystemFlags:
              State.SystemFlags = Value;
              break;
            case LowIRLoopSpace::FunctionTemporary:
              // A template replaces values in real prefix storage. It cannot
              // turn a lifetime declaration into an initialization fact.
              checkTemporary(NdVar::tmp(A.Location.Offset, A.Location.Bytes),
                             State.DefinedFunctionTemporaries);
              State.State.write(SymSpace::Temporary, A.Location.Offset, Value);
              break;
            }
            nodes();
          }
        };
    Apply(T.Original, Cut.OriginalState);
    Apply(T.Candidate, Cut.CandidateState);
    if (!Original) {
      // Parameters must be recoverable from actual state. Otherwise an
      // ambiguous representation could reset a rank without machine progress.
      const auto Assumption = Ctx.mkAnd(Domain, T.Predicate);
      for (size_t I = 0; I != Cut.Inputs.size(); ++I) {
        const auto &Input = Cut.Inputs[I];
        auto *State = InputState(Input.Side, &T.Original, &T.Candidate);
        equal(Assumption, loopRead(*State, Input.Location), Inputs[I],
              "loop parameter projection");
      }
    }
    return T;
  }

  void loopStateEqual(SymRef Predicate, TerminalState &Actual,
                      TerminalState &Expected, llvm::StringRef Side) {
    // A lifetime declaration never initializes storage. Only real prefix
    // definitions survive a cut. Every arrival retains exactly those defined
    // bytes and matches the fixed or explicitly assigned template values.
    if (Actual.DefinedFunctionTemporaries !=
        Expected.DefinedFunctionTemporaries)
      fail(Status::Dependent,
           (Side + " loop function-temporary definedness differs").str());
    for (uint64_t Byte : Actual.DefinedFunctionTemporaries)
      equal(
          Predicate, Actual.State.read(SymSpace::Temporary, Byte, 1),
          Expected.State.read(SymSpace::Temporary, Byte, 1),
          (Side + " loop function-temporary byte " + llvm::Twine(Byte)).str());
    // Unassigned state starts at the same entry snapshot. Every program
    // register write and every template assignment enters RegisterBytes, so
    // later-discovered untouched registers are still identical by construction.
    for (uint64_t Byte : Refinement->RegisterBytes)
      equal(Predicate, Actual.State.read(SymSpace::Register, Byte, 1),
            Expected.State.read(SymSpace::Register, Byte, 1),
            (Side + " loop register byte " + llvm::Twine(Byte)).str());
    for (uint64_t Byte = 0; Byte != FrameBytes; ++Byte) {
      const auto Address = Ctx.mkAdd(MemoryRoot, Ctx.mkConst(64, Byte));
      equal(Predicate, Actual.State.load(Address, 1),
            Expected.State.load(Address, 1),
            (Side + " loop frame byte " + llvm::Twine(Byte)).str());
    }
    if (Contract.X64FlagsProfile)
      equal(Predicate, Actual.SystemFlags, Expected.SystemFlags,
            "loop system flags");
  }

public:
  bool startLoopSegment(size_t Index) {
    try {
      EntryRoot = Refinement->EntryRoot;
      MemoryRoot = Refinement->MemoryRoot;
      auto T = loopTemplate(Index, nullptr, nullptr, Refinement->Predicate);
      const auto Predicate = Ctx.mkAnd(Refinement->Predicate, T.Predicate);
      if (query(Predicate) == solver::SatResult::Unsat)
        fail(Status::Invalid, "loop invariant has an empty induction domain");
      T.Original.Predicate = T.Candidate.Predicate = Predicate;
      Refinement->OriginalStart = std::move(T.Original);
      Refinement->CandidateStart = std::move(T.Candidate);
      Refinement->StartingRank = std::move(T.Rank);
      Refinement->SegmentPredicate = Predicate;
      Refinement->StartingCutpoint = static_cast<int>(Index);
      Refinement->OriginalReturns.clear();
      Refinement->CandidateReturns.clear();
      return true;
    } catch (const Stop &) {
      return false;
    }
  }

  bool validateInput() {
    try {
      if (!Validated) {
        validate();
        Validated = true;
      }
      return true;
    } catch (const Stop &) {
      return false;
    }
  }

  bool compareSelectedReturns() {
    try {
      const auto Coverage = [&](const std::vector<TerminalState> &Returns) {
        auto Covered = Ctx.mkFalse();
        for (const auto &T : Returns) {
          Covered = Ctx.mkOr(Covered, T.Predicate);
          nodes();
        }
        const auto Domain =
            inductive() ? Refinement->SegmentPredicate : Refinement->Predicate;
        if (query(Ctx.mkAnd(Domain, Ctx.mkNot(Covered))) !=
            solver::SatResult::Unsat)
          fail(Status::Unsupported,
               "terminal paths do not cover the entry domain");
      };
      const bool PrefixSearch = Refinement->PrefixSearchCutpoint >= 0;
      if (!PrefixSearch) {
        Coverage(Refinement->OriginalReturns);
        Coverage(Refinement->CandidateReturns);
      }
      for (auto &Original : Refinement->OriginalReturns)
        for (auto &Candidate : Refinement->CandidateReturns) {
          // A prefix witness needs an actually executed, feasible pair. It
          // cannot certify the entry domain or substitute for a loop segment.
          if (PrefixSearch &&
              (Original.Cutpoint != Refinement->PrefixSearchCutpoint ||
               Candidate.Cutpoint != Refinement->PrefixSearchCutpoint))
            continue;
          if (Refinement->TerminalPairs >= Refinement->Limits.MaxTerminalPairs)
            fail(Status::BudgetExceeded, "terminal-pair budget exhausted");
          ++Refinement->TerminalPairs;
          const auto Predicate =
              Ctx.mkAnd(Original.Predicate, Candidate.Predicate);
          if (query(Predicate) == solver::SatResult::Unsat)
            continue;
          if (inductive() &&
              (Original.Cutpoint >= 0 || Candidate.Cutpoint >= 0)) {
            if (Original.Cutpoint != Candidate.Cutpoint)
              fail(Status::Dependent,
                   "loop segment successors do not correspond");
            auto Template = loopTemplate(Original.Cutpoint, &Original,
                                         &Candidate, Predicate);
            equal(Predicate, Template.Predicate, Ctx.mkTrue(),
                  "loop invariant predicate");
            loopStateEqual(Predicate, Original, Template.Original, "original");
            loopStateEqual(Predicate, Candidate, Template.Candidate,
                           "candidate");
            if (Refinement->StartingCutpoint < 0) {
              ++Refinement->LoopInitiations;
            } else {
              auto Less = Ctx.mkFalse(), PrefixEqual = Ctx.mkTrue();
              for (size_t I = 0; I != Template.Rank.size(); ++I) {
                const auto Before = Refinement->StartingRank[I];
                const auto After = Template.Rank[I];
                Less = Ctx.mkOr(
                    Less, Ctx.mkAnd(PrefixEqual, Ctx.mkUlt(After, Before)));
                PrefixEqual = Ctx.mkAnd(PrefixEqual, Ctx.mkEq(After, Before));
                nodes();
              }
              equal(Predicate, Less, Ctx.mkTrue(), "strict loop rank decrease");
              ++Refinement->RankingChecks;
              ++Refinement->LoopTransitions;
            }
            continue;
          }
          if (bool(Original.ReturnOperand) != bool(Candidate.ReturnOperand))
            fail(Status::Dependent,
                 "original and candidate RETURN arities differ");
          if (Original.ReturnOperand)
            equal(Predicate, Original.ReturnOperand, Candidate.ReturnOperand,
                  "RETURN operand");
          if (Contract.X64FlagsProfile)
            equal(Predicate, Original.SystemFlags, Candidate.SystemFlags,
                  "final system flags");
          for (const auto &R : Contract.ReturnRegisters)
            equal(Predicate,
                  Original.State.read(SymSpace::Register, R.Offset, R.Bytes),
                  Candidate.State.read(SymSpace::Register, R.Offset, R.Bytes),
                  "return register");
          if (Contract.ObserveWrittenFrameBytes) {
            auto Written = Original.Written;
            Written.insert(Candidate.Written.begin(), Candidate.Written.end());
            if (inductive())
              for (uint64_t I = 0; I != FrameBytes; ++I)
                Written.insert(I);
            for (uint64_t Byte : Written) {
              const auto Address = Ctx.mkAdd(MemoryRoot, Ctx.mkConst(64, Byte));
              equal(Predicate, Original.State.load(Address, 1),
                    Candidate.State.load(Address, 1), "written frame byte");
            }
          }
        }
      return true;
    } catch (const Stop &) {
      return false;
    }
  }

  Checker(const LowFunc &F, llvm::ArrayRef<LowIRUndefinedInstruction> R,
          const LowIRIndependenceContract &C, const LowIRIndependenceLimits &L,
          RefinementSession *Session = nullptr, bool Candidate = false,
          SpecializationProvider *Reader = nullptr,
          detail::NativeUndefinedIndependenceResult *NativeOutput = nullptr)
      : Function(&F), Records(R), NativeResult(NativeOutput),
        NativeInstructions(Session ? Session->NativeInstructions
                                   : OwnedNativeInstructions),
        NativeRanges(Session ? Session->NativeRanges : OwnedNativeRanges),
        NativeAuditBoundaries(Session ? Session->NativeAuditBoundaries
                                      : OwnedNativeAuditBoundaries),
        ImmutableBytes(Session ? Session->ImmutableBytes : OwnedImmutableBytes),
        NativeInputOperations(Session ? Session->NativeInputOperations
                                      : OwnedNativeInputOperations),
        NativeInputEffects(Session ? Session->NativeInputEffects
                                   : OwnedNativeInputEffects),
        NativeInputBytes(Session ? Session->NativeInputBytes
                                 : OwnedNativeInputBytes),
        NativeReadEvidenceBytes(Session ? Session->ReadEvidenceBytes
                                        : OwnedReadEvidenceBytes),
        Contract(C), Limits(L),
        Result(Session ? Session->Statistics : OwnedResult),
        Ctx(Session ? Session->Context : OwnedContext), Refinement(Session),
        CandidateExecution(Candidate), ReadProvider(Reader),
        ScheduledPaths(Session ? Session->ScheduledPaths
                               : OwnedScheduledPaths) {}

  Checker(SpecializationProvider &P, SpecializationCursor Entry,
          const LowIRIndependenceContract &C, const LowIRIndependenceLimits &L,
          detail::NativeUndefinedIndependenceResult &Output,
          RefinementSession *Session = nullptr)
      : Provider(&P), NativeEntry(Entry), NativeResult(&Output),
        NativeInstructions(Session ? Session->NativeInstructions
                                   : OwnedNativeInstructions),
        NativeRanges(Session ? Session->NativeRanges : OwnedNativeRanges),
        NativeAuditBoundaries(Session ? Session->NativeAuditBoundaries
                                      : OwnedNativeAuditBoundaries),
        ImmutableBytes(Session ? Session->ImmutableBytes : OwnedImmutableBytes),
        NativeInputOperations(Session ? Session->NativeInputOperations
                                      : OwnedNativeInputOperations),
        NativeInputEffects(Session ? Session->NativeInputEffects
                                   : OwnedNativeInputEffects),
        NativeInputBytes(Session ? Session->NativeInputBytes
                                 : OwnedNativeInputBytes),
        NativeReadEvidenceBytes(Session ? Session->ReadEvidenceBytes
                                        : OwnedReadEvidenceBytes),
        Contract(C), Limits(L),
        Result(Session ? Session->Statistics : OwnedResult),
        Ctx(Session ? Session->Context : OwnedContext), Refinement(Session),
        ReadProvider(&P), ScheduledPaths(Session ? Session->ScheduledPaths
                                                 : OwnedScheduledPaths) {
    NativeTrace.Entry = Entry.Address;
  }

  LowIRIndependenceResult run() {
    try {
      if (!validateInput())
        return Result;
      if (Refinement && Refinement->PrefixSearchCutpoint >= 0 &&
          (Refinement->StartingCutpoint >= 0 || Refinement->OriginalStart ||
           Refinement->CandidateStart))
        fail(Status::Invalid,
             "loop prefix search must start at the real entry");
      if (Provider)
        collectNative(NativeEntry.Address);
      SymState Initial(Ctx, Contract.ByteOrder);
      // Shared lazy byte identities also cover registers first discovered by
      // a later native segment. Different first-read widths cannot introduce
      // independent entry values into an induction template.
      if (inductive())
        Initial.clobberRegistersExcept({});
      SymRef EntrySystem;
      auto Predicate = Ctx.mkTrue();
      if (Refinement && Refinement->Initial) {
        Initial = *Refinement->Initial;
        EntryRoot = Refinement->EntryRoot;
        MemoryRoot = Refinement->MemoryRoot;
        EntrySystem = Refinement->EntrySystem;
        Predicate = Refinement->Predicate;
      } else {
        if (Contract.X64FlagsProfile) {
          const auto Raw = Ctx.mkFreshVar(64, "entry_flags");
          const auto Packed =
              Ctx.mkOr(Ctx.mkAnd(Raw, Ctx.mkConst(64, FlagProfile::EntryMask)),
                       Ctx.mkConst(64, 2));
          EntrySystem =
              Ctx.mkAnd(Packed, Ctx.mkConst(64, ~FlagProfile::SplitMask));
          for (const auto &[Offset, Bit] : FlagProfile::Flags)
            Initial.write(SymSpace::Register, Offset,
                          Ctx.mkZExtOrTrunc(Ctx.mkExtract(Packed, Bit, 1), 8));
          if (inductive())
            for (const auto &[Offset, Bit] : FlagProfile::Flags) {
              (void)Bit;
              trackRegister(NdVar::reg(Offset, 1));
            }
          nodes();
        }
        for (const auto &C : Contract.EntryConstants) {
          Initial.write(SymSpace::Register, C.Location.Offset,
                        Ctx.mkConst(C.Location.Size * 8, C.Value));
          nodes();
        }
        if (Contract.Frame) {
          const auto &F = *Contract.Frame;
          EntryRoot =
              Initial.read(SymSpace::Register, F.RootRegister.Offset, 8);
          if (F.EntryAlignment) {
            const auto &A = *F.EntryAlignment;
            Predicate = Ctx.mkAnd(
                Predicate,
                Ctx.mkEq(Ctx.mkAnd(EntryRoot, Ctx.mkConst(64, A.Alignment - 1)),
                         Ctx.mkConst(64, A.Residue)));
            nodes();
          }
          Predicate = Ctx.mkAnd(Predicate, detail::nonwrappingFramePredicate(
                                               Ctx, EntryRoot, F.Begin, F.End));
          // The existing root bounds make these the unsigned first and last
          // accessible bytes without wraparound. Exclusion is a symbolic entry
          // precondition: adjacency is allowed, but no byte may overlap a
          // range.
          const auto First = Ctx.mkAdd(
              EntryRoot, Ctx.mkConst(64, static_cast<uint64_t>(F.Begin)));
          const auto Last = Ctx.mkAdd(
              EntryRoot, Ctx.mkConst(64, static_cast<uint64_t>(F.End) - 1));
          for (const auto &R : F.ExcludedAddressRanges) {
            Predicate = Ctx.mkAnd(
                Predicate, Ctx.mkOr(Ctx.mkUlt(Last, Ctx.mkConst(64, R.Begin)),
                                    Ctx.mkUle(Ctx.mkConst(64, R.End), First)));
            nodes();
          }
          MemoryRoot = Ctx.mkFreshVar(64, "proof_frame");
          // Seed before the twin-state fork: normal frame bytes must stay
          // shared even when subsequent accesses use different overlapping
          // widths.
          for (uint64_t I = 0; I != FrameBytes; ++I) {
            Initial.store(Ctx.mkAdd(MemoryRoot, Ctx.mkConst(64, I)),
                          Ctx.mkFreshVar(8, "entry_frame_byte"));
            nodes();
          }
        }
        if (Refinement) {
          // Seed a common entry bank before either program executes. Different
          // first-read widths and overlapping writes cannot invent different
          // ordinary inputs in the two independently explored graphs.
          const auto Seed = [&](const NdVar &V) {
            if (!V.isReg())
              return;
            if (!scalar(V))
              fail(Status::Invalid, "invalid candidate register range");
            for (uint16_t I = 0; I != V.Size; ++I)
              Initial.read(SymSpace::Register, V.Offset + I, 1);
            nodes();
          };
          for (const auto &B : Refinement->Candidate.Blocks)
            for (const auto &Op : B.Ops) {
              if (Op.NumInputs > 6)
                fail(Status::Invalid, "candidate operand capacity exceeded");
              if (Op.Output.Size)
                Seed(Op.Output);
              for (unsigned I = 0; I != Op.NumInputs; ++I)
                Seed(Op.Inputs[I]);
            }
          for (const auto &R : Contract.ReturnRegisters)
            Seed(NdVar::reg(R.Offset, R.Bytes));
          for (const auto &R : Contract.PreservedRegisters)
            Seed(NdVar::reg(R.Offset, R.Bytes));
          Refinement->Initial = Initial;
          Refinement->EntryRoot = EntryRoot;
          Refinement->MemoryRoot = MemoryRoot;
          Refinement->EntrySystem = EntrySystem;
          Refinement->Predicate = Predicate;
          Refinement->SegmentPredicate = Predicate;
        }
      }
      if (query(Predicate) == solver::SatResult::Unsat)
        fail(Status::InfeasibleEntry,
             "entry frame precondition is unsatisfiable");
      // Materialize one shared entry snapshot after all caller constants and
      // ordinary frame bytes are initialized, before either execution starts.
      for (const auto &R : Contract.PreservedRegisters) {
        PreservedRegisterEntries.push_back(
            Initial.read(SymSpace::Register, R.Offset, R.Bytes));
        nodes();
      }
      for (const auto &R : Contract.PreservedFrameRanges) {
        const uint64_t First = static_cast<uint64_t>(R.Offset) -
                               static_cast<uint64_t>(Contract.Frame->Begin);
        for (uint16_t I = 0; I != R.Bytes; ++I) {
          const auto Address =
              Ctx.mkAdd(MemoryRoot, Ctx.mkConst(64, First + I));
          PreservedFrameEntries.emplace(First + I, Initial.load(Address, 1));
          nodes();
        }
      }
      Path Entry{Provider ? -1 : Addresses.at(Function->Entry),
                 Initial,
                 Initial,
                 Predicate,
                 {},
                 {}};
      Entry.LeftSystemFlags = Entry.RightSystemFlags = EntrySystem;
      va_t StartAddress = Provider ? NativeEntry.Address : Function->Entry;
      if (inductive()) {
        NextNativeBlock = Refinement->NextNativeBlock;
        const auto &Start = CandidateExecution ? Refinement->CandidateStart
                                               : Refinement->OriginalStart;
        if (Start) {
          Entry.Left = Entry.Right = Start->State;
          Entry.DefinedFunctionTemporaries = Start->DefinedFunctionTemporaries;
          Entry.LeftSystemFlags = Entry.RightSystemFlags = Start->SystemFlags;
          Entry.Predicate = Predicate = Start->Predicate;
          Entry.SkipCutpoint = true;
          const auto &Cut =
              Refinement->LoopPlan->Cutpoints[Refinement->StartingCutpoint];
          StartAddress =
              CandidateExecution ? Cut.CandidateAddress : Cut.OriginalAddress;
        }
      }
      if (Provider) {
        NativeTrace.Entry = StartAddress;
        scheduleNative(std::move(Entry), StartAddress, Predicate);
      } else {
        schedule(std::move(Entry), Addresses.at(StartAddress), Predicate);
      }
      // A replay seeks one feasible prefix. Visit queued siblings before
      // repeatedly unfolding a loop, which could otherwise starve a short
      // witness on another branch. Full relation checks retain their order
      // and still require complete entry and transition coverage.
      const bool PrefixSearch =
          Refinement && Refinement->PrefixSearchCutpoint >= 0;
      while (!Pending.empty()) {
        auto P = PrefixSearch ? std::move(Pending.front())
                              : std::move(Pending.back());
        if (PrefixSearch)
          Pending.pop_front();
        else
          Pending.pop_back();
        runPath(std::move(P));
      }
      if (Refinement
              ? (CandidateExecution ? Refinement->CandidateReturns.empty()
                                    : Refinement->OriginalReturns.empty())
              : !Result.Paths)
        fail(Status::Unsupported, "no reachable return was certified");
      if (Refinement) {
        if (Provider) {
          for (auto &B : NativeTrace.Blocks)
            B.Succs = NativeTraceEdges[B.Id];
          const auto Digest =
              inputDigest(NativeTrace, NativeRecords, Contract, Limits,
                          NativeFlagTransitions, NativeProfileProjections,
                          auditBoundaryReceipts(NativeAuditBoundaries));
          Refinement->OriginalDigest = Digest;
          if (inductive()) {
            Refinement->OriginalSegmentDigests.push_back(Digest);
            Refinement->NextNativeBlock = NextNativeBlock;
          }
          Refinement->OriginalInstructions.insert(
              Refinement->OriginalInstructions.end(), NativeRecords.begin(),
              NativeRecords.end());
          Refinement->Flags.insert(Refinement->Flags.end(),
                                   NativeFlagTransitions.begin(),
                                   NativeFlagTransitions.end());
          Refinement->Projections.insert(Refinement->Projections.end(),
                                         NativeProfileProjections.begin(),
                                         NativeProfileProjections.end());
          NativeResult->Instructions.clear();
          for (const auto &[Address, Insn] : NativeInstructions)
            NativeResult->Instructions.push_back(Insn);
        } else if (CandidateExecution) {
          Refinement->CandidateDigest = inputDigest(
              *Function, Records, Contract, Limits, NativeFlagTransitions);
          if (inductive())
            Refinement->CandidateSegmentDigests.push_back(
                Refinement->CandidateDigest);
        } else {
          Refinement->OriginalDigest =
              inputDigest(*Function, Records, Contract, Limits);
          Refinement->OriginalInstructions.assign(Records.begin(),
                                                  Records.end());
        }
      } else if (Provider) {
        for (auto &B : NativeTrace.Blocks)
          B.Succs = NativeTraceEdges[B.Id];
        Result.Certificate = LowIRIndependenceCertificate{
            LowIRIndependenceScope::CompleteFiniteNativePaths,
            inputDigest(NativeTrace, NativeRecords, Contract, Limits,
                        NativeFlagTransitions, NativeProfileProjections,
                        auditBoundaryReceipts(NativeAuditBoundaries)),
            std::move(NativeRecords),
            Contract,
            Limits,
            std::move(NativeFlagTransitions),
            std::move(NativeProfileProjections),
            auditBoundaryReceipts(NativeAuditBoundaries)};
        for (auto &[Address, Insn] : NativeInstructions)
          NativeResult->Instructions.push_back(std::move(Insn));
      } else {
        Result.Certificate = LowIRIndependenceCertificate{
            LowIRIndependenceScope::CompleteAcyclicLowIR,
            inputDigest(*Function, Records, Contract, Limits),
            std::vector<LowIRUndefinedInstruction>(Records.begin(),
                                                   Records.end()),
            Contract, Limits};
      }
      Result.Status = Status::Proved;
      Result.Diagnostic.clear();
    } catch (const Stop &) {
      // No incomplete run owns a certificate.
    }
    return Refinement ? Result : std::move(Result);
  }
};

LowIRRefinementResult
refinementResult(RefinementSession &Session,
                 const LowIRIndependenceContract &Contract, bool Native,
                 bool Success) {
  LowIRRefinementResult Result;
  const auto &Stats = Session.Statistics;
  switch (Stats.Status) {
  case Status::Proved:
    Result.Status = LowIRRefinementStatus::Proved;
    break;
  case Status::Dependent:
    Result.Status = LowIRRefinementStatus::Different;
    break;
  case Status::Unsupported:
    Result.Status = LowIRRefinementStatus::Unsupported;
    break;
  case Status::Invalid:
    Result.Status = LowIRRefinementStatus::Invalid;
    break;
  case Status::BudgetExceeded:
    Result.Status = LowIRRefinementStatus::BudgetExceeded;
    break;
  case Status::InfeasibleEntry:
    Result.Status = LowIRRefinementStatus::InfeasibleEntry;
    break;
  case Status::ContractViolation:
    Result.Status = LowIRRefinementStatus::ContractViolation;
    break;
  }
  Result.Diagnostic = Stats.Diagnostic;
  Result.BlockId = Stats.BlockId;
  Result.InstructionAddress = Stats.InstructionAddress;
  Result.OpSeq = Stats.OpSeq;
  Result.Operations = Stats.Operations;
  Result.Instructions = Stats.Instructions;
  Result.OriginalPaths = Session.OriginalPathCount;
  Result.CandidatePaths = Session.CandidatePathCount;
  Result.BlockVisits = Stats.BlockVisits;
  Result.Producers = Stats.Producers;
  Result.SolverQueries = Stats.SolverQueries;
  Result.Observations = Stats.Observations;
  Result.TerminalPairs = Session.TerminalPairs;
  Result.OriginalCutpoints = Session.OriginalCutpoints;
  Result.CandidateCutpoints = Session.CandidateCutpoints;
  Result.LoopInitiations = Session.LoopInitiations;
  Result.LoopTransitions = Session.LoopTransitions;
  Result.RankingChecks = Session.RankingChecks;
  if (!Success)
    return Result;
  LowIRRefinementCertificate Certificate;
  Certificate.Scope =
      Native ? LowIRRefinementScope::CompleteFiniteNativeToLowIRPaths
             : LowIRRefinementScope::CompleteFiniteLowIRPaths;
  if (Session.LoopPlan) {
    Certificate.Scope = Native
                            ? LowIRRefinementScope::InductiveNativeToLowIRLoops
                            : LowIRRefinementScope::InductiveLowIRLoops;
    Certificate.LoopPlan = *Session.LoopPlan;
    const auto SegmentDigest = [](llvm::StringRef Domain,
                                  const std::vector<std::string> &Digests) {
      llvm::SHA256 Segments;
      Segments.update(Domain);
      // Each digest is exactly 64 hexadecimal bytes; segment boundaries are
      // unambiguous, and order follows the entry plus the bound cutpoint plan.
      for (const auto &Digest : Digests)
        Segments.update(Digest);
      return llvm::toHex(Segments.final());
    };
    Session.CandidateDigest =
        SegmentDigest("neverd-candidate-inductive-segments-v1",
                      Session.CandidateSegmentDigests);
    if (Native) {
      Session.OriginalDigest =
          SegmentDigest("neverd-native-inductive-segments-v1",
                        Session.OriginalSegmentDigests);
    }
  }
  Certificate.OriginalDigest = Session.OriginalDigest;
  Certificate.CandidateDigest = Session.CandidateDigest;
  Certificate.Witness = Session.Witness;
  Certificate.Contract = Contract;
  Certificate.Limits = Session.Limits;
  Certificate.OriginalInstructions = std::move(Session.OriginalInstructions);
  Certificate.Producers = std::move(Session.Producers);
  Certificate.NativeFlagTransitions = std::move(Session.Flags);
  Certificate.NativeProfileProjections = std::move(Session.Projections);
  Certificate.NativeAuditBoundaries =
      auditBoundaryReceipts(Session.NativeAuditBoundaries);
  llvm::SHA256 Hash;
  Hash.update(Session.LoopPlan ? "neverd-inductive-refinement-v2"
                               : "neverd-selected-value-refinement-v2");
  const auto Number = [&](uint64_t N) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = static_cast<uint8_t>(N >> (8 * I));
    Hash.update(Bytes);
  };
  Number(static_cast<unsigned>(Certificate.Scope));
  Number(static_cast<unsigned>(Certificate.Witness));
  Number(Session.Limits.MaxTerminalPairs);
  Hash.update(Certificate.OriginalDigest);
  Hash.update(Certificate.CandidateDigest);
  if (Session.LoopPlan) {
    const auto Variable = [&](const NdVar &V) {
      Number(static_cast<unsigned>(V.Space));
      Number(V.Offset);
      Number(V.Size);
      Number(static_cast<unsigned>(V.Provenance));
      Number(V.AddressOwnerVA);
    };
    const auto Location = [&](const LowIRLoopLocation &L) {
      Number(static_cast<unsigned>(L.Space));
      Number(L.Offset);
      Number(L.Bytes);
    };
    Number(Session.LoopPlan->Cutpoints.size());
    for (const auto &Cut : Session.LoopPlan->Cutpoints) {
      Number(Cut.OriginalAddress);
      Number(Cut.CandidateAddress);
      Number(Cut.UseEntryPrefix);
      Number(Cut.GeneralizeEntryPrefix);
      Number(Cut.Inputs.size());
      for (const auto &Input : Cut.Inputs) {
        Number(static_cast<unsigned>(Input.Side));
        Location(Input.Location);
        Variable(Input.Temporary);
      }
      Hash.update(lowUndefinedOperationDigest(Cut.Expressions));
      for (const auto *Assignments :
           {&Cut.OriginalState, &Cut.CandidateState}) {
        Number(Assignments->size());
        for (const auto &Assignment : *Assignments) {
          Location(Assignment.Location);
          Variable(Assignment.Value);
        }
      }
      Variable(Cut.Predicate);
      Number(Cut.Rank.size());
      for (const auto &Rank : Cut.Rank)
        Variable(Rank);
    }
  }
  Number(Certificate.Producers.size());
  for (const auto &Producer : Certificate.Producers) {
    Number(Producer.InstructionVisit);
    Number(static_cast<uint64_t>(Producer.BlockId));
    Number(Producer.InstructionAddress);
    Number(Producer.EffectIndex);
  }
  Certificate.InputDigest = llvm::toHex(Hash.final());
  Result.Certificate = std::move(Certificate);
  return Result;
}

bool prepareCandidateRecords(
    RefinementSession &Session,
    std::vector<LowIRUndefinedInstruction> &CandidateRecords) {
  const auto &Candidate = Session.Candidate;
  const auto &Limits = Session.Limits;
  // This is candidate semantic IR, not a claim of architecture coverage. Bound
  // its input before copying records or seeding the common entry snapshot.
  uint64_t Operations = 0;
  if (Candidate.Blocks.size() > Limits.Execution.MaxBlockVisits) {
    Session.Statistics.Status = Status::BudgetExceeded;
    Session.Statistics.Diagnostic = "candidate block metadata budget exhausted";
    return false;
  }
  for (const auto &B : Candidate.Blocks) {
    if (B.Ops.size() > Limits.Execution.MaxOperations - Operations ||
        B.InstructionBoundaries.size() >
            Limits.Execution.MaxInstructions - CandidateRecords.size()) {
      Session.Statistics.Status = Status::BudgetExceeded;
      Session.Statistics.Diagnostic = "candidate metadata budget exhausted";
      return false;
    }
    Operations += B.Ops.size();
    for (const auto &Op : B.Ops)
      if (Op.NumInputs > 6) {
        Session.Statistics.Diagnostic = "candidate operand capacity exceeded";
        return false;
      }
    for (const auto &Boundary : B.InstructionBoundaries) {
      if (Boundary.FirstOp > B.Ops.size() ||
          Boundary.OpCount > B.Ops.size() - Boundary.FirstOp) {
        Session.Statistics.Diagnostic =
            "candidate instruction span exceeds its block";
        return false;
      }
      LowInstructionUndefinedEffects Effects;
      Effects.Coverage = LowUndefinedCoverage::Complete;
      Effects.OpCount = Boundary.OpCount;
      Effects.OperationDigest =
          lowUndefinedOperationDigest(llvm::ArrayRef<LowOp>(B.Ops).slice(
              Boundary.FirstOp, Boundary.OpCount));
      CandidateRecords.push_back({B.Id, Boundary, std::move(Effects)});
    }
  }
  return true;
}

LowIRRefinementResult runRefinement(
    const LowFunc *Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    SpecializationProvider *Provider, SpecializationCursor Entry,
    detail::NativeUndefinedIndependenceResult *NativeResult,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness, const LowIRRefinementLimits &Limits,
    const LowIRLoopRefinementPlan *LoopPlan = nullptr) {
  RefinementSession Session{{}, {}, Candidate, Witness, Limits};
  Session.Original = Original;
  Session.LoopPlan = LoopPlan;
  Session.NativeFinite = Provider && !LoopPlan;
  const auto Finish = [&](bool Success) {
    return refinementResult(Session, Contract, Provider != nullptr, Success);
  };
  if (Contract.RetainUnauditedNativeBoundaries && !Provider) {
    Session.Statistics.Status = Status::Unsupported;
    Session.Statistics.Diagnostic =
        "unaudited boundaries require a native proof provider";
    return Finish(false);
  }
  if (Contract.AllowOverlappingNativeInstructions && !Session.NativeFinite) {
    Session.Statistics.Status = Status::Unsupported;
    Session.Statistics.Diagnostic =
        "overlapping instructions require the finite native proof API";
    return Finish(false);
  }
  if (Contract.DeferNativeConditionalEdges && !Provider) {
    Session.Statistics.Status = Status::Unsupported;
    Session.Statistics.Diagnostic =
        "deferred conditional edges require a native proof provider";
    return Finish(false);
  }
  if (Witness != LowIRRefinementWitness::LiftedBits &&
      Witness != LowIRRefinementWitness::ZeroBits) {
    Session.Statistics.Diagnostic = "unknown refinement witness policy";
    return Finish(false);
  }
  if (LoopPlan &&
      LoopPlan->Cutpoints.size() > Limits.Execution.MaxBlockVisits) {
    Session.Statistics.Status = Status::BudgetExceeded;
    Session.Statistics.Diagnostic = "loop cutpoint metadata budget exhausted";
    return Finish(false);
  }
  if (LoopPlan)
    Session.LoopPrefixes.resize(LoopPlan->Cutpoints.size());
  std::vector<LowIRUndefinedInstruction> CandidateRecords;
  if (!prepareCandidateRecords(Session, CandidateRecords))
    return Finish(false);
  Checker Preflight(Candidate, CandidateRecords, Contract, Limits.Execution,
                    &Session, true, Provider, NativeResult);
  if (!Preflight.validateInput() || (LoopPlan && !Preflight.validateLoopPlan()))
    return Finish(false);
  const auto Segment = [&] {
    const auto First = Provider
                           ? Checker(*Provider, Entry, Contract,
                                     Limits.Execution, *NativeResult, &Session)
                                 .run()
                           : Checker(*Original, OriginalInstructions, Contract,
                                     Limits.Execution, &Session)
                                 .run();
    if (!First.proved())
      return false;
    Checker CandidateChecker(Candidate, CandidateRecords, Contract,
                             Limits.Execution, &Session, true, Provider,
                             NativeResult);
    return CandidateChecker.run().proved() &&
           CandidateChecker.compareSelectedReturns();
  };
  if (!Segment())
    return Finish(false);
  if (LoopPlan) {
    // The normal entry segment can stop before a nested cut. Establish any
    // missing prefix through a separate bounded replay from the real entry,
    // retaining the common input snapshot, budgets and segment evidence.
    // A witness need not cover inputs that bypass this cut. Every actual
    // arrival still has to imply its predicate in the complete entry and
    // inductive segment checks; a witness alone proves no equivalence.
    for (size_t I = 0; I != LoopPlan->Cutpoints.size(); ++I) {
      if (!LoopPlan->Cutpoints[I].UseEntryPrefix || Session.LoopPrefixes[I])
        continue;
      Session.OriginalReturns.clear();
      Session.CandidateReturns.clear();
      Session.PrefixSearchCutpoint = static_cast<int>(I);
      if (!Segment())
        return Finish(false);
      if (!Session.LoopPrefixes[I]) {
        Session.Statistics.Status = Status::Unsupported;
        Session.Statistics.Diagnostic =
            "loop template has no reachable entry prefix";
        return Finish(false);
      }
    }
    Session.PrefixSearchCutpoint = -1;
    for (size_t I = 0; I != LoopPlan->Cutpoints.size(); ++I)
      if (!Preflight.startLoopSegment(I) || !Segment())
        return Finish(false);
  }
  return Finish(true);
}

// Inference only proposes a plan. In particular, neither a sampled prefix nor
// a successful candidate transition is a native/candidate certificate. The
// caller must run the complete relation with the resulting, untrusted plan.
class LoopPlanInference {
  const LowFunc &Candidate;
  const LowIRIndependenceContract &Contract;
  const LowIRLoopInferenceLimits &Limits;
  LowIRRefinementLimits ExecutionLimits;
  RefinementSession Session;
  std::vector<LowIRUndefinedInstruction> Records;
  std::vector<TerminalState> EntryArrivals;
  LowIRLoopRefinementPlan Plan;
  LowIRLoopInferenceResult Result;
  std::map<int, const LowBlock *> Blocks;
  std::vector<va_t> Choices;
  std::vector<va_t> BranchArmEntries;
  std::vector<int> ReachableBlocks;
  bool MultipleCycles = false;
  bool GeneralContractFailure = false;
  std::string Stage;
  SpecializationProvider *ReadProvider;
  detail::NativeUndefinedIndependenceResult ReadEvidence;

  struct Word {
    struct Bound {
      LowIRLoopLocation Location;
      bool Lower, Strict;
    };
    LowIRLoopLocation Location;
    uint64_t FixedMask;
    bool UpperBound = false, LowerBound = false;
    bool Nonzero = false, Nonmax = false;
    struct LaneGuard {
      uint64_t Mask;
      bool Maximum;
      bool operator==(const LaneGuard &) const = default;
    };
    std::vector<LaneGuard> LaneGuards, LaneAttempts;
    std::vector<Bound> CounterBounds;
    bool CounterBoundsSeeded = false;
    std::vector<LowIRLoopLocation> Equalities;
    struct BitRelation {
      unsigned Bit;
      LowIRLoopLocation Counter, Bound;
      bool Invert;
    };
    std::vector<BitRelation> BitRelations;
    std::vector<BitRelation> BitAttempts;
  };
  size_t ActiveCutpoint = 0;
  std::vector<std::vector<Word>> Models{1};
  struct EqualitySeed {
    LowIRLoopLocation Source, Target;
  };
  std::vector<std::vector<EqualitySeed>> EqualitySeeds;
  std::vector<std::vector<EqualitySeed>> EqualityAttempts;
  std::vector<LowIRLoopLocation> RelationCounters;
  struct LaneCounter {
    LowIRLoopLocation Location;
    Word::LaneGuard Guard;
  };
  std::vector<LaneCounter> LaneCounters;
  std::vector<std::vector<LowIRLoopLocation>> RelationBounds;
  std::vector<std::vector<TerminalState>> RelationArrivals;
  std::vector<std::map<SymRef, llvm::SmallVector<uint32_t, 16>>>
      RelationVariables;
  std::vector<Word> &words() { return Models[ActiveCutpoint]; }

  [[noreturn]] void stop(Status S, llvm::StringRef Diagnostic) {
    Session.Statistics.Status = S;
    Session.Statistics.Diagnostic = Diagnostic.str();
    throw Stop{};
  }

  Checker checker() {
    return Checker(Candidate, Records, Contract, Limits.Execution, &Session,
                   true, ReadProvider, &ReadEvidence);
  }

  SymRef read(TerminalState &State, const LowIRLoopLocation &Location) {
    auto C = checker();
    return C.loopRead(State, Location);
  }

  bool entails(SymRef Domain, SymRef Fact) {
    auto C = checker();
    return C.query(
               Session.Context.mkAnd(Domain, Session.Context.mkNot(Fact))) ==
           solver::SatResult::Unsat;
  }

  void initializePrefixBounds(Word &W) {
    auto &Ctx = Session.Context;
    auto &Prefix = Session.LoopPrefixes[ActiveCutpoint]->Candidate;
    const auto Initial = read(Prefix, W.Location);
    W.UpperBound = W.LowerBound = true;
    W.Nonzero =
        entails(Prefix.Predicate,
                Ctx.mkNe(Initial, Ctx.mkConst(W.Location.Bytes * 8, 0)));
    W.Nonmax = entails(Prefix.Predicate,
                       Ctx.mkNe(Initial, Ctx.mkConst(W.Location.Bytes * 8,
                                                     ones(W.Location.Bytes))));
    // A cache can first vary after another cut's state has been generalized.
    // Reuse only candidates checked on the original concrete arrivals. Old
    // words are never reseeded after a failed relation has been pruned.
    if (ActiveCutpoint < EqualitySeeds.size())
      for (const auto &E : EqualitySeeds[ActiveCutpoint])
        if (sameLocation(E.Source, W.Location))
          W.Equalities.push_back(E.Target);
  }

  void runCandidate() {
    Session.CandidateReturns.clear();
    if (!checker().run().proved())
      throw Stop{};
  }

  static uint64_t ones(uint16_t Bytes) {
    return Bytes == 8 ? UINT64_MAX : (uint64_t{1} << (Bytes * 8)) - 1;
  }

  uint64_t commonBits(SymRef A, SymRef B, uint16_t Bytes) {
    auto &Ctx = Session.Context;
    if (A == B)
      return ones(Bytes);
    uint64_t Mask = 0;
    for (unsigned Bit = 0; Bit != Bytes * 8; ++Bit)
      if (Ctx.mkExtract(A, Bit, 1) == Ctx.mkExtract(B, Bit, 1))
        Mask |= uint64_t{1} << Bit;
    checker().nodes();
    return Mask;
  }

  uint64_t commonBitsUnder(SymRef A, SymRef B, uint16_t Bytes, SymRef Domain,
                           uint64_t Wanted = UINT64_MAX) {
    auto &Ctx = Session.Context;
    uint64_t Mask = commonBits(A, B, Bytes) & Wanted;
    std::vector<uint64_t> Pending{ones(Bytes) & Wanted & ~Mask};
    while (!Pending.empty()) {
      const uint64_t Bits = Pending.back();
      Pending.pop_back();
      if (!Bits)
        continue;
      const auto M = Ctx.mkConst(Bytes * 8, Bits);
      if (entails(Domain, Ctx.mkEq(Ctx.mkAnd(A, M), Ctx.mkAnd(B, M)))) {
        Mask |= Bits;
        continue;
      }
      if (std::has_single_bit(Bits))
        continue;
      // Divide only the undecided bits. Large stable lanes usually require
      // one query, while a differing bit is never promoted to a fact.
      unsigned Remaining = std::popcount(Bits) / 2;
      uint64_t Low = 0;
      for (unsigned Bit = 0; Remaining; ++Bit)
        if (Bits & (uint64_t{1} << Bit)) {
          Low |= uint64_t{1} << Bit;
          --Remaining;
        }
      Pending.push_back(Low);
      Pending.push_back(Bits & ~Low);
    }
    return Mask;
  }

  bool preservesOutsideLane(SymRef Before, SymRef After, unsigned Offset,
                            unsigned Width, unsigned Total) {
    auto &Ctx = Session.Context;
    return (!Offset || Ctx.mkExtract(Before, 0, Offset) ==
                           Ctx.mkExtract(After, 0, Offset)) &&
           (Offset + Width == Total ||
            Ctx.mkExtract(Before, Offset + Width, Total - Offset - Width) ==
                Ctx.mkExtract(After, Offset + Width, Total - Offset - Width));
  }

  // A zero mask denotes the existing full/zero-extended recurrence. A proper
  // preserved lane also supplies its endpoint mask for invariant proposals.
  std::optional<uint64_t> unitStepLane(SymRef Before, SymRef After,
                                       uint16_t Bytes, bool Increment) {
    auto &Ctx = Session.Context;
    const unsigned Total = Bytes * 8;
    // Keep existing successes ahead of any additional expression construction.
    for (unsigned Width = Total; Width; Width -= 8) {
      const auto Input = Ctx.mkExtract(Before, 0, Width);
      const auto Step = Ctx.mkConst(Width, Increment ? 1 : UINT64_MAX);
      const auto Expected = Ctx.mkZExtOrTrunc(Ctx.mkAdd(Input, Step), Total);
      checker().nodes();
      if (After == Expected)
        return 0;
    }
    for (unsigned Width = Total - 8; Width; Width -= 8)
      for (unsigned Offset = 0; Offset + Width <= Total; Offset += 8) {
        const auto Input = Ctx.mkExtract(Before, Offset, Width);
        const auto Step = Ctx.mkConst(Width, Increment ? 1 : UINT64_MAX);
        const auto Updated = Ctx.mkAdd(Input, Step);
        const bool Matches =
            Ctx.mkExtract(After, Offset, Width) == Updated &&
            preservesOutsideLane(Before, After, Offset, Width, Total);
        checker().nodes();
        if (Matches)
          return (UINT64_MAX >> (64 - Width)) << Offset;
      }
    return {};
  }

  bool unitStep(SymRef Before, SymRef After, uint16_t Bytes, bool Increment) {
    return unitStepLane(Before, After, Bytes, Increment).has_value();
  }

  bool additiveRecurrence(SymRef Before, SymRef After, uint16_t Bytes) {
    auto &Ctx = Session.Context;
    const unsigned Total = Bytes * 8;
    for (unsigned Width = Total; Width; Width -= 8) {
      const auto Value = Ctx.mkExtract(After, 0, Width);
      const bool ZeroExtended = Ctx.op(Value) == SymOp::Add &&
                                After == Ctx.mkZExtOrTrunc(Value, Total);
      checker().nodes();
      if (!ZeroExtended)
        continue;
      const auto Input = Ctx.mkExtract(Before, 0, Width);
      checker().nodes();
      const auto Terms = Ctx.operands(Value);
      if (std::find(Terms.begin(), Terms.end(), Input) != Terms.end())
        return true;
    }
    for (unsigned Width = Total - 8; Width; Width -= 8)
      for (unsigned Offset = 0; Offset + Width <= Total; Offset += 8) {
        const auto Value = Ctx.mkExtract(After, Offset, Width);
        const auto Input = Ctx.mkExtract(Before, Offset, Width);
        const bool Preserved =
            Ctx.op(Value) == SymOp::Add &&
            preservesOutsideLane(Before, After, Offset, Width, Total);
        checker().nodes();
        if (!Preserved)
          continue;
        const auto Terms = Ctx.operands(Value);
        if (std::find(Terms.begin(), Terms.end(), Input) != Terms.end())
          return true;
      }
    return false;
  }

  std::vector<LowIRLoopLocation> locations() {
    std::vector<LowIRLoopLocation> Locations;
    std::set<uint64_t> Registers;
    for (uint64_t Byte : Session.RegisterBytes)
      Registers.insert(Byte & ~uint64_t{7});
    for (uint64_t Offset : Registers)
      Locations.push_back({LowIRLoopSpace::Register, Offset, 8});
    if (Contract.Frame)
      for (int64_t Offset = Contract.Frame->Begin;
           Offset < Contract.Frame->End;) {
        // Respect unaligned contract ends without merging overlapping words.
        const auto Bytes = static_cast<uint16_t>(
            std::min<uint64_t>(8 - (static_cast<uint64_t>(Offset) & 7),
                               static_cast<uint64_t>(Contract.Frame->End) -
                                   static_cast<uint64_t>(Offset)));
        Locations.push_back(
            {LowIRLoopSpace::Frame, static_cast<uint64_t>(Offset), Bytes});
        Offset += Bytes;
      }
    if (Contract.X64FlagsProfile)
      Locations.push_back({LowIRLoopSpace::SystemFlags, 0, 8});
    // Only real prefix definitions may become parameters. Missing bytes stay
    // missing even when their declared lifetime spans the loop. Group adjacent
    // defined bytes without rounding into an undefined or undeclared byte.
    const auto &Defined = Session.LoopPrefixes[ActiveCutpoint]
                              ->Candidate.DefinedFunctionTemporaries;
    for (auto It = Defined.begin(); It != Defined.end();) {
      const uint64_t Offset = *It++;
      uint16_t Bytes = 1;
      while (Bytes != 8 && It != Defined.end() && *It == Offset + Bytes) {
        ++Bytes;
        ++It;
      }
      Locations.push_back({LowIRLoopSpace::FunctionTemporary, Offset, Bytes});
    }
    return Locations;
  }

  bool addChangedWords(llvm::ArrayRef<TerminalState> States) {
    auto &Prefix = Session.LoopPrefixes[ActiveCutpoint]->Candidate;
    bool Added = false;
    for (const auto &Location : locations()) {
      if (std::any_of(words().begin(), words().end(), [&](const auto &W) {
            return std::tie(W.Location.Space, W.Location.Offset) ==
                   std::tie(Location.Space, Location.Offset);
          }))
        continue;
      const auto Initial = read(Prefix, Location);
      auto Mask = ones(Location.Bytes);
      for (const auto &State : States) {
        if (State.Cutpoint != static_cast<int>(ActiveCutpoint))
          continue;
        auto Copy = State;
        const auto Value = read(Copy, Location);
        // The transition can rebuild an unchanged value from constrained
        // parameters. Structural inequality must not discard those facts.
        Mask &=
            commonBitsUnder(Initial, Value, Location.Bytes, State.Predicate);
      }
      if (Mask != ones(Location.Bytes)) {
        words().push_back({Location, Mask});
        Added = true;
      }
    }
    return Added;
  }

  // A failed fixed-bit predicate is widened to bits structurally preserved
  // by the general transition. This avoids learning a spurious small counter
  // bound from the first few iterations. No widened predicate is trusted.
  bool widen() {
    auto &Ctx = Session.Context;
    auto &Prefix = Session.LoopPrefixes[ActiveCutpoint]->Candidate;
    bool Changed = addChangedWords(Session.CandidateReturns);
    for (auto &W : words()) {
      if (!W.FixedMask)
        continue;
      const auto Initial = read(Prefix, W.Location);
      const auto Before = read(*Session.CandidateStart, W.Location);
      auto Stable = W.FixedMask;
      bool Holds = true, Additive = false;
      for (auto &State : Session.CandidateReturns) {
        if (State.Cutpoint != static_cast<int>(ActiveCutpoint))
          continue;
        const auto After = read(State, W.Location);
        Additive |= additiveRecurrence(Before, After, W.Location.Bytes);
        Stable &= commonBits(Initial, After, W.Location.Bytes);
        const auto Mask = Ctx.mkConst(W.Location.Bytes * 8, W.FixedMask);
        Holds &= entails(State.Predicate, Ctx.mkEq(Ctx.mkAnd(Initial, Mask),
                                                   Ctx.mkAnd(After, Mask)));
      }
      // One additive arm can invalidate spurious prefix bits even when a
      // sibling stutters or resets. Structural bits still intersect every
      // returning arm, and an already valid mask is never weakened here.
      if (!Holds && !Additive) {
        // Relations such as packed flags often require the path predicate:
        // equal system bits need not have identical expression trees. Keep
        // exactly the candidate bits proved stable under this transition.
        Stable = W.FixedMask;
        for (auto &State : Session.CandidateReturns)
          if (State.Cutpoint == static_cast<int>(ActiveCutpoint))
            Stable &=
                commonBitsUnder(Initial, read(State, W.Location),
                                W.Location.Bytes, State.Predicate, W.FixedMask);
      }
      if (!Holds && Stable != W.FixedMask) {
        W.FixedMask = Stable;
        Changed = true;
      }
    }
    return Changed;
  }

  std::vector<LowIRLoopLocation> boundLocations() {
    auto &Ctx = Session.Context;
    std::set<SymRef> Seen, Compared;
    std::vector<SymRef> Pending;
    for (const auto &State : Session.CandidateReturns)
      if (State.Cutpoint == static_cast<int>(ActiveCutpoint))
        Pending.push_back(State.Predicate);
    while (!Pending.empty()) {
      const auto Ref = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(Ref).second)
        continue;
      if (++Result.PredicateNodes > Limits.Execution.MaxSymbolicNodes)
        stop(Status::BudgetExceeded,
             "loop inference predicate analysis budget exhausted");
      const auto &Node = Ctx.node(Ref);
      const auto Children = Ctx.operands(Ref);
      if (Node.Op == SymOp::Ult || Node.Op == SymOp::Ule)
        Compared.insert(Children.begin(), Children.end());
      Pending.insert(Pending.end(), Children.begin(), Children.end());
    }
    std::vector<LowIRLoopLocation> Bounds;
    std::set<SymRef> Values;
    auto &Prefix = Session.LoopPrefixes[ActiveCutpoint]->Candidate;
    for (const auto &Location : locations()) {
      if (std::any_of(words().begin(), words().end(), [&](const auto &W) {
            return W.Location.Space == Location.Space &&
                   W.Location.Offset == Location.Offset;
          }))
        continue;
      const auto Value = read(Prefix, Location);
      if (Compared.count(Value) && Values.insert(Value).second)
        Bounds.push_back(Location);
    }
    return Bounds;
  }

  void makeTemplate(std::optional<size_t> Ranked = {}, bool Complement = false,
                    bool ProgressGuard = false,
                    std::optional<LowIRLoopLocation> Bound = {}) {
    auto &Cut = Plan.Cutpoints[ActiveCutpoint];
    Cut.Inputs.clear();
    Cut.Expressions.clear();
    Cut.OriginalState.clear();
    Cut.CandidateState.clear();
    Cut.Predicate = NdVar::scalar(1, 1);
    Cut.Rank = {NdVar::scalar(0, 8)};
    uint64_t Next = 0;
    const auto Temp = [&](uint16_t Bytes) {
      const auto V = NdVar::tmp(Next, Bytes);
      Next += 8;
      return V;
    };
    const auto Expr = [&](NdOp Opcode, NdVar A, NdVar B, uint16_t Bytes) {
      LowOp Op;
      Op.Opcode = Opcode;
      Op.Output = Temp(Bytes);
      Op.addInput(A);
      Op.addInput(B);
      Cut.Expressions.push_back(Op);
      return Op.Output;
    };
    // Bind each represented word once. Relations between words must reuse
    // those parameters, rather than introduce unrelated copies of an input.
    std::vector<NdVar> Parameters;
    for (const auto &W : words()) {
      const auto Parameter = Temp(W.Location.Bytes);
      Parameters.push_back(Parameter);
      Cut.Inputs.push_back({LowIRLoopSide::Original, W.Location, Parameter});
    }
    for (size_t I = 0; I != words().size(); ++I) {
      const auto &W = words()[I];
      const auto Bytes = W.Location.Bytes;
      const auto Parameter = Parameters[I];
      Cut.OriginalState.push_back({W.Location, Parameter});
      Cut.CandidateState.push_back({W.Location, Parameter});
      if (W.FixedMask) {
        const auto Prefix = Temp(Bytes);
        Cut.Inputs.push_back(
            {LowIRLoopSide::OriginalPrefix, W.Location, Prefix});
        const auto Mask = NdVar::scalar(W.FixedMask, Bytes);
        const auto A = Expr(NdOp::INT_AND, Parameter, Mask, Bytes);
        const auto B = Expr(NdOp::INT_AND, Prefix, Mask, Bytes);
        const auto Same = Expr(NdOp::INT_EQUAL, A, B, 1);
        Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Same, 1);
      }
      if (W.UpperBound || W.LowerBound) {
        const auto Prefix = Temp(Bytes);
        Cut.Inputs.push_back(
            {LowIRLoopSide::OriginalPrefix, W.Location, Prefix});
        if (W.UpperBound) {
          const auto Bound = Expr(NdOp::INT_LESSEQUAL, Parameter, Prefix, 1);
          Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Bound, 1);
        }
        if (W.LowerBound) {
          const auto Bound = Expr(NdOp::INT_LESSEQUAL, Prefix, Parameter, 1);
          Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Bound, 1);
        }
      }
      const auto ValueAt = [&](const LowIRLoopLocation &L) {
        const auto Target =
            std::find_if(words().begin(), words().end(), [&](const auto &Q) {
              return sameLocation(Q.Location, L);
            });
        if (Target != words().end())
          return Parameters[std::distance(words().begin(), Target)];
        const auto Value = Temp(L.Bytes);
        Cut.Inputs.push_back({LowIRLoopSide::OriginalPrefix, L, Value});
        return Value;
      };
      for (const auto &B : W.BitRelations) {
        const auto Expected =
            Expr(B.Invert ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL,
                 ValueAt(B.Counter), ValueAt(B.Bound), 1);
        const auto Shifted = Expr(NdOp::INT_RIGHT, Parameter,
                                  NdVar::scalar(B.Bit, Bytes), Bytes);
        const auto Bit =
            Expr(NdOp::INT_AND, Shifted, NdVar::scalar(1, Bytes), Bytes);
        const auto Canonical =
            Expr(NdOp::INT_NOTEQUAL, Bit, NdVar::scalar(0, Bytes), 1);
        const auto Same = Expr(NdOp::INT_EQUAL, Canonical, Expected, 1);
        Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Same, 1);
      }
      for (const auto &L : W.Equalities) {
        const auto Same = Expr(NdOp::INT_EQUAL, Parameter, ValueAt(L), 1);
        Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Same, 1);
      }
      for (const auto &B : W.CounterBounds) {
        const auto Limit = Temp(Bytes);
        Cut.Inputs.push_back(
            {LowIRLoopSide::OriginalPrefix, B.Location, Limit});
        const auto Guard =
            Expr(B.Strict ? NdOp::INT_LESS : NdOp::INT_LESSEQUAL,
                 B.Lower ? Limit : Parameter, B.Lower ? Parameter : Limit, 1);
        Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Guard, 1);
      }
      for (bool Maximum : {false, true}) {
        if (!(Maximum ? W.Nonmax : W.Nonzero))
          continue;
        const auto Guard =
            Expr(NdOp::INT_NOTEQUAL, Parameter,
                 NdVar::scalar(Maximum ? ones(Bytes) : 0, Bytes), 1);
        Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Guard, 1);
      }
      for (const auto &G : W.LaneGuards) {
        const auto Mask = NdVar::scalar(G.Mask, Bytes);
        const auto Value = Expr(NdOp::INT_AND, Parameter, Mask, Bytes);
        const auto Guard =
            Expr(NdOp::INT_NOTEQUAL, Value,
                 NdVar::scalar(G.Maximum ? G.Mask : 0, Bytes), 1);
        Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Guard, 1);
      }
      if (Ranked != I)
        continue;
      Cut.Rank = {Complement ? Expr(NdOp::INT_XOR, Parameter,
                                    NdVar::scalar(ones(Bytes), Bytes), Bytes)
                             : Parameter};
      if (ProgressGuard) {
        const auto Varying = Expr(NdOp::INT_AND, Parameter,
                                  NdVar::scalar(~W.FixedMask, Bytes), Bytes);
        const auto Limit = NdVar::scalar(Complement ? ~W.FixedMask : 0, Bytes);
        const auto Guard = Expr(NdOp::INT_NOTEQUAL, Varying, Limit, 1);
        Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Guard, 1);
      }
      if (Bound) {
        const auto Limit = Temp(Bound->Bytes);
        Cut.Inputs.push_back({LowIRLoopSide::OriginalPrefix, *Bound, Limit});
        const auto Guard = Complement
                               ? Expr(NdOp::INT_LESS, Parameter, Limit, 1)
                               : Expr(NdOp::INT_LESS, Limit, Parameter, 1);
        Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Guard, 1);
      }
    }
    if (!checker().validateLoopPlan())
      throw Stop{};
  }

  void startGeneralTransition() {
    auto C = checker();
    if (!C.startLoopSegment(ActiveCutpoint))
      throw Stop{};
    try {
      runCandidate();
    } catch (const Stop &) {
      GeneralContractFailure =
          Session.Statistics.Status == Status::ContractViolation;
      throw;
    }
  }

  bool fits(TerminalState State) {
    auto C = checker();
    if (!C.validateInput())
      throw Stop{};
    C.EntryRoot = Session.EntryRoot;
    C.MemoryRoot = Session.MemoryRoot;
    const auto T =
        C.loopTemplate(ActiveCutpoint, &State, &State, State.Predicate);
    if (!entails(State.Predicate, T.Predicate))
      return false;
    try {
      auto Expected = T.Candidate;
      C.loopStateEqual(State.Predicate, State, Expected, "inferred candidate");
      return true;
    } catch (const Stop &) {
      if (Session.Statistics.Status != Status::Dependent)
        throw;
      return false;
    }
  }

  bool checkRank(llvm::ArrayRef<TerminalState> Prefixes) {
    for (const auto &State : Prefixes)
      if (State.Cutpoint == static_cast<int>(ActiveCutpoint) && !fits(State))
        return false;
    startGeneralTransition();
    bool HasBackedge = false;
    for (auto &State : Session.CandidateReturns) {
      if (State.Cutpoint != static_cast<int>(ActiveCutpoint))
        continue;
      HasBackedge = true;
      auto C = checker();
      auto T = C.loopTemplate(ActiveCutpoint, &State, &State, State.Predicate);
      if (!entails(State.Predicate,
                   Session.Context.mkUlt(T.Rank.front(),
                                         Session.StartingRank.front())))
        return false;
      if (!fits(State))
        return false;
    }
    return HasBackedge;
  }

  struct Edge {
    size_t Source;
    TerminalState Before, After;
  };

  bool inferAt(va_t Address) {
    GeneralContractFailure = false;
    auto &Cut = Plan.Cutpoints[ActiveCutpoint];
    Cut = {};
    Cut.OriginalAddress = Cut.CandidateAddress = Address;
    Cut.UseEntryPrefix = true;
    Cut.Rank = {NdVar::scalar(0, 8)};
    words().clear();
    Session.LoopPrefixes[ActiveCutpoint].reset();
    Session.OriginalStart.reset();
    Session.CandidateStart.reset();
    Session.StartingCutpoint = -1;
    runCandidate();
    auto Prefixes = Session.CandidateReturns;
    const auto It = std::find_if(Prefixes.begin(), Prefixes.end(),
                                 [](const auto &S) { return S.Cutpoint == 0; });
    if (It == Prefixes.end())
      return false;
    Session.LoopPrefixes[ActiveCutpoint] = LoopPrefix{*It, *It, It->Predicate};
    for (const auto &S : Prefixes)
      if (S.Cutpoint == static_cast<int>(ActiveCutpoint) &&
          !entails(S.Predicate, It->Predicate))
        Cut.GeneralizeEntryPrefix = true;
    Session.CandidateStart = *It;
    Session.StartingCutpoint = 0;
    Session.SegmentPredicate = It->Predicate;
    runCandidate();
    if (std::none_of(Session.CandidateReturns.begin(),
                     Session.CandidateReturns.end(),
                     [](const auto &S) { return S.Cutpoint == 0; }) &&
        !Cut.GeneralizeEntryPrefix)
      return false;
    addChangedWords(Prefixes);
    addChangedWords(Session.CandidateReturns);
    if (words().empty())
      return false;
    bool Stable = false;
    for (uint32_t Round = 0; Round != Limits.MaxWideningRounds; ++Round) {
      ++Result.WideningRounds;
      makeTemplate();
      startGeneralTransition();
      if (!widen()) {
        Stable = true;
        break;
      }
    }
    if (!Stable)
      stop(Status::BudgetExceeded, "loop inference widening budget exhausted");

    // Scalar guesses add guards and replace the transition snapshots. Keep
    // the complete stable domain for a later lexicographic proposal: a failed
    // scalar guard must not hide a stuttering sibling transition.
    const auto StableCut = Cut;
    std::vector<Edge> StableEdges;
    for (const auto &After : Session.CandidateReturns)
      if (After.Cutpoint == static_cast<int>(ActiveCutpoint))
        StableEdges.push_back({ActiveCutpoint, *Session.CandidateStart, After});

    // Prefer directly observed unit recurrences, avoiding expensive guesses
    // about accumulators before trying a plain induction variable.
    const auto Bounds = boundLocations();
    std::vector<std::pair<size_t, bool>> Ranks, OtherRanks;
    for (size_t I = 0; I != words().size(); ++I) {
      for (bool Complement : {false, true}) {
        const auto &W = words()[I];
        const auto Before = read(*Session.CandidateStart, W.Location);
        bool Unit = true;
        for (auto &State : Session.CandidateReturns)
          if (State.Cutpoint == static_cast<int>(ActiveCutpoint))
            Unit &= unitStep(Before, read(State, W.Location), W.Location.Bytes,
                             Complement);
        (Unit ? Ranks : OtherRanks).emplace_back(I, Complement);
      }
    }
    std::optional<TupleRankSearch> TupleSearch;
    bool TupleInitialized = false, TupleExhausted = false;
    const auto TryTuples = [&](std::optional<uint32_t> Attempts) {
      if (TupleExhausted)
        return false;
      Cut = StableCut;
      if (!TupleInitialized) {
        TupleInitialized = true;
        for (const auto &State : Prefixes)
          if (State.Cutpoint == static_cast<int>(ActiveCutpoint) &&
              !fits(State)) {
            TupleExhausted = true;
            return false;
          }
        for (const auto &E : StableEdges)
          if (!fits(E.After)) {
            TupleExhausted = true;
            return false;
          }
        TupleSearch = makeTupleSearch(StableEdges);
      }
      const auto Status = inferTupleRanks(StableEdges, *TupleSearch, Attempts);
      TupleExhausted = Status == TupleRankStatus::Exhausted;
      return Status == TupleRankStatus::Inferred;
    };
    uint32_t EarlyTupleAttempts = 0;
    const auto TryEarlyTuple = [&] {
      // Keep this window independent of the remaining budget, so an exact
      // replay follows the same proposal prefix. It grants no extra budget.
      if (EarlyTupleAttempts == 8)
        return false;
      const auto Before = Result.RankCandidates;
      const bool Inferred = TryTuples(1);
      EarlyTupleAttempts += Result.RankCandidates - Before;
      return Inferred;
    };
    const auto TryScalar = [&](size_t Index, bool Complement, bool Interleave) {
      const auto Try = [&](bool Guard, std::optional<LowIRLoopLocation> Bound) {
        if (Result.RankCandidates >= Limits.MaxRankCandidates)
          stop(Status::BudgetExceeded, "loop inference rank budget exhausted");
        ++Result.RankCandidates;
        makeTemplate(Index, Complement, Guard, Bound);
        return checkRank(Prefixes) || (Interleave && TryEarlyTuple());
      };
      if (Try(false, {}) || Try(true, {}))
        return true;
      for (const auto &Bound : Bounds)
        if (Bound.Bytes == words()[Index].Location.Bytes && Try(false, Bound))
          return true;
      return false;
    };
    for (const auto &[Index, Complement] : Ranks)
      if (TryScalar(Index, Complement, false))
        return true;
    if (TryEarlyTuple())
      return true;
    for (const auto &[Index, Complement] : OtherRanks)
      if (TryScalar(Index, Complement, true))
        return true;
    return TryTuples({});
  }

  void chargeCutSelection(uint64_t Count = 1) {
    if (Count > Limits.MaxCutSelectionWork - Result.CutSelectionWork)
      stop(Status::BudgetExceeded,
           "loop inference cut selection budget exhausted");
    Result.CutSelectionWork += Count;
  }

  struct CutSelectionCompare {
    LoopPlanInference *Owner;
    bool operator()(int A, int B) const {
      Owner->chargeCutSelection();
      return A < B;
    }
  };

  std::set<int> reconvergentBranches(llvm::ArrayRef<int> Order,
                                     llvm::ArrayRef<int> Backedges) {
    const CutSelectionCompare Compare{this};
    using SelectionSet = std::set<int, CutSelectionCompare>;
    SelectionSet Boundaries(Compare);
    std::map<int, SelectionSet> CommonPaths;
    for (int Id : Backedges) {
      chargeCutSelection();
      Boundaries.insert(Id);
    }
    // Cutting all DFS backedge targets leaves acyclic segments. Initialize
    // every boundary before postorder propagation: successors may reach an
    // ancestor boundary whose ordinary DFS visit has not finished yet.
    for (int Id : Boundaries) {
      chargeCutSelection(2);
      SelectionSet Singleton(Compare);
      Singleton.insert(Id);
      CommonPaths.emplace(Id, std::move(Singleton));
    }
    const auto CommonSuccessors = [&](int Id) {
      SelectionSet Common(Compare);
      bool First = true;
      chargeCutSelection();
      for (int S : Blocks.at(Id)->Succs) {
        chargeCutSelection();
        const auto &Other = CommonPaths.at(S);
        if (First) {
          chargeCutSelection(Other.size());
          Common = Other;
          First = false;
        } else {
          for (auto It = Common.begin(); It != Common.end();) {
            chargeCutSelection();
            if (!Other.count(*It))
              It = Common.erase(It);
            else
              ++It;
          }
        }
      }
      return Common;
    };
    for (int Id : Order) {
      chargeCutSelection();
      if (Boundaries.count(Id))
        continue;
      auto Common = CommonSuccessors(Id);
      chargeCutSelection(2);
      Common.insert(Id);
      CommonPaths.emplace(Id, std::move(Common));
    }
    std::set<int> Reconvergent;
    for (int Id : Order) {
      chargeCutSelection();
      if (Blocks.at(Id)->Succs.size() < 2)
        continue;
      // Include exits and boundaries, not just cyclic or eligible successors.
      // A boundary's successor intersection is temporary: its saved set must
      // remain the singleton used by transitions returning to that boundary.
      for (int Join : CommonSuccessors(Id)) {
        chargeCutSelection();
        if (!Boundaries.count(Join) && !Blocks.at(Join)->Succs.empty()) {
          chargeCutSelection();
          Reconvergent.insert(Id);
          break;
        }
      }
    }
    return Reconvergent;
  }

  void findChoices(llvm::ArrayRef<va_t> Eligible, bool BranchArms,
                   bool FilterBranches) {
    for (const auto &B : Candidate.Blocks)
      Blocks.emplace(B.Id, &B);
    const auto Root = std::find_if(
        Candidate.Blocks.begin(), Candidate.Blocks.end(),
        [&](const auto &B) { return B.StartAddr == Candidate.Entry; });
    std::map<int, unsigned> Color;
    std::vector<int> Order, Backedges;
    std::vector<std::pair<int, size_t>> Stack{{Root->Id, 0}};
    Color[Root->Id] = 1;
    while (!Stack.empty()) {
      auto &[Id, Next] = Stack.back();
      const auto &Succs = Blocks.at(Id)->Succs;
      if (Next == Succs.size()) {
        Color[Id] = 2;
        Order.push_back(Id);
        Stack.pop_back();
        continue;
      }
      const int S = Succs[Next++];
      if (Color[S] == 1)
        Backedges.push_back(S);
      else if (!Color[S]) {
        Color[S] = 1;
        Stack.emplace_back(S, 0);
      }
    }
    ReachableBlocks = Order;
    MultipleCycles =
        std::set<int>(Backedges.begin(), Backedges.end()).size() > 1;
    std::map<int, std::vector<int>> Preds;
    for (int Id : Order)
      for (int S : Blocks.at(Id)->Succs)
        Preds[S].push_back(Id);
    std::map<int, size_t> Components;
    std::vector<std::vector<int>> Members;
    for (auto It = Order.rbegin(); It != Order.rend(); ++It) {
      if (Components.count(*It))
        continue;
      const auto Index = Members.size();
      Members.emplace_back();
      std::vector<int> Pending{*It};
      Components[*It] = Index;
      while (!Pending.empty()) {
        const auto Id = Pending.back();
        Pending.pop_back();
        Members.back().push_back(Id);
        for (int P : Preds[Id])
          if (Components.emplace(P, Index).second)
            Pending.push_back(P);
      }
    }
    const auto Add = [&](int Id) {
      const auto Address = Blocks.at(Id)->StartAddr;
      if (!Eligible.empty() && std::find(Eligible.begin(), Eligible.end(),
                                         Address) == Eligible.end())
        return;
      if (std::find(Choices.begin(), Choices.end(), Address) == Choices.end())
        Choices.push_back(Address);
    };
    for (int Id : Order) {
      const auto Component = Components.at(Id);
      const auto &B = *Blocks.at(Id);
      const bool HasExit =
          std::any_of(B.Succs.begin(), B.Succs.end(),
                      [&](int S) { return Components.at(S) != Component; });
      if (HasExit && Members[Component].size() > 1)
        for (int S : B.Succs)
          if (Components.at(S) == Component)
            Add(S);
    }
    for (int Id : Backedges)
      Add(Id);
    for (int Id : Order)
      if (Members[Components.at(Id)].size() > 1)
        Add(Id);
    const auto Reconvergent = FilterBranches
                                  ? reconvergentBranches(Order, Backedges)
                                  : std::set<int>{};
    if (BranchArms)
      for (int Id : Order) {
        if (FilterBranches) {
          chargeCutSelection();
          if (Reconvergent.count(Id))
            continue;
        }
        const auto &B = *Blocks.at(Id);
        if (Members[Components.at(Id)].size() < 2 || B.Succs.size() < 2)
          continue;
        for (int S : B.Succs) {
          const auto Address = Blocks.at(S)->StartAddr;
          if (Components.at(S) == Components.at(Id) &&
              Blocks.at(S)->Succs.size() == 1 &&
              std::find(Choices.begin(), Choices.end(), Address) !=
                  Choices.end() &&
              std::find(BranchArmEntries.begin(), BranchArmEntries.end(),
                        Address) == BranchArmEntries.end()) {
            if (BranchArmEntries.size() >= Limits.MaxCutpointAttempts)
              stop(Status::BudgetExceeded,
                   "loop inference cutpoint budget exhausted");
            BranchArmEntries.push_back(Address);
          }
        }
      }
  }

  std::vector<int> uncoveredCycle(const std::set<va_t> &Selected) {
    std::map<int, unsigned> Color;
    std::vector<int> Cycle;
    // Removing a cut can disconnect a reachable cycle from the entry. Its
    // transitions still need coverage, so retain every originally reachable
    // block as a possible DFS root.
    for (int Root : ReachableBlocks) {
      if (Color[Root] || Selected.count(Blocks.at(Root)->StartAddr))
        continue;
      std::vector<std::pair<int, size_t>> Stack{{Root, 0}};
      Color[Root] = 1;
      while (!Stack.empty() && Cycle.empty()) {
        auto &[Id, Next] = Stack.back();
        const auto &Succs = Blocks.at(Id)->Succs;
        if (Next == Succs.size()) {
          Color[Id] = 2;
          Stack.pop_back();
          continue;
        }
        const int S = Succs[Next++];
        if (Selected.count(Blocks.at(S)->StartAddr))
          continue;
        if (Color[S] == 1) {
          const auto First = std::find_if(Stack.begin(), Stack.end(),
                                          [&](auto P) { return P.first == S; });
          for (auto It = First; It != Stack.end(); ++It)
            Cycle.push_back(It->first);
        } else if (!Color[S]) {
          Color[S] = 1;
          Stack.emplace_back(S, 0);
        }
      }
      if (!Cycle.empty())
        break;
    }
    return Cycle;
  }

  // Select a feedback set before symbolic execution. Trying a single cut on
  // a nested cycle first would spend the whole finite budget unrolling it.
  std::optional<std::vector<va_t>> feedbackCuts(std::set<va_t> Selected = {}) {
    if (Selected.size() > Limits.MaxCutpointAttempts)
      stop(Status::BudgetExceeded, "loop inference cutpoint budget exhausted");
    for (;;) {
      const auto Cycle = uncoveredCycle(Selected);
      if (Cycle.empty()) {
        std::vector<va_t> Cuts;
        for (va_t Address : Choices)
          if (Selected.count(Address))
            Cuts.push_back(Address);
        return Cuts;
      }
      const auto Choice =
          std::find_if(Choices.begin(), Choices.end(), [&](va_t A) {
            return std::any_of(Cycle.begin(), Cycle.end(), [&](int Id) {
              return Blocks.at(Id)->StartAddr == A;
            });
          });
      if (Choice == Choices.end())
        return {};
      if (Selected.size() >= Limits.MaxCutpointAttempts)
        stop(Status::BudgetExceeded,
             "loop inference cutpoint budget exhausted");
      Selected.insert(*Choice);
    }
  }

  std::vector<Edge> transitions(bool General) {
    std::vector<Edge> Edges;
    for (size_t I = 0; I != Plan.Cutpoints.size(); ++I) {
      ActiveCutpoint = I;
      Stage = "candidate transition " + std::to_string(I);
      if (General) {
        startGeneralTransition();
      } else {
        Session.CandidateStart = Session.LoopPrefixes[I]->Candidate;
        Session.StartingCutpoint = static_cast<int>(I);
        Session.SegmentPredicate = Session.CandidateStart->Predicate;
        runCandidate();
      }
      for (auto &After : Session.CandidateReturns)
        if (After.Cutpoint >= 0)
          Edges.push_back({I, *Session.CandidateStart, After});
    }
    return Edges;
  }

  // Cache variable-set scans across widening rounds in each cut. Rejected
  // dependency filters still consume the shared predicate DAG budget.
  const llvm::SmallVector<uint32_t, 16> &relationVars(SymRef Value) {
    auto [It, Added] = RelationVariables[ActiveCutpoint].try_emplace(Value);
    if (Added) {
      const auto Nodes = Session.Context.dagSize(Value);
      if (Nodes > Limits.Execution.MaxSymbolicNodes - Result.PredicateNodes)
        stop(Status::BudgetExceeded,
             "loop inference predicate analysis budget exhausted");
      Result.PredicateNodes += Nodes;
      Session.Context.collectVars(Value, It->second);
    }
    return It->second;
  }

  bool seedBitRelations(Word &W,
                        llvm::ArrayRef<TerminalState> NewIncoming = {}) {
    if (RelationArrivals[ActiveCutpoint].empty() ||
        std::popcount(ones(W.Location.Bytes) & ~W.FixedMask) > 16)
      return false;
    auto &Ctx = Session.Context;
    auto Incoming = RelationArrivals[ActiveCutpoint];
    Incoming.insert(Incoming.end(), NewIncoming.begin(), NewIncoming.end());
    const auto OldSize = W.BitRelations.size();
    // A cached field can first vary only after another cut generalizes. Its
    // initializer can also be constant-folded under an entry guard. Use any
    // incoming expression for the dependency filter, then prove the relation
    // on every saved concrete arrival and every current incoming state.
    for (unsigned Bit = 0; Bit != W.Location.Bytes * 8; ++Bit) {
      const auto Mask = uint64_t{1} << Bit;
      if (W.FixedMask & Mask)
        continue;
      for (const auto &L : RelationBounds[ActiveCutpoint]) {
        bool Depends = false;
        for (auto &S : Incoming) {
          const auto Initial = Ctx.mkExtract(read(S, W.Location), Bit, 1);
          if (Ctx.isConst(Initial))
            continue;
          const auto &BoundVars = relationVars(read(S, L));
          if (BoundVars.empty())
            continue;
          const auto &Vars = relationVars(Initial);
          if (std::includes(Vars.begin(), Vars.end(), BoundVars.begin(),
                            BoundVars.end())) {
            Depends = true;
            break;
          }
        }
        if (!Depends)
          continue;
        for (const auto &C : RelationCounters) {
          if (C.Bytes != L.Bytes)
            continue;
          for (bool Invert : {false, true}) {
            // A later transition can reveal another comparison for this bit.
            // Never retry a tuple whose relation was rejected or pruned.
            if (std::any_of(W.BitAttempts.begin(), W.BitAttempts.end(),
                            [&](const auto &A) {
                              return A.Bit == Bit && A.Invert == Invert &&
                                     sameLocation(A.Counter, C) &&
                                     sameLocation(A.Bound, L);
                            }))
              continue;
            W.BitAttempts.push_back({Bit, C, L, Invert});
            bool Holds = true;
            for (auto &S : Incoming) {
              const auto A = read(S, C), B = read(S, L);
              const auto Expected = Invert ? Ctx.mkNe(A, B) : Ctx.mkEq(A, B);
              if (!entails(S.Predicate,
                           Ctx.mkEq(Ctx.mkExtract(read(S, W.Location), Bit, 1),
                                    Expected))) {
                Holds = false;
                break;
              }
            }
            if (Holds)
              W.BitRelations.push_back({Bit, C, L, Invert});
          }
        }
      }
    }
    return OldSize != W.BitRelations.size();
  }

  bool seedCounterBounds(Word &W,
                         llvm::ArrayRef<TerminalState> NewIncoming = {}) {
    if (W.CounterBoundsSeeded || RelationArrivals[ActiveCutpoint].empty() ||
        std::none_of(
            RelationCounters.begin(), RelationCounters.end(),
            [&](const auto &L) { return sameLocation(W.Location, L); }))
      return false;
    W.CounterBoundsSeeded = true;
    auto &Ctx = Session.Context;
    auto &Prefix = Session.LoopPrefixes[ActiveCutpoint]->Candidate;
    auto Incoming = RelationArrivals[ActiveCutpoint];
    Incoming.insert(Incoming.end(), NewIncoming.begin(), NewIncoming.end());
    const auto OldSize = W.CounterBounds.size();
    for (const auto &L : RelationBounds[ActiveCutpoint]) {
      if (L.Bytes != W.Location.Bytes)
        continue;
      const auto Limit = read(Prefix, L);
      for (bool Lower : {false, true})
        for (bool Strict : {false, true}) {
          bool Holds = true;
          for (auto &S : Incoming) {
            const auto Value = read(S, W.Location);
            const auto A = Lower ? Limit : Value;
            const auto B = Lower ? Value : Limit;
            if (!entails(S.Predicate,
                         Strict ? Ctx.mkUlt(A, B) : Ctx.mkUle(A, B))) {
              Holds = false;
              break;
            }
          }
          if (Holds)
            W.CounterBounds.push_back({L, Lower, Strict});
        }
    }
    return OldSize != W.CounterBounds.size();
  }

  SymRef laneGuard(TerminalState &State, const Word &W,
                   const Word::LaneGuard &Guard) {
    auto &Ctx = Session.Context;
    const auto Bits = W.Location.Bytes * 8;
    const auto Value =
        Ctx.mkAnd(read(State, W.Location), Ctx.mkConst(Bits, Guard.Mask));
    return Ctx.mkNe(Value, Ctx.mkConst(Bits, Guard.Maximum ? Guard.Mask : 0));
  }

  bool seedLaneGuards(Word &W, llvm::ArrayRef<TerminalState> NewIncoming = {}) {
    if (RelationArrivals[ActiveCutpoint].empty())
      return false;
    auto Incoming = RelationArrivals[ActiveCutpoint];
    Incoming.insert(Incoming.end(), NewIncoming.begin(), NewIncoming.end());
    bool Changed = false;
    for (const auto &C : LaneCounters) {
      if (!sameLocation(W.Location, C.Location) ||
          std::find(W.LaneAttempts.begin(), W.LaneAttempts.end(), C.Guard) !=
              W.LaneAttempts.end())
        continue;
      // Once rejected or pruned, a guard must not be reseeded from a narrower
      // later domain. Saved concrete arrivals remain part of every seed proof.
      W.LaneAttempts.push_back(C.Guard);
      if (std::all_of(Incoming.begin(), Incoming.end(), [&](auto &S) {
            return entails(S.Predicate, laneGuard(S, W, C.Guard));
          })) {
        W.LaneGuards.push_back(C.Guard);
        Changed = true;
      }
    }
    return Changed;
  }

  bool seedWordCopies(llvm::ArrayRef<TerminalState> NewIncoming = {}) {
    if (RelationArrivals[ActiveCutpoint].empty())
      return false;
    auto &Ctx = Session.Context;
    auto &Prefix = Session.LoopPrefixes[ActiveCutpoint]->Candidate;
    auto Incoming = RelationArrivals[ActiveCutpoint];
    Incoming.insert(Incoming.end(), NewIncoming.begin(), NewIncoming.end());
    auto Targets = RelationCounters;
    Targets.insert(Targets.end(), RelationBounds[ActiveCutpoint].begin(),
                   RelationBounds[ActiveCutpoint].end());
    bool Changed = false;
    for (const auto &Source : locations())
      for (const auto &L : Targets) {
        if (L.Bytes != Source.Bytes || sameLocation(Source, L) ||
            read(Prefix, Source) != read(Prefix, L))
          continue;
        auto &Attempts = EqualityAttempts[ActiveCutpoint];
        if (std::any_of(Attempts.begin(), Attempts.end(), [&](const auto &A) {
              return sameLocation(A.Source, Source) &&
                     sameLocation(A.Target, L);
            }))
          continue;
        Attempts.push_back({Source, L});
        bool Holds = true;
        for (auto &S : Incoming)
          if (!entails(S.Predicate, Ctx.mkEq(read(S, Source), read(S, L)))) {
            Holds = false;
            break;
          }
        if (!Holds)
          continue;
        EqualitySeeds[ActiveCutpoint].push_back({Source, L});
        for (auto &W : words())
          if (sameLocation(W.Location, Source)) {
            W.Equalities.push_back(L);
            Changed = true;
          }
      }
    return Changed;
  }

  bool findNewCounters(std::vector<Edge> &Edges) {
    bool Changed = false;
    for (const auto &L : locations()) {
      bool Unit = false;
      for (auto &E : Edges)
        for (bool Increment : {false, true}) {
          const auto Mask = unitStepLane(read(E.Before, L), read(E.After, L),
                                         L.Bytes, Increment);
          Unit |= Mask.has_value();
          if (!Mask || !*Mask)
            continue;
          const Word::LaneGuard Guard{*Mask, Increment};
          if (std::none_of(
                  LaneCounters.begin(), LaneCounters.end(), [&](const auto &C) {
                    return sameLocation(C.Location, L) && C.Guard == Guard;
                  })) {
            LaneCounters.push_back({L, Guard});
            Changed = true;
          }
        }
      if (Unit &&
          std::none_of(RelationCounters.begin(), RelationCounters.end(),
                       [&](const auto &C) { return sameLocation(C, L); })) {
        RelationCounters.push_back(L);
        Changed = true;
      }
    }
    return Changed;
  }

  // Seed unsigned bounds and copies only from concrete reachable witnesses.
  // A bound can be an unchanged input used by an equality exit. A cached
  // operand can equal a current counter or bound. These remain untrusted
  // predicates, pruned by widening and independently proved after inference.
  void seedCounterRelations(std::vector<Edge> &Edges) {
    auto &Ctx = Session.Context;
    const auto Locations = locations();
    findNewCounters(Edges);
    const auto &Counters = RelationCounters;
    std::vector<SymRef> Pending;
    for (const auto &E : Edges)
      Pending.push_back(E.After.Predicate);
    for (const auto &S : EntryArrivals)
      Pending.push_back(S.Predicate);
    for (const auto &P : Session.LoopPrefixes)
      Pending.push_back(P->Predicate);
    std::set<SymRef> Seen;
    while (!Pending.empty()) {
      const auto Ref = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(Ref).second)
        continue;
      if (++Result.PredicateNodes > Limits.Execution.MaxSymbolicNodes)
        stop(Status::BudgetExceeded,
             "loop inference predicate analysis budget exhausted");
      const auto Children = Ctx.operands(Ref);
      Pending.insert(Pending.end(), Children.begin(), Children.end());
    }
    for (size_t I = 0; I != Plan.Cutpoints.size(); ++I) {
      ActiveCutpoint = I;
      auto &Prefix = Session.LoopPrefixes[I]->Candidate;
      for (const auto &L : Counters)
        if (std::none_of(words().begin(), words().end(), [&](const auto &W) {
              return sameLocation(W.Location, L);
            })) {
          words().push_back({L, ones(L.Bytes)});
          initializePrefixBounds(words().back());
        }
      std::vector<TerminalState> Incoming{Prefix};
      for (const auto &S : EntryArrivals)
        if (S.Cutpoint == static_cast<int>(I))
          Incoming.push_back(S);
      for (const auto &E : Edges)
        if (E.After.Cutpoint == static_cast<int>(I))
          Incoming.push_back(E.After);
      std::vector<LowIRLoopLocation> Bounds;
      std::set<SymRef> Values;
      for (const auto &L : Locations) {
        if (std::any_of(words().begin(), words().end(), [&](const auto &W) {
              return sameLocation(W.Location, L);
            }))
          continue;
        const auto V = read(Prefix, L);
        if (!Ctx.isConst(V) && Seen.count(V) && Values.insert(V).second)
          Bounds.push_back(L);
      }
      RelationBounds[I] = Bounds;
      RelationArrivals[I] = Incoming;
      for (auto &W : words())
        seedBitRelations(W);
      seedWordCopies();
      for (auto &W : words()) {
        seedCounterBounds(W);
        seedLaneGuards(W);
      }
    }
  }

  // Houdini-style pruning: keep a candidate bound only while every observed
  // and general incoming transition proves it. Fixed bits share the scalar
  // widening rules with single-cut inference; no learned fact is assumed by
  // the final original/candidate checker.
  bool widenIncoming(std::vector<Edge> &Edges) {
    auto &Ctx = Session.Context;
    // An outer counter's first step may be hidden until an inner cut widens.
    // Discovery supplies candidates only; all predicates and ranks are proved.
    bool Changed = findNewCounters(Edges);
    for (size_t I = 0; I != Plan.Cutpoints.size(); ++I) {
      ActiveCutpoint = I;
      auto &Prefix = Session.LoopPrefixes[I]->Candidate;
      std::vector<TerminalState> Incoming{Prefix};
      for (const auto &S : EntryArrivals)
        if (S.Cutpoint == static_cast<int>(I))
          Incoming.push_back(S);
      for (const auto &E : Edges)
        if (E.After.Cutpoint == static_cast<int>(I))
          Incoming.push_back(E.After);
      auto &Cut = Plan.Cutpoints[I];
      if (!Cut.GeneralizeEntryPrefix)
        for (const auto &S : Incoming)
          if (!entails(S.Predicate, Prefix.Predicate)) {
            // A feasible incoming state need not belong to the first witness
            // path. Propose a total template and restart widening over its
            // expanded domain; the final checker still proves every arrival.
            Cut.GeneralizeEntryPrefix = true;
            Changed = true;
            break;
          }
      const auto OldSize = words().size();
      Changed |= addChangedWords(Incoming);
      if (!RelationArrivals[I].empty())
        for (const auto &L : RelationCounters)
          if (std::none_of(words().begin(), words().end(), [&](const auto &W) {
                return sameLocation(W.Location, L);
              })) {
            words().push_back({L, ones(L.Bytes)});
            Changed = true;
          }
      for (size_t J = OldSize; J != words().size(); ++J)
        initializePrefixBounds(words()[J]);
      Changed |= seedWordCopies(Incoming);
      for (auto &W : words()) {
        const auto Initial = read(Prefix, W.Location);
        auto Stable = W.FixedMask;
        bool Holds = true, Additive = false;
        for (auto &E : Edges) {
          if (E.After.Cutpoint != static_cast<int>(I))
            continue;
          const auto After = read(E.After, W.Location);
          Additive |= additiveRecurrence(read(E.Before, W.Location), After,
                                         W.Location.Bytes);
          // A copied counter has a distinct state parameter, constrained to
          // its source by the template. Recognize that source's recurrence
          // when dropping spurious fixed bits; the relation itself is still
          // pruned on every incoming state below.
          for (const auto &L : W.Equalities)
            Additive |=
                additiveRecurrence(read(E.Before, L), After, W.Location.Bytes);
          Stable &= commonBits(Initial, After, W.Location.Bytes);
          const auto Mask = Ctx.mkConst(W.Location.Bytes * 8, W.FixedMask);
          if (W.FixedMask)
            Holds &=
                entails(E.After.Predicate, Ctx.mkEq(Ctx.mkAnd(Initial, Mask),
                                                    Ctx.mkAnd(After, Mask)));
        }
        if (!Holds && !Additive) {
          Stable = W.FixedMask;
          for (auto &State : Incoming)
            Stable &=
                commonBitsUnder(Initial, read(State, W.Location),
                                W.Location.Bytes, State.Predicate, W.FixedMask);
        }
        if (!Holds && Stable != W.FixedMask) {
          W.FixedMask = Stable;
          Changed = true;
        }
        for (auto &State : Incoming) {
          const auto Value = read(State, W.Location);
          const auto Keep = [&](bool &Enabled, SymRef Fact) {
            if (Enabled && !entails(State.Predicate, Fact)) {
              Enabled = false;
              Changed = true;
            }
          };
          Keep(W.UpperBound, Ctx.mkUle(Value, Initial));
          Keep(W.LowerBound, Ctx.mkUle(Initial, Value));
          Keep(W.Nonzero,
               Ctx.mkNe(Value, Ctx.mkConst(W.Location.Bytes * 8, 0)));
          Keep(W.Nonmax, Ctx.mkNe(Value, Ctx.mkConst(W.Location.Bytes * 8,
                                                     ones(W.Location.Bytes))));
        }
        Changed |= std::erase_if(W.BitRelations, [&](const auto &B) {
                     for (auto &S : Incoming) {
                       const auto A = read(S, B.Counter), Z = read(S, B.Bound);
                       const auto Expected =
                           B.Invert ? Ctx.mkNe(A, Z) : Ctx.mkEq(A, Z);
                       if (!entails(S.Predicate,
                                    Ctx.mkEq(Ctx.mkExtract(read(S, W.Location),
                                                           B.Bit, 1),
                                             Expected)))
                         return true;
                     }
                     return false;
                   }) != 0;
        Changed |= std::erase_if(W.Equalities, [&](const auto &L) {
                     for (auto &S : Incoming)
                       if (!entails(S.Predicate,
                                    Ctx.mkEq(read(S, W.Location), read(S, L))))
                         return true;
                     return false;
                   }) != 0;
        Changed |= std::erase_if(W.CounterBounds, [&](const auto &B) {
                     const auto Limit = read(Prefix, B.Location);
                     for (auto &S : Incoming) {
                       const auto Value = read(S, W.Location);
                       const auto A = B.Lower ? Limit : Value;
                       const auto Z = B.Lower ? Value : Limit;
                       if (!entails(S.Predicate, B.Strict ? Ctx.mkUlt(A, Z)
                                                          : Ctx.mkUle(A, Z)))
                         return true;
                     }
                     return false;
                   }) != 0;
        Changed |= std::erase_if(W.LaneGuards, [&](const auto &Guard) {
                     return std::any_of(
                         Incoming.begin(), Incoming.end(), [&](auto &S) {
                           return !entails(S.Predicate, laneGuard(S, W, Guard));
                         });
                   }) != 0;
        Changed |= seedLaneGuards(W, Incoming);
        Changed |= seedCounterBounds(W, Incoming);
        Changed |= seedBitRelations(W, Incoming);
      }
    }
    return Changed;
  }

  struct Counter {
    LowIRLoopLocation Location;
    bool Complement;
  };
  struct Phases {
    std::vector<std::vector<uint64_t>> Between;
    std::vector<uint64_t> Leading;
  };

  struct PhaseConstraint {
    size_t Source, Target;
    uint64_t Step;
  };

  static bool solvePhaseConstraints(llvm::ArrayRef<PhaseConstraint> Constraints,
                                    std::vector<uint64_t> &Phase) {
    // Nonnegative weighted difference constraints. A positive cycle cannot
    // be discharged by phase constants; it requires another changing rank.
    for (size_t Round = 0; Round != Phase.size(); ++Round) {
      bool Changed = false;
      for (const auto &C : Constraints)
        if (Phase[C.Source] < Phase[C.Target] + C.Step) {
          Phase[C.Source] = Phase[C.Target] + C.Step;
          Changed = true;
        }
      if (!Changed)
        return true;
      if (Round + 1 == Phase.size())
        return false;
    }
    return true;
  }

  static bool sameLocation(const LowIRLoopLocation &A,
                           const LowIRLoopLocation &B) {
    return std::tie(A.Space, A.Offset, A.Bytes) ==
           std::tie(B.Space, B.Offset, B.Bytes);
  }

  SymRef counterValue(TerminalState &State, const Counter &Rank) {
    const auto Value = read(State, Rank.Location);
    return Rank.Complement
               ? Session.Context.mkXor(
                     Value, Session.Context.mkConst(Rank.Location.Bytes * 8,
                                                    ones(Rank.Location.Bytes)))
               : Value;
  }

  SymRef tupleLess(Edge &E, llvm::ArrayRef<Counter> Ranks, const Phases &Phase,
                   size_t First = 0) {
    auto &Ctx = Session.Context;
    auto Equal = Ctx.mkTrue(), Less = Ctx.mkFalse();
    const auto Append = [&](SymRef Before, SymRef After) {
      Less = Ctx.mkOr(Less, Ctx.mkAnd(Equal, Ctx.mkUlt(After, Before)));
      Equal = Ctx.mkAnd(Equal, Ctx.mkEq(After, Before));
    };
    if (First == 0 && !Phase.Leading.empty())
      Append(Ctx.mkConst(64, Phase.Leading[E.Source]),
             Ctx.mkConst(64, Phase.Leading[E.After.Cutpoint]));
    for (size_t I = First; I != Ranks.size(); ++I) {
      Append(counterValue(E.Before, Ranks[I]), counterValue(E.After, Ranks[I]));
      if (I + 1 != Ranks.size())
        Append(Ctx.mkConst(64, Phase.Between[I][E.Source]),
               Ctx.mkConst(64, Phase.Between[I][E.After.Cutpoint]));
    }
    return Less;
  }

  bool inferPhases(std::vector<Edge> &Edges, llvm::ArrayRef<Counter> Ranks,
                   Phases &Phase, bool Leading) {
    auto &Ctx = Session.Context;
    const auto Count = Plan.Cutpoints.size();
    Phase.Between.assign(Ranks.size() - 1, std::vector<uint64_t>(Count));
    if (Leading)
      Phase.Leading.assign(Count, 0);
    for (size_t Level = Ranks.size() - 1; Level-- > 0;) {
      std::vector<PhaseConstraint> Constraints;
      for (auto &E : Edges) {
        auto Domain = E.After.Predicate;
        for (size_t I = 0; I <= Level; ++I)
          Domain = Ctx.mkAnd(Domain, Ctx.mkEq(counterValue(E.Before, Ranks[I]),
                                              counterValue(E.After, Ranks[I])));
        if (entails(Domain, Ctx.mkFalse()))
          continue;
        Constraints.push_back(
            {E.Source, static_cast<size_t>(E.After.Cutpoint),
             entails(Domain, tupleLess(E, Ranks, Phase, Level + 1)) ? 0U : 1U});
      }
      if (!solvePhaseConstraints(Constraints, Phase.Between[Level]))
        return false;
    }
    if (Leading) {
      std::vector<PhaseConstraint> Constraints;
      for (auto &E : Edges) {
        // The leading phase must not increase on any feasible transition.
        // Unlike an inner phase, it cannot assume equal counter prefixes.
        if (entails(E.After.Predicate, Ctx.mkFalse()))
          continue;
        Constraints.push_back(
            {E.Source, static_cast<size_t>(E.After.Cutpoint),
             entails(E.After.Predicate, tupleLess(E, Ranks, Phase)) ? 0U : 1U});
      }
      if (!solvePhaseConstraints(Constraints, Phase.Leading))
        return false;
    }
    for (auto &E : Edges)
      if (!entails(E.After.Predicate, tupleLess(E, Ranks, Phase)))
        return false;
    return !Edges.empty();
  }

  void installRanks(llvm::ArrayRef<Counter> Ranks, const Phases &Phase) {
    for (size_t I = 0; I != Plan.Cutpoints.size(); ++I) {
      auto &Cut = Plan.Cutpoints[I];
      uint64_t Next = 0;
      for (const auto &Input : Cut.Inputs)
        Next = std::max(Next, Input.Temporary.Offset + 8);
      for (const auto &Op : Cut.Expressions)
        Next = std::max(Next, Op.Output.Offset + 8);
      Cut.Rank.clear();
      if (!Phase.Leading.empty())
        Cut.Rank.push_back(NdVar::scalar(Phase.Leading[I], 8));
      for (size_t J = 0; J != Ranks.size(); ++J) {
        const auto &R = Ranks[J];
        auto Input = std::find_if(
            Cut.Inputs.begin(), Cut.Inputs.end(), [&](const auto &Input) {
              return Input.Side == LowIRLoopSide::Original &&
                     sameLocation(Input.Location, R.Location);
            });
        NdVar Value;
        if (Input != Cut.Inputs.end()) {
          Value = Input->Temporary;
        } else {
          Value = NdVar::tmp(Next, R.Location.Bytes);
          Next += 8;
          Cut.Inputs.push_back(
              {LowIRLoopSide::OriginalPrefix, R.Location, Value});
        }
        if (R.Complement) {
          LowOp Op;
          Op.Opcode = NdOp::INT_XOR;
          Op.Output = NdVar::tmp(Next, R.Location.Bytes);
          Next += 8;
          Op.addInput(Value);
          Op.addInput(NdVar::scalar(ones(R.Location.Bytes), R.Location.Bytes));
          Cut.Expressions.push_back(Op);
          Value = Op.Output;
        }
        Cut.Rank.push_back(Value);
        if (J + 1 != Ranks.size())
          Cut.Rank.push_back(NdVar::scalar(Phase.Between[J][I], 8));
      }
    }
    if (!checker().validateLoopPlan())
      throw Stop{};
  }

  struct TupleRankSearch {
    std::vector<Counter> Candidates, Ranks;
    std::vector<size_t> Next{0};
    size_t Size = 1, MaxSize = 0;
    bool Resume = false, PendingLeading = false;
  };
  enum class TupleRankStatus { Inferred, Exhausted, Paused };

  TupleRankSearch makeTupleSearch(std::vector<Edge> &Edges) {
    TupleRankSearch Search;
    for (const auto &Location : locations()) {
      const auto Before = Search.Candidates.size();
      for (bool Complement : {false, true}) {
        bool Unit = false;
        for (auto &E : Edges)
          Unit |= unitStep(read(E.Before, Location), read(E.After, Location),
                           Location.Bytes, Complement);
        if (Unit)
          Search.Candidates.push_back({Location, Complement});
      }
      Search.MaxSize += Search.Candidates.size() != Before;
    }
    return Search;
  }

  static bool nextTuple(TupleRankSearch &Search) {
    auto &[Candidates, Ranks, Next, Size, MaxSize, Resume, PendingLeading] =
        Search;
    if (Resume) {
      Ranks.pop_back();
      Next.pop_back();
      Resume = false;
    }
    while (Size <= MaxSize) {
      if (Ranks.size() == Size) {
        Resume = true;
        return true;
      }
      if (Next.back() == Candidates.size()) {
        if (Ranks.empty()) {
          ++Size;
          Next.front() = 0;
        } else {
          Ranks.pop_back();
          Next.pop_back();
        }
        continue;
      }
      const auto &C = Candidates[Next.back()++];
      if (std::any_of(Ranks.begin(), Ranks.end(), [&](const auto &R) {
            return sameLocation(C.Location, R.Location);
          }))
        continue;
      Ranks.push_back(C);
      Next.push_back(0);
    }
    return false;
  }

  TupleRankStatus inferTupleRanks(std::vector<Edge> &Edges,
                                  TupleRankSearch &Search,
                                  std::optional<uint32_t> MaxAttempts = {}) {
    for (uint32_t Attempt = 0; !MaxAttempts || Attempt < *MaxAttempts;
         ++Attempt) {
      const bool Leading = Search.PendingLeading;
      if (!Leading && !nextTuple(Search))
        return TupleRankStatus::Exhausted;
      if (Result.RankCandidates >= Limits.MaxRankCandidates)
        stop(Status::BudgetExceeded, "loop inference rank budget exhausted");
      ++Result.RankCandidates;
      // Keep each variant as one budgeted attempt, including across a
      // one-attempt pause. A single cut cannot benefit from a constant prefix.
      Search.PendingLeading = !Leading && Plan.Cutpoints.size() > 1;
      Phases Phase;
      if (inferPhases(Edges, Search.Ranks, Phase, Leading)) {
        installRanks(Search.Ranks, Phase);
        return TupleRankStatus::Inferred;
      }
    }
    return TupleRankStatus::Paused;
  }

  bool inferMultiple(llvm::ArrayRef<va_t> Cuts) {
    Result.CutpointAttempts += Cuts.size();
    Models.assign(Cuts.size(), {});
    EqualitySeeds.assign(Cuts.size(), {});
    EqualityAttempts.assign(Cuts.size(), {});
    RelationCounters.clear();
    LaneCounters.clear();
    RelationBounds.assign(Cuts.size(), {});
    RelationArrivals.assign(Cuts.size(), {});
    RelationVariables.assign(Cuts.size(), {});
    Plan.Cutpoints.assign(Cuts.size(), {});
    Session.LoopPrefixes.assign(Cuts.size(), {});
    for (size_t I = 0; I != Cuts.size(); ++I) {
      auto &Cut = Plan.Cutpoints[I];
      Cut.OriginalAddress = Cut.CandidateAddress = Cuts[I];
      Cut.UseEntryPrefix = true;
      Cut.Rank = {NdVar::scalar(0, 8)};
    }
    // These are candidate-only reachable witnesses. The final checker must
    // recapture paired original/candidate prefixes with its own fresh state.
    for (size_t I = 0; I != Cuts.size(); ++I) {
      Session.PrefixSearchCutpoint = static_cast<int>(I);
      Stage = "candidate entry prefix " + std::to_string(I);
      runCandidate();
      const auto It = std::find_if(
          Session.CandidateReturns.begin(), Session.CandidateReturns.end(),
          [&](const auto &S) { return S.Cutpoint == static_cast<int>(I); });
      if (It == Session.CandidateReturns.end())
        return false;
      Session.LoopPrefixes[I] = LoopPrefix{*It, *It, It->Predicate};
    }
    Session.PrefixSearchCutpoint = -1;
    Session.OriginalStart.reset();
    Session.CandidateStart.reset();
    Session.StartingCutpoint = -1;
    Stage = "candidate entry arrivals";
    runCandidate();
    EntryArrivals = Session.CandidateReturns;
    auto Edges = transitions(false);
    widenIncoming(Edges);
    seedCounterRelations(Edges);
    // Newly represented counters also need their fixed bits and prefix facts
    // pruned on every concrete arrival before constructing a general domain.
    widenIncoming(Edges);
    bool Stable = false;
    for (uint32_t Round = 0; Round != Limits.MaxWideningRounds; ++Round) {
      ++Result.WideningRounds;
      for (size_t I = 0; I != Cuts.size(); ++I) {
        ActiveCutpoint = I;
        makeTemplate();
      }
      Edges = transitions(true);
      if (!widenIncoming(Edges)) {
        Stable = true;
        break;
      }
    }
    if (!Stable)
      stop(Status::BudgetExceeded, "loop inference widening budget exhausted");
    for (size_t I = 0; I != Cuts.size(); ++I) {
      ActiveCutpoint = I;
      if (!fits(Session.LoopPrefixes[I]->Candidate))
        return false;
    }
    for (const auto &E : Edges) {
      ActiveCutpoint = E.After.Cutpoint;
      if (!fits(E.After))
        return false;
    }
    for (const auto &S : EntryArrivals)
      if (S.Cutpoint >= 0) {
        ActiveCutpoint = S.Cutpoint;
        if (!fits(S))
          return false;
      }
    auto Search = makeTupleSearch(Edges);
    return inferTupleRanks(Edges, Search) == TupleRankStatus::Inferred;
  }

public:
  LoopPlanInference(const LowFunc &F, const LowIRIndependenceContract &C,
                    const LowIRLoopInferenceLimits &L,
                    SpecializationProvider *Reader = nullptr)
      : Candidate(F), Contract(C), Limits(L), ExecutionLimits{L.Execution},
        Session{{}, {}, F, LowIRRefinementWitness::LiftedBits, ExecutionLimits},
        ReadProvider(Reader) {
    Plan.Cutpoints.resize(1);
    Plan.Cutpoints[ActiveCutpoint].OriginalAddress =
        Plan.Cutpoints[ActiveCutpoint].CandidateAddress = F.Entry;
    Plan.Cutpoints[ActiveCutpoint].Rank = {NdVar::scalar(0, 8)};
    Session.LoopPlan = &Plan;
    Session.Original = &F;
    Session.LoopPrefixes.resize(1);
  }

  LowIRLoopInferenceResult
  run(llvm::ArrayRef<va_t> Eligible,
      detail::LowIRLoopCutFamily Family = detail::LowIRLoopCutFamily::Default,
      llvm::ArrayRef<const LowIRLoopRefinementPlan *> PreviousPlans = {}) {
    std::string LastTemplateFailure;
    const bool BranchArms = Family != detail::LowIRLoopCutFamily::Default;
    const bool FilterBranches =
        Family == detail::LowIRLoopCutFamily::FilteredBranchArms;
    try {
      if (Contract.RetainUnauditedNativeBoundaries && !ReadProvider)
        stop(Status::Unsupported,
             "unaudited boundaries are unsupported by loop inference");
      if (Contract.AllowOverlappingNativeInstructions)
        stop(Status::Unsupported,
             "overlapping instructions are unsupported by loop inference");
      if (Contract.DeferNativeConditionalEdges && !ReadProvider)
        stop(Status::Unsupported,
             "deferred conditional edges are unsupported by loop inference");
      if (!prepareCandidateRecords(Session, Records) ||
          !checker().validateInput())
        throw Stop{};
      if (Eligible.size() > Limits.Execution.MaxBlockVisits)
        stop(Status::BudgetExceeded,
             "loop inference eligibility budget exhausted");
      if (!Limits.MaxCutpointAttempts || !Limits.MaxWideningRounds ||
          !Limits.MaxRankCandidates)
        stop(Status::BudgetExceeded, "loop inference search budget exhausted");
      findChoices(Eligible, BranchArms, FilterBranches);
      if (BranchArms && BranchArmEntries.empty())
        stop(Status::Unsupported, "no cyclic branch-arm cutpoints");
      // A complete branch-arm set can preserve phases that a shared header
      // collapses. Add feedback cuts only for cycles it does not yet cover.
      const auto Feedback =
          BranchArms ? feedbackCuts(std::set<va_t>(BranchArmEntries.begin(),
                                                   BranchArmEntries.end()))
                     : feedbackCuts();
      if (!Feedback)
        stop(Status::Unsupported,
             "eligible cutpoints do not cover every cycle");
      for (const auto *Previous : PreviousPlans)
        if (BranchArms && Feedback->size() == Previous->Cutpoints.size() &&
            std::all_of(Feedback->begin(), Feedback->end(), [&](va_t Address) {
              return std::any_of(
                  Previous->Cutpoints.begin(), Previous->Cutpoints.end(),
                  [&](const auto &C) { return C.OriginalAddress == Address; });
            }))
          stop(Status::Unsupported,
               FilterBranches ? "filtered cuts duplicate previous plan"
                              : "branch-arm cuts duplicate previous plan");
      if (Feedback->size() > 1 || MultipleCycles) {
        if (inferMultiple(*Feedback)) {
          Result.Status = LowIRLoopInferenceStatus::Inferred;
          Result.Plan = Plan;
        }
      } else
        for (va_t Address : BranchArms ? *Feedback : Choices) {
          if (Result.CutpointAttempts >= Limits.MaxCutpointAttempts)
            stop(Status::BudgetExceeded,
                 "loop inference cutpoint budget exhausted");
          ++Result.CutpointAttempts;
          if (!uncoveredCycle({Address}).empty())
            continue;
          try {
            if (inferAt(Address)) {
              Result.Status = LowIRLoopInferenceStatus::Inferred;
              Result.Plan = Plan;
              break;
            }
          } catch (const Stop &) {
            // An overgeneralized template can violate a return or frame
            // contract even when another cut admits a valid invariant. Retry
            // only that hypothesis failure. Real prefixes, malformed input,
            // unsupported execution and exhausted shared budgets still stop.
            if (!GeneralContractFailure)
              throw;
            LastTemplateFailure = Session.Statistics.Diagnostic;
          }
        }
      if (!Result.inferred())
        Result.Diagnostic = LastTemplateFailure.empty()
                                ? "no loop template and unsigned rank inferred"
                                : LastTemplateFailure;
    } catch (const Stop &) {
      Result.Diagnostic = Session.Statistics.Diagnostic;
      if (!Stage.empty())
        Result.Diagnostic += " while exploring " + Stage;
      Result.Status = Session.Statistics.Status == Status::BudgetExceeded
                          ? LowIRLoopInferenceStatus::BudgetExceeded
                      : Session.Statistics.Status == Status::Invalid
                          ? LowIRLoopInferenceStatus::Invalid
                          : LowIRLoopInferenceStatus::Unsupported;
    }
    Result.Operations = Session.Statistics.Operations;
    Result.BlockId = Session.Statistics.BlockId;
    Result.InstructionAddress = Session.Statistics.InstructionAddress;
    Result.OpSeq = Session.Statistics.OpSeq;
    Result.SolverQueries = Session.Statistics.SolverQueries;
    Result.ScheduledPaths = Session.ScheduledPaths;
    return std::move(Result);
  }
};
} // namespace

LowIRLoopInferenceResult
inferLowIRLoopRefinementPlan(const LowFunc &Candidate,
                             const LowIRIndependenceContract &Contract,
                             const LowIRLoopInferenceLimits &Limits,
                             llvm::ArrayRef<va_t> EligibleCutpoints) {
  return LoopPlanInference(Candidate, Contract, Limits).run(EligibleCutpoints);
}

LowIRLoopInferenceResult detail::inferBranchArmLowIRLoopRefinementPlan(
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    const LowIRLoopInferenceLimits &Limits,
    const LowIRLoopRefinementPlan *DefaultPlan) {
  return inferLowIRLoopRefinementPlanFamily(
      Candidate, Contract, Limits, LowIRLoopCutFamily::BranchArms,
      DefaultPlan
          ? llvm::ArrayRef<const LowIRLoopRefinementPlan *>(&DefaultPlan, 1)
          : llvm::ArrayRef<const LowIRLoopRefinementPlan *>{});
}

LowIRLoopInferenceResult detail::inferLowIRLoopRefinementPlanFamily(
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    const LowIRLoopInferenceLimits &Limits, LowIRLoopCutFamily Family,
    llvm::ArrayRef<const LowIRLoopRefinementPlan *> PreviousPlans) {
  return LoopPlanInference(Candidate, Contract, Limits)
      .run({}, Family, PreviousPlans);
}

LowIRLoopInferenceResult detail::inferNativeLowIRLoopRefinementPlan(
    SpecializationProvider &Provider, const LowFunc &Candidate,
    const LowIRIndependenceContract &Contract,
    const LowIRLoopInferenceLimits &Limits,
    llvm::ArrayRef<va_t> EligibleCutpoints) {
  return LoopPlanInference(Candidate, Contract, Limits, &Provider)
      .run(EligibleCutpoints);
}

LowIRIndependenceResult checkLowIRUndefinedIndependence(
    const LowFunc &Function,
    llvm::ArrayRef<LowIRUndefinedInstruction> Instructions,
    const LowIRIndependenceContract &Contract,
    const LowIRIndependenceLimits &Limits) {
  return Checker(Function, Instructions, Contract, Limits).run();
}

detail::NativeUndefinedIndependenceResult
detail::checkNativeUndefinedIndependence(
    SpecializationProvider &Provider, SpecializationCursor Entry,
    const LowIRIndependenceContract &Contract,
    const LowIRIndependenceLimits &Limits) {
  NativeUndefinedIndependenceResult Result;
  Result.Proof = Checker(Provider, Entry, Contract, Limits, Result).run();
  if (!Result.Proof.proved()) {
    Result.Instructions.clear();
    Result.Reads.clear();
  }
  return Result;
}

LowIRRefinementResult checkLowIRRefinement(
    const LowFunc &Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness, const LowIRRefinementLimits &Limits) {
  return runRefinement(&Original, OriginalInstructions, nullptr, {}, nullptr,
                       Candidate, Contract, Witness, Limits);
}

LowIRRefinementResult checkLowIRLoopRefinement(
    const LowFunc &Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    const LowIRLoopRefinementPlan &Plan, LowIRRefinementWitness Witness,
    const LowIRRefinementLimits &Limits) {
  return runRefinement(&Original, OriginalInstructions, nullptr, {}, nullptr,
                       Candidate, Contract, Witness, Limits, &Plan);
}

detail::NativeLowIRRefinementResult detail::checkNativeLowIRRefinement(
    SpecializationProvider &Provider, SpecializationCursor Entry,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness, const LowIRRefinementLimits &Limits,
    const LowIRLoopRefinementPlan *LoopPlan) {
  NativeLowIRRefinementResult Result;
  NativeUndefinedIndependenceResult Evidence;
  Result.Proof = runRefinement(nullptr, {}, &Provider, Entry, &Evidence,
                               Candidate, Contract, Witness, Limits, LoopPlan);
  if (Result.Proof.proved()) {
    Result.Instructions = std::move(Evidence.Instructions);
    Result.Reads = std::move(Evidence.Reads);
  }
  return Result;
}
} // namespace neverd::analysis
