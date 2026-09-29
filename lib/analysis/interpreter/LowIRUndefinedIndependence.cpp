//===- LowIRUndefinedIndependence.cpp - Correlated arbitrary values
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LowIRUndefinedIndependence.h"

#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace neverd::analysis {
namespace {
using namespace symbolic;
using Status = LowIRIndependenceStatus;
constexpr uint64_t AddressTemporary = UINT64_MAX - 7;

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
std::string inputDigest(const LowFunc &F,
                        llvm::ArrayRef<LowIRUndefinedInstruction> Records,
                        const LowIRIndependenceContract &Contract,
                        const LowIRIndependenceLimits &Limits) {
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
  Number(4); // Certificate semantic schema, independent of report formatting.
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
  const LowFunc &Function;
  llvm::ArrayRef<LowIRUndefinedInstruction> Records;
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
    const auto &B = *Blocks.at(P.BlockId);
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
      if (Record == Effects.end() ||
          Record->second->Effects.Coverage == LowUndefinedCoverage::Missing)
        fail(Status::Unsupported,
             "missing architectural undefined-effect coverage");
      const auto &Description = Record->second->Effects;
      if (Description.Coverage != LowUndefinedCoverage::Complete)
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
          if (!Difference || Difference->getBitWidth() != 64)
            fail(Status::Unsupported,
                 "memory address is not an exact entry-frame offset");
          const int64_t Offset = Difference->getSExtValue();
          const auto &Frame = *Contract.Frame;
          if (Offset < Frame.Begin || Offset >= Frame.End ||
              static_cast<uint64_t>(Frame.End) - static_cast<uint64_t>(Offset) <
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
    if (B.Succs.size() != 1)
      fail(Status::Unsupported, "fallthrough has no unique successor");
    const auto Predicate = P.Predicate;
    schedule(std::move(P), B.Succs.front(), Predicate);
  }

  void validate() {
    if (!Limits.Solver.Blast.MaxGates || !Limits.Solver.Sat.MaxConflicts ||
        !Limits.Solver.Sat.MaxPropagations || !Limits.Solver.Sat.MaxWatchVisits)
      fail(Status::Invalid, "relational solver limits must be bounded");
    if (Contract.ByteOrder != llvm::endianness::little &&
        Contract.ByteOrder != llvm::endianness::big)
      fail(Status::Invalid, "invalid byte order");
    uint64_t InputOperations = 0;
    uint64_t InputInstructions = 0;
    uint64_t InputEdges = 0;
    if (Function.Blocks.size() > Limits.MaxBlockVisits ||
        Function.ModuleAnalysisRoots.size() > Limits.MaxBlockVisits ||
        Function.OrdinaryModuleAnalysisRoots.size() > Limits.MaxBlockVisits ||
        Records.size() > Limits.MaxInstructions ||
        Contract.EntryConstants.size() > Limits.MaxInstructions ||
        Contract.ReturnRegisters.size() > Limits.MaxInstructions ||
        Contract.PreservedRegisters.size() > Limits.MaxInstructions ||
        Contract.PreservedFrameRanges.size() > Limits.MaxInstructions ||
        (Contract.Frame &&
         Contract.Frame->ExcludedAddressRanges.size() > Limits.MaxInstructions))
      fail(Status::BudgetExceeded, "input metadata budget exhausted");
    for (const auto &B : Function.Blocks) {
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
    for (const auto &B : Function.Blocks)
      for (int Successor : B.Succs)
        if (!Blocks.count(Successor))
          fail(Status::Invalid, "CFG successor names a missing block");
    if (auto Error = validateLowInstructionBoundaries(
            Function, LowInstructionBoundaryRequirement::Required))
      fail(Status::Invalid, llvm::toString(std::move(Error)));
    for (va_t Root : Function.ModuleAnalysisRoots)
      if (Root != Function.Entry)
        fail(Status::Unsupported,
             "additional function entry roots are unsupported");
    for (va_t Root : Function.OrdinaryModuleAnalysisRoots)
      if (Root != Function.Entry)
        fail(Status::Unsupported,
             "additional function entry roots are unsupported");
    if (!Addresses.count(Function.Entry))
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
      if (R.Effects.Coverage == LowUndefinedCoverage::Complete &&
          (R.Effects.OperationDigest.empty() ||
           R.Effects.OperationDigest !=
               lowUndefinedOperationDigest(
                   llvm::ArrayRef<LowOp>(B->second->Ops)
                       .slice(R.Boundary.FirstOp, R.Boundary.OpCount))))
        fail(Status::Invalid,
             "undefined-effect operation digest is stale or missing");
      if (R.Effects.Effects.size() > Limits.MaxProducers - InputEffects)
        fail(Status::BudgetExceeded, "input arbitrary-effect budget exhausted");
      InputEffects += R.Effects.Effects.size();
      for (const auto &E : R.Effects.Effects) {
        if (E.AfterOp > R.Boundary.OpCount || !scalar(E.Output) ||
            E.Output.isConst() || !E.BitCount ||
            E.BitOffset >= E.Output.Size * 8 ||
            E.BitCount > E.Output.Size * 8 - E.BitOffset ||
            (E.When &&
             (!scalar(*E.When) || E.When->Size != 1 || E.When->isReg())))
          fail(Status::Invalid, "malformed architecture-arbitrary effect");
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
  }

public:
  Checker(const LowFunc &F, llvm::ArrayRef<LowIRUndefinedInstruction> R,
          const LowIRIndependenceContract &C, const LowIRIndependenceLimits &L)
      : Function(F), Records(R), Contract(C), Limits(L) {}

  LowIRIndependenceResult run() {
    try {
      validate();
      SymState Initial(Ctx, Contract.ByteOrder);
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
      Path Entry{
          Addresses.at(Function.Entry), Initial, Initial, Predicate, {}, {}};
      schedule(std::move(Entry), Addresses.at(Function.Entry), Predicate);
      while (!Pending.empty()) {
        auto P = std::move(Pending.back());
        Pending.pop_back();
        runPath(std::move(P));
      }
      if (!Result.Paths)
        fail(Status::Unsupported, "no reachable return was certified");
      Result.Certificate = LowIRIndependenceCertificate{
          LowIRIndependenceScope::CompleteAcyclicLowIR,
          inputDigest(Function, Records, Contract, Limits),
          std::vector<LowIRUndefinedInstruction>(Records.begin(),
                                                 Records.end()),
          Contract, Limits};
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
} // namespace neverd::analysis
