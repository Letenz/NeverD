//===- COFFRegistrationSEHNullAccess.cpp - Source null memory effects -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationFrameStores.h"
#include "COFFRegistrationSEHException.h"

#include "neverd/Limits.h"
#include "neverd/backend/Limits.h"

#include <functional>
#include <tuple>

namespace neverd::coff_registration {

llvm::Expected<std::set<const llvm::LoadInst *>>
bindSEHNullReads(const MedFunc &Source,
                 llvm::ArrayRef<const llvm::Function *> Functions) {
  struct Access {
    int Block;
    uint16_t Width;
  };
  std::map<std::pair<va_t, int>, Access> Expected;
  size_t Work = 0;
  using Key = std::tuple<MedVar::VarKind, int, int>;
  auto Variable = [](const MedVar &V) { return Key{V.Kind, V.Id, V.SSAVer}; };
  std::map<Key, const MedOp *> Definitions;
  for (const auto &Block : Source.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("SEH null-read definitions exceed their work budget");
      Definitions[Variable(Phi.Output)] = nullptr;
    }
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("SEH null-read definitions exceed their work budget");
      if (!Op.Output.Size || Op.Output.isConst())
        continue;
      const auto [Found, New] = Definitions.emplace(Variable(Op.Output), &Op);
      if (!New)
        Found->second = nullptr;
    }
  }
  std::function<bool(const MedVar &, unsigned)> SourceZero = [&](const MedVar
                                                                     &Value,
                                                                 unsigned
                                                                     Depth) {
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        Depth >
            ::neverd::limits::backend::
                kCOFFRegistrationSEHNullAccessBindSEHNullReadsSourceZeroDepthLimit ||
        !Value.Size || Value.Size > 8)
      return false;
    if (Value.isConst())
      return Value.ConstVal == 0;
    const auto Found = Definitions.find(Variable(Value));
    if (Found == Definitions.end() || !Found->second)
      return false;
    const auto &Op = *Found->second;
    return Op.Output.Size == Value.Size && Op.NumInputs == 1 &&
           (Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
            Op.Opcode == NdOp::INT_SEXT) &&
           SourceZero(Op.Inputs[0], Depth + 1);
  };
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("SEH null-read source exceeds its work budget");
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs != 1 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          !SourceZero(Op.Inputs[0], 0))
        continue;
      if (Op.OriginSeq < 0 || !Op.Output.Size ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          !Expected
               .emplace(std::make_pair(Op.Addr, Op.OriginSeq),
                        Access{Block.Id, Op.Output.Size})
               .second)
        return rejectIR("SEH null read has no unique source occurrence");
    }
  std::set<std::pair<va_t, int>> Seen;
  std::set<const llvm::LoadInst *> Result;
  RegistrationFrameStores Stores;
  if (!Stores.build(Functions, Work))
    return rejectIR("SEH null-read spill ownership exceeds its work budget");
  std::function<bool(const llvm::Value *, unsigned)> IsZero = [&](const llvm::
                                                                      Value *
                                                                          Value,
                                                                  unsigned
                                                                      Depth) {
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        Depth >
            ::neverd::limits::backend::
                kCOFFRegistrationSEHNullAccessBindSEHNullReadsIsZeroDepthLimit)
      return false;
    if (llvm::isa<llvm::ConstantPointerNull>(Value))
      return true;
    if (const auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(Value))
      return Constant->isZero();
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value))
      return !Cast->hasPoisonGeneratingFlags() &&
             (Cast->getOpcode() == llvm::Instruction::IntToPtr ||
              Cast->getOpcode() == llvm::Instruction::ZExt ||
              Cast->getOpcode() == llvm::Instruction::SExt ||
              Cast->getOpcode() == llvm::Instruction::Trunc ||
              Cast->getOpcode() == llvm::Instruction::BitCast) &&
             IsZero(Cast->getOperand(0), Depth + 1);
    const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value);
    if (!Load || Load->isAtomic() || !Load->getType()->isIntegerTy())
      return false;
    if (!Stores.directDefinitions(Load->getPointerOperand(), Work))
      return false;
    const auto Prefix =
        Stores.candidates(Load->getPointerOperand(), Load->getParent(),
                          Load->getPrevNode(), Work);
    if (!Prefix || Prefix->empty())
      return false;
    const auto *Store = Prefix->back();
    return !Store->isAtomic() &&
           Store->getValueOperand()->getType() == Load->getType() &&
           Store->getParent() == Load->getParent() &&
           Store->comesBefore(Load) &&
           IsZero(Store->getValueOperand(), Depth + 1);
  };
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("SEH null-read replay exceeds its work budget");
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        const auto Address = metadataInteger(*MD, 1, 64);
        const auto Seq = metadataInteger(*MD, 2, 32);
        if (!Address || !Seq || *Seq > INT_MAX)
          return rejectIR("SEH null-read occurrence is malformed");
        const auto Identity = std::make_pair(*Address, int(*Seq));
        const auto Found = Expected.find(Identity);
        if (Found == Expected.end())
          continue;
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
        if (!Load || MD->getNumOperands() != 5 ||
            metadataInteger(*MD, 0, 64) != Source.Entry ||
            metadataInteger(*MD, 3, 32) != uint32_t(Found->second.Block) ||
            metadataInteger(*MD, 4, 8) != 0 || !Seen.insert(Identity).second ||
            !Load->isVolatile() || Load->isAtomic() ||
            Load->getAlign() != llvm::Align(1) ||
            !Load->getType()->isIntegerTy(Found->second.Width * 8) ||
            !IsZero(Load->getPointerOperand(), 0) ||
            Load->getPointerAddressSpace() != 0 ||
            !Function->hasFnAttribute(llvm::Attribute::NullPointerIsValid))
          return rejectIR("SEH null read changed its source memory effect");
        Result.insert(Load);
      }
  if (Seen.size() != Expected.size())
    return rejectIR("SEH null-read occurrence set is incomplete");
  return Result;
}

} // namespace neverd::coff_registration
