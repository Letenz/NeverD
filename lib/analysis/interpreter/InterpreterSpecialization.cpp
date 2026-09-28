//===- InterpreterSpecialization.cpp - LowIR partial evaluation -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/InterpreterSpecialization.h"

#include "ControlDiscovery.h"
#include "FiniteValues.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <deque>
#include <iterator>
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
using detail::FiniteValues;
using detail::FiniteValueStatus;

constexpr uint64_t X64PushfModelledFlags[] = {
    x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
    x86reg::SF, x86reg::DF, x86reg::OF};

/// A finite relation over selected control fields. Empty Fields denotes Top.
/// Field IDs index the register hints followed by the frame-slot hints. Only
/// numeric tuples cross a node boundary; symbolic identities remain local.
struct ControlRelation {
  std::vector<uint32_t> Fields;
  std::vector<std::vector<uint64_t>> Tuples;

  bool operator==(const ControlRelation &) const = default;
};

struct FrameOrigins {
  /// May originate from the entry root or memory of unknown provenance.
  /// This survives projection even when the affine value itself is lost.
  std::set<ByteKey> UnsafeScalars;
  /// May-undefined x64 flags; joins take the union. An undefined entry flag
  /// cannot become source-defined merely because symbolic algebra simplifies
  /// an expression that still reads it in the residual program.
  std::set<uint64_t> UndefinedFlags;
  /// Unlike physical entry registers, an unseen lifter temporary is not a
  /// caller-supplied external input. It needs a positive defining provenance.
  std::set<uint64_t> ExternalTemporaries;
  /// Must facts: these frame bytes were written with external scalar origins.
  /// Values may be dynamic, e.g. a caller's output pointer spilled to a frame.
  std::set<uint64_t> ExternalFrameBytes;
};

struct FrameFacts {
  /// Exact, complete root-relative pointers stored at root-relative addresses.
  /// Partial and potentially aliasing writes invalidate the complete pointer.
  std::map<uint64_t, uint64_t> AffineValues;
  bool NativeStackActive = false;
  std::set<uint64_t> NativeReturnSlots;
};

struct Projection {
  Constants Scalars;
  /// Complete 64-bit register values equal to the entry frame root plus a
  /// modular displacement. No arbitrary expression escapes a node context.
  std::map<uint64_t, uint64_t> AffineRegisters;
  std::map<uint64_t, uint8_t> FrameBytes;
  FrameFacts Frame;
  FrameOrigins Origins;
  ControlRelation Controls;
};

struct ContextKey {
  SpecializationCursor Cursor;
  std::vector<std::optional<uint64_t>> Controls;
  bool NativeStackActive = false;
  std::optional<uint64_t> StackDisplacement;
  std::vector<std::pair<uint64_t, std::optional<uint64_t>>> ReturnSlots;

  bool operator<(const ContextKey &Other) const {
    return std::tie(Cursor.Address, Cursor.Mode, Controls, NativeStackActive,
                    StackDisplacement, ReturnSlots) <
           std::tie(Other.Cursor.Address, Other.Cursor.Mode, Other.Controls,
                    Other.NativeStackActive, Other.StackDisplacement,
                    Other.ReturnSlots);
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
  return detail::frameRelativeOffset(Ctx, Value, Root);
}

Projection project(SymState &State, SymRef Root,
                   const std::set<uint64_t> &AffineCandidates,
                   const FrameOrigins &Origins, const FrameFacts &Frame) {
  Projection Result;
  Result.Origins = Origins;
  Result.Frame = Frame;
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
  for (const auto &[Offset, Displacement] : Values.Frame.AffineValues)
    State.store(Ctx.mkAdd(Root, Ctx.mkConst(64, Offset)),
                Ctx.mkAdd(Root, Ctx.mkConst(64, Displacement)));
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

bool joinControls(ControlRelation &Into, const ControlRelation &Incoming,
                  uint32_t Limit, uint32_t &Widenings) {
  if (Into.Fields.empty())
    return false;
  ControlRelation Joined;
  std::set_intersection(Into.Fields.begin(), Into.Fields.end(),
                        Incoming.Fields.begin(), Incoming.Fields.end(),
                        std::back_inserter(Joined.Fields));
  if (!Joined.Fields.empty()) {
    std::set<std::vector<uint64_t>> Rows;
    const auto AddRows = [&](const ControlRelation &Source) {
      for (const auto &Row : Source.Tuples) {
        std::vector<uint64_t> Projected;
        for (uint32_t Field : Joined.Fields) {
          const auto At = std::lower_bound(Source.Fields.begin(),
                                           Source.Fields.end(), Field);
          Projected.push_back(Row[At - Source.Fields.begin()]);
        }
        Rows.insert(std::move(Projected));
        if (Rows.size() > Limit)
          return false;
      }
      return true;
    };
    if (!AddRows(Into) || !AddRows(Incoming)) {
      Joined = {};
      ++Widenings;
    } else {
      Joined.Tuples.assign(Rows.begin(), Rows.end());
    }
  }
  if (Joined == Into)
    return false;
  Into = std::move(Joined);
  return true;
}

bool intersect(Projection &Into, const Projection &Incoming,
               uint32_t TupleLimit, uint32_t &Widenings) {
  bool Changed = intersectMap(Into.Scalars, Incoming.Scalars);
  Changed |= intersectMap(Into.AffineRegisters, Incoming.AffineRegisters);
  Changed |= intersectMap(Into.FrameBytes, Incoming.FrameBytes);
  Changed |= intersectMap(Into.Frame.AffineValues, Incoming.Frame.AffineValues);
  Changed |=
      joinControls(Into.Controls, Incoming.Controls, TupleLimit, Widenings);
  const auto OldUnsafe = Into.Origins.UnsafeScalars.size();
  Into.Origins.UnsafeScalars.insert(Incoming.Origins.UnsafeScalars.begin(),
                                    Incoming.Origins.UnsafeScalars.end());
  Changed |= OldUnsafe != Into.Origins.UnsafeScalars.size();
  const auto OldUndefined = Into.Origins.UndefinedFlags.size();
  Into.Origins.UndefinedFlags.insert(Incoming.Origins.UndefinedFlags.begin(),
                                     Incoming.Origins.UndefinedFlags.end());
  Changed |= OldUndefined != Into.Origins.UndefinedFlags.size();
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

/// The x64 lifter uses these exact shapes to read and restore the system
/// portion of RFLAGS. The residual keeps both intrinsics. During analysis a
/// PUSHFQ snapshot is an unconstrained runtime value: this overapproximates
/// every architectural flag image without inventing a constant or an alias
/// fact. POPFQ has no modelled scalar output; later snapshots are fresh again.
bool runtimeFlagsIntrinsic(const LowOp &Op) {
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::Pushf)
    return Op.NumInputs == 1 && Op.Output.isTemp() && Op.Output.Size == 8;
  if (Id == Intrinsic::Popf)
    return Op.NumInputs == 2 && Op.Output.Size == 0 && Op.Inputs[1].Size == 8;
  return false;
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
  case NdOp::INTRINSIC:
    return runtimeFlagsIntrinsic(Op);
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

// Candidates are precision requests, never semantic facts. A failed attempt
// may propose fields, but only a complete fresh fixed point may be published.
enum class ControlDemand { Target, Memory, Producer };
using ProducerDemand =
    std::tuple<va_t, InstructionMode, bool, uint64_t, uint16_t>;

struct ControlRefinement {
  std::vector<SymRegisterRange> Registers;
  std::vector<SpecializationFrameSlot> Slots;
  std::vector<SymRegisterRange> ContextRegisters;
  std::vector<SpecializationFrameSlot> ContextSlots;
  std::vector<SymRegisterRange> PendingContextRegisters;
  std::vector<SpecializationFrameSlot> PendingContextSlots;
  std::set<ProducerDemand> ProducerDemands;
  std::set<ProducerDemand> PendingProducerDemands;
  uint64_t Visits = 0;
  uint32_t CreatedNodes = 0;
  bool BudgetExceeded = false;
  bool PrecisionFailure = false;
};

class Specializer {
public:
  Specializer(SpecializationProvider &Provider, SpecializationCursor Entry,
              const SpecializationOptions &Options, size_t ManualRegisters,
              size_t ManualSlots, ControlRefinement &Refinement,
              SpecializationResult Previous)
      : Provider(Provider), Entry(Entry), Options(Options),
        ManualRegisters(ManualRegisters), ManualSlots(ManualSlots),
        Refinement(Refinement), Result(std::move(Previous)) {
    Result.Diagnostic.clear();
  }

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
  void discover(SymState &State, SymRef Value, SymRef Root,
                ControlDemand Demand = ControlDemand::Target);
  bool needsProducer(SpecializationCursor Successor, uint32_t Field) const;
  int enqueue(SpecializationCursor Cursor, const Projection &Incoming);
  bool evaluate(int Id);
  bool emitTargets(Node &Draft, const SpecializationInstruction &Instruction,
                   const LowOp &Original, LowOp Residual, SymExec &Exec,
                   SymContext &Ctx, SymState &State, SymRef FrameRoot,
                   const FrameOrigins &Origins, const FrameFacts &Frame,
                   StepResult Flow);
  bool publish();
  SymRef controlValue(SymState &State, SymRef Root, uint32_t Field);
  SymRef controlPredicate(SymState &State, SymRef Root,
                          const ControlRelation &Relation);
  FiniteValues enumerate(SymContext &Ctx, SymRef Predicate,
                         llvm::ArrayRef<SymRef> Values, uint32_t Limit);
  bool projectEdge(SymState &State, SymRef Root, const FrameOrigins &Origins,
                   const FrameFacts &Frame, SymRef Predicate,
                   SpecializationCursor Successor, Projection &Out,
                   bool &Reachable);
  int addDispatchNode(const LowInstructionBoundary &Origin, const NdVar &Target,
                      uint64_t Value, int Taken, int Other);
  bool lowerImmutableRead(const LowOp &Original,
                          llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Values,
                          std::vector<LowOp> &Ops);

  SpecializationProvider &Provider;
  SpecializationCursor Entry;
  const SpecializationOptions &Options;
  const size_t ManualRegisters;
  const size_t ManualSlots;
  ControlRefinement &Refinement;
  SpecializationResult Result;
  std::vector<Node> Nodes;
  std::map<ContextKey, int> Indices;
  std::map<std::pair<va_t, InstructionMode>, uint32_t> ContextCounts;
  std::deque<int> Pending;
  bool Failed = false;
  va_t FailureCursor = InvalidVA;
  SpecializationCursor DemandCursor;
  std::set<uint64_t> AffineCandidates;
  // A distinct temporary is needed only in synthetic finite-target dispatch.
  // Native temporaries are checked before publication to prevent collisions.
  static constexpr uint64_t DispatchTemp = uint64_t{1} << 62;
  static constexpr uint64_t ReadAddressTemp = DispatchTemp + 8;
  static constexpr uint64_t ReadConditionTemp = DispatchTemp + 16;
  static constexpr uint64_t NativeReturnTemp = DispatchTemp + 24;
};

void Specializer::discover(SymState &State, SymRef Value, SymRef Root,
                           ControlDemand Demand) {
  if (!Options.DiscoverControlState || Refinement.BudgetExceeded)
    return;
  auto Dependencies = detail::gatherControlDependencies(
      State, Value, Root, Options.MaxDiscoveryVisits - Refinement.Visits);
  Refinement.Visits += Dependencies.Visited;
  if (Dependencies.Status == detail::ControlDiscoveryStatus::BudgetExceeded) {
    Refinement.BudgetExceeded = true;
    return;
  }
  const auto Add = [&](auto &Pending, const auto &Existing, const auto &Field,
                       auto &ContextFields, auto &PendingContexts,
                       bool Register, size_t ManualCount) {
    const auto Same = [&](const auto &Other) {
      return Field.Offset == Other.Offset && Field.Bytes == Other.Bytes;
    };
    // Symbolic inputs belong to this node's entry. Restrict backward producer
    // demands to that program point: a physical register may hold unrelated
    // business data in another handler. Locations nominate precision only.
    const ProducerDemand Producer{DemandCursor.Address, DemandCursor.Mode,
                                  Register, static_cast<uint64_t>(Field.Offset),
                                  Field.Bytes};
    if (!Refinement.ProducerDemands.contains(Producer))
      Refinement.PendingProducerDemands.insert(Producer);
    if (std::any_of(Existing.begin(), Existing.end(), Same)) {
      // First try relational propagation. If an exact memory address still
      // cannot be established on a later attempt, separate only its already
      // tracked dependencies whose incoming bytes are proven constant. This
      // is bounded context refinement, never a sample-based input binding.
      if (Demand == ControlDemand::Memory &&
          !std::any_of(Existing.begin(), Existing.begin() + ManualCount,
                       Same) &&
          !std::any_of(ContextFields.begin(), ContextFields.end(), Same) &&
          !std::any_of(PendingContexts.begin(), PendingContexts.end(), Same))
        PendingContexts.push_back(Field);
      return;
    }
    if (std::any_of(Pending.begin(), Pending.end(), Same))
      return;
    const size_t Count = Options.ControlRegisters.size() +
                         Options.ControlFrameSlots.size() +
                         Refinement.Registers.size() + Refinement.Slots.size();
    if (Count >= Options.MaxControlFields) {
      Refinement.BudgetExceeded = true;
      return;
    }
    Pending.push_back(Field);
  };
  for (const auto &Range : Dependencies.RegisterRanges)
    Add(Refinement.Registers, Options.ControlRegisters, Range,
        Refinement.ContextRegisters, Refinement.PendingContextRegisters, true,
        ManualRegisters);
  for (const auto &Slot : Dependencies.FrameSlots)
    Add(Refinement.Slots, Options.ControlFrameSlots, Slot,
        Refinement.ContextSlots, Refinement.PendingContextSlots, false,
        ManualSlots);
}

bool Specializer::needsProducer(SpecializationCursor Successor,
                                uint32_t Field) const {
  const bool Register = Field < Options.ControlRegisters.size();
  const uint64_t Offset =
      Register
          ? Options.ControlRegisters[Field].Offset
          : static_cast<uint64_t>(
                Options
                    .ControlFrameSlots[Field - Options.ControlRegisters.size()]
                    .Offset);
  const uint16_t Bytes =
      Register
          ? Options.ControlRegisters[Field].Bytes
          : Options.ControlFrameSlots[Field - Options.ControlRegisters.size()]
                .Bytes;
  return Refinement.ProducerDemands.contains(
      {Successor.Address, Successor.Mode, Register, Offset, Bytes});
}

bool Specializer::lowerImmutableRead(
    const LowOp &Original, llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Values,
    std::vector<LowOp> &Ops) {
  const bool SameValue =
      std::all_of(Values.begin(), Values.end(), [&](const auto &Value) {
        return Value.second == Values.front().second;
      });
  const uint64_t Count = SameValue ? 1 : 2 * Values.size();
  // The original LOAD was already charged. Charge every additional residual
  // operation before materializing the replacement, including address capture.
  if (Count - 1 > Options.MaxOperations - Result.EvaluatedOperations)
    return fail(SpecializationStatus::BudgetExceeded,
                "immutable-read lowering operation budget exhausted");
  Result.EvaluatedOperations += Count - 1;
  const auto Add = [&](NdOp Opcode, NdVar Output,
                       std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = Original.Addr;
    Op.Seq = Original.Seq;
    for (const NdVar &Input : Inputs)
      Op.addInput(Input);
    Ops.push_back(Op);
  };
  const auto Address = *lowMemoryOperands(Original).Address;
  const NdVar Captured = NdVar::tmp(ReadAddressTemp, Address.Size);
  const NdVar Condition = NdVar::tmp(ReadConditionTemp, 1);
  if (!SameValue)
    // Capture before writing Output: a LOAD may overwrite its address register
    // or only some overlapping byte lanes of that register.
    Add(NdOp::COPY, Captured, {Address});
  Add(NdOp::COPY, Original.Output,
      {NdVar::scalar(Values.back().second, Original.Output.Size)});
  if (!SameValue)
    for (size_t I = Values.size() - 1; I > 0; --I) {
      Add(NdOp::INT_EQUAL, Condition,
          {Captured, NdVar::scalar(Values[I - 1].first, Address.Size)});
      Add(NdOp::SELECT, Original.Output,
          {Condition, NdVar::scalar(Values[I - 1].second, Original.Output.Size),
           Original.Output});
    }
  return true;
}

SymRef Specializer::controlValue(SymState &State, SymRef Root, uint32_t Field) {
  if (Field < Options.ControlRegisters.size()) {
    const auto &Range = Options.ControlRegisters[Field];
    return State.read(SymSpace::Register, Range.Offset, Range.Bytes);
  }
  const auto &Slot =
      Options.ControlFrameSlots[Field - Options.ControlRegisters.size()];
  SymContext &Ctx = State.context();
  return State.load(Ctx.mkAdd(Root, Ctx.mkConst(64, Slot.Offset)), Slot.Bytes);
}

SymRef Specializer::controlPredicate(SymState &State, SymRef Root,
                                     const ControlRelation &Relation) {
  SymContext &Ctx = State.context();
  if (Relation.Fields.empty())
    return Ctx.mkTrue();
  llvm::SmallVector<SymRef, 8> Values;
  for (uint32_t Field : Relation.Fields)
    Values.push_back(controlValue(State, Root, Field));
  llvm::SmallVector<SymRef, 8> Cases;
  for (const auto &Tuple : Relation.Tuples) {
    llvm::SmallVector<SymRef, 8> Equal;
    for (size_t I = 0; I < Values.size(); ++I)
      Equal.push_back(
          Ctx.mkEq(Values[I], Ctx.mkConst(Ctx.width(Values[I]), Tuple[I])));
    Cases.push_back(Ctx.mkAnd(Equal));
  }
  return Ctx.mkOr(Cases);
}

FiniteValues Specializer::enumerate(SymContext &Ctx, SymRef Predicate,
                                    llvm::ArrayRef<SymRef> Values,
                                    uint32_t Limit) {
  if (Ctx.numNodes() > Options.MaxSymbolicNodes) {
    fail(SpecializationStatus::BudgetExceeded,
         "specialization symbolic-node budget exhausted");
    return {FiniteValueStatus::Unknown, {}};
  }
  auto Domain = detail::enumerateFiniteValues(Ctx, Predicate, Values, Limit,
                                              Options, Result.SolverQueries);
  if (Ctx.numNodes() > Options.MaxSymbolicNodes) {
    fail(SpecializationStatus::BudgetExceeded,
         "specialization symbolic-node budget exhausted");
    return {FiniteValueStatus::Unknown, {}};
  }
  if (Domain.Status == FiniteValueStatus::Invalid)
    fail(SpecializationStatus::InvalidInput,
         "invalid finite-value proof query");
  else if (Domain.Status == FiniteValueStatus::QueryBudgetExceeded)
    fail(SpecializationStatus::BudgetExceeded,
         "specialization solver-query budget exhausted");
  return Domain;
}

bool Specializer::projectEdge(SymState &State, SymRef Root,
                              const FrameOrigins &Origins,
                              const FrameFacts &Frame, SymRef Predicate,
                              SpecializationCursor Successor, Projection &Out,
                              bool &Reachable) {
  SymContext &Ctx = State.context();
  Out = project(State, Root, AffineCandidates, Origins, Frame);
  Reachable = !Ctx.isConstZero(Predicate);
  if (!Reachable)
    return true;

  const auto RecordConstant = [&](uint32_t Field, uint64_t Value) {
    const bool Register = Field < Options.ControlRegisters.size();
    const uint64_t Offset =
        Register ? Options.ControlRegisters[Field].Offset
                 : static_cast<uint64_t>(
                       Options
                           .ControlFrameSlots[Field -
                                              Options.ControlRegisters.size()]
                           .Offset);
    const uint16_t Bytes =
        Register
            ? Options.ControlRegisters[Field].Bytes
            : Options.ControlFrameSlots[Field - Options.ControlRegisters.size()]
                  .Bytes;
    for (uint16_t I = 0; I < Bytes; ++I) {
      const unsigned Shift =
          8 *
          (Options.ByteOrder == llvm::endianness::little ? I : Bytes - I - 1);
      if (Register)
        Out.Scalars[{SymSpace::Register, Offset + I}] = Value >> Shift;
      else
        Out.FrameBytes[Offset + I] = Value >> Shift;
    }
    // Conditional numeric facts do not erase frame provenance. In particular
    // an affine entry-root register is restored as that root at the next node,
    // even when this edge also proves its numeric value.
  };

  std::vector<uint32_t> Fields;
  llvm::SmallVector<SymRef, 8> Values;
  const size_t FieldCount =
      Options.ControlRegisters.size() + Options.ControlFrameSlots.size();
  for (uint32_t Field = 0; Field < FieldCount; ++Field) {
    SymRef Value = controlValue(State, Root, Field);
    if (const auto Constant = Ctx.asConst(Value)) {
      RecordConstant(Field, Constant->getZExtValue());
    } else {
      const bool Producer = needsProducer(Successor, Field);
      if (Producer)
        discover(State, Value, Root, ControlDemand::Producer);
      if (detail::hasUnconstrainedProjectionInput(Ctx, Predicate, Value,
                                                  Options.MaxControlTuples,
                                                  Options.MaxSymbolicNodes)) {
        continue;
      }
      const auto Single =
          enumerate(Ctx, Predicate, {Value}, Options.MaxControlTuples);
      if (Failed)
        return false;
      if (Single.Status != FiniteValueStatus::Complete) {
        continue;
      }
      if (Single.Tuples.empty()) {
        Reachable = false;
        return true;
      }
      if (Single.Tuples.size() == 1)
        RecordConstant(Field, Single.Tuples.front().front());
    }
    Fields.push_back(Field);
    Values.push_back(Value);
  }
  // Enumerating the complete tuple, rather than multiplying the separately
  // proved field ranges, preserves correlations in dispatch decoding. An
  // unknown field is omitted, never concretized from a model witness.
  auto Joint = enumerate(Ctx, Predicate, Values, Options.MaxControlTuples);
  if (Failed)
    return false;
  if (Joint.Status != FiniteValueStatus::Complete) {
    if (!Fields.empty())
      ++Result.RelationalWidenings;
    return true;
  }
  Reachable = !Joint.Tuples.empty();
  if (Reachable && !Fields.empty())
    Out.Controls = {std::move(Fields), std::move(Joint.Tuples)};
  return true;
}

int Specializer::enqueue(SpecializationCursor Cursor,
                         const Projection &Incoming) {
  if (Cursor.Address >= (uint64_t{1} << 63)) {
    fail(SpecializationStatus::UnresolvedControl,
         "control destination overlaps reserved residual labels");
    return -1;
  }
  ContextKey Key{Cursor, {}};
  // Ordinary automatically demanded fields refine joins only. Context
  // separation additionally requires a repeated unresolved memory dependency;
  // it stays subject to the same cumulative node and per-address bounds.
  for (size_t I = 0; I < ManualRegisters; ++I)
    Key.Controls.push_back(constantWord(
        Incoming.Scalars, Options.ControlRegisters[I], Options.ByteOrder));
  for (size_t I = 0; I < ManualSlots; ++I)
    Key.Controls.push_back(constantFrameWord(
        Incoming.FrameBytes, Options.ControlFrameSlots[I], Options.ByteOrder));
  for (const auto &Range : Refinement.ContextRegisters)
    Key.Controls.push_back(
        constantWord(Incoming.Scalars, Range, Options.ByteOrder));
  for (const auto &Slot : Refinement.ContextSlots)
    Key.Controls.push_back(
        constantFrameWord(Incoming.FrameBytes, Slot, Options.ByteOrder));
  Key.NativeStackActive = Incoming.Frame.NativeStackActive;
  if (Key.NativeStackActive) {
    const auto At =
        Incoming.AffineRegisters.find(Options.FrameBaseRegister->Offset);
    if (At != Incoming.AffineRegisters.end())
      Key.StackDisplacement = At->second;
    for (uint64_t Offset : Incoming.Frame.NativeReturnSlots)
      Key.ReturnSlots.emplace_back(
          Offset, constantFrameWord(Incoming.FrameBytes,
                                    {static_cast<int64_t>(Offset), 8},
                                    Options.ByteOrder));
  }
  const auto Existing = Indices.find(Key);
  if (Existing != Indices.end()) {
    Node &Old = Nodes[Existing->second];
    if (intersect(Old.Incoming, Incoming, Options.MaxControlTuples,
                  Result.RelationalWidenings) &&
        !Old.Pending) {
      Old.Pending = true;
      Pending.push_back(Existing->second);
    }
    return Existing->second;
  }
  auto &Count = ContextCounts[{Cursor.Address, Cursor.Mode}];
  if (Refinement.CreatedNodes >= Options.MaxNodes ||
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
  ++Refinement.CreatedNodes;
  Nodes.push_back(std::move(New));
  Indices.emplace(std::move(Key), Id);
  Pending.push_back(Id);
  return Id;
}

int Specializer::addDispatchNode(const LowInstructionBoundary &Origin,
                                 const NdVar &Target, uint64_t Value, int Taken,
                                 int Other) {
  if (Refinement.CreatedNodes >= Options.MaxNodes) {
    fail(SpecializationStatus::BudgetExceeded,
         "finite dispatch node budget exhausted");
    return -1;
  }
  if (2 > Options.MaxOperations - Result.EvaluatedOperations) {
    fail(SpecializationStatus::BudgetExceeded,
         "finite dispatch operation budget exhausted");
    return -1;
  }
  Result.EvaluatedOperations += 2;
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
  ++Refinement.CreatedNodes;
  Nodes.push_back(std::move(New));
  return Id;
}

bool Specializer::emitTargets(Node &Draft,
                              const SpecializationInstruction &Instruction,
                              const LowOp &Original, LowOp Residual,
                              SymExec &Exec, SymContext &Ctx, SymState &State,
                              SymRef FrameRoot, const FrameOrigins &Origins,
                              const FrameFacts &Frame, StepResult Flow) {
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

  const SymRef Path = Exec.pathPredicate();
  const auto edge = [&](SpecializationCursor Cursor, SymRef Guard,
                        std::optional<uint64_t> KnownTarget) -> int {
    Projection EdgeState;
    bool Reachable = true;
    if (!projectEdge(State, FrameRoot, Origins, Frame, Ctx.mkAnd(Path, Guard),
                     Cursor, EdgeState, Reachable))
      return -1;
    if (!Reachable)
      return -2;
    if (KnownTarget && Flow == StepResult::IndirectBranch &&
        scalarLocation(Original.Inputs[0])) {
      // The retained live operand is exactly what residual dispatch tests.
      // Its selected edge proves this value, as well as the joint control
      // relation projected under the same target equality above.
      const NdVar &Operand = Original.Inputs[0];
      const SymSpace Space =
          Operand.isReg() ? SymSpace::Register : SymSpace::Temporary;
      for (uint16_t I = 0; I < Operand.Size; ++I) {
        const unsigned Shift =
            8 * (Options.ByteOrder == llvm::endianness::little
                     ? I
                     : Operand.Size - I - 1);
        EdgeState.Scalars[{Space, Operand.Offset + I}] =
            static_cast<uint8_t>(*KnownTarget >> Shift);
      }
      // Numeric edge equality does not erase the operand's frame origin.
      // seed() may restore its retained affine entry-root expression instead
      // of these scalar bytes; treating that root as external would let a
      // subsequent non-affine address bypass the return-slot write guard.
    }
    return enqueue(Cursor, EdgeState);
  };
  const auto destination = [&](uint64_t Address, SymRef Guard) -> int {
    auto Target = canonicalizeLowControlTarget(Address, Instruction.Origin.Mode,
                                               Instruction.Origin.TargetMode);
    if (!Target) {
      fail(SpecializationStatus::UnresolvedControl,
           llvm::toString(Target.takeError()));
      return -1;
    }
    return edge({Target->Address, Target->Mode}, Guard, Address);
  };

  if (Flow == StepResult::CondBranch) {
    const auto Address = Ctx.asConst(Exec.branchTarget());
    if (!Address || Address->getBitWidth() > 64)
      return fail(SpecializationStatus::UnresolvedControl,
                  "conditional branch has no exact destination");
    const SymRef Condition = Exec.branchCondition();
    const int Taken = destination(Address->getZExtValue(), Condition);
    if (Taken == -1)
      return false;
    const int Other =
        edge(Instruction.Fallthrough, Ctx.mkNot(Condition), std::nullopt);
    if (Other == -1)
      return false;
    if (Taken == -2 && Other == -2)
      return fail(SpecializationStatus::InvalidInput,
                  "conditional context has no satisfiable successor");
    if (Taken == -2 || Other == -2 || Taken == Other) {
      const int Next = Taken == -2 ? Other : Taken;
      Draft.Block.Ops.push_back(branchTo(Next));
      Draft.Block.Succs = {Next};
      return true;
    }
    Residual.Inputs[0] = NdVar::cst(static_cast<uint64_t>(Taken), 8);
    Draft.Block.Ops.push_back(std::move(Residual));
    Draft.Block.Succs = {Taken, Other};
    return true;
  }

  std::set<uint64_t> Targets;
  const SymRef TargetValue = Exec.branchTarget();
  const auto Structural =
      finiteTargets(Ctx, TargetValue, Targets, Options.MaxIndirectTargets);
  if (Structural != TargetSetResult::Exact) {
    Targets.clear();
    auto Domain =
        enumerate(Ctx, Path, {TargetValue}, Options.MaxIndirectTargets);
    if (Failed)
      return false;
    if (Domain.Status == FiniteValueStatus::Unknown)
      return fail(SpecializationStatus::BudgetExceeded,
                  "finite indirect-target proof exceeded its solver budget");
    if (Domain.Status != FiniteValueStatus::Complete) {
      discover(State, TargetValue, FrameRoot);
      return fail(
          Structural == TargetSetResult::BudgetExceeded
              ? SpecializationStatus::BudgetExceeded
              : SpecializationStatus::UnresolvedControl,
          "indirect control target is not an exact finite constant set");
    }
    for (const auto &Tuple : Domain.Tuples)
      Targets.insert(Tuple.front());
  }
  std::vector<std::pair<uint64_t, int>> Destinations;
  for (uint64_t Target : Targets) {
    SymRef Guard =
        Ctx.mkEq(TargetValue, Ctx.mkConst(Ctx.width(TargetValue), Target));
    const int Next = destination(Target, Guard);
    if (Next == -1)
      return false;
    if (Next != -2)
      Destinations.emplace_back(Target, Next);
  }
  if (Destinations.empty())
    return fail(SpecializationStatus::InvalidInput,
                "indirect context has no satisfiable successor");
  int Head = Destinations.back().second;
  // Dispatch reads the retained target operand. No expression mentioning an
  // old or overwritten physical register is re-serialized into the program.
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
  DemandCursor = Draft.Key.Cursor;
  Draft.Incoming = Nodes[Id].Incoming;
  SymContext Ctx;
  SymState State(Ctx, Options.ByteOrder);
  const SymRef FrameRoot =
      Options.FrameBaseRegister ? Ctx.mkFreshVar(64, "entry_frame") : SymRef();
  seed(Ctx, State, FrameRoot, Draft.Incoming);
  FrameOrigins Origins = Draft.Incoming.Origins;
  FrameFacts Frame = Draft.Incoming.Frame;
  SymExec Exec(Ctx, State);
  Exec.assume(controlPredicate(State, FrameRoot, Draft.Incoming.Controls));
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
    std::vector<LowOp> Operations = Instruction.Ops;
    bool ExpandedReturn = false;
    if (Instruction.NativeStackControl !=
        SpecializationNativeStackControl::None) {
      if (!FrameRoot || !Options.RequireRestoredFrameAtReturn ||
          Instruction.Fallthrough.Address != InputBlock.EndAddr ||
          Instruction.Fallthrough.Mode != Cursor.Mode ||
          Instruction.Ops.size() != 1)
        return fail(SpecializationStatus::InvalidInput,
                    "native stack control requires an exact instruction and "
                    "a verified entry stack root");
      const auto &Base = *Options.FrameBaseRegister;
      const NdVar Stack = NdVar::reg(Base.Offset, Base.Bytes);
      const auto Displacement = affineDisplacement(
          Ctx, State.read(SymSpace::Register, Base.Offset, Base.Bytes),
          FrameRoot);
      if (!Displacement)
        return fail(SpecializationStatus::Unsupported,
                    "native stack control has no exact entry-relative stack "
                    "pointer");
      const LowOp &Control = Instruction.Ops.front();
      const auto Make = [&](NdOp Opcode, NdVar Output,
                            std::initializer_list<NdVar> Inputs) {
        LowOp Op;
        Op.Opcode = Opcode;
        Op.Output = Output;
        Op.Addr = Cursor.Address;
        Op.Seq = static_cast<int>(Operations.size());
        for (const NdVar &Input : Inputs)
          Op.addInput(Input);
        Operations.push_back(Op);
      };
      Frame.NativeStackActive = true;
      if (Instruction.NativeStackControl ==
          SpecializationNativeStackControl::Call) {
        if (Control.Opcode != NdOp::CALL || Control.NumInputs != 1 ||
            !Control.Inputs[0].isConst() || Control.Inputs[0].Size != 8 ||
            !Control.Output.isReg() || Control.Output.Size != 8 ||
            !validValue(Control.Output) ||
            Control.MemoryOrdering != NdMemoryOrdering::None ||
            Control.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            Instruction.Origin.Control != LowInstructionControl::Call)
          return fail(SpecializationStatus::InvalidInput,
                      "native near-call certificate does not match its LowIR");
        Frame.NativeReturnSlots.insert(*Displacement - 8);
        if (Frame.NativeReturnSlots.size() > Options.MaxNativeReturnSlots)
          return fail(SpecializationStatus::BudgetExceeded,
                      "native return-slot context budget exhausted");
        Operations.clear();
        Make(NdOp::INT_SUB, Stack, {Stack, NdVar::scalar(8, 8)});
        Make(NdOp::STORE, {},
             {Stack, NdVar::scalar(Instruction.Fallthrough.Address, 8)});
        Make(NdOp::BRANCH, {}, {Control.Inputs[0]});
      } else if (Instruction.NativeStackControl ==
                 SpecializationNativeStackControl::Return) {
        if (Control.Opcode != NdOp::RETURN || Control.NumInputs > 1 ||
            !supported(Control) ||
            Instruction.Origin.Control != LowInstructionControl::Return ||
            (Instruction.Origin.Immediate &&
             *Instruction.Origin.Immediate != 0))
          return fail(SpecializationStatus::InvalidInput,
                      "native near-return certificate does not match its "
                      "LowIR");
        if (*Displacement != 0) {
          const SymRef ReturnValue =
              State.load(State.read(SymSpace::Register, Base.Offset, 8), 8);
          auto Targets = enumerate(Ctx, Exec.pathPredicate(), {ReturnValue}, 1);
          if (Failed)
            return false;
          if (Targets.Status == FiniteValueStatus::Unknown)
            return fail(SpecializationStatus::BudgetExceeded,
                        "native return-target proof exceeded its solver "
                        "budget");
          if (Targets.Status != FiniteValueStatus::Complete ||
              Targets.Tuples.size() != 1) {
            discover(State, ReturnValue, FrameRoot);
            return fail(SpecializationStatus::UnresolvedControl,
                        "native return address is not one exact target");
          }
          Frame.NativeReturnSlots.erase(*Displacement);
          Operations.clear();
          ExpandedReturn = true;
          Make(NdOp::LOAD, NdVar::tmp(NativeReturnTemp, 8), {Stack});
          Make(NdOp::INT_ADD, Stack, {Stack, NdVar::scalar(8, 8)});
          Make(NdOp::BRANCH, {},
               {NdVar::cst(Targets.Tuples.front().front(), 8)});
        }
        // Reaching the untouched entry return slot is an outer exit even
        // when native code has discarded one or more intermediate frames.
      } else {
        return fail(SpecializationStatus::InvalidInput,
                    "unknown native stack-control certificate");
      }
    }
    NativeSlice Slice{Instruction.Origin, Draft.Block.Ops.size(), 0};
    // LowIR temporary offsets are reused by each native instruction. A prior
    // instruction (or a previous loop iteration) cannot define this one's
    // temporary inputs, even if the numeric offset happens to match.
    std::set<uint64_t> DefinedTemporaries;
    for (size_t I = 0; I < Operations.size(); ++I) {
      if (++Result.EvaluatedOperations > Options.MaxOperations)
        return fail(SpecializationStatus::BudgetExceeded,
                    "specialization operation budget exhausted");
      if (Ctx.numNodes() > Options.MaxSymbolicNodes)
        return fail(SpecializationStatus::BudgetExceeded,
                    "specialization symbolic-node budget exhausted");
      const LowOp &Original = Operations[I];
      if (!supported(Original))
        return fail(SpecializationStatus::Unsupported,
                    std::string("unsupported specialization operation: ") +
                        ndOpName(Original.Opcode));
      for (unsigned J = 0; J < Original.NumInputs; ++J) {
        const NdVar &Input = Original.Inputs[J];
        if (!Input.isReg())
          continue;
        for (uint16_t B = 0; B < Input.Size; ++B)
          if (Origins.UndefinedFlags.count(Input.Offset + B))
            return fail(SpecializationStatus::Unsupported,
                        "native operation reads an unbound entry flag");
      }
      const auto ReservedTemporary = [&](const NdVar &V) {
        if (ExpandedReturn && V == NdVar::tmp(NativeReturnTemp, 8))
          return false;
        return V.isTemp() &&
               (V.Offset >= DispatchTemp || V.Size > DispatchTemp - V.Offset);
      };
      if (ReservedTemporary(Original.Output) ||
          std::any_of(Original.Inputs, Original.Inputs + Original.NumInputs,
                      ReservedTemporary))
        return fail(SpecializationStatus::InvalidInput,
                    "native temporary overlaps the reserved dispatch range");
      for (unsigned J = 0; J < Original.NumInputs; ++J) {
        const NdVar &Input = Original.Inputs[J];
        if (!Input.isTemp())
          continue;
        for (uint16_t B = 0; B < Input.Size; ++B)
          if (!DefinedTemporaries.count(Input.Offset + B))
            return fail(SpecializationStatus::Unsupported,
                        "native operation reads an unbound temporary");
      }
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
          if (Displacement) {
            for (auto It = Frame.AffineValues.begin();
                 It != Frame.AffineValues.end();) {
              bool Overlaps = false;
              for (unsigned B = 0; B < Memory.AccessSize; ++B)
                Overlaps |= *Displacement + B - It->first < 8;
              if (Overlaps)
                It = Frame.AffineValues.erase(It);
              else
                ++It;
            }
            if (Memory.AccessSize == 8)
              if (const auto Value = affineDisplacement(
                      Ctx, Exec.operandValue(*Memory.StoredValue), FrameRoot))
                Frame.AffineValues[*Displacement] = *Value;
          } else {
            Frame.AffineValues.clear();
          }
          if (Options.RequireRestoredFrameAtReturn) {
            if (Displacement) {
              for (unsigned B = 0; B < Memory.AccessSize; ++B)
                if (*Displacement + B < 8)
                  return fail(SpecializationStatus::Unsupported,
                              "STORE overlaps the entry return control slot");
            } else if (unsafeOrigin(*Memory.Address, Origins)) {
              discover(State, Exec.operandValue(*Memory.Address), FrameRoot,
                       ControlDemand::Memory);
              Refinement.PrecisionFailure = true;
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
      std::vector<LowOp> ImmutableOps;
      SymRef FiniteReadValue;
      if (Original.Opcode == NdOp::LOAD) {
        const auto Memory = lowMemoryOperands(Original);
        const SymRef Address = Exec.operandValue(*Memory.Address);
        // An entry-relative address translates the unconstrained frame root
        // bijectively. When the path does not constrain that root, its domain
        // cannot fit an immutable-address projection. Skip this optional proof
        // without inferring reachability or changing the retained memory op.
        const bool FreeFrameAddress =
            affineDisplacement(Ctx, Address, FrameRoot).has_value() &&
            detail::hasUnconstrainedProjectionInput(
                Ctx, Exec.pathPredicate(), FrameRoot,
                Options.MaxImmutableReadAddresses, Options.MaxSymbolicNodes);
        auto Addresses =
            FreeFrameAddress
                ? FiniteValues{FiniteValueStatus::TooManyValues, {}}
                : enumerate(Ctx, Exec.pathPredicate(), {Address},
                            Options.MaxImmutableReadAddresses);
        if (Failed)
          return false;
        if (Addresses.Status == FiniteValueStatus::Complete &&
            !Addresses.Tuples.empty()) {
          std::vector<SpecializationReadWitness> Witnesses;
          std::vector<std::pair<uint64_t, uint64_t>> Values;
          bool Certified = true;
          for (const auto &Tuple : Addresses.Tuples) {
            const va_t VA = Tuple.front();
            auto Read = Provider.immutableRead(VA, Memory.AccessSize);
            if (!Read) {
              Certified = false;
              break;
            }
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
            Values.emplace_back(VA, Value);
            Witnesses.push_back({Original.Addr, Original.Seq, VA,
                                 std::move(Read->Bytes),
                                 std::move(Read->Evidence)});
          }
          if (Certified) {
            FiniteReadValue =
                Ctx.mkConst(Original.Output.Size * 8, Values.back().second);
            for (size_t J = Values.size() - 1; J > 0; --J)
              FiniteReadValue = Ctx.mkIte(
                  Ctx.mkEq(Address, Ctx.mkConst(Ctx.width(Address),
                                                Values[J - 1].first)),
                  Ctx.mkConst(Original.Output.Size * 8, Values[J - 1].second),
                  FiniteReadValue);
            if (!lowerImmutableRead(Original, Values, ImmutableOps))
              return false;
            if (ImmutableOps.size() == 1) {
              Residual = ImmutableOps.front();
              FoldedImmutableRead = true;
            }
            // Numeric data values are not loader-authenticated host pointers,
            // even when their bits happen to equal an image string/code VA.
            Draft.Reads.insert(Draft.Reads.end(),
                               std::make_move_iterator(Witnesses.begin()),
                               std::make_move_iterator(Witnesses.end()));
          }
        }
      }
      const unsigned BeforeOpaque = Exec.opaqueOperationCount();
      // Do not seed certified reads by pretending a guest STORE happened:
      // that would clobber retained frame facts under the correct alias rules.
      StepResult Flow = StepResult::Continue;
      if (Original.Opcode == NdOp::INTRINSIC) {
        // The allowlist above admits only the two exact x64 flag shapes. Keep
        // the original runtime operation in the residual while using a fresh
        // symbolic input for each machine snapshot. Its origin is unknown,
        // especially for the entry-return-slot nonalias proof.
        if (static_cast<Intrinsic>(Original.Inputs[0].Offset) ==
            Intrinsic::Pushf) {
          if (!Origins.UndefinedFlags.empty())
            return fail(SpecializationStatus::Unsupported,
                        "PUSHFQ reads a modelled flag without a source "
                        "definition");
          State.write(SymSpace::Temporary, Original.Output.Offset,
                      State.freshInput("x64_pushfq", 64));
          OutputUnsafe = true;
        }
      } else {
        Flow = Exec.step(FoldedImmutableRead ? Residual : Original);
      }
      if (Flow == StepResult::Unmodelled ||
          Exec.opaqueOperationCount() != BeforeOpaque)
        return fail(SpecializationStatus::Unsupported,
                    "symbolic execution declined exact scalar semantics");
      if (Ctx.numNodes() > Options.MaxSymbolicNodes)
        return fail(SpecializationStatus::BudgetExceeded,
                    "specialization symbolic-node budget exhausted");
      if (FiniteReadValue) {
        State.write(Original.Output.isReg() ? SymSpace::Register
                                            : SymSpace::Temporary,
                    Original.Output.Offset, FiniteReadValue);
        OutputUnsafe = false;
      }
      if (scalarLocation(Original.Output)) {
        if (Ctx.asConst(Exec.operandValue(Original.Output)))
          OutputUnsafe = false;
        setUnsafeOrigin(Original.Output, OutputUnsafe, Origins);
        if (Original.Output.isTemp())
          for (uint16_t B = 0; B < Original.Output.Size; ++B)
            DefinedTemporaries.insert(Original.Output.Offset + B);
        if (Original.Output.isReg())
          for (uint16_t B = 0; B < Original.Output.Size; ++B)
            Origins.UndefinedFlags.erase(Original.Output.Offset + B);
      }
      if (Flow == StepResult::Continue) {
        if (ImmutableOps.empty())
          Draft.Block.Ops.push_back(std::move(Residual));
        else
          Draft.Block.Ops.insert(Draft.Block.Ops.end(),
                                 std::make_move_iterator(ImmutableOps.begin()),
                                 std::make_move_iterator(ImmutableOps.end()));
        continue;
      }
      if (I + 1 != Operations.size())
        return fail(SpecializationStatus::Unsupported,
                    "control transfer before the end of a lifted instruction");
      if (!emitTargets(Draft, Instruction, Original, std::move(Residual), Exec,
                       Ctx, State, FrameRoot, Origins, Frame, Flow))
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
  if (Ctx.numNodes() > Options.MaxSymbolicNodes)
    return fail(SpecializationStatus::BudgetExceeded,
                "specialization symbolic-node budget exhausted");
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
  Result.Status = SpecializationStatus::Complete;
  return true;
}

SpecializationResult Specializer::run() {
  if (Entry.Address == InvalidVA || Entry.Address >= (uint64_t{1} << 63) ||
      (Options.ByteOrder != llvm::endianness::little &&
       Options.ByteOrder != llvm::endianness::big) ||
      !Options.MaxNodes || !Options.MaxContextsPerAddress ||
      !Options.MaxOperations || !Options.MaxNodeEvaluations ||
      !Options.MaxIndirectTargets || !Options.MaxNativeReturnSlots ||
      !Options.MaxImmutableReadAddresses || !Options.MaxControlTuples ||
      !Options.MaxControlFields || !Options.MaxSolverQueries ||
      !Options.MaxSolverGates ||
      Options.MaxSolverGates > std::numeric_limits<size_t>::max() ||
      !Options.MaxSolverConflicts || !Options.MaxSolverPropagations ||
      !Options.MaxSolverWatchVisits || !Options.MaxSymbolicNodes) {
    fail(SpecializationStatus::InvalidInput,
         "invalid specialization entry or budget");
    return std::move(Result);
  }
  if ((Options.NormalNonfaultingExecution || Options.X64CetDisabled) &&
      !Options.ExplicitMachineState) {
    fail(SpecializationStatus::InvalidInput,
         "machine execution preconditions require an explicit machine-state "
         "interface");
    return std::move(Result);
  }
  if (Options.ControlRegisters.size() + Options.ControlFrameSlots.size() >
      Options.MaxControlFields) {
    fail(SpecializationStatus::BudgetExceeded,
         "control projection field budget exhausted");
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
  // Source ABIs do not provide arbitrary incoming arithmetic flag values.
  // A retained PUSHFQ may use them only after reachable native code has
  // defined every modelled flag; must-provenance joins preserve this condition.
  for (uint64_t Flag : X64PushfModelledFlags) {
    setUnsafeOrigin(NdVar::reg(Flag, 1), true, InitialOrigins);
    if (!Options.ExplicitMachineState)
      InitialOrigins.UndefinedFlags.insert(Flag);
  }
  for (const auto &Constant : Options.EntryConstants) {
    const NdVar &Location = Constant.Location;
    if (!Location.isReg() || !validValue(Location)) {
      fail(SpecializationStatus::InvalidInput,
           "entry constants must bind physical registers");
      return std::move(Result);
    }
    constexpr SymSpace Space = SymSpace::Register;
    for (unsigned I = 0; I < Location.Size; ++I)
      if (!Seeded.emplace(Space, Location.Offset + I).second) {
        fail(SpecializationStatus::InvalidInput,
             "overlapping entry constant bindings");
        return std::move(Result);
      }
    Initial.write(Space, Location.Offset,
                  Ctx.mkConst(8 * Location.Size, Constant.Value));
    setUnsafeOrigin(Location, false, InitialOrigins);
    for (uint16_t I = 0; I < Location.Size; ++I)
      InitialOrigins.UndefinedFlags.erase(Location.Offset + I);
  }
  enqueue(Entry,
          project(Initial, FrameRoot, AffineCandidates, InitialOrigins, {}));
  while (!Failed && !Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    Nodes[Id].Pending = false;
    evaluate(Id);
  }
  if (!Failed)
    publish();
  Result.Contexts += static_cast<uint32_t>(Indices.size());
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
  SpecializationOptions Effective = Options;
  ControlRefinement Refinement;
  SpecializationResult Result;
  for (;;) {
    Result =
        Specializer(Provider, Entry, Effective, Options.ControlRegisters.size(),
                    Options.ControlFrameSlots.size(), Refinement,
                    std::move(Result))
            .run();
    Result.DiscoveryVisits = Refinement.Visits;
    if (Result.complete() || !Options.DiscoverControlState ||
        (Result.Status != SpecializationStatus::UnresolvedControl &&
         !Refinement.PrecisionFailure))
      return Result;
    const bool HasCandidates = !Refinement.Registers.empty() ||
                               !Refinement.Slots.empty() ||
                               !Refinement.PendingContextRegisters.empty() ||
                               !Refinement.PendingContextSlots.empty() ||
                               !Refinement.PendingProducerDemands.empty();
    if (Refinement.BudgetExceeded ||
        (HasCandidates &&
         Result.ControlRefinements >= Options.MaxControlRefinements)) {
      Result.Status = SpecializationStatus::BudgetExceeded;
      Result.Diagnostic = "control-state discovery/refinement budget exhausted";
      return Result;
    }
    if (!HasCandidates)
      return Result;
    Effective.ControlRegisters.insert(Effective.ControlRegisters.end(),
                                      Refinement.Registers.begin(),
                                      Refinement.Registers.end());
    Effective.ControlFrameSlots.insert(Effective.ControlFrameSlots.end(),
                                       Refinement.Slots.begin(),
                                       Refinement.Slots.end());
    Result.DiscoveredControlFields +=
        Refinement.Registers.size() + Refinement.Slots.size();
    Refinement.ContextRegisters.insert(
        Refinement.ContextRegisters.end(),
        Refinement.PendingContextRegisters.begin(),
        Refinement.PendingContextRegisters.end());
    Refinement.ContextSlots.insert(Refinement.ContextSlots.end(),
                                   Refinement.PendingContextSlots.begin(),
                                   Refinement.PendingContextSlots.end());
    Result.DiscoveredContextFields +=
        Refinement.PendingContextRegisters.size() +
        Refinement.PendingContextSlots.size();
    Refinement.ProducerDemands.insert(Refinement.PendingProducerDemands.begin(),
                                      Refinement.PendingProducerDemands.end());
    ++Result.ControlRefinements;
    Refinement.PendingProducerDemands.clear();
    Refinement.PendingContextRegisters.clear();
    Refinement.PendingContextSlots.clear();
    Refinement.Registers.clear();
    Refinement.Slots.clear();
    Refinement.PrecisionFailure = false;
    // Top joined with a new relation remains Top: retaining the old graph
    // would never recover the lost precision. Field IDs and all symbolic
    // identities are rebuilt together; failed residuals/witnesses stay empty.
  }
}

} // namespace neverd::analysis
