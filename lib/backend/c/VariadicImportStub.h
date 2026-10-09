//===- VariadicImportStub.h - Stubs of variadic imports ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A stub that jumps to a variadic import (MinGW's `fprintf: jmp
/// [__imp_fprintf]`, a PLT entry of __fprintf_chk) passes every argument on,
/// the variadic ones too.  A C function receives `...` but cannot pass it on,
/// so both C backends print such a stub as the import it is, as its callers
/// already call the import.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_VARIADICIMPORTSTUB_H
#define NEVERD_BACKEND_C_VARIADICIMPORTSTUB_H

#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace neverd::c_stub {

/// The C name of the variadic import the function at \p Entry of \p Image is
/// a stub of, or empty.
inline std::string variadicImportOfStub(const BinaryImage &Image, va_t Entry) {
  const Import *Imp = Entry ? Image.findImportAt(Entry) : nullptr;
  if (!Imp || Imp->IATAddr == Entry || Imp->Name.empty())
    return {};
  // A PE import entry is the C name; other formats name the symbol.
  const std::string Name =
      importNamesAreCNames(Image.Format)
          ? Imp->Name
          : cNameOfSymbol(Imp->Name, Image.Format, Image.Arch).str();
  if (const libc::LibCPrototype *Prototype =
          libc::libcPrototype(Name, Image.Format))
    return Prototype->Variadic ? Name : std::string();
  return libc::isKnownFunction(Name) && libc::varArgFixedCount(Name) > 0
             ? Name
             : std::string();
}

/// Writes \p Stub, a stub of the variadic import \p Import that jumps through
/// \p Slot where the format names the slot, as that import.
inline void writeVariadicImportStub(llvm::raw_ostream &OS, llvm::StringRef Stub,
                                    llvm::StringRef Import,
                                    llvm::StringRef Slot) {
  OS << "/* " << Stub << " jumps to the import " << Import;
  if (!Slot.empty())
    OS << " through " << Slot;
  OS << ",\n   which receives every argument passed here, the variadic ones "
        "too.\n   C cannot pass those on: the stub is the import, and a call "
        "to it\n   is a call to "
     << Import << ". */\n";
}

} // namespace neverd::c_stub

#endif // NEVERD_BACKEND_C_VARIADICIMPORTSTUB_H
