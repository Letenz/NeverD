//===- AffineFrameState.h - Anchored affine frame equations -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_AFFINEFRAMESTATE_H
#define NEVERD_IR_AFFINEFRAMESTATE_H

#include "neverd/ir/Limits.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::detail {

/// Signed frame-coordinate arithmetic never wraps, even if a later transfer
/// would cancel the overflowing intermediate offset.
inline std::optional<int64_t> checkedAffineOffset(int64_t Base, int64_t Delta,
                                                  bool Subtract = false) {
  constexpr int64_t Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t Max = std::numeric_limits<int64_t>::max();
  if (!Subtract) {
    if ((Delta > 0 && Base > Max - Delta) || (Delta < 0 && Base < Min - Delta))
      return std::nullopt;
    return Base + Delta;
  }
  if ((Delta > 0 && Base < Min + Delta) || (Delta < 0 && Base > Max + Delta))
    return std::nullopt;
  return Base - Delta;
}

/// Must-equality equations for frame coordinates. A coordinate is an offset
/// from an anchor: anchor 0 is the incoming stack pointer, and every other
/// anchor is the value one alignment (`and sp, -2^k`) produced from an exact
/// coordinate. Two anchors are a distance apart that is known only within
/// that alignment, so coordinates of different anchors are never equal;
/// anchorBounds() gives the incoming-SP offsets an anchor may lie at.
/// A node is equal to every incoming value, optionally after a checked offset
/// or an alignment. References allow cyclic definitions without recursively
/// re-expanding them. Each state changes at most twice: Unknown -> Value ->
/// Invalid. Unanchored cycles are invalidated after propagation, including
/// their downstream users.
class AffineFrameState {
public:
  enum class Kind : uint8_t { Invalid, Value, Reference };
  struct Result {
    Kind K = Kind::Invalid;
    int64_t Offset = 0;
    size_t Node = 0;
    uint32_t Anchor = 0;
  };

  /// The largest alignment that defines an anchor.
  static constexpr uint64_t MaxAlignment =
      ::neverd::limits::ir::kAffineFrameStateMaxAlignment;

  explicit AffineFrameState(llvm::function_ref<bool(size_t)> Consume)
      : Consume(Consume) {}

  static Result invalid() { return {}; }
  static Result constant(int64_t Offset) { return {Kind::Value, Offset}; }
  static Result anchored(uint32_t Anchor, int64_t Offset) {
    return {Kind::Value, Offset, 0, Anchor};
  }

  Result reference() {
    // Node storage, initialization and eventual destruction, including the
    // inlined reverse-edge vector. Its heap growth is charged per edge below.
    if (!Consume(16))
      return invalid();
    const size_t Index = Nodes.size();
    Nodes.emplace_back();
    return {Kind::Reference, 0, Index};
  }

  bool define(Result Target, Result Input) {
    return defineInputs(Target, {Input});
  }

  Result merge(llvm::ArrayRef<Result> Inputs) {
    if (!Consume(Inputs.size()) || Inputs.empty())
      return invalid();
    bool HasReference = false;
    std::optional<std::pair<uint32_t, int64_t>> Common;
    for (const Result &Input : Inputs) {
      if (Input.K == Kind::Invalid)
        return invalid();
      if (Input.K == Kind::Reference) {
        HasReference = true;
      } else if (Common && *Common != std::pair(Input.Anchor, Input.Offset)) {
        return invalid();
      } else {
        Common = std::pair(Input.Anchor, Input.Offset);
      }
    }
    if (!HasReference)
      return anchored(Common->first, Common->second);
    if (Inputs.size() == 1)
      return Inputs.front();
    const Result Target = reference();
    return defineInputs(Target, Inputs) ? Target : invalid();
  }

  Result adjust(Result Input, int64_t Delta, bool Subtract) {
    if (Input.K == Kind::Invalid)
      return invalid();
    if (Input.K == Kind::Value) {
      const auto Offset = checkedAffineOffset(Input.Offset, Delta, Subtract);
      return Offset ? anchored(Input.Anchor, *Offset) : invalid();
    }
    // Retain each arithmetic step: combining deltas could hide an overflowing
    // intermediate frame epoch, even if the final sum fits.
    const Result Target = reference();
    return defineInputs(Target, {Input}, Delta, Subtract) ? Target : invalid();
  }

  /// `Input & -Alignment` for a power-of-two Alignment of at least two.
  Result align(Result Input, uint64_t Alignment) {
    if (Input.K == Kind::Invalid || Alignment < 2 || Alignment > MaxAlignment ||
        !std::has_single_bit(Alignment))
      return invalid();
    if (Input.K == Kind::Value) {
      State Aligned;
      if (!step({StateKind::Value, Input.Offset, Input.Anchor},
                {0, 0, false, Alignment}, Aligned) ||
          Aligned.K != StateKind::Value)
        return invalid();
      return anchored(Aligned.Anchor, Aligned.Offset);
    }
    const Result Target = reference();
    return defineInputs(Target, {Input}, 0, false, Alignment) ? Target
                                                              : invalid();
  }

  Result value(Result Input) {
    if (Input.K != Kind::Reference)
      return Input;
    if (Input.Node >= SolvedCount && !solve())
      return invalid();
    const State &S = Nodes[Input.Node].Current;
    return S.K == StateKind::Value ? anchored(S.Anchor, S.Offset) : invalid();
  }

  /// The incoming-SP offsets, inclusive, that \p Anchor may lie at.
  std::optional<std::pair<int64_t, int64_t>>
  anchorBounds(uint32_t Anchor) const {
    int64_t Low = 0;
    int64_t High = 0;
    // Each anchor is interned after the anchor of its origin, so the chain
    // strictly descends to the incoming stack pointer.
    while (Anchor != 0) {
      if (Anchor > Anchors.size())
        return std::nullopt;
      const Origin &From = Anchors[Anchor - 1];
      const auto NewHigh = checkedAffineOffset(High, From.Offset);
      const auto Shifted = checkedAffineOffset(Low, From.Offset);
      const auto NewLow =
          Shifted
              ? checkedAffineOffset(*Shifted,
                                    static_cast<int64_t>(From.Alignment - 1),
                                    /*Subtract=*/true)
              : std::nullopt;
      if (!NewHigh || !NewLow)
        return std::nullopt;
      Low = *NewLow;
      High = *NewHigh;
      Anchor = From.Anchor;
    }
    return std::pair(Low, High);
  }

  void clear() {
    Nodes.clear();
    Pending.clear();
    Anchors.clear();
    SolvedCount = 0;
  }

private:
  enum class StateKind : uint8_t { Unknown, Value, Invalid };
  struct State {
    StateKind K = StateKind::Unknown;
    int64_t Offset = 0;
    uint32_t Anchor = 0;
  };
  /// An edge applies Delta, or aligns its source when Alignment is nonzero.
  struct User {
    size_t Node;
    int64_t Delta;
    bool Subtract;
    uint64_t Alignment = 0;
  };
  struct Node {
    llvm::SmallVector<User, 2> Users;
    State Initial, Current;
    bool Defined = false;
    bool Queued = false;
  };
  struct PendingEdge {
    size_t Source;
    User Target;
  };
  /// The exact coordinate an anchor aligned, and the alignment.
  struct Origin {
    uint32_t Anchor;
    int64_t Offset;
    uint64_t Alignment;
    bool operator==(const Origin &) const = default;
  };

  static bool meet(State &Target, State Input) {
    if (Input.K == StateKind::Unknown || Target.K == StateKind::Invalid)
      return false;
    if (Target.K == StateKind::Unknown) {
      Target = Input;
      return true;
    }
    if (Input.K == StateKind::Invalid || Target.Offset != Input.Offset ||
        Target.Anchor != Input.Anchor) {
      Target = {StateKind::Invalid};
      return true;
    }
    return false;
  }

  /// The state \p U gives a \p Source value. False only when the work budget
  /// is exhausted.
  bool step(State Source, const User &U, State &Out) {
    Out = {StateKind::Invalid};
    if (Source.K != StateKind::Value)
      return true;
    if (!U.Alignment) {
      if (const auto Offset =
              checkedAffineOffset(Source.Offset, U.Delta, U.Subtract))
        Out = {StateKind::Value, *Offset, Source.Anchor};
      return true;
    }
    // An anchor at least as aligned is a multiple of the alignment, which
    // commutes with it: (A + off) & -a == A + (off & -a).
    if (Source.Anchor != 0 && Source.Anchor <= Anchors.size() &&
        Anchors[Source.Anchor - 1].Alignment >= U.Alignment) {
      Out = {StateKind::Value,
             Source.Offset & -static_cast<int64_t>(U.Alignment), Source.Anchor};
      return true;
    }
    const Origin From{Source.Anchor, Source.Offset, U.Alignment};
    if (!Consume(Anchors.size() + 1))
      return false;
    for (size_t I = 0; I < Anchors.size(); ++I)
      if (Anchors[I] == From) {
        Out = {StateKind::Value, 0, static_cast<uint32_t>(I + 1)};
        return true;
      }
    if (Anchors.size() >= std::numeric_limits<uint32_t>::max())
      return true;
    Anchors.push_back(From);
    Out = {StateKind::Value, 0, static_cast<uint32_t>(Anchors.size())};
    return true;
  }

  bool defineInputs(Result Target, llvm::ArrayRef<Result> Inputs,
                    int64_t Delta = 0, bool Subtract = false,
                    uint64_t Alignment = 0) {
    if (Target.K != Kind::Reference)
      return false;
    assert(Target.Node < Nodes.size() && !Nodes[Target.Node].Defined);
    Nodes[Target.Node].Defined = true;
    if (Target.Node < SolvedCount) {
      // A previously queried placeholder acquired its definition. Rebuild
      // the old users too; a normal completed graph grows only downstream.
      SolvedCount = 0;
      Pending.clear();
    }
    const User U{Target.Node, Delta, Subtract, Alignment};
    for (Result Input : Inputs) {
      if (!Consume(12))
        return false;
      if (Input.K == Kind::Reference) {
        Nodes[Input.Node].Users.push_back(U);
        if (Input.Node < SolvedCount)
          Pending.push_back({Input.Node, U});
      } else {
        State Incoming;
        if (!step(Input.K == Kind::Value
                      ? State{StateKind::Value, Input.Offset, Input.Anchor}
                      : State{StateKind::Invalid},
                  U, Incoming))
          return false;
        meet(Nodes[Target.Node].Initial, Incoming);
      }
    }
    return true;
  }

  bool solve() {
    const size_t Count = Nodes.size() - SolvedCount;
    if (Count > std::numeric_limits<size_t>::max() / 8 || !Consume(Count * 8))
      return false;
    std::vector<size_t> Queue;
    Queue.reserve(Count * 2);
    const auto enqueue = [&](size_t Index) {
      if (!Nodes[Index].Queued) {
        Nodes[Index].Queued = true;
        Queue.push_back(Index);
      }
    };
    for (size_t I = SolvedCount; I < Nodes.size(); ++I) {
      Node &N = Nodes[I];
      N.Queued = false;
      N.Current = N.Initial;
      if (N.Current.K != StateKind::Unknown)
        enqueue(I);
    }
    const auto propagateEdge = [&](State Source, const User &U) {
      if (!Consume(4))
        return false;
      State Incoming;
      if (!step(Source, U, Incoming))
        return false;
      if (meet(Nodes[U.Node].Current, Incoming))
        enqueue(U.Node);
      return true;
    };
    // Existing equations are immutable. New queries may depend on their
    // results but cannot change them; replay only newly added outgoing edges.
    for (const PendingEdge &P : Pending)
      if (!propagateEdge(Nodes[P.Source].Current, P.Target))
        return false;
    size_t Next = 0;
    const auto propagate = [&] {
      while (Next < Queue.size()) {
        const size_t Index = Queue[Next++];
        Node &N = Nodes[Index];
        N.Queued = false;
        const State Source = N.Current;
        for (const User &U : N.Users)
          if (!propagateEdge(Source, U))
            return false;
      }
      return true;
    };
    if (!propagate())
      return false;
    // A value from another predecessor cannot authenticate an unanchored
    // incoming cycle. Make these unknown inputs explicit and propagate them.
    for (size_t I = SolvedCount; I < Nodes.size(); ++I)
      if (Nodes[I].Current.K == StateKind::Unknown) {
        Nodes[I].Current = {StateKind::Invalid};
        enqueue(I);
      }
    if (!propagate())
      return false;
    Pending.clear();
    SolvedCount = Nodes.size();
    return true;
  }

  llvm::function_ref<bool(size_t)> Consume;
  std::vector<Node> Nodes;
  std::vector<PendingEdge> Pending;
  std::vector<Origin> Anchors;
  size_t SolvedCount = 0;
};

} // namespace neverd::detail

#endif
