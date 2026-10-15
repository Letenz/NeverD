//===- COFFRegistrationSEHScopeState.cpp - Protected logical state -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHDynamicScope.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/X86RegistrationSEHStateStore.h"

#include <functional>
#include <tuple>

namespace neverd::coff_registration {

llvm::Expected<RegistrationSEHScopeState>
bindDynamicSEHState(const MedFunc &Source, const RegistrationFrame &Frame,
                    llvm::ArrayRef<const llvm::Function *> Functions,
                    const std::set<const llvm::StoreInst *> &RuntimeWrites) {
  RegistrationSEHScopeState Result;
  Result.Offset = int64_t(Frame.EntrySP) - 8;
  const auto CallbackStores = getX86RegistrationSEHStateStores(Source);
  if (!CallbackStores)
    return rejectIR("logical state has incomplete callback store receipts");
  if (CallbackStores->empty() &&
      !llvm::any_of(Source.RegistrationStates->Blocks, [](const auto &Block) {
        return !Block.CallbackOnly && Block.Levels.size() > 1;
      }))
    return Result;
  Result.Enabled = true;
  for (const auto *Write : RuntimeWrites)
    Result.Writes.emplace(Write, 4);
  using Key = std::tuple<MedVar::VarKind, int, int>;
  auto KeyOf = [](const MedVar &V) { return Key{V.Kind, V.Id, V.SSAVer}; };
  using Identity = std::pair<va_t, int>;
  std::map<Identity, std::optional<int32_t>> Frames;
  size_t Work = 0;
  for (const auto &Value : Source.RegistrationStates->FrameValues) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return rejectIR("dynamic scope frame values exceed their work budget");
    if (!Frames
             .emplace(Identity{Value.Address, Value.OpSeq},
                      Value.EstablishedFrameOffset)
             .second)
      return rejectIR("dynamic scope frame value identity is ambiguous");
  }
  std::map<Key, const MedOp *> Definitions;
  std::map<Key, int32_t> Offsets;
  for (const auto &Block : Source.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("dynamic scope phis exceed their work budget");
      Definitions[KeyOf(Phi.Output)] = nullptr;
    }
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("dynamic scope definitions exceed their work budget");
      if (!Op.Output.Size || Op.Output.isConst())
        continue;
      const auto [It, Added] = Definitions.emplace(KeyOf(Op.Output), &Op);
      if (!Added) {
        It->second = nullptr;
        Offsets.erase(KeyOf(Op.Output));
        continue;
      }
      const auto Found = Frames.find({Op.Addr, Op.OriginSeq});
      if (Found != Frames.end() && Found->second)
        Offsets.emplace(KeyOf(Op.Output), *Found->second);
    }
  }
  std::function<std::optional<int64_t>(const MedVar &, bool, unsigned)>
      Resolve = [&](const MedVar &Value, bool Address,
                    unsigned Depth) -> std::optional<int64_t> {
    if (++Work > limits::kMaxRegistrationEHStateWork || Depth > 32)
      return std::nullopt;
    if (Value.isConst())
      return Address ? std::nullopt
                     : std::optional<int64_t>(int32_t(Value.ConstVal));
    const auto Found = Definitions.find(KeyOf(Value));
    if (Found == Definitions.end() || !Found->second)
      return std::nullopt;
    if (Address) {
      const auto Offset = Offsets.find(KeyOf(Value));
      if (Offset != Offsets.end())
        return Offset->second;
    }
    const auto &Op = *Found->second;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
      return Resolve(Op.Inputs[0], Address, Depth + 1);
    return std::nullopt;
  };
  struct Write {
    int Block;
    uint32_t Value;
    uint16_t Width;
  };
  std::map<Identity, Write> Expected;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("dynamic scope source exceeds its work budget");
      if (Op.Opcode != NdOp::STORE || Op.NumInputs != 2 ||
          (Op.Inputs[1].Size != 1 && Op.Inputs[1].Size != 2 &&
           Op.Inputs[1].Size != 4) ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Resolve(Op.Inputs[0], true, 0) != -4)
        continue;
      const auto Value = Resolve(Op.Inputs[1], false, 0);
      const uint16_t Width = Op.Inputs[1].Size;
      const uint32_t Mask =
          Width == 4 ? UINT32_MAX : (uint32_t(1) << (8 * Width)) - 1;
      if (!Value || Op.OriginSeq < 0 ||
          !Expected
               .emplace(Identity{Op.Addr, Op.OriginSeq},
                        Write{Block.Id, uint32_t(*Value) & Mask, Width})
               .second)
        return rejectIR("dynamic scope has an unproved source state write");
    }
  std::set<Identity> Seen;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("dynamic scope write replay exceeds its work budget");
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        const auto Address = metadataInteger(*MD, 1, 64);
        const auto Seq = metadataInteger(*MD, 2, 32);
        if (!Address || !Seq || *Seq > INT_MAX)
          return rejectIR("dynamic scope write identity is malformed");
        const Identity Key{*Address, int(*Seq)};
        const auto Found = Expected.find(Key);
        if (Found == Expected.end())
          continue;
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
        if (!Store || !Seen.insert(Key).second || MD->getNumOperands() != 5 ||
            metadataInteger(*MD, 0, 64) != Source.Entry ||
            metadataInteger(*MD, 3, 32) != uint32_t(Found->second.Block) ||
            metadataInteger(*MD, 4, 8) != 1 || Store->isAtomic() ||
            !Store->isVolatile() || Store->getAlign() != llvm::Align(1) ||
            integer(Store->getValueOperand(), 8 * Found->second.Width) !=
                Found->second.Value)
          return rejectIR("dynamic scope changed its source state write");
        Result.Writes.emplace(Store, Found->second.Width);
      }
  if (Seen.size() != Expected.size())
    return rejectIR("dynamic scope state writes are incomplete");
  return Result;
}

} // namespace neverd::coff_registration
