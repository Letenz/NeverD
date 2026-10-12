//===- SEHFrameState.h - Exact frame and runtime values ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_SEHFRAMESTATE_H
#define NEVERD_IR_LOW_SEHFRAMESTATE_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/Support/MathExtras.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace neverd::seh_frame {

using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
inline Key key(const NdVar &V) { return {V.Space, V.Offset, V.Size}; }

struct Value {
  enum class Kind { Frame, Constant, CRTStream, ImportTarget } Identity;
  uint64_t Bits = 0;
  static Value frame(int64_t Offset) {
    return {Kind::Frame, static_cast<uint64_t>(Offset)};
  }
  static Value constant(uint64_t Bits) { return {Kind::Constant, Bits}; }
  bool operator==(const Value &) const = default;
};

struct State {
  std::map<Key, Value> Values;
  std::map<int64_t, Value> Spills;
  /// A concrete write withdraws the loader's binding for future slot loads
  /// and veneers. Restoring arbitrary bits does not authenticate a provider.
  std::set<va_t> InvalidatedImportSlots;
  /// An opaque call/store may retain an alias to this invocation's frame.
  /// Restoring a saved value later does not recover runtime-object privacy.
  bool EscapedFrame = false;
  bool operator==(const State &) const = default;

  std::optional<Value> value(const NdVar &V) const {
    if (!V.Size || V.Size > 8)
      return std::nullopt;
    if (V.isConst())
      return Value::constant(V.Offset & (UINT64_MAX >> ((8 - V.Size) * 8)));
    if (!V.isReg() && !V.isTemp())
      return std::nullopt;
    const auto It = Values.find(key(V));
    return It == Values.end() ? std::nullopt : std::optional<Value>{It->second};
  }

  std::optional<int64_t> get(const NdVar &V) const {
    const auto Fact = value(V);
    return V.Size == 8 && Fact && Fact->Identity == Value::Kind::Frame
               ? std::optional<int64_t>{static_cast<int64_t>(Fact->Bits)}
               : std::nullopt;
  }

  std::optional<uint64_t> constant(const NdVar &V) const {
    const auto Fact = value(V);
    return Fact && Fact->Identity == Value::Kind::Constant
               ? std::optional<uint64_t>{Fact->Bits}
               : std::nullopt;
  }

  void dropTemporaries() {
    std::erase_if(Values, [](const auto &V) {
      return std::get<0>(V.first) == VnodeSpace::TEMP;
    });
  }

  void intersect(const State &Other) {
    EscapedFrame |= Other.EscapedFrame;
    InvalidatedImportSlots.insert(Other.InvalidatedImportSlots.begin(),
                                  Other.InvalidatedImportSlots.end());
    const auto Missing = [](const auto &Map, const auto &Item) {
      const auto It = Map.find(Item.first);
      return It == Map.end() || It->second != Item.second;
    };
    std::erase_if(Values,
                  [&](const auto &V) { return Missing(Other.Values, V); });
    std::erase_if(Spills,
                  [&](const auto &V) { return Missing(Other.Spills, V); });
  }

  void invalidateMemory(int64_t Start, int64_t End) {
    std::erase_if(Spills, [&](const auto &S) {
      int64_t Limit = 0;
      return llvm::AddOverflow(S.first, int64_t{8}, Limit) ||
             (Start < Limit && S.first < End);
    });
  }
};

} // namespace neverd::seh_frame

#endif
