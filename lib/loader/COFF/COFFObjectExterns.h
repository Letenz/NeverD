//===- COFFObjectExterns.h - Addresses of a COFF object's externs -*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What a COFF object references without defining, placed by the shared
/// object-extern layer (neverd/loader/ObjectExterns.h).  An `__imp_` symbol is
/// the pointer an import library supplies to its function: it is a cell of the
/// layer, holding the function's extern address.  Nothing outside this
/// directory may include this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_COFF_COFFOBJECTEXTERNS_H
#define NEVERD_LIB_LOADER_COFF_COFFOBJECTEXTERNS_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjectExterns.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {
namespace coff_loader {

/// A COFF object's externs, and the import pointer cell each `__imp_` symbol
/// names, by that symbol's name.
struct ObjectExterns : object_externs::ExternLayout {
  std::map<std::string, va_t> ImportCells;
};

/// Whether relocation \p Type of \p Machine at \p Offset of a section holding
/// \p Contents relocates a call or branch (COFFObjectRelocations.def).
bool isBranchReference(uint16_t Machine, uint32_t Type,
                       llvm::ArrayRef<uint8_t> Contents, uint64_t Offset,
                       bool InCode);

/// Place the undefined and common symbols the relocations of \p Obj name, and
/// the cells of its `__imp_` symbols, past \p ImageEnd.  \p SectionVAs maps a
/// section number to the address its section is mapped at.
llvm::Expected<ObjectExterns>
planObjectExterns(const llvm::object::COFFObjectFile &Obj,
                  const std::vector<va_t> &SectionVAs, va_t ImageEnd);

/// Add the planned externs and cells to \p Img: the undefined symbols as
/// symbols, the called ones as imports named as an import entry names them,
/// the common symbols as data symbols, and each cell as the storage slot of
/// its import.
void addObjectExterns(const ObjectExterns &Externs, Arch Target,
                      BinaryImage &Img);

/// The address an undefined or common symbol \p Name resolves to, with the
/// owner its address provenance names: InvalidVA for a call or branch, which
/// reaches an import rather than storage.
std::optional<std::pair<va_t, va_t>>
resolveObjectExtern(const ObjectExterns &Externs, llvm::StringRef Name,
                    bool Common, bool Branch);

} // namespace coff_loader
} // namespace neverd

#endif // NEVERD_LIB_LOADER_COFF_COFFOBJECTEXTERNS_H
