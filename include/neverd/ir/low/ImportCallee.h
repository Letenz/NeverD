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

#include "neverd/loader/BinaryImage.h"

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

} // namespace neverd

#endif // NEVERD_IR_LOW_IMPORTCALLEE_H
