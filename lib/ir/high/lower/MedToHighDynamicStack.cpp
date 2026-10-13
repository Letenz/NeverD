//===- MedToHighDynamicStack.cpp - Run-time stack allocations -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A VLA or alloca lowers to `sp - size` with a runtime size, whose result
/// becomes the stack pointer.  As raw arithmetic its storage lies below the
/// frame the C declares, outside any object, so the C allocates it itself:
/// `(uintptr_t)__builtin_alloca(size)`, aligned as the stack is.  An
/// `old_sp - size` that computes the base again after it reads the variable
/// holding the allocation.  MedDynamicStack.h classifies both, as it does
/// for the LLVM route.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedDynamicStack.h"

namespace neverd {

void MedToHighConverter::indexDynamicStack() {
  if (!CurMed || DynamicStackFor == CurMed)
    return;
  DynamicStackFor = CurMed;
  DynamicStackAllocations.clear();
  DynamicStackPlaces.clear();
  DynamicStackBases.clear();
  DynamicStackPhis.clear();
  DynamicStackBaseUses.clear();
  for (const MedBlock &Block : CurMed->Blocks)
    for (const PhiNode &Phi : Block.Phis)
      DynamicStackPhis[{static_cast<int>(Phi.Output.Kind), Phi.Output.Id,
                        Phi.Output.SSAVer}] = &Phi;

  MedDynamicStackLookups Lookups;
  Lookups.Func = CurMed;
  Lookups.StackPointer = getTargetRegInfo(TargetArch).StackPointer;
  Lookups.Definition = [this](const MedVar &V) {
    return uniqueMedDefinition(V);
  };
  Lookups.Phi = [this](const MedVar &V) -> const PhiNode * {
    const auto It =
        DynamicStackPhis.find({static_cast<int>(V.Kind), V.Id, V.SSAVer});
    return It == DynamicStackPhis.end() ? nullptr : It->second;
  };
  size_t Place = 0;
  for (const MedBlock &Block : CurMed->Blocks)
    for (const MedOp &Op : Block.Ops) {
      ++Place;
      if (Op.Opcode != NdOp::INT_SUB && Op.Opcode != NdOp::INT_ADD)
        continue;
      DynamicStackPlaces[&Op] = Place;
      const MedVar *Size = medDynamicStackAllocationSize(Op, Lookups);
      if (!Size)
        continue;
      DynamicStackAllocations[&Op] = *Size;
      DynamicStackBases.emplace(medSizeRootKey(*Size, Lookups),
                                std::make_pair(Place, Op.Output));
    }
  if (DynamicStackAllocations.empty())
    return;
  // An operation computing an allocation's base again reads the allocation
  // made before it in block order, as the LLVM route reads the one it emitted.
  for (const MedBlock &Block : CurMed->Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (DynamicStackAllocations.count(&Op))
        continue;
      const auto At = DynamicStackPlaces.find(&Op);
      if (At == DynamicStackPlaces.end())
        continue;
      medTryDynamicStackAddressSizes(Op, Lookups, [&](const MedVar &Size) {
        const auto Base = DynamicStackBases.find(medSizeRootKey(Size, Lookups));
        if (Base == DynamicStackBases.end() || Base->second.first >= At->second)
          return false;
        DynamicStackBaseUses[&Op] = Base->second.second;
        return true;
      });
    }
}

ExprPtr MedToHighConverter::dynamicStackExpr(const MedOp &Op) {
  if (!CurMed || (Op.Opcode != NdOp::INT_SUB && Op.Opcode != NdOp::INT_ADD))
    return nullptr;
  indexDynamicStack();
  if (const auto It = DynamicStackAllocations.find(&Op);
      It != DynamicStackAllocations.end()) {
    auto Call =
        HighExpr::makeCall("__builtin_alloca", 0, {medvarToExpr(It->second)});
    Call->Type = NdType::makePtr(NdType::makeVoid());
    auto Address = std::make_shared<HighExpr>();
    Address->Kind = ExprKind::Cast;
    Address->Type = Address->CastTo = NdType::makeInt(Op.Output.Size, false);
    Address->Operands.push_back(std::move(Call));
    return Address;
  }
  if (const auto It = DynamicStackBaseUses.find(&Op);
      It != DynamicStackBaseUses.end())
    return HighExpr::makeVar(It->second);
  return nullptr;
}

} // namespace neverd
