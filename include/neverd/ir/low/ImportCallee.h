//===- ImportCallee.h - The import a call address names --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The import that a call to an address reaches, for the register summaries
/// and the prototypes that fix an import's entry reads.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_IMPORTCALLEE_H
#define NEVERD_IR_LOW_IMPORTCALLEE_H

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <optional>
#include <string>

namespace neverd {

/// The name of the import that a call to \p Addr reaches: through an
/// executable stub or an import address slot the import directory lists, or
/// through a pointer slot the loader binds to the import (an ELF GLOB_DAT or
/// JUMP_SLOT relocation, a Mach-O bind) with no addend.  Empty when \p Addr
/// names no import, as for a slot holding `import + addend` or one that two
/// imports claim.
inline std::string importCalleeName(const BinaryImage &Img, va_t Addr) {
  if (const Import *Imp = Img.findImportAt(Addr); Imp && !Imp->Name.empty())
    return Imp->Name;
  const ImportStorageSlotCollection Storage =
      Img.collectImportStorageSlot(Addr);
  if (auto It = Storage.Slots.find(Addr); It != Storage.Slots.end() &&
                                          !Storage.Conflicts.count(Addr) &&
                                          It->second.Addend == 0)
    return It->second.Name;
  return {};
}

/// The constant address the value \p Var holds at op \p Index of \p Block
/// was loaded from: its last write before there, in the block or the chain
/// of single predecessors above it, is a load from an address that folds to
/// a constant (`mov rax, [rip+slot]`).
inline std::optional<va_t> loadedCallSlot(const LowFunc &F,
                                          const LowBlock &Block, size_t Index,
                                          NdVar Var) {
  auto Same = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };
  const LowBlock *Cur = &Block;
  size_t End = Index;
  bool Loaded = false;
  uint64_t Addend = 0;
  for (int Step = 0; Step < limits::kCallTargetSlotDepth; ++Step) {
    if (Loaded && Var.isConst())
      return static_cast<va_t>(Var.Offset + Addend);
    const LowOp *Def = nullptr;
    for (size_t J = End; J-- > 0;)
      if (Cur->Ops[J].Output.Size && Same(Cur->Ops[J].Output, Var)) {
        Def = &Cur->Ops[J];
        End = J;
        break;
      }
    if (!Def) {
      if (Cur->Preds.size() != 1)
        return std::nullopt;
      const int Pred = Cur->Preds.front();
      auto It = std::find_if(F.Blocks.begin(), F.Blocks.end(),
                             [&](const LowBlock &B) { return B.Id == Pred; });
      if (It == F.Blocks.end())
        return std::nullopt;
      Cur = &*It;
      End = Cur->Ops.size();
      continue;
    }
    if (Def->NumInputs < 1)
      return std::nullopt;
    if (!Loaded && Def->Opcode == NdOp::LOAD) {
      Loaded = true;
      Var = Def->Inputs[0];
      continue;
    }
    if (Def->Opcode == NdOp::COPY && (Loaded || !Def->Inputs[0].isConst())) {
      Var = Def->Inputs[0];
      continue;
    }
    if (Loaded && Def->Opcode == NdOp::INT_ADD && Def->NumInputs == 2 &&
        Def->Inputs[1].isConst()) {
      Addend += Def->Inputs[1].Offset;
      Var = Def->Inputs[0];
      continue;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_IMPORTCALLEE_H
