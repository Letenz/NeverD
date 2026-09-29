//===- LowIRUndefinedIndependence.cpp - Correlated arbitrary values
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LowIRUndefinedIndependence.h"

#include "FiniteValues.h"
#include "NativeStackControl.h"
#include "NativeUndefinedIndependence.h"
#include "X64UserFlags.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <bit>
#include <climits>
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

// A retained trap has exact semantics even though its exception-state outputs
// have no undefined-effect audit. It may only be collected, never executed in
// this nonfaulting proof. Do not turn its Missing sidecar into Complete.
bool isRetainedNativeTrap(const SpecializationInstruction &Insn) {
  if (Insn.Origin.Control != LowInstructionControl::Terminator ||
      Insn.Origin.Immediate || Insn.IsNativeCall ||
      Insn.NativeStackControl != SpecializationNativeStackControl::None ||
      Insn.ProfileProjection != InterpreterProfileProjection::None ||
      Insn.Ops.size() != 1 || !Insn.UndefinedEffects.Effects.empty() ||
      Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Missing ||
      Insn.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(Insn.Ops))
    return false;
  const auto &Op = Insn.Ops.front();
  if (Op.Opcode != NdOp::INTRINSIC || Op.Output.Size || Op.NumInputs != 1 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  auto Flags = LowInstructionControlFlag::Terminator;
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Int3))
    Flags |= LowInstructionControlFlag::Resumable;
  else if (Op.Inputs[0].Offset != static_cast<uint64_t>(Intrinsic::Ud2))
    return false;
  return Insn.Origin.ControlFlags == Flags;
}

// Intel SDM RDSSPD/RDSSPQ: when shadow stacks are disabled this encoding has
// no architectural effects, including no 32-bit destination zero-extension.
// The provider classifies the decoded instruction; also bind its canonical
// encoding and exact projected operation here. Missing coverage stays Missing.
bool isCetDisabledReadShadowStack(const SpecializationInstruction &Insn) {
  if (Insn.ProfileProjection !=
          InterpreterProfileProjection::CetDisabledReadShadowStackV1 ||
      Insn.IsNativeCall ||
      Insn.NativeStackControl != SpecializationNativeStackControl::None ||
      Insn.Origin.Control != LowInstructionControl::None ||
      Insn.Origin.ControlFlags != LowInstructionControlFlag::None ||
      Insn.Origin.Immediate || Insn.Ops.size() != 1 ||
      Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Missing ||
      !Insn.UndefinedEffects.Effects.empty() ||
      Insn.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(Insn.Ops))
    return false;
  const auto &Op = Insn.Ops.front();
  if (Op.Opcode != NdOp::NOP || Op.Output.Size || Op.NumInputs ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto &Bytes = Insn.NativeBytes;
  if ((Bytes.size() != 4 && Bytes.size() != 5) || Bytes[0] != 0xf3)
    return false;
  size_t I = 1;
  if (Bytes.size() == 5) {
    // REX.W and REX.B select width/bank. Extra prefix forms are not certified.
    if (Bytes[I] != 0x40 && Bytes[I] != 0x41 && Bytes[I] != 0x48 &&
        Bytes[I] != 0x49)
      return false;
    ++I;
  }
  return Bytes[I] == 0x0f && Bytes[I + 1] == 0x1e &&
         (Bytes[I + 2] & 0xf8) == 0xc8;
}

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
            llvm::ArrayRef<LowIRNativeProfileProjection> Projections = {}) {
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
  Number(6); // Certificate semantic schema, independent of report formatting.
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
  Number(F.Entry);
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
  Number(Limits.MaxObservations);
  Number(Limits.MaxSymbolicNodes);
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

class Checker {
  const LowFunc *Function = nullptr;
  llvm::ArrayRef<LowIRUndefinedInstruction> Records;
  SpecializationProvider *Provider = nullptr;
  SpecializationCursor NativeEntry;
  detail::NativeUndefinedIndependenceResult *NativeResult = nullptr;
  std::map<va_t, SpecializationInstruction> NativeInstructions;
  std::map<va_t, va_t> NativeRanges;
  std::map<va_t, uint8_t> ImmutableBytes;
  LowFunc NativeTrace;
  std::vector<LowIRUndefinedInstruction> NativeRecords;
  std::vector<LowIRNativeFlagTransition> NativeFlagTransitions;
  std::vector<LowIRNativeProfileProjection> NativeProfileProjections;
  std::map<int, std::vector<int>> NativeTraceEdges;
  uint64_t NativeInputOperations = 0;
  uint64_t NativeInputEffects = 0;
  uint64_t NativeReadEvidenceBytes = 0;
  int NextNativeBlock = 0;
  const LowIRIndependenceContract &Contract;
  const LowIRIndependenceLimits &Limits;
  LowIRIndependenceResult Result;
  SymContext Ctx;
  SymRef EntryRoot, MemoryRoot;
  std::vector<SymRef> PreservedRegisterEntries;
  std::map<uint64_t, SymRef> PreservedFrameEntries;
  std::map<int, const LowBlock *> Blocks;
  std::map<va_t, int> Addresses;
  std::map<std::pair<int, va_t>, const LowInstructionBoundary *> Boundaries;
  std::map<std::pair<int, va_t>, const LowIRUndefinedInstruction *> Effects;
  std::unordered_map<uint32_t, SymRef> ZeroLeftChoices;
  uint64_t FrameBytes = 0;
  uint64_t ScheduledPaths = 0;

  struct Path {
    int BlockId;
    SymState Left, Right;
    SymRef Predicate;
    std::set<int> Ancestors;
    std::set<uint64_t> Written;
    va_t NativeAddress = 0;
    SymRef LeftSystemFlags, RightSystemFlags;
  };
  std::vector<Path> Pending;

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
    if (Result.SolverQueries >= Limits.MaxSolverQueries)
      fail(Status::BudgetExceeded, "solver-query budget exhausted");
    ++Result.SolverQueries;
    const auto Answer =
        solver::checkSat(Ctx, Predicate, nullptr, Limits.Solver);
    if (Answer == solver::SatResult::Unknown)
      fail(Status::BudgetExceeded, "relational solver budget exhausted");
    if (Answer == solver::SatResult::Invalid)
      fail(Status::Invalid, "invalid relational solver query");
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
    if (query(Ctx.mkAnd(Predicate, Ctx.mkNe(Left, Right))) !=
        solver::SatResult::Unsat)
      fail(Failure,
           Failure == Status::ContractViolation
               ? "return contract does not preserve " + Observation.str()
               : "architecture-arbitrary value affects " + Observation.str());
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

  void checkTemporary(const NdVar &V, const std::set<uint64_t> &Defined) {
    if (V.isTemp())
      for (uint16_t I = 0; I != V.Size; ++I)
        if (!Defined.count(V.Offset + I))
          fail(Status::Invalid, "instruction reads an unbound temporary");
  }

  void define(const NdVar &V, std::set<uint64_t> &Defined) {
    if (V.isTemp())
      for (uint16_t I = 0; I != V.Size; ++I)
        Defined.insert(V.Offset + I);
  }

  bool flagIntrinsic(Path &P, const LowOp &Original,
                     const LowInstructionUndefinedEffects &Effects,
                     const std::set<uint64_t> &Defined) {
    if (!Provider || !Contract.X64FlagsProfile ||
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
                 std::set<uint64_t> &Defined) {
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
    const auto U = Ctx.mkFreshVar(Effect.BitCount, "undefined_left");
    const auto V = Ctx.mkFreshVar(Effect.BitCount, "undefined_right");
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
    define(Effect.Output, Defined);
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
    if (P.Ancestors.count(Destination))
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
      auto Next = NativeRanges.lower_bound(Address);
      if ((Next != NativeRanges.end() && Next->first < Address + B.Size) ||
          (Next != NativeRanges.begin() && std::prev(Next)->second > Address))
        fail(Status::Unsupported, "overlapping original instruction ranges");
      NativeRanges.emplace(Address, Address + B.Size);
      if (Insn.Ops.size() > Limits.MaxOperations - NativeInputOperations)
        fail(Status::BudgetExceeded, "original operation budget exhausted");
      NativeInputOperations += Insn.Ops.size();
      for (const auto &Op : Insn.Ops)
        if (Op.NumInputs > 6)
          fail(Status::Invalid, "operand capacity exceeded");
      LowBlock Raw;
      Raw.StartAddr = Address;
      Raw.EndAddr = Address + B.Size;
      Raw.Ops = Insn.Ops;
      Raw.InstructionBoundaries.push_back(B);
      if (auto Error = validateLowInstructionBoundaries(
              Raw, LowInstructionBoundaryRequirement::Required))
        fail(Status::Invalid, llvm::toString(std::move(Error)));
      validateEffects(B, Insn.Ops, Insn.UndefinedEffects);
      const bool Trap = isRetainedNativeTrap(Insn);
      const bool Projection =
          Contract.X64FlagsProfile && isCetDisabledReadShadowStack(Insn);
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
      if (!Trap && !Projection &&
          Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Complete)
        fail(Status::Unsupported,
             "original instruction lacks complete undefined-output evidence");
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
            Successors.push_back(Op.Inputs[0].Offset);
          }
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
              Op.Opcode == NdOp::COND_BR)
            Successors.push_back(Insn.Fallthrough.Address);
        }
      }
      if (!Terminal)
        Successors.push_back(Insn.Fallthrough.Address);
      NativeInstructions.emplace(Address, std::move(Insn));
      for (va_t Target : Successors)
        Enqueue(Target);
    }
    // Collection visits each original instruction once, including cyclic
    // arms. Execution creates a fresh trace block on every visit, preserving
    // state and fresh undefined choices. Direct and indirect cycles obey the
    // same path/visit/operation budgets; only exhaustion of all feasible paths
    // at ordinary returns can certify a finite unrolling, never a prefix.
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
    uint64_t Queries = Result.SolverQueries;
    const auto Values = detail::enumerateFiniteValues(
        Ctx, Predicate, {ordinary(Value)}, Limits.MaxIndirectTargets,
        Limits.Solver, Limits.MaxSolverQueries, Limits.MaxSymbolicNodes,
        Queries);
    Result.SolverQueries = static_cast<uint32_t>(Queries);
    if (Values.Status != detail::FiniteValueStatus::Complete)
      fail(Values.Status == detail::FiniteValueStatus::Invalid
               ? Status::Invalid
               : Status::BudgetExceeded,
           "native control target enumeration is incomplete");
    for (const auto &Tuple : Values.Tuples) {
      if (Tuple.size() != 1)
        fail(Status::Invalid, "invalid native target tuple");
      scheduleNative(P, Tuple[0],
                     Ctx.mkAnd(Predicate, Ctx.mkEq(ordinary(Value),
                                                   Ctx.mkConst(64, Tuple[0]))));
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
        const auto Offset = Ctx.asConst(Ctx.mkSub(ordinary(Left), EntryRoot));
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
    if (++Result.BlockVisits > Limits.MaxBlockVisits)
      fail(Status::BudgetExceeded, "block-visit budget exhausted");
    LowInstructionUndefinedEffects NativeEffects;
    std::optional<LowBlock> NativeBlock;
    if (Provider)
      NativeBlock = prepareNative(P, NativeEffects);
    const auto &B = NativeBlock ? *NativeBlock : *Blocks.at(P.BlockId);
    Result.BlockId = B.Id;
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
          isCetDisabledReadShadowStack(NativeInstructions.at(B.StartAddr));
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
      std::set<uint64_t> Defined;
      std::multimap<uint64_t, const LowUndefinedEffect *> Events;
      for (const auto &Effect : Description.Effects)
        Events.emplace(Effect.AfterOp, &Effect);
      const auto Apply = [&](uint64_t Completed) {
        const auto Range = Events.equal_range(Completed);
        for (auto It = Range.first; It != Range.second; ++It)
          arbitrary(P, *It->second, Defined);
      };
      Apply(0);
      for (uint64_t I = 0; I != Boundary.OpCount; ++I) {
        const auto &Original = B.Ops[Boundary.FirstOp + I];
        Result.OpSeq = Original.Seq;
        if (++Result.Operations > Limits.MaxOperations)
          fail(Status::BudgetExceeded, "LowIR operation budget exhausted");
        if (Original.MemoryOrdering != NdMemoryOrdering::None ||
            Original.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          fail(Status::Unsupported,
               "ordered or nondefault memory is unsupported");
        if (flagIntrinsic(P, Original, Description, Defined)) {
          define(Original.Output, Defined);
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
          const auto Difference =
              Ctx.asConst(Ctx.mkSub(ordinary(A), EntryRoot));
          const auto &Frame = *Contract.Frame;
          const bool InFrame =
              Difference && Difference->getBitWidth() == 64 &&
              Difference->getSExtValue() >= Frame.Begin &&
              Difference->getSExtValue() < Frame.End &&
              static_cast<uint64_t>(Frame.End) - Difference->getZExtValue() >=
                  View.AccessSize;
          if (!InFrame && Provider && !Store && Ctx.asConst(ordinary(A))) {
            const auto Absolute = Ctx.asConst(ordinary(A));
            if (!Absolute || Absolute->getActiveBits() > 64 ||
                Absolute->getZExtValue() > UINT64_MAX - View.AccessSize)
              fail(Status::Unsupported, "immutable load address is not exact");
            const uint64_t Address = Absolute->getZExtValue();
            const auto First = Ctx.mkAdd(
                EntryRoot, Ctx.mkConst(64, static_cast<uint64_t>(Frame.Begin)));
            const auto Last = Ctx.mkAdd(
                EntryRoot,
                Ctx.mkConst(64, static_cast<uint64_t>(Frame.End) - 1));
            const auto Disjoint = Ctx.mkOr(
                Ctx.mkUlt(Last, Ctx.mkConst(64, Address)),
                Ctx.mkUle(Ctx.mkConst(64, Address + View.AccessSize), First));
            if (query(Ctx.mkAnd(P.Predicate, Ctx.mkNot(Disjoint))) !=
                solver::SatResult::Unsat)
              fail(Status::Unsupported,
                   "immutable read can alias the mutable frame");
            const auto Read = Provider->immutableRead(Address, View.AccessSize);
            if (!Read || Read->Bytes.size() != View.AccessSize ||
                Read->Evidence.empty())
              fail(Status::Unsupported, "missing immutable-read evidence");
            for (uint64_t Size : {Read->Bytes.size(), Read->Evidence.size()}) {
              if (Size > Limits.MaxOperations - NativeReadEvidenceBytes)
                fail(Status::BudgetExceeded,
                     "immutable-read evidence budget exhausted");
              NativeReadEvidenceBytes += Size;
            }
            uint64_t Value = 0;
            for (uint16_t I = 0; I != View.AccessSize; ++I) {
              const auto [It, Inserted] =
                  ImmutableBytes.emplace(Address + I, Read->Bytes[I]);
              if (!Inserted && It->second != Read->Bytes[I])
                fail(Status::Invalid, "immutable provider bytes changed");
              const unsigned Shift =
                  Contract.ByteOrder == llvm::endianness::little
                      ? I
                      : View.AccessSize - 1 - I;
              Value |= uint64_t{Read->Bytes[I]} << (Shift * 8);
            }
            NativeResult->Reads.push_back({Boundary.Address, Original.Seq,
                                           Address, Read->Bytes,
                                           Read->Evidence});
            Op.Opcode = NdOp::COPY;
            Op.NumInputs = 1;
            Op.Inputs[0] = NdVar::scalar(Value, View.AccessSize);
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
        define(Original.Output, Defined);
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
          return;
        }
        equal(P.Predicate, Left.branchTarget(), Right.branchTarget(),
              "control target");
        if (Provider) {
          if (LF == StepResult::CondBranch) {
            equal(P.Predicate, Left.branchCondition(), Right.branchCondition(),
                  "branch predicate");
            const auto Condition = ordinary(Left.branchCondition());
            nativeTargets(P, Left.branchTarget(),
                          Ctx.mkAnd(P.Predicate, Condition));
            const auto Other = Ctx.mkAnd(P.Predicate, Ctx.mkNot(Condition));
            scheduleNative(
                std::move(P),
                NativeInstructions.at(B.StartAddr).Fallthrough.Address, Other);
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
    if (Contract.X64FlagsProfile) {
      if (!Provider)
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
         Contract.Frame->End < 8 || !Limits.MaxIndirectTargets))
      fail(Status::Invalid,
           "native proof requires a stack frame and bounded targets");
    uint64_t InputOperations = 0;
    uint64_t InputInstructions = 0;
    uint64_t InputEdges = 0;
    if ((Function &&
         (Function->Blocks.size() > Limits.MaxBlockVisits ||
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
      }
      for (const auto &B : Function->Blocks)
        for (int Successor : B.Succs)
          if (!Blocks.count(Successor))
            fail(Status::Invalid, "CFG successor names a missing block");
      if (auto Error = validateLowInstructionBoundaries(
              *Function, LowInstructionBoundaryRequirement::Required))
        fail(Status::Invalid, llvm::toString(std::move(Error)));
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
  Checker(const LowFunc &F, llvm::ArrayRef<LowIRUndefinedInstruction> R,
          const LowIRIndependenceContract &C, const LowIRIndependenceLimits &L)
      : Function(&F), Records(R), Contract(C), Limits(L) {}

  Checker(SpecializationProvider &P, SpecializationCursor Entry,
          const LowIRIndependenceContract &C, const LowIRIndependenceLimits &L,
          detail::NativeUndefinedIndependenceResult &Output)
      : Provider(&P), NativeEntry(Entry), NativeResult(&Output), Contract(C),
        Limits(L) {
    NativeTrace.Entry = Entry.Address;
  }

  LowIRIndependenceResult run() {
    try {
      validate();
      if (Provider)
        collectNative(NativeEntry.Address);
      SymState Initial(Ctx, Contract.ByteOrder);
      SymRef EntrySystem;
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
        nodes();
      }
      for (const auto &C : Contract.EntryConstants) {
        Initial.write(SymSpace::Register, C.Location.Offset,
                      Ctx.mkConst(C.Location.Size * 8, C.Value));
        nodes();
      }
      auto Predicate = Ctx.mkTrue();
      if (Contract.Frame) {
        const auto &F = *Contract.Frame;
        EntryRoot = Initial.read(SymSpace::Register, F.RootRegister.Offset, 8);
        if (F.Begin < 0)
          Predicate = Ctx.mkAnd(
              Predicate,
              Ctx.mkUle(
                  Ctx.mkConst(64, uint64_t{0} - static_cast<uint64_t>(F.Begin)),
                  EntryRoot));
        if (F.End > 0)
          Predicate = Ctx.mkAnd(
              Predicate,
              Ctx.mkUle(EntryRoot,
                        Ctx.mkConst(64, UINT64_MAX -
                                            static_cast<uint64_t>(F.End - 1))));
        // The existing root bounds make these the unsigned first and last
        // accessible bytes without wraparound. Exclusion is a symbolic entry
        // precondition: adjacency is allowed, but no byte may overlap a range.
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
        // Seed before the twin-state fork: normal frame bytes must stay shared
        // even when subsequent accesses use different overlapping widths.
        for (uint64_t I = 0; I != FrameBytes; ++I) {
          Initial.store(Ctx.mkAdd(MemoryRoot, Ctx.mkConst(64, I)),
                        Ctx.mkFreshVar(8, "entry_frame_byte"));
          nodes();
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
      if (Provider)
        scheduleNative(std::move(Entry), NativeEntry.Address, Predicate);
      else
        schedule(std::move(Entry), Addresses.at(Function->Entry), Predicate);
      while (!Pending.empty()) {
        auto P = std::move(Pending.back());
        Pending.pop_back();
        runPath(std::move(P));
      }
      if (!Result.Paths)
        fail(Status::Unsupported, "no reachable return was certified");
      if (Provider) {
        for (auto &B : NativeTrace.Blocks)
          B.Succs = NativeTraceEdges[B.Id];
        Result.Certificate = LowIRIndependenceCertificate{
            LowIRIndependenceScope::CompleteFiniteNativePaths,
            inputDigest(NativeTrace, NativeRecords, Contract, Limits,
                        NativeFlagTransitions, NativeProfileProjections),
            std::move(NativeRecords),
            Contract,
            Limits,
            std::move(NativeFlagTransitions),
            std::move(NativeProfileProjections)};
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
    return std::move(Result);
  }
};
} // namespace

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
} // namespace neverd::analysis
