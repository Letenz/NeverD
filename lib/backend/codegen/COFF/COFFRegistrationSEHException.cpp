//===- COFFRegistrationSEHException.cpp - SEH runtime objects
//--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHException.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHException.h"

namespace neverd::coff_registration {

const llvm::Value *RegistrationSEHExceptionContract::pointerResult(
    const llvm::LoadInst &Load) const {
  const auto Bridge = Bridges.find(Load.getFunction());
  if (Bridge != Bridges.end() && Bridge->second->getValueOperand() == &Load)
    return Bridge->second;
  const auto Found = Reads.find(&Load);
  if (Found == Reads.end())
    return nullptr;
  const auto &Access = Found->second;
  using Kind = RegistrationSEHExceptionAccess::Kind;
  if (Access.Field == Kind::PointersSlot)
    return Access.Bridge;
  if (Access.Field == Kind::RecordPointer)
    return Access.Bridge->getValueOperand();
  return nullptr;
}

bool RegistrationSEHExceptionContract::validRead(const llvm::LoadInst &Load,
                                                 const llvm::Value *Root,
                                                 int64_t Offset) const {
  const auto Found = Reads.find(&Load);
  if (Found == Reads.end() || !Load.getType()->isIntegerTy(32) ||
      Load.isAtomic() || !Load.isVolatile() ||
      Load.getAlign() != llvm::Align(1))
    return false;
  const auto &Access = Found->second;
  if (Load.getFunction() != Access.Bridge->getFunction())
    return false;
  using Kind = RegistrationSEHExceptionAccess::Kind;
  switch (Access.Field) {
  case Kind::PointersSlot:
    return Root == Frame && Offset == PointersOffset;
  case Kind::RecordPointer:
    return Root == Access.Bridge && Offset == 0;
  case Kind::ExceptionCode:
    return Root == Access.Bridge->getValueOperand() && Offset == 0;
  case Kind::ExceptionFlags:
    return Root == Access.Bridge->getValueOperand() && Offset == 4;
  }
  return false;
}

bool RegistrationSEHExceptionContract::clobbersPointers(
    const llvm::StoreInst &Store, const llvm::Value *Root, int64_t Offset,
    uint64_t Size) const {
  const auto Bridge = Bridges.find(Store.getFunction());
  if (Root != Frame || Bridge == Bridges.end() || Bridge->second == &Store)
    return false;
  return Offset < PointersOffset + 4 &&
         (Offset >= PointersOffset || Size > uint64_t(PointersOffset - Offset));
}

llvm::Expected<RegistrationSEHExceptionContract> bindSEHExceptionReads(
    const MedFunc &Source, const RegistrationFrame &Frame,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    const std::map<const llvm::Function *, const llvm::StoreInst *> &Bridges,
    llvm::ArrayRef<const llvm::Function *> Functions) {
  const auto Expected = getX86RegistrationSEHExceptionAccesses(Source);
  if (!Expected || !Frame.Slot || Frame.EntrySP < 24)
    return rejectIR("source SEH exception fields have no checked identities");
  RegistrationSEHExceptionContract Result;
  auto NullReads = bindSEHNullReads(Source, Functions);
  if (!NullReads)
    return NullReads.takeError();
  Result.NullReads = std::move(*NullReads);
  Result.Frame = Frame.Slot;
  Result.PointersOffset = Frame.EntrySP - 24;
  Result.Bridges = Bridges;
  for (const auto &[Function, Bridge] : Bridges) {
    Result.Roots.insert(Bridge);
    Result.Roots.insert(Bridge->getValueOperand());
  }
  std::set<std::pair<va_t, int>> Seen;
  size_t Work = 0;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("SEH exception field replay exceeds its work budget");
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        const auto Address = metadataInteger(*MD, 1, 64);
        const auto Seq = metadataInteger(*MD, 2, 32);
        if (!Address || !Seq || *Seq > INT_MAX)
          return rejectIR("SEH exception field occurrence is malformed");
        const auto Identity = std::make_pair(*Address, int(*Seq));
        const auto Access = Expected->find(Identity);
        if (Access == Expected->end())
          continue;
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
        const auto Owner = Callbacks.find({Access->second.FilterVA, false});
        const auto Bridge = Bridges.find(Function);
        if (!Load || Owner == Callbacks.end() || Owner->second != Function ||
            Bridge == Bridges.end() || MD->getNumOperands() != 5 ||
            metadataInteger(*MD, 0, 64) != Source.Entry ||
            metadataInteger(*MD, 4, 8) != 0 || !Seen.insert(Identity).second)
          return rejectIR("SEH exception read lost its source filter");
        Result.Reads.emplace(Load, RegistrationSEHExceptionContract::Access{
                                       Access->second.Field, Bridge->second});
      }
  if (Seen.size() != Expected->size())
    return rejectIR("SEH exception read set is incomplete");
  return Result;
}

} // namespace neverd::coff_registration
