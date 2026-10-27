//===- RegistrationSEHRuntimeReturn.cpp - Unobserved finally results -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Proves a closed runtime-only finally body before ignoring its scalar return.

#include "RegistrationSEHRuntimeReturn.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"

#include <map>
#include <vector>

namespace neverd {

std::optional<std::set<const MedOp *>>
getRuntimeSEHFinallyReturns(const MedFunc &Function, const MedOp &Call,
                            va_t Entry, size_t &Work) {
  auto Charge = [&] {
    if (Work >= limits::kMaxRegistrationEHStateWork)
      return false;
    ++Work;
    return true;
  };
  if (!Entry || Function.SkippedSSA || !Function.ExceptionMetadata ||
      !Function.RegistrationStates)
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  const auto &States = *Function.RegistrationStates;
  if (!EH.Registration || EH.Cxx ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      (EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 &&
       EH.Encoding != ExceptionEncoding::X86ScopeTableEH4) ||
      !States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete ||
      !States.SEHInvocationExitsComplete)
    return std::nullopt;
  bool Finally = false;
  std::set<va_t> OtherEntries{Function.Entry};
  for (const auto &Scope : EH.Registration->Scopes) {
    if (!Charge())
      return std::nullopt;
    if (Scope.IsFinally && Scope.HandlerVA == Entry)
      Finally = true;
    else {
      OtherEntries.insert(Scope.HandlerVA);
      if (!Scope.IsFinally)
        OtherEntries.insert(Scope.FilterVA);
    }
  }
  if (!Finally || OtherEntries.count(Entry))
    return std::nullopt;
  std::map<int, const MedBlock *> Blocks;
  std::map<int, const RegistrationBlockState *> Reaching;
  const MedBlock *Root = nullptr;
  for (const auto &Block : Function.Blocks) {
    if (!Charge() || !Blocks.emplace(Block.Id, &Block).second)
      return std::nullopt;
    if (Block.StartAddr == Entry) {
      if (Root)
        return std::nullopt;
      Root = &Block;
    }
  }
  for (const auto &State : States.Blocks)
    if (!Charge() || !Reaching.emplace(State.BlockId, &State).second)
      return std::nullopt;
  if (!Root)
    return std::nullopt;
  std::vector<int> Pending{Root->Id};
  std::set<int> Body;
  std::set<const MedOp *> Returns;
  bool ContainsCall = false;
  while (!Pending.empty()) {
    const int Id = Pending.back();
    Pending.pop_back();
    if (!Charge())
      return std::nullopt;
    if (!Body.insert(Id).second)
      continue;
    const auto Found = Blocks.find(Id);
    const auto State = Reaching.find(Id);
    if (Found == Blocks.end() || State == Reaching.end())
      return std::nullopt;
    const auto &Block = *Found->second;
    if (!State->second->Reached || State->second->Unknown ||
        !State->second->CallbackOnly || !State->second->CanDispatch ||
        State->second->Range.Begin != Block.StartAddr ||
        State->second->Range.End != Block.EndAddr ||
        Block.StartAddr >= Block.EndAddr ||
        OtherEntries.count(Block.StartAddr) || Block.Ops.empty())
      return std::nullopt;
    for (const auto &Op : Block.Ops) {
      if (!Charge())
        return std::nullopt;
      ContainsCall |= &Op == &Call;
      if (Op.Opcode != NdOp::RETURN)
        continue;
      if (&Op != &Block.Ops.back() || !Block.Succs.empty() || Op.Dead ||
          Op.Addr < Block.StartAddr || Op.Addr >= Block.EndAddr ||
          Op.OriginSeq < 0 || Op.NumInputs != 1 || Op.Output.Size ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return std::nullopt;
      unsigned Matches = 0;
      for (const auto &Control : States.SEHControls) {
        if (!Charge())
          return std::nullopt;
        if (Control.Address != Op.Addr)
          continue;
        if (++Matches != 1 ||
            Control.Control !=
                RegistrationSEHControlContract::Kind::NearReturn ||
            Control.EndAddress != Block.EndAddr || Control.Target != InvalidVA)
          return std::nullopt;
      }
      if (Matches != 1)
        return std::nullopt;
      Returns.insert(&Op);
    }
    for (int Successor : Block.Succs) {
      if (!Charge())
        return std::nullopt;
      Pending.push_back(Successor);
    }
  }
  if (!ContainsCall || Returns.empty())
    return std::nullopt;
  // A finally return has no scalar ABI only on runtime entry. Ordinary calls
  // and shared entries need a separate caller-result proof.
  for (const auto &Exit : States.SEHInvocationExits)
    if (!Charge() || (Exit.EntryVA == Entry && Exit.CallVA))
      return std::nullopt;
  auto InBody = [&](va_t Address) {
    for (int Id : Body) {
      if (!Charge())
        return true;
      const auto &Block = *Blocks.at(Id);
      if (Address >= Block.StartAddr && Address < Block.EndAddr)
        return true;
    }
    return false;
  };
  for (va_t Other : OtherEntries)
    if (InBody(Other))
      return std::nullopt;
  for (const auto &Block : Function.Blocks) {
    for (int Successor : Block.Succs)
      if (!Charge() || (!Body.count(Block.Id) && Body.count(Successor)))
        return std::nullopt;
    for (const auto &Op : Block.Ops) {
      if (!Charge())
        return std::nullopt;
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.NumInputs && Op.Inputs[0].isConst() &&
          InBody(Op.Inputs[0].ConstVal))
        return std::nullopt;
    }
  }
  return Returns;
}

} // namespace neverd
