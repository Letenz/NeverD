//===- RegistrationStateTryLevel.cpp - Checked state writes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds source state stores and preserves untouched bytes of narrow writes.

#include "RegistrationStateSolver.h"

#include <algorithm>

namespace neverd::registration_state {

bool RegistrationStateSolver::transferTryLevelStore(
    size_t I, const LowOp &Op, const FrameValue &Address,
    const FrameValue &Stored, const std::set<int32_t> &Levels, bool Unknown,
    std::set<int32_t> &WrittenLevels) {
  const auto Refuse = [&] {
    if (!KnownCxx)
      Result.SEHStateStoresComplete = false;
    return false;
  };
  const auto Observation = Stores.find(Op.Addr);
  const uint16_t Width = Op.NumInputs == 2 ? Op.Inputs[1].Size : 0;
  if (Op.Opcode != NdOp::STORE || Op.NumInputs != 2 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Address.Offset != Chain.TryLevelOffset || Address.EntryOffset ||
      Address.CallbackAddress || (Width != 1 && Width != 2 && Width != 4) ||
      !Stored.Constant || Observation == Stores.end() ||
      Observation->second.Width != Width ||
      uint32_t(Observation->second.Level) != *Stored.Constant)
    return Refuse();

  std::vector<std::pair<int32_t, int32_t>> Transitions;
  if (Width == 4) {
    if (!validLevel(int32_t(*Stored.Constant)))
      return Refuse();
    WrittenLevels.insert(int32_t(*Stored.Constant));
    if (!Unknown)
      for (int32_t Level : Levels) {
        if (!charge(1))
          return false;
        Transitions.emplace_back(Level, int32_t(*Stored.Constant));
      }
  } else {
    const uint32_t Mask = Width == 1 ? UINT8_MAX : UINT16_MAX;
    if (Unknown || Levels.empty() || *Stored.Constant > Mask)
      return Refuse();
    for (int32_t Level : Levels) {
      if (!charge(1))
        return false;
      const int32_t Written =
          int32_t((uint32_t(Level) & ~Mask) | (*Stored.Constant & Mask));
      if (!validLevel(Written))
        return Refuse();
      WrittenLevels.insert(Written);
      Transitions.emplace_back(Level, Written);
    }
  }
  if (KnownCxx)
    return true;

  const auto Boundary = Boundaries.find(Op.Addr);
  const auto &Block = Function.Blocks[I];
  const size_t OpIndex = &Op - Block.Ops.data();
  // State analysis already checked the decoded extent and the actual lifted
  // address/value. An absent operation index cannot authorize native writes,
  // but does not invalidate that separate semantic transfer.
  if (Boundary == Boundaries.end()) {
    Result.SEHStateStoresComplete = false;
    return true;
  }
  if (Boundary->second.first != Block.Id ||
      Boundary->second.second.Address != Op.Addr ||
      Boundary->second.second.Size > UINT64_MAX - Op.Addr ||
      Boundary->second.second.Address + Boundary->second.second.Size !=
          Observation->second.EndVA)
    return Refuse();
  if (Op.Seq < 0 || !Boundary->second.second.OpCount) {
    Result.SEHStateStoresComplete = false;
    return true;
  }
  if (Boundary->second.second.FirstOp > OpIndex ||
      OpIndex - Boundary->second.second.FirstOp >=
          Boundary->second.second.OpCount ||
      Boundary->second.second.OpCount >
          Block.Ops.size() - Boundary->second.second.FirstOp)
    return Refuse();
  // A full overwrite may repair an unknown source state, but cannot publish
  // native transition authority for the unknown preceding invocation.
  if (Transitions.empty()) {
    Result.SEHStateStoresComplete = false;
    return true;
  }
  RegistrationSEHStateStore Receipt{
      Block.Id, Op.Addr,        Observation->second.EndVA,
      Op.Seq,   uint8_t(Width), *Stored.Constant,
      {}};
  const auto [Found, New] =
      SEHStateStores.emplace(std::make_pair(Op.Addr, Op.Seq), Receipt);
  if (!New) {
    Receipt.Transitions = Found->second.Transitions;
    if (Receipt != Found->second)
      return Refuse();
  }
  auto &Recorded = Found->second.Transitions;
  if (!charge(Recorded.size() + Transitions.size()))
    return false;
  Recorded.insert(Recorded.end(), Transitions.begin(), Transitions.end());
  std::sort(Recorded.begin(), Recorded.end());
  Recorded.erase(std::unique(Recorded.begin(), Recorded.end()), Recorded.end());
  return true;
}

bool RegistrationStateSolver::mergeSEHFrameOutcome(SEHFrameOutcomes &Outcomes,
                                                   FrameState State) {
  const auto Level = State.load(Chain.TryLevelOffset, 4).Constant;
  if (!Level || !validLevel(int32_t(*Level)) ||
      !charge(State.cellCount() + State.OtherRegisterBytes.size() + 10))
    return false;
  const auto [It, New] = Outcomes.emplace(int32_t(*Level), State);
  if (!New)
    It->second.merge(State);
  return true;
}

} // namespace neverd::registration_state
