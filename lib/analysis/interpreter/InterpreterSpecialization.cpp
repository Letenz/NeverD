//===- InterpreterSpecialization.cpp - LowIR partial evaluation -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/InterpreterSpecialization.h"

#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace neverd::analysis {
namespace {

using namespace symbolic;
using ByteKey = std::pair<SymSpace, uint64_t>;
using Constants = std::map<ByteKey, uint8_t>;

struct FrameOrigins {
  /// May originate from the entry root or from memory of unknown provenance.
  /// This survives projection even when the affine value itself is lost.
  std::set<ByteKey> UnsafeScalars;
  /// Unlike physical entry registers, an unseen lifter temporary is not a
  /// caller-supplied external input. It needs a positive defining provenance.
  std::set<uint64_t> ExternalTemporaries;
  /// Must facts: these frame bytes were written with external scalar origins.
  /// Values may be dynamic, e.g. a caller's output pointer spilled to a frame.
  std::set<uint64_t> ExternalFrameBytes;
};

struct Projection {
  Constants Scalars;
  /// Complete 64-bit register values equal to the entry frame root plus a
  /// modular displacement. No arbitrary expression escapes a node context.
  std::map<uint64_t, uint64_t> AffineRegisters;
  std::map<uint64_t, uint8_t> FrameBytes;
  FrameOrigins Origins;
};

struct ContextKey {
  SpecializationCursor Cursor;
  std::vector<std::optional<uint64_t>> Controls;

  bool operator<(const ContextKey &Other) const {
    return std::tie(Cursor.Address, Cursor.Mode, Controls) <
           std::tie(Other.Cursor.Address, Other.Cursor.Mode, Other.Controls);
  }
};

struct NativeSlice {
  LowInstructionBoundary Origin;
  size_t FirstOp = 0;
  size_t OpCount = 0;
};

struct Node {
  ContextKey Key;
  Projection Incoming;
  LowBlock Block;
  std::vector<NativeSlice> Slices;
  std::vector<SpecializationReadWitness> Reads;
  bool Pending = false;
};

std::optional<uint64_t> affineDisplacement(SymContext &Ctx, SymRef Value,
                                           SymRef Root) {
  if (!Root || !Value || Ctx.width(Value) != 64)
    return std::nullopt;
  if (Value == Root)
    return 0;
  if (Ctx.op(Value) != SymOp::Add || Ctx.operands(Value).size() != 2)
    return std::nullopt;
  for (unsigned I = 0; I < 2; ++I)
    if (Ctx.operand(Value, I) == Root)
      if (const auto Offset = Ctx.asConst(Ctx.operand(Value, 1 - I)))
        return Offset->getZExtValue();
  return std::nullopt;
}

Projection project(SymState &State, SymRef Root,
                   const std::set<uint64_t> &AffineCandidates,
                   const FrameOrigins &Origins) {
  Projection Result;
  Result.Origins = Origins;
  for (const auto &Byte : State.constantScalarBytes())
    Result.Scalars.emplace(ByteKey{Byte.Space, Byte.Offset}, Byte.Value);
  if (Root) {
    for (uint64_t Offset : AffineCandidates)
      if (const auto Displacement = affineDisplacement(
              State.context(), State.read(SymSpace::Register, Offset, 8), Root))
        Result.AffineRegisters.emplace(Offset, *Displacement);
    for (const auto &Byte : State.constantRegionBytes(Root))
      Result.FrameBytes.emplace(Byte.Offset, Byte.Value);
  }
  return Result;
}

void seed(SymContext &Ctx, SymState &State, SymRef Root,
          const Projection &Values) {
  for (const auto &[Key, Value] : Values.Scalars)
    State.write(Key.first, Key.second, Ctx.mkConst(8, Value));
  if (!Root)
    return;
  for (const auto &[Register, Displacement] : Values.AffineRegisters)
    State.write(SymSpace::Register, Register,
                Ctx.mkAdd(Root, Ctx.mkConst(64, Displacement)));
  // The node starts with no memory facts. All imported bytes have the same
  // root, so using ordinary store() here cannot discard another imported
  // region or bypass a guest alias invalidation from the predecessor.
  for (const auto &[Offset, Value] : Values.FrameBytes)
    State.store(Ctx.mkAdd(Root, Ctx.mkConst(64, Offset)),
                Ctx.mkConst(8, Value));
}

template <class Map> bool intersectMap(Map &Into, const Map &Incoming) {
  bool Changed = false;
  for (auto It = Into.begin(); It != Into.end();) {
    const auto Found = Incoming.find(It->first);
    if (Found == Incoming.end() || Found->second != It->second) {
      It = Into.erase(It);
      Changed = true;
    } else {
      ++It;
    }
  }
  return Changed;
}

bool intersect(Projection &Into, const Projection &Incoming) {
  bool Changed = intersectMap(Into.Scalars, Incoming.Scalars);
  Changed |= intersectMap(Into.AffineRegisters, Incoming.AffineRegisters);
  Changed |= intersectMap(Into.FrameBytes, Incoming.FrameBytes);
  const auto OldUnsafe = Into.Origins.UnsafeScalars.size();
  Into.Origins.UnsafeScalars.insert(Incoming.Origins.UnsafeScalars.begin(),
                                    Incoming.Origins.UnsafeScalars.end());
  Changed |= OldUnsafe != Into.Origins.UnsafeScalars.size();
  for (auto It = Into.Origins.ExternalTemporaries.begin();
       It != Into.Origins.ExternalTemporaries.end();) {
    if (!Incoming.Origins.ExternalTemporaries.count(*It)) {
      It = Into.Origins.ExternalTemporaries.erase(It);
      Changed = true;
    } else {
      ++It;
    }
  }
  for (auto It = Into.Origins.ExternalFrameBytes.begin();
       It != Into.Origins.ExternalFrameBytes.end();) {
    if (!Incoming.Origins.ExternalFrameBytes.count(*It)) {
      It = Into.Origins.ExternalFrameBytes.erase(It);
      Changed = true;
    } else {
      ++It;
    }
  }
  return Changed;
}

bool unsafeOrigin(const NdVar &Value, const FrameOrigins &Origins) {
  if (Value.isConst())
    return false;
  const SymSpace Space =
      Value.isReg() ? SymSpace::Register : SymSpace::Temporary;
  for (uint16_t I = 0; I < Value.Size; ++I)
    if (Origins.UnsafeScalars.count({Space, Value.Offset + I}) ||
        (Value.isTemp() &&
         !Origins.ExternalTemporaries.count(Value.Offset + I)))
      return true;
  return false;
}

void setUnsafeOrigin(const NdVar &Value, bool Unsafe, FrameOrigins &Origins) {
  if (!Value.isReg() && !Value.isTemp())
    return;
  const SymSpace Space =
      Value.isReg() ? SymSpace::Register : SymSpace::Temporary;
  for (uint16_t I = 0; I < Value.Size; ++I) {
    const ByteKey Key{Space, Value.Offset + I};
    if (Unsafe)
      Origins.UnsafeScalars.insert(Key);
    else
      Origins.UnsafeScalars.erase(Key);
    if (Value.isTemp()) {
      if (Unsafe)
        Origins.ExternalTemporaries.erase(Value.Offset + I);
      else
        Origins.ExternalTemporaries.insert(Value.Offset + I);
    }
  }
}

std::optional<uint64_t> constantWord(const Constants &State,
                                     SymRegisterRange Range,
                                     llvm::endianness Order) {
  uint64_t Value = 0;
  for (uint16_t I = 0; I < Range.Bytes; ++I) {
    const auto Found = State.find({SymSpace::Register, Range.Offset + I});
    if (Found == State.end())
      return std::nullopt;
    const unsigned Shift =
        8 * (Order == llvm::endianness::little ? I : Range.Bytes - I - 1);
    Value |= uint64_t{Found->second} << Shift;
  }
  return Value;
}

std::optional<uint64_t>
constantFrameWord(const std::map<uint64_t, uint8_t> &Bytes,
                  SpecializationFrameSlot Slot, llvm::endianness Order) {
  uint64_t Value = 0;
  for (uint16_t I = 0; I < Slot.Bytes; ++I) {
    const auto Found = Bytes.find(static_cast<uint64_t>(Slot.Offset) + I);
    if (Found == Bytes.end())
      return std::nullopt;
    const unsigned Shift =
        8 * (Order == llvm::endianness::little ? I : Slot.Bytes - I - 1);
    Value |= uint64_t{Found->second} << Shift;
  }
  return Value;
}

bool scalarLocation(const NdVar &Value) {
  return Value.isReg() || Value.isTemp();
}

bool validValue(const NdVar &Value) {
  return (scalarLocation(Value) || Value.isConst()) && Value.Size != 0 &&
         Value.Size <= 8 &&
         (Value.isConst() || Value.Offset <= InvalidVA - (Value.Size - 1));
}

/// The allowlist is intentionally narrower than SymExec. In particular its BV
/// division model does not certify architectural division faults, and naming an
/// opaque result is not permission to publish a replacement of its instruction.
bool supported(const LowOp &Op) {
  if (Op.NumInputs > 6 || Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 0; I < Op.NumInputs; ++I)
    if (!validValue(Op.Inputs[I]))
      return false;
  const bool Output = scalarLocation(Op.Output) && validValue(Op.Output);
  const bool NoOutput = Op.Output.Size == 0;
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
    return Output && Op.NumInputs == 1;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_MULT:
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
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
    return Output && Op.NumInputs == 2;
  case NdOp::SELECT:
    return Output && Op.NumInputs == 3;
  case NdOp::EXTRACT:
    return Output && Op.NumInputs == 3;
  case NdOp::INSERT:
    return Output && Op.NumInputs == 4;
  case NdOp::LOAD:
    return Output && (Op.NumInputs == 1 || Op.NumInputs == 2) &&
           lowMemoryOperands(Op).Complete;
  case NdOp::STORE:
    return NoOutput && (Op.NumInputs == 2 || Op.NumInputs == 3) &&
           lowMemoryOperands(Op).Complete;
  case NdOp::BRANCH:
    return NoOutput && Op.NumInputs == 1 && Op.Inputs[0].isConst();
  case NdOp::COND_BR:
    return NoOutput && Op.NumInputs == 2 && Op.Inputs[0].isConst();
  case NdOp::INDIR_BR:
    return NoOutput && Op.NumInputs == 1;
  case NdOp::RETURN:
    return NoOutput && Op.NumInputs <= 1;
  case NdOp::NOP:
    return NoOutput && Op.NumInputs == 0;
  default:
    return false;
  }
}

enum class TargetSetResult { Exact, Unknown, BudgetExceeded };

TargetSetResult finiteTargets(SymContext &Ctx, SymRef Value,
                              std::set<uint64_t> &Targets, uint32_t Limit) {
  std::set<SymRef> Visited;
  std::vector<SymRef> Pending{Value};
  while (!Pending.empty()) {
    const SymRef Current = Pending.back();
    Pending.pop_back();
    if (!Current)
      return TargetSetResult::Unknown;
    if (!Visited.insert(Current).second)
      continue;
    if (const auto Constant = Ctx.asConst(Current)) {
      if (Constant->getBitWidth() > 64)
        return TargetSetResult::Unknown;
      Targets.insert(Constant->getZExtValue());
      if (Targets.size() > Limit)
        return TargetSetResult::BudgetExceeded;
      continue;
    }
    if (Ctx.node(Current).Op != SymOp::Ite)
      return TargetSetResult::Unknown;
    Pending.push_back(Ctx.operand(Current, 1));
    Pending.push_back(Ctx.operand(Current, 2));
  }
  return TargetSetResult::Exact;
}

LowOp branchTo(int NodeId) {
  LowOp Op;
  Op.Opcode = NdOp::BRANCH;
  // Until final publication control constants name node IDs, not guest VAs.
  Op.addInput(NdVar::cst(static_cast<uint64_t>(NodeId), 8));
  return Op;
}

class Specializer {
public:
  Specializer(SpecializationProvider &Provider, SpecializationCursor Entry,
              const SpecializationOptions &Options)
      : Provider(Provider), Entry(Entry), Options(Options) {}

  SpecializationResult run();

private:
  bool fail(SpecializationStatus Status, std::string Message) {
    Result.Status = Status;
    Result.Diagnostic =
        FailureCursor == InvalidVA
            ? std::move(Message)
            : "at 0x" + llvm::utohexstr(FailureCursor) + ": " + Message;
    Failed = true;
    return false;
  }
  int enqueue(SpecializationCursor Cursor, const Projection &Incoming);
  bool evaluate(int Id);
  bool emitTargets(Node &Draft, const SpecializationInstruction &Instruction,
                   const LowOp &Original, LowOp Residual, SymExec &Exec,
                   SymContext &Ctx, SymState &State, SymRef FrameRoot,
                   const FrameOrigins &Origins, StepResult Flow);
  bool publish();
  int addDispatchNode(const LowInstructionBoundary &Origin, const NdVar &Target,
                      uint64_t Value, int Taken, int Other);

  SpecializationProvider &Provider;
  SpecializationCursor Entry;
  const SpecializationOptions &Options;
  SpecializationResult Result;
  std::vector<Node> Nodes;
  std::map<ContextKey, int> Indices;
  std::map<std::pair<va_t, InstructionMode>, uint32_t> ContextCounts;
  std::deque<int> Pending;
  bool Failed = false;
  va_t FailureCursor = InvalidVA;
  std::set<uint64_t> AffineCandidates;
  // A distinct temporary is needed only in synthetic finite-target dispatch.
  // Native temporaries are checked before publication to prevent collisions.
  static constexpr uint64_t DispatchTemp = uint64_t{1} << 62;
};

int Specializer::enqueue(SpecializationCursor Cursor,
                         const Projection &Incoming) {
  if (Cursor.Address >= (uint64_t{1} << 63)) {
    fail(SpecializationStatus::UnresolvedControl,
         "control destination overlaps reserved residual labels");
    return -1;
  }
  ContextKey Key{Cursor, {}};
  for (const auto &Range : Options.ControlRegisters)
    Key.Controls.push_back(
        constantWord(Incoming.Scalars, Range, Options.ByteOrder));
  for (const auto &Slot : Options.ControlFrameSlots)
    Key.Controls.push_back(
        constantFrameWord(Incoming.FrameBytes, Slot, Options.ByteOrder));
  const auto Existing = Indices.find(Key);
  if (Existing != Indices.end()) {
    Node &Old = Nodes[Existing->second];
    if (intersect(Old.Incoming, Incoming) && !Old.Pending) {
      Old.Pending = true;
      Pending.push_back(Existing->second);
    }
    return Existing->second;
  }
  auto &Count = ContextCounts[{Cursor.Address, Cursor.Mode}];
  if (Nodes.size() >= Options.MaxNodes ||
      Count >= Options.MaxContextsPerAddress) {
    fail(SpecializationStatus::BudgetExceeded,
         "specialization context budget exhausted");
    return -1;
  }
  ++Count;
  const int Id = static_cast<int>(Nodes.size());
  Node New;
  New.Key = Key;
  New.Incoming = Incoming;
  New.Pending = true;
  Nodes.push_back(std::move(New));
  Indices.emplace(std::move(Key), Id);
  Pending.push_back(Id);
  return Id;
}

int Specializer::addDispatchNode(const LowInstructionBoundary &Origin,
                                 const NdVar &Target, uint64_t Value, int Taken,
                                 int Other) {
  if (Nodes.size() >= Options.MaxNodes) {
    fail(SpecializationStatus::BudgetExceeded,
         "finite dispatch node budget exhausted");
    return -1;
  }
  Node New;
  LowOp Compare;
  Compare.Opcode = NdOp::INT_EQUAL;
  Compare.Output = NdVar::tmp(DispatchTemp, 1);
  Compare.addInput(Target);
  Compare.addInput(NdVar::scalar(Value, Target.Size));
  New.Block.Ops.push_back(Compare);
  LowOp Branch;
  Branch.Opcode = NdOp::COND_BR;
  Branch.addInput(NdVar::cst(static_cast<uint64_t>(Taken), 8));
  Branch.addInput(Compare.Output);
  New.Block.Ops.push_back(Branch);
  New.Block.Succs = {Taken, Other};
  New.Slices.push_back({Origin, 0, 2});
  const int Id = static_cast<int>(Nodes.size());
  Nodes.push_back(std::move(New));
  return Id;
}

bool Specializer::emitTargets(Node &Draft,
                              const SpecializationInstruction &Instruction,
                              const LowOp &Original, LowOp Residual,
                              SymExec &Exec, SymContext &Ctx, SymState &State,
                              SymRef FrameRoot, const FrameOrigins &Origins,
                              StepResult Flow) {
  const Projection Out = project(State, FrameRoot, AffineCandidates, Origins);
  const auto destination = [&](uint64_t Address) -> int {
    auto Target = canonicalizeLowControlTarget(Address, Instruction.Origin.Mode,
                                               Instruction.Origin.TargetMode);
    if (!Target) {
      fail(SpecializationStatus::UnresolvedControl,
           llvm::toString(Target.takeError()));
      return -1;
    }
    Projection EdgeState = Out;
    if (Flow == StepResult::IndirectBranch &&
        scalarLocation(Original.Inputs[0])) {
      // The residual dispatch tests this exact live operand. Its selected
      // edge therefore proves every byte of that operand, without constraining
      // unrelated registers or treating an SMT sample as an exhaustive case.
      const NdVar &Operand = Original.Inputs[0];
      const SymSpace Space =
          Operand.isReg() ? SymSpace::Register : SymSpace::Temporary;
      for (uint16_t I = 0; I < Operand.Size; ++I) {
        const unsigned Shift =
            8 * (Options.ByteOrder == llvm::endianness::little
                     ? I
                     : Operand.Size - I - 1);
        EdgeState.Scalars[{Space, Operand.Offset + I}] =
            static_cast<uint8_t>(Address >> Shift);
      }
      setUnsafeOrigin(Operand, false, EdgeState.Origins);
    }
    return enqueue({Target->Address, Target->Mode}, EdgeState);
  };
  if (Flow == StepResult::Return) {
    if (Options.RequireRestoredFrameAtReturn) {
      const auto &Base = *Options.FrameBaseRegister;
      const auto Displacement = affineDisplacement(
          Ctx, State.read(SymSpace::Register, Base.Offset, Base.Bytes),
          FrameRoot);
      if (!Displacement || *Displacement != 0)
        return fail(SpecializationStatus::Unsupported,
                    "RETURN requires the restored entry frame; RET dispatch is "
                    "unsupported");
    }
    Draft.Block.Ops.push_back(std::move(Residual));
    return true;
  }
  if (Flow == StepResult::CondBranch) {
    const auto Address = Ctx.asConst(Exec.branchTarget());
    if (!Address || Address->getBitWidth() > 64)
      return fail(SpecializationStatus::UnresolvedControl,
                  "conditional branch has no exact destination");
    const auto Condition = Ctx.asConst(Exec.branchCondition());
    if (Condition) {
      const int Next = Condition->isZero()
                           ? enqueue(Instruction.Fallthrough, Out)
                           : destination(Address->getZExtValue());
      if (Next < 0)
        return false;
      Draft.Block.Ops.push_back(branchTo(Next));
      Draft.Block.Succs = {Next};
      return true;
    }
    const int Taken = destination(Address->getZExtValue());
    const int Other = enqueue(Instruction.Fallthrough, Out);
    if (Taken < 0 || Other < 0)
      return false;
    if (Taken == Other) {
      Draft.Block.Ops.push_back(branchTo(Taken));
      Draft.Block.Succs = {Taken};
      return true;
    }
    Residual.Inputs[0] = NdVar::cst(static_cast<uint64_t>(Taken), 8);
    Draft.Block.Ops.push_back(std::move(Residual));
    Draft.Block.Succs = {Taken, Other};
    return true;
  }
  std::set<uint64_t> Targets;
  const auto TargetResult = finiteTargets(Ctx, Exec.branchTarget(), Targets,
                                          Options.MaxIndirectTargets);
  if (TargetResult == TargetSetResult::BudgetExceeded)
    return fail(SpecializationStatus::BudgetExceeded,
                "finite indirect-target budget exhausted");
  if (TargetResult != TargetSetResult::Exact || Targets.empty())
    return fail(SpecializationStatus::UnresolvedControl,
                "indirect control target is not an exact finite constant set");
  std::vector<std::pair<uint64_t, int>> Destinations;
  for (uint64_t Target : Targets) {
    const int Next = destination(Target);
    if (Next < 0)
      return false;
    Destinations.emplace_back(Target, Next);
  }
  int Head = Destinations.back().second;
  // Tests use the retained target value, so no expression containing old or
  // overwritten physical registers must be re-serialized from SymExpr.
  for (size_t I = Destinations.size() - 1; I > 0; --I) {
    Head = addDispatchNode(Instruction.Origin, Original.Inputs[0],
                           Destinations[I - 1].first,
                           Destinations[I - 1].second, Head);
    if (Head < 0)
      return false;
  }
  Draft.Block.Ops.push_back(branchTo(Head));
  Draft.Block.Succs = {Head};
  return true;
}

bool Specializer::evaluate(int Id) {
  if (++Result.NodeEvaluations > Options.MaxNodeEvaluations)
    return fail(SpecializationStatus::BudgetExceeded,
                "specialization fixed-point evaluation budget exhausted");
  // No reference into Nodes survives enqueue(), which may grow the vector.
  Node Draft;
  Draft.Key = Nodes[Id].Key;
  Draft.Incoming = Nodes[Id].Incoming;
  SymContext Ctx;
  SymState State(Ctx, Options.ByteOrder);
  const SymRef FrameRoot =
      Options.FrameBaseRegister ? Ctx.mkFreshVar(64, "entry_frame") : SymRef();
  seed(Ctx, State, FrameRoot, Draft.Incoming);
  FrameOrigins Origins = Draft.Incoming.Origins;
  SymExec Exec(Ctx, State);
  SpecializationCursor Cursor = Draft.Key.Cursor;
  bool Finished = false;
  while (!Finished) {
    FailureCursor = Cursor.Address;
    auto Fetched = Provider.instruction(Cursor);
    if (!Fetched)
      return fail(SpecializationStatus::Unsupported,
                  llvm::toString(Fetched.takeError()));
    const auto &Instruction = *Fetched;
    if (std::any_of(Instruction.Ops.begin(), Instruction.Ops.end(),
                    [](const LowOp &Op) { return Op.NumInputs > 6; }))
      return fail(SpecializationStatus::InvalidInput,
                  "provider operation exceeds its operand capacity");
    if (Instruction.Origin.Address != Cursor.Address ||
        Instruction.Origin.Mode != Cursor.Mode || !Instruction.Origin.Size ||
        Instruction.Fallthrough.Address == InvalidVA ||
        hasLowInstructionControlFlag(
            Instruction.Origin.ControlFlags,
            LowInstructionControlFlag::InstructionGuard) ||
        Instruction.Origin.Control == LowInstructionControl::Terminator)
      return fail(SpecializationStatus::InvalidInput,
                  "provider returned an inconsistent or unsupported "
                  "instruction boundary");
    LowBlock InputBlock;
    InputBlock.StartAddr = Cursor.Address;
    if (Instruction.Origin.Size > InvalidVA - Cursor.Address)
      return fail(SpecializationStatus::InvalidInput,
                  "provider instruction extent overflows");
    InputBlock.EndAddr = Cursor.Address + Instruction.Origin.Size;
    InputBlock.Ops = Instruction.Ops;
    InputBlock.InstructionBoundaries.push_back(Instruction.Origin);
    if (auto Error = validateLowInstructionBoundaries(
            InputBlock, LowInstructionBoundaryRequirement::Required))
      return fail(SpecializationStatus::InvalidInput,
                  llvm::toString(std::move(Error)));
    NativeSlice Slice{Instruction.Origin, Draft.Block.Ops.size(), 0};
    for (size_t I = 0; I < Instruction.Ops.size(); ++I) {
      if (++Result.EvaluatedOperations > Options.MaxOperations)
        return fail(SpecializationStatus::BudgetExceeded,
                    "specialization operation budget exhausted");
      const LowOp &Original = Instruction.Ops[I];
      if (!supported(Original))
        return fail(SpecializationStatus::Unsupported,
                    std::string("unsupported specialization operation: ") +
                        ndOpName(Original.Opcode));
      if ((Original.Output.isTemp() &&
           Original.Output.Offset >= DispatchTemp) ||
          std::any_of(Original.Inputs, Original.Inputs + Original.NumInputs,
                      [](const NdVar &V) {
                        return V.isTemp() && V.Offset >= DispatchTemp;
                      }))
        return fail(SpecializationStatus::InvalidInput,
                    "native temporary overlaps the reserved dispatch range");
      LowOp Residual = Original;
      bool OutputUnsafe = false;
      for (unsigned J = 0; J < Original.NumInputs; ++J)
        OutputUnsafe |= unsafeOrigin(Original.Inputs[J], Origins);
      if (FrameRoot &&
          (Original.Opcode == NdOp::LOAD || Original.Opcode == NdOp::STORE)) {
        const auto Memory = lowMemoryOperands(Original);
        const auto Displacement = affineDisplacement(
            Ctx, Exec.operandValue(*Memory.Address), FrameRoot);
        if (Original.Opcode == NdOp::STORE) {
          if (Options.RequireRestoredFrameAtReturn) {
            if (Displacement) {
              for (unsigned B = 0; B < Memory.AccessSize; ++B)
                if (*Displacement + B < 8)
                  return fail(SpecializationStatus::Unsupported,
                              "STORE overlaps the entry return control slot");
            } else if (unsafeOrigin(*Memory.Address, Origins)) {
              return fail(SpecializationStatus::Unsupported,
                          "STORE has unresolved frame-derived or unknown "
                          "address origin");
            } else if (!Options.ExternalStoresPreserveEntryReturnSlot) {
              return fail(SpecializationStatus::Unsupported,
                          "external STORE requires an entry control-slot "
                          "nonalias contract");
            }
          }
          const bool ValueUnsafe = unsafeOrigin(*Memory.StoredValue, Origins);
          if (Displacement) {
            for (unsigned B = 0; B < Memory.AccessSize; ++B) {
              const uint64_t Offset = *Displacement + B;
              if (ValueUnsafe)
                Origins.ExternalFrameBytes.erase(Offset);
              else
                Origins.ExternalFrameBytes.insert(Offset);
            }
          } else if (ValueUnsafe) {
            Origins.ExternalFrameBytes.clear();
          }
          // An unknown write of external-origin bytes may change known frame
          // values, but cannot turn already-external bytes into root pointers.
          // SymState still invalidates their constant values independently.
        } else {
          OutputUnsafe = true;
          if (Displacement) {
            OutputUnsafe = false;
            for (unsigned B = 0; B < Memory.AccessSize; ++B)
              OutputUnsafe |=
                  !Origins.ExternalFrameBytes.count(*Displacement + B);
          }
        }
      }
      if (FrameRoot) {
        if (Original.Output.isReg() && Original.Output.Size == 8)
          AffineCandidates.insert(Original.Output.Offset);
        for (unsigned J = 0; J < Original.NumInputs; ++J)
          if (Original.Inputs[J].isReg() && Original.Inputs[J].Size == 8)
            AffineCandidates.insert(Original.Inputs[J].Offset);
      }
      for (unsigned J = 0; J < Original.NumInputs; ++J) {
        if (!scalarLocation(Original.Inputs[J]))
          continue;
        if (const auto Constant =
                Ctx.asConst(Exec.operandValue(Original.Inputs[J]))) {
          if (Constant->getBitWidth() <= 64)
            Residual.Inputs[J] = isNumericConstantOperand(Original.Opcode, J)
                                     ? NdVar::scalar(Constant->getZExtValue(),
                                                     Original.Inputs[J].Size)
                                     : NdVar::cst(Constant->getZExtValue(),
                                                  Original.Inputs[J].Size);
        }
      }
      bool FoldedImmutableRead = false;
      if (Original.Opcode == NdOp::LOAD) {
        const auto Memory = lowMemoryOperands(Original);
        const auto Address = Ctx.asConst(Exec.operandValue(*Memory.Address));
        if (Address && Address->getBitWidth() <= 64) {
          const va_t VA = Address->getZExtValue();
          if (auto Read = Provider.immutableRead(VA, Memory.AccessSize)) {
            if (Read->Bytes.size() != Memory.AccessSize ||
                Read->Bytes.empty() || Read->Bytes.size() > 8 ||
                VA > InvalidVA - (Read->Bytes.size() - 1))
              return fail(
                  SpecializationStatus::InvalidInput,
                  "immutable-read certificate has an invalid byte extent");
            uint64_t Value = 0;
            for (size_t B = 0; B < Read->Bytes.size(); ++B) {
              const unsigned Shift =
                  8 * (Options.ByteOrder == llvm::endianness::little
                           ? B
                           : Read->Bytes.size() - B - 1);
              Value |= uint64_t{Read->Bytes[B]} << Shift;
            }
            Residual.Opcode = NdOp::COPY;
            Residual.NumInputs = 0;
            Residual.addInput(NdVar::cst(Value, Original.Output.Size));
            FoldedImmutableRead = true;
            Draft.Reads.push_back({Original.Addr, Original.Seq, VA,
                                   std::move(Read->Bytes),
                                   std::move(Read->Evidence)});
          }
        }
      }
      const unsigned BeforeOpaque = Exec.opaqueOperationCount();
      // A certified immutable read equals this exact COPY. Do not seed it by
      // pretending a guest STORE happened: that would unnecessarily clobber
      // retained frame facts under SymState's correct alias rules.
      const StepResult Flow =
          Exec.step(FoldedImmutableRead ? Residual : Original);
      if (Flow == StepResult::Unmodelled ||
          Exec.opaqueOperationCount() != BeforeOpaque)
        return fail(SpecializationStatus::Unsupported,
                    "symbolic execution declined exact scalar semantics");
      if (FrameRoot && scalarLocation(Original.Output)) {
        if (Ctx.asConst(Exec.operandValue(Original.Output)))
          OutputUnsafe = false;
        setUnsafeOrigin(Original.Output, OutputUnsafe, Origins);
      }
      if (Flow == StepResult::Continue) {
        Draft.Block.Ops.push_back(std::move(Residual));
        continue;
      }
      if (I + 1 != Instruction.Ops.size())
        return fail(SpecializationStatus::Unsupported,
                    "control transfer before the end of a lifted instruction");
      if (!emitTargets(Draft, Instruction, Original, std::move(Residual), Exec,
                       Ctx, State, FrameRoot, Origins, Flow))
        return false;
      Finished = true;
    }
    Slice.OpCount = Draft.Block.Ops.size() - Slice.FirstOp;
    Draft.Slices.push_back(std::move(Slice));
    if (Instruction.Ops.empty() &&
        ++Result.EvaluatedOperations > Options.MaxOperations)
      return fail(SpecializationStatus::BudgetExceeded,
                  "empty-instruction exploration budget exhausted");
    if (!Finished) {
      if (Instruction.Fallthrough == Cursor)
        return fail(SpecializationStatus::InvalidInput,
                    "non-branching instruction does not advance the cursor");
      Cursor = Instruction.Fallthrough;
    }
  }
  Nodes[Id].Block = std::move(Draft.Block);
  Nodes[Id].Slices = std::move(Draft.Slices);
  Nodes[Id].Reads = std::move(Draft.Reads);
  return true;
}

bool Specializer::publish() {
  FailureCursor = InvalidVA;
  // Reprocessing may leave old synthetic dispatch chains unreachable. Publish
  // only the final graph, in root-first order, and derive all predecessor
  // lists.
  std::vector<int> Order;
  std::set<int> Reachable;
  std::deque<int> Queue{0};
  while (!Queue.empty()) {
    const int Id = Queue.front();
    Queue.pop_front();
    if (Id < 0 || static_cast<size_t>(Id) >= Nodes.size())
      return fail(SpecializationStatus::InvalidInput,
                  "invalid residual CFG edge");
    if (!Reachable.insert(Id).second)
      continue;
    Order.push_back(Id);
    for (int Next : Nodes[Id].Block.Succs)
      Queue.push_back(Next);
  }
  std::map<int, int> Remap;
  std::map<int, va_t> Labels;
  // Labels are control identities, never runtime addresses. Keep the first
  // label equal to the real entry for existing ABI/root consumers; all clones
  // live in a disjoint synthetic label range, with native origins in a sidecar.
  va_t NextLabel = uint64_t{1} << 63;
  for (int Id : Order) {
    Remap[Id] = static_cast<int>(Remap.size()) + 1;
    Labels[Id] = NextLabel;
    NextLabel += Nodes[Id].Slices.size();
  }
  LowFunc Function;
  Function.Entry = Entry.Address;
  // An entry anchor keeps the public native entry while all cloned instruction
  // spans remain contiguous and disjoint. Mixing one native start with distant
  // synthetic instruction addresses would violate LowIR boundary invariants.
  LowBlock Anchor;
  Anchor.Id = 0;
  Anchor.StartAddr = Entry.Address;
  Anchor.EndAddr = Entry.Address + 1;
  Anchor.Succs = {Remap.at(0)};
  LowOp AnchorBranch = branchTo(0);
  AnchorBranch.Inputs[0] = NdVar::cst(Labels.at(0), 8);
  AnchorBranch.Addr = Entry.Address;
  AnchorBranch.Seq = 0;
  Anchor.Ops.push_back(AnchorBranch);
  LowInstructionBoundary AnchorBoundary;
  AnchorBoundary.Address = Entry.Address;
  AnchorBoundary.Size = 1;
  AnchorBoundary.OpCount = 1;
  AnchorBoundary.Control = LowInstructionControl::Branch;
  AnchorBoundary.ControlFlags = LowInstructionControlFlag::Branch;
  AnchorBoundary.Immediate = Labels.at(0);
  Anchor.InstructionBoundaries.push_back(AnchorBoundary);
  Function.Blocks.push_back(std::move(Anchor));
  ++Function.DecodedInstructionCount;
  ++Function.LiftedInstructionCount;
  for (int Id : Order) {
    LowBlock Block = Nodes[Id].Block;
    Block.Id = Remap[Id];
    Block.StartAddr = Labels[Id];
    Block.Preds.clear();
    for (int &Successor : Block.Succs)
      Successor = Remap.at(Successor);
    for (LowOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR)
        Op.Inputs[0] =
            NdVar::cst(Labels.at(static_cast<int>(Op.Inputs[0].Offset)), 8);
    va_t InstructionLabel = Block.StartAddr;
    for (const NativeSlice &Slice : Nodes[Id].Slices) {
      const va_t Address = InstructionLabel++;
      LowInstructionBoundary Boundary;
      Boundary.Address = Address;
      Boundary.Size = 1;
      Boundary.FirstOp = Slice.FirstOp;
      Boundary.OpCount = Slice.OpCount;
      // These are residual instructions, not a claim that synthetic labels
      // obey the original ISA's encoded instruction width or execution mode.
      for (size_t I = Slice.FirstOp; I < Slice.FirstOp + Slice.OpCount; ++I) {
        LowOp &Op = Block.Ops[I];
        Op.Addr = Address;
        Op.Seq = static_cast<int>(I - Slice.FirstOp);
        if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
          Boundary.Control = LowInstructionControl::Branch;
          Boundary.ControlFlags = LowInstructionControlFlag::Branch;
          Boundary.Immediate = Op.Inputs[0].Offset;
          if (Op.Opcode == NdOp::COND_BR)
            Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
        } else if (Op.Opcode == NdOp::RETURN) {
          Boundary.Control = LowInstructionControl::Return;
          Boundary.ControlFlags = LowInstructionControlFlag::Return;
          Boundary.Immediate = Slice.Origin.Immediate;
        }
      }
      Block.InstructionBoundaries.push_back(Boundary);
      Result.Origins.push_back({Address, Slice.Origin});
      ++Function.DecodedInstructionCount;
      ++Function.LiftedInstructionCount;
    }
    Block.EndAddr = Block.InstructionBoundaries.empty()
                        ? Block.StartAddr + 1
                        : Block.InstructionBoundaries.back().Address + 1;
    Result.Reads.insert(Result.Reads.end(), Nodes[Id].Reads.begin(),
                        Nodes[Id].Reads.end());
    Function.Blocks.push_back(std::move(Block));
  }
  for (const LowBlock &Block : Function.Blocks)
    for (int Next : Block.Succs)
      Function.Blocks[Next].Preds.push_back(Block.Id);
  Function.ModuleAnalysisRoots.insert(Entry.Address);
  Function.OrdinaryModuleAnalysisRoots.insert(Entry.Address);
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required))
    return fail(SpecializationStatus::InvalidInput,
                llvm::toString(std::move(Error)));
  Result.Residual = std::move(Function);
  Result.Contexts = static_cast<uint32_t>(Indices.size());
  Result.Status = SpecializationStatus::Complete;
  return true;
}

SpecializationResult Specializer::run() {
  if (Entry.Address == InvalidVA || Entry.Address >= (uint64_t{1} << 63) ||
      (Options.ByteOrder != llvm::endianness::little &&
       Options.ByteOrder != llvm::endianness::big) ||
      !Options.MaxNodes || !Options.MaxContextsPerAddress ||
      !Options.MaxOperations || !Options.MaxNodeEvaluations ||
      !Options.MaxIndirectTargets) {
    fail(SpecializationStatus::InvalidInput,
         "invalid specialization entry or budget");
    return std::move(Result);
  }
  for (const auto &Range : Options.ControlRegisters)
    if (!Range.Bytes || Range.Bytes > 8 ||
        Range.Offset > InvalidVA - (Range.Bytes - 1)) {
      fail(SpecializationStatus::InvalidInput,
           "invalid control register range");
      return std::move(Result);
    }
  if (Options.FrameBaseRegister &&
      (Options.FrameBaseRegister->Bytes != 8 ||
       Options.FrameBaseRegister->Offset > InvalidVA - 7)) {
    fail(SpecializationStatus::InvalidInput,
         "frame root must name one complete 64-bit register");
    return std::move(Result);
  }
  if (Options.RequireRestoredFrameAtReturn && !Options.FrameBaseRegister) {
    fail(SpecializationStatus::InvalidInput,
         "return-frame verification requires an entry frame root");
    return std::move(Result);
  }
  for (const auto &Slot : Options.ControlFrameSlots)
    if (!Options.FrameBaseRegister || !Slot.Bytes || Slot.Bytes > 8 ||
        static_cast<uint64_t>(Slot.Offset) > InvalidVA - (Slot.Bytes - 1)) {
      fail(SpecializationStatus::InvalidInput,
           "invalid entry-relative control frame slot");
      return std::move(Result);
    }
  SymContext Ctx;
  SymState Initial(Ctx, Options.ByteOrder);
  std::set<ByteKey> Seeded;
  SymRef FrameRoot;
  FrameOrigins InitialOrigins;
  if (Options.FrameBaseRegister) {
    // The root names the function-entry value, independently of each node's
    // current register inputs. Losing an affine relation must not silently
    // identify a later, dynamic stack pointer with this original value.
    FrameRoot = Ctx.mkFreshVar(64, "entry_frame");
    const uint64_t Offset = Options.FrameBaseRegister->Offset;
    Initial.write(SymSpace::Register, Offset, FrameRoot);
    AffineCandidates.insert(Offset);
    for (unsigned I = 0; I < 8; ++I)
      Seeded.emplace(SymSpace::Register, Offset + I);
    setUnsafeOrigin(NdVar::reg(Offset, 8), true, InitialOrigins);
  }
  for (const auto &Constant : Options.EntryConstants) {
    const NdVar &Location = Constant.Location;
    if (!scalarLocation(Location) || !validValue(Location)) {
      fail(SpecializationStatus::InvalidInput,
           "invalid entry constant location");
      return std::move(Result);
    }
    const SymSpace Space =
        Location.isReg() ? SymSpace::Register : SymSpace::Temporary;
    for (unsigned I = 0; I < Location.Size; ++I)
      if (!Seeded.emplace(Space, Location.Offset + I).second) {
        fail(SpecializationStatus::InvalidInput,
             "overlapping entry constant bindings");
        return std::move(Result);
      }
    Initial.write(Space, Location.Offset,
                  Ctx.mkConst(8 * Location.Size, Constant.Value));
    setUnsafeOrigin(Location, false, InitialOrigins);
  }
  enqueue(Entry, project(Initial, FrameRoot, AffineCandidates, InitialOrigins));
  while (!Failed && !Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    Nodes[Id].Pending = false;
    evaluate(Id);
  }
  if (!Failed)
    publish();
  Result.Contexts = static_cast<uint32_t>(Indices.size());
  if (Failed) {
    Result.Residual = {};
    Result.Origins.clear();
    Result.Reads.clear();
  }
  return std::move(Result);
}

} // namespace

SpecializationResult
specializeInterpreter(SpecializationProvider &Provider,
                      SpecializationCursor Entry,
                      const SpecializationOptions &Options) {
  return Specializer(Provider, Entry, Options).run();
}

} // namespace neverd::analysis
