//===- ImportSlots.h - Where an image stores imports' addresses -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The slots an image stores imports' addresses in: the listing names them
/// after their imports, and the LLVM IR view names them beside the code that
/// reads them.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_IMPORTSLOTS_H
#define NEVERD_SDK_CAPI_IMPORTSLOTS_H

#include "neverd/Common.h"

#include <cstdint>
#include <map>
#include <string>

namespace neverd {
struct BinaryImage;

namespace sdk {

/// A slot holding an import's address: the import, and the addend the loader
/// adds to its address.
struct ImportAddressSlot {
  std::string Name;
  int64_t Addend = 0;
};

/// The image's exact import storage slots by address: those its loader
/// collected and, in a linked ELF image, the GOT entries its slot-binding
/// relocations fill.  A slot two imports or addends claim is left out.
std::map<va_t, ImportAddressSlot> importAddressSlots(const BinaryImage &Img);

} // namespace sdk
} // namespace neverd

#endif // NEVERD_SDK_CAPI_IMPORTSLOTS_H
