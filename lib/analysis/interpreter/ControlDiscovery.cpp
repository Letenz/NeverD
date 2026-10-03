//===- ControlDiscovery.cpp - Bounded control dependency discovery -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ControlDiscovery.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace neverd::analysis::detail {

using namespace symbolic;

std::optional<uint64_t> frameRelativeOffset(const SymContext &Ctx, SymRef Value,
                                            SymRef Root) {
  if (!Root || !Value || Ctx.width(Root) != 64 || Ctx.width(Value) != 64)
    return std::nullopt;
  if (Value == Root)
    return 0;
  if (Ctx.op(Value) != SymOp::Add || Ctx.operands(Value).size() != 2)
    return std::nullopt;
  for (unsigned I = 0; I < 2; ++I)
    if (Ctx.operand(Value, I) == Root) {
      const auto Offset = Ctx.operand(Value, 1 - I);
      // This unbudgeted affine helper only reads a single 64-bit word.
      if (Ctx.isConst(Offset) && Ctx.width(Offset) == 64)
        return Ctx.constValue(Offset).getZExtValue();
    }
  return std::nullopt;
}

namespace {

struct Demand {
  SymRef Value;
  uint32_t Low;
  uint32_t Bits;
};

class Discoverer {
public:
  Discoverer(const SymState &State, SymRef Root, uint64_t MaxVisited)
      : State(State), Ctx(State.context()), Root(Root), MaxVisited(MaxVisited) {
  }

  ControlDiscovery run(SymRef Value) {
    if (!valid(Value) ||
        (State.byteOrder() != llvm::endianness::little &&
         State.byteOrder() != llvm::endianness::big) ||
        (Root && (!valid(Root) || Ctx.width(Root) != 64))) {
      unsupported();
      return std::move(Result);
    }
    enqueue(Value, 0, Ctx.width(Value));
    while (!Pending.empty() && !exhausted()) {
      const Demand Current = Pending.pop_back_val();
      visit(Current);
    }
    if (exhausted()) {
      Result.RegisterRanges.clear();
      Result.FrameSlots.clear();
    } else {
      for (const auto &[Range, Mask] : Registers)
        Result.RegisterRanges.push_back({Range.first, Range.second, Mask});
      for (const auto &[Range, Mask] : Slots)
        Result.FrameSlots.push_back({Range.first, Range.second, Mask});
    }
    if (Result.Status == ControlDiscoveryStatus::Complete)
      Result.FrameRootBits = RootBits;
    return std::move(Result);
  }

private:
  bool valid(SymRef Value) const {
    return Value && Value.index() < Ctx.numNodes() && Ctx.width(Value);
  }

  bool exhausted() const {
    return Result.Status == ControlDiscoveryStatus::BudgetExceeded;
  }

  void unsupported() {
    if (!exhausted())
      Result.Status = ControlDiscoveryStatus::UnsupportedOrigin;
  }

  bool charge(uint64_t Count = 1) {
    if (Count > MaxVisited - Result.Visited) {
      Result.Visited = MaxVisited;
      Result.Status = ControlDiscoveryStatus::BudgetExceeded;
      return false;
    }
    Result.Visited += Count;
    return true;
  }

  void enqueue(SymRef Value, uint32_t Low, uint32_t Bits) {
    if (exhausted() || !Bits)
      return;
    if (!valid(Value) || uint64_t(Low) + Bits > Ctx.width(Value)) {
      unsupported();
      return;
    }
    const auto Key = std::make_tuple(Value.index(), Low, Bits);
    if (Seen.contains(Key) || !charge())
      return;
    Seen.insert(Key);
    Pending.push_back({Value, Low, Bits});
  }

  /// Locate the smallest byte range covering a bit slice. byteRangeMask keeps
  /// the exact demanded bits within it; the other bits of a partial byte do
  /// not become dependencies or facts.
  std::optional<std::pair<uint16_t, uint16_t>> byteRange(const Demand &Current,
                                                         uint16_t Bytes) {
    if (!Bytes || uint64_t(Bytes) * 8 != Ctx.width(Current.Value)) {
      unsupported();
      return std::nullopt;
    }
    const uint32_t First = Current.Low / 8;
    const uint32_t End = (uint64_t(Current.Low) + Current.Bits + 7) / 8;
    if (End > Bytes || First >= End || End - First > 8) {
      unsupported();
      return std::nullopt;
    }
    return std::pair<uint16_t, uint16_t>{
        State.byteOrder() == llvm::endianness::little ? First : Bytes - End,
        End - First};
  }

  uint64_t byteRangeMask(const Demand &Current) const {
    // byteRange has already established that the slice fits at most 8 bytes.
    // The selected scalar's least-significant byte is the source byte at
    // floor(Low / 8), regardless of its address in the machine byte order.
    return llvm::APInt::getBitsSet(64, Current.Low % 8,
                                   Current.Low % 8 + Current.Bits)
        .getZExtValue();
  }

  void inspectMemoryInput(const Demand &Current) {
    const auto Inputs = State.memoryInputOrigins(Current.Value);
    for (const auto &Origin : Inputs) {
      if (!charge())
        return;
      if (!valid(Origin.Base)) {
        unsupported();
        continue;
      }
      const auto Range = byteRange(Current, Origin.Bytes);
      if (Origin.Base == Root && Range &&
          Origin.Offset <=
              std::numeric_limits<uint64_t>::max() - (Origin.Bytes - 1)) {
        Slots[{static_cast<int64_t>(Origin.Offset + Range->first),
               Range->second}] |= byteRangeMask(Current);
      } else {
        unsupported();
        enqueue(Origin.Base, 0, Ctx.width(Origin.Base));
      }
    }
    if (!Inputs.empty())
      return;

    // A later memory epoch is not the node's entry memory. Historical loads
    // may still explain its address, but never nominate their storage as an
    // input. In particular, spilling a value does not change its birthplace.
    unsupported();
    for (const auto &Origin : State.loadOrigins(Current.Value)) {
      if (!charge())
        return;
      if (valid(Origin.Address))
        enqueue(Origin.Address, 0, Ctx.width(Origin.Address));
    }
  }

  bool sameWidth(const Demand &Current, llvm::ArrayRef<SymRef> Operands) {
    for (SymRef Operand : Operands)
      if (!valid(Operand) || Ctx.width(Operand) != Ctx.width(Current.Value)) {
        unsupported();
        return false;
      }
    return true;
  }

  void fullOperands(llvm::ArrayRef<SymRef> Operands) {
    for (SymRef Operand : Operands) {
      enqueue(Operand, 0, Ctx.width(Operand));
      if (exhausted())
        return;
    }
  }

  void leftShift(SymRef Operand, const Demand &Current, uint32_t Shift) {
    const uint64_t End = uint64_t(Current.Low) + Current.Bits;
    const uint64_t Low = std::max(uint64_t(Current.Low), uint64_t(Shift));
    if (Low < End)
      enqueue(Operand, Low - Shift, End - Low);
  }

  /// A shallow overapproximation for a word-sized operand. Bits omitted from
  /// the mask are structurally zero for every input; unknown syntax keeps all
  /// bits possible. Each inspected node/operand costs discovery work.
  uint64_t possibleOneBits(SymRef Value) {
    const uint32_t Width = Ctx.width(Value);
    const uint64_t All = UINT64_MAX >> (64 - Width);
    if (!charge())
      return All;
    if (Ctx.isConst(Value))
      return Ctx.constValue(Value).getZExtValue();
    const auto Operands = Ctx.operands(Value);
    switch (Ctx.op(Value)) {
    case SymOp::And: {
      uint64_t Mask = All;
      for (auto Operand : Operands) {
        if (!charge() || !valid(Operand) || Ctx.width(Operand) != Width)
          return All;
        if (Ctx.isConst(Operand))
          Mask &= Ctx.constValue(Operand).getZExtValue();
      }
      return Mask;
    }
    case SymOp::Mul: {
      uint32_t Shift = 0;
      for (auto Operand : Operands) {
        if (!charge() || !valid(Operand) || Ctx.width(Operand) != Width)
          return All;
        if (Ctx.isConst(Operand))
          Shift =
              std::min(Width, Shift + Ctx.constValue(Operand).countr_zero());
      }
      return Shift >= Width ? 0 : (All << Shift) & All;
    }
    case SymOp::ZExt:
      if (Operands.size() == 1 && charge() && valid(Operands.front()) &&
          Ctx.width(Operands.front()) < Width)
        return UINT64_MAX >> (64 - Ctx.width(Operands.front()));
      return All;
    case SymOp::Shl:
    case SymOp::LShr:
      if (Operands.size() != 2 || !charge(2) || !valid(Operands[0]) ||
          !valid(Operands[1]) || Ctx.width(Operands[0]) != Width ||
          Ctx.width(Operands[1]) > 64 || !Ctx.isConst(Operands[1]))
        return All;
      if (const uint64_t Shift = Ctx.constValue(Operands[1]).getZExtValue();
          Shift < Width)
        return Ctx.op(Value) == SymOp::Shl ? (All << Shift) & All
                                           : All >> Shift;
      return 0;
    default:
      return All;
    }
  }

  bool carryFreeSum(llvm::ArrayRef<SymRef> Operands) {
    uint64_t Occupied = 0;
    for (auto Operand : Operands) {
      const uint64_t Bits = possibleOneBits(Operand);
      if (exhausted() || (Occupied & Bits))
        return false;
      Occupied |= Bits;
    }
    return true;
  }

  void visit(const Demand &Current) {
    if (Ctx.isConst(Current.Value))
      return;
    if (Current.Value == Root) {
      RootBits |=
          llvm::APInt::getBitsSet(64, Current.Low, Current.Low + Current.Bits)
              .getZExtValue();
      return;
    }

    const auto Operands = Ctx.operands(Current.Value);
    switch (Ctx.op(Current.Value)) {
    case SymOp::Var: {
      const auto &Info = Ctx.varInfo(Ctx.varId(Current.Value));
      if (!Info.Fresh && Info.InputOrigin &&
          Info.InputOrigin->Kind == SymInputKind::Register &&
          Info.InputOrigin->Epoch == 0) {
        const auto &Origin = *Info.InputOrigin;
        const auto Range = byteRange(Current, Origin.Bytes);
        if (Range && Origin.Offset <= std::numeric_limits<uint64_t>::max() -
                                          (Origin.Bytes - 1))
          Registers[{Origin.Offset + Range->first, Range->second}] |=
              byteRangeMask(Current);
        else
          unsupported();
      } else
        inspectMemoryInput(Current);
      return;
    }
    case SymOp::And:
    case SymOp::Or: {
      if (!sameWidth(Current, Operands))
        return;
      if (std::none_of(Operands.begin(), Operands.end(),
                       [&](SymRef V) { return Ctx.isConst(V); })) {
        for (SymRef Operand : Operands) {
          enqueue(Operand, Current.Low, Current.Bits);
          if (exhausted())
            return;
        }
        return;
      }
      // A constant zero in AND, or one in OR, kills that output bit's
      // dependence on every other operand. Keep disjoint live runs separate.
      // Charge mask construction and each scan in 64-bit words, so sparse or
      // very wide masks cannot perform unbounded work outside the DAG budget.
      const uint64_t Words = (uint64_t(Current.Bits) + 63) / 64;
      if (!charge(Words))
        return;
      llvm::APInt Live = llvm::APInt::getAllOnes(Current.Bits);
      for (SymRef Operand : Operands)
        if (Ctx.isConst(Operand)) {
          // constValue copies the entire payload, even for a one-bit slice.
          if (!charge((uint64_t(Ctx.width(Operand)) + 63) / 64))
            return;
          const auto Constant = Ctx.constValue(Operand);
          const auto Slice = Constant.extractBits(Current.Bits, Current.Low);
          Live &= Ctx.op(Current.Value) == SymOp::And ? Slice : ~Slice;
        }
      while (!Live.isZero()) {
        if (!charge(Words))
          return;
        const uint32_t Low = Live.countr_zero();
        const uint32_t Bits = Live.lshr(Low).countr_one();
        for (SymRef Operand : Operands) {
          if (!Ctx.isConst(Operand))
            enqueue(Operand, Current.Low + Low, Bits);
          if (exhausted())
            return;
        }
        Live.clearBits(Low, Low + Bits);
      }
      return;
    }
    case SymOp::Xor:
    case SymOp::Not:
      if (!sameWidth(Current, Operands) ||
          (Ctx.op(Current.Value) == SymOp::Not && Operands.size() != 1)) {
        unsupported();
        return;
      }
      for (SymRef Operand : Operands) {
        if (!Ctx.isConst(Operand))
          enqueue(Operand, Current.Low, Current.Bits);
        if (exhausted())
          return;
      }
      return;
    case SymOp::Add:
    case SymOp::Mul: {
      if (!sameWidth(Current, Operands))
        return;
      // Pairwise-disjoint possible-one bits prove that this sum cannot carry
      // at any position. Its selected upper slice then depends only on those
      // same operand bits, as with OR. Otherwise retain the full low prefix.
      // No solver premise or sampled value is used to omit dependencies.
      if (Ctx.op(Current.Value) == SymOp::Add && Current.Low &&
          Ctx.width(Current.Value) <= 64 && carryFreeSum(Operands)) {
        for (auto Operand : Operands) {
          if (!Ctx.isConst(Operand))
            enqueue(Operand, Current.Low, Current.Bits);
          if (exhausted())
            return;
        }
        return;
      }
      // Modulo 2^N arithmetic cannot carry from a higher bit into a lower
      // one. Subtraction and negation are canonical Add/Mul nodes as well.
      uint32_t End = Current.Low + Current.Bits;
      if (Ctx.op(Current.Value) == SymOp::Mul) {
        const uint32_t Width = Ctx.width(Current.Value);
        uint32_t Shift = 0;
        bool PowerOfTwo = true;
        llvm::SmallVector<SymRef, 4> Inputs;
        for (SymRef Operand : Operands)
          if (Ctx.isConst(Operand)) {
            if (!charge((uint64_t(Ctx.width(Operand)) + 63) / 64))
              return;
            const auto Constant = Ctx.constValue(Operand);
            Shift = std::min(uint64_t(Width),
                             uint64_t(Shift) + Constant.countr_zero());
            PowerOfTwo &= Constant.isPowerOf2();
          } else
            Inputs.push_back(Operand);
        if (Shift >= End)
          return;
        // SymContext represents a constant left shift as multiplication by a
        // power of two. Preserve the exact shifted slice for this shape.
        if (Inputs.size() == 1 && PowerOfTwo) {
          leftShift(Inputs.front(), Current, Shift);
          return;
        }
        End -= Shift;
      }
      for (SymRef Operand : Operands) {
        if (!Ctx.isConst(Operand))
          enqueue(Operand, 0, End);
        if (exhausted())
          return;
      }
      return;
    }
    case SymOp::Shl:
    case SymOp::LShr:
    case SymOp::AShr: {
      if (Operands.size() != 2 ||
          Ctx.width(Operands.front()) != Ctx.width(Current.Value)) {
        unsupported();
        return;
      }
      if (!Ctx.isConst(Operands[1])) {
        fullOperands(Operands);
        return;
      }
      if (!charge((uint64_t(Ctx.width(Operands[1])) + 63) / 64))
        return;
      const auto Amount = Ctx.constValue(Operands[1]);
      const uint32_t Width = Ctx.width(Current.Value);
      const bool Arithmetic = Ctx.op(Current.Value) == SymOp::AShr;
      if (Amount.uge(Width)) {
        if (Arithmetic)
          enqueue(Operands.front(), Width - 1, 1);
        return;
      }
      const uint32_t Shift = Amount.getZExtValue();
      if (Ctx.op(Current.Value) == SymOp::Shl) {
        leftShift(Operands.front(), Current, Shift);
        return;
      }
      const uint64_t Low = uint64_t(Current.Low) + Shift;
      const uint64_t End = Low + Current.Bits;
      if (Low < Width)
        enqueue(Operands.front(), Low, std::min(End, uint64_t(Width)) - Low);
      if (Arithmetic && End > Width)
        enqueue(Operands.front(), Width - 1, 1);
      return;
    }
    case SymOp::Extract:
      if (Operands.size() != 1 ||
          Ctx.node(Current.Value).Aux >
              std::numeric_limits<uint32_t>::max() - uint64_t(Current.Low)) {
        unsupported();
        return;
      }
      enqueue(Operands.front(), Ctx.node(Current.Value).Aux + Current.Low,
              Current.Bits);
      return;
    case SymOp::Concat: {
      uint64_t PartLow = 0;
      for (auto It = Operands.rbegin(); It != Operands.rend(); ++It) {
        const uint64_t PartHigh = PartLow + Ctx.width(*It);
        const uint64_t Low = std::max(uint64_t(Current.Low), PartLow);
        const uint64_t High =
            std::min(uint64_t(Current.Low) + Current.Bits, PartHigh);
        if (Low < High)
          enqueue(*It, Low - PartLow, High - Low);
        PartLow = PartHigh;
        if (exhausted())
          return;
      }
      if (PartLow != Ctx.width(Current.Value))
        unsupported();
      return;
    }
    case SymOp::ZExt:
    case SymOp::SExt: {
      if (Operands.size() != 1 ||
          Ctx.width(Operands.front()) >= Ctx.width(Current.Value)) {
        unsupported();
        return;
      }
      const uint32_t InnerBits = Ctx.width(Operands.front());
      const uint64_t End = uint64_t(Current.Low) + Current.Bits;
      if (Current.Low < InnerBits)
        enqueue(Operands.front(), Current.Low,
                std::min(End, uint64_t(InnerBits)) - Current.Low);
      if (Ctx.op(Current.Value) == SymOp::SExt && End > InnerBits)
        enqueue(Operands.front(), InnerBits - 1, 1);
      return;
    }
    case SymOp::Ite:
      if (Operands.size() != 3 || Ctx.width(Operands[0]) != 1 ||
          Ctx.width(Operands[1]) != Ctx.width(Current.Value) ||
          Ctx.width(Operands[2]) != Ctx.width(Current.Value)) {
        unsupported();
        return;
      }
      enqueue(Operands[0], 0, 1);
      enqueue(Operands[1], Current.Low, Current.Bits);
      enqueue(Operands[2], Current.Low, Current.Bits);
      return;
    default:
      // Unknown arithmetic dependency is conservatively widened to complete
      // operands. Bit dependence never certifies values or pointer facts.
      fullOperands(Operands);
      return;
    }
  }

  const SymState &State;
  const SymContext &Ctx;
  SymRef Root;
  uint64_t MaxVisited;
  uint64_t RootBits = 0;
  ControlDiscovery Result;
  llvm::SmallVector<Demand, 16> Pending;
  std::set<std::tuple<uint32_t, uint32_t, uint32_t>> Seen;
  std::map<std::pair<uint64_t, uint16_t>, uint64_t> Registers;
  std::map<std::pair<int64_t, uint16_t>, uint64_t> Slots;
};

} // namespace

ControlDiscovery gatherControlDependencies(const SymState &State, SymRef Value,
                                           SymRef FrameRoot,
                                           uint64_t MaxVisited) {
  return Discoverer(State, FrameRoot, MaxVisited).run(Value);
}

bool frameRootDomainExceedsLimit(
    const SymState &State, SymRef Predicate, SymRef FrameRoot,
    const std::optional<SpecializationEntryFrameBounds> &Bounds, uint32_t Limit,
    uint64_t MaxVisited) {
  const SymContext &Ctx = State.context();
  uint64_t Remaining = MaxVisited;
  const auto Charge = [&](uint64_t Count = 1) {
    if (Count > Remaining)
      return false;
    Remaining -= Count;
    return true;
  };
  const auto Valid = [&](SymRef Value, uint32_t Width) {
    return Value && Value.index() < Ctx.numNodes() && Ctx.width(Value) == Width;
  };
  if (!Limit || !Charge() || !Valid(FrameRoot, 64) || !Ctx.isVar(FrameRoot) ||
      Ctx.node(FrameRoot).Aux >= Ctx.numVars() ||
      !Ctx.varInfo(Ctx.varId(FrameRoot)).Fresh ||
      Ctx.varInfo(Ctx.varId(FrameRoot)).Width != 64 || !Valid(Predicate, 1) ||
      (Bounds && Bounds->Begin >= Bounds->End))
    return false;

  const uint64_t Lower =
      Bounds && Bounds->Begin < 0 ? uint64_t{0} - uint64_t(Bounds->Begin) : 0;
  const uint64_t Upper = Bounds && Bounds->End > 0
                             ? UINT64_MAX - uint64_t(Bounds->End - 1)
                             : UINT64_MAX;
  const uint64_t Span = Upper - Lower;
  // Every fixed low32 residue occurs at least floor((Span + 1) / 2^32)
  // times in [Lower, Upper]. Do not overflow at the full 64-bit domain or
  // narrow its 2^32-value lower bound to the uint32_t enumeration limit.
  const uint64_t Guaranteed =
      (Span >> 32) + ((Span & UINT32_MAX) == UINT32_MAX);
  if (Guaranteed <= Limit)
    return false;

  llvm::SmallVector<SymRef, 8> Pending{Predicate};
  while (!Pending.empty()) {
    const SymRef Clause = Pending.pop_back_val();
    if (!Charge() || !Valid(Clause, 1))
      return false;
    const auto Operands = Ctx.operands(Clause);
    if (Ctx.op(Clause) == SymOp::And) {
      if (Operands.empty())
        return false;
      for (SymRef Operand : Operands) {
        if (!Charge() || !Valid(Operand, 1))
          return false;
        Pending.push_back(Operand);
      }
      continue;
    }
    // Remove only exact top-level conjuncts of the declared interval. In
    // particular, stricter bounds and bounds beneath OR/NOT stay in the
    // dependency walk; omitting them could turn a singleton into a large set.
    if (Bounds && Ctx.op(Clause) == SymOp::Ule) {
      if (Operands.size() != 2 || !Charge(2))
        return false;
      const bool LowerBound =
          Operands[1] == FrameRoot && Valid(Operands[0], 64);
      const bool UpperBound =
          Operands[0] == FrameRoot && Valid(Operands[1], 64);
      if (LowerBound || UpperBound) {
        const auto Constant = Operands[LowerBound ? 0 : 1];
        if (Ctx.isConst(Constant)) {
          if (!Charge())
            return false;
          if (Ctx.constValue(Constant).getZExtValue() ==
              (LowerBound ? Lower : Upper))
            continue;
        }
      }
    }
    const auto Dependencies =
        gatherControlDependencies(State, Clause, FrameRoot, Remaining);
    if (!Charge(Dependencies.Visited) ||
        Dependencies.Status != ControlDiscoveryStatus::Complete ||
        !Dependencies.FrameRootBits || (*Dependencies.FrameRootBits >> 32))
      return false;
  }
  return true;
}

} // namespace neverd::analysis::detail
