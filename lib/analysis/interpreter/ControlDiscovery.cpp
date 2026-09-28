//===- ControlDiscovery.cpp - Bounded control dependency discovery -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ControlDiscovery.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>
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
    if (Ctx.operand(Value, I) == Root)
      if (const auto Offset = Ctx.asConst(Ctx.operand(Value, 1 - I)))
        return Offset->getZExtValue();
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
      for (const auto &[Offset, Bytes] : Registers)
        Result.RegisterRanges.push_back({Offset, Bytes});
      for (const auto &[Offset, Bytes] : Slots)
        Result.FrameSlots.push_back({Offset, Bytes});
    }
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

  bool charge() {
    if (Result.Visited >= MaxVisited) {
      Result.Status = ControlDiscoveryStatus::BudgetExceeded;
      return false;
    }
    ++Result.Visited;
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

  /// Cover the demanded bits with exact byte lanes, conservatively including
  /// the other bits of a partially demanded byte. This never widens a one-byte
  /// demand into the unknown high bytes of a word.
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

  bool inspectLoads(const Demand &Current) {
    bool FrameSource = false;
    for (const auto &Origin : State.loadOrigins(Current.Value)) {
      if (!charge())
        break;
      if (!valid(Origin.Address)) {
        unsupported();
        continue;
      }
      const auto Offset = frameRelativeOffset(Ctx, Origin.Address, Root);
      const auto Range = byteRange(Current, Origin.Bytes);
      if (Offset && Range &&
          *Offset <=
              std::numeric_limits<uint64_t>::max() - (Origin.Bytes - 1)) {
        Slots.emplace(static_cast<int64_t>(*Offset + Range->first),
                      Range->second);
        FrameSource = true;
      } else {
        // An unresolved loaded value has no replayable external-memory field.
        // Its address may still explain which entry control inputs were lost.
        unsupported();
        enqueue(Origin.Address, 0, Ctx.width(Origin.Address));
      }
    }
    return FrameSource;
  }

  void visit(const Demand &Current) {
    if (Ctx.isConst(Current.Value))
      return;
    const bool FrameSource = inspectLoads(Current);
    if (exhausted())
      return;

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
          Registers.emplace(Origin.Offset + Range->first, Range->second);
        else
          unsupported();
      } else if (!FrameSource) {
        unsupported();
      }
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
      // operands. We do not infer finite values, pointer provenance, or facts
      // from bit masks while discovering candidate locations.
      for (SymRef Operand : Operands) {
        enqueue(Operand, 0, Ctx.width(Operand));
        if (exhausted())
          return;
      }
      return;
    }
  }

  const SymState &State;
  const SymContext &Ctx;
  SymRef Root;
  uint64_t MaxVisited;
  ControlDiscovery Result;
  llvm::SmallVector<Demand, 16> Pending;
  std::set<std::tuple<uint32_t, uint32_t, uint32_t>> Seen;
  std::set<std::pair<uint64_t, uint16_t>> Registers;
  std::set<std::pair<int64_t, uint16_t>> Slots;
};

} // namespace

ControlDiscovery gatherControlDependencies(const SymState &State, SymRef Value,
                                           SymRef FrameRoot,
                                           uint64_t MaxVisited) {
  return Discoverer(State, FrameRoot, MaxVisited).run(Value);
}

} // namespace neverd::analysis::detail
