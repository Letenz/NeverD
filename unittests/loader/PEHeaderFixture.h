//===- PEHeaderFixture.h - PE header fixtures -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authored linked PE images with original, file-backed headers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LOADER_PEHEADERFIXTURE_H
#define NEVERD_UNITTESTS_LOADER_PEHEADERFIXTURE_H

#include "../web/NativeFixture.h"

#include "neverd/object/PELayout.h"

namespace neverd::test {
using namespace llvm::COFF;
using namespace llvm::object;

inline std::string peHeaderImage(bool Is64, uint16_t Machine,
                                 bool LongStub = false) {
  auto Bytes = LongStub ? web::test::nativePELongStub() : web::test::nativePE();
  auto Headers =
      locatePEHeaders(reinterpret_cast<uint8_t *>(Bytes.data()), Bytes.size());
  Headers.FileHeader->Machine = Machine;
  if (!Is64) {
    // Retain the declared optional-header size and section-table position.
    // Only the format-native fields differ between PE32 and PE32+.
    const auto Old = *getPE32PlusOptionalHeader(Headers);
    auto *PE = reinterpret_cast<pe32_header *>(Headers.OptionalHeader);
    std::memset(Headers.OptionalHeader, 0,
                Headers.FileHeader->SizeOfOptionalHeader);
    PE->Magic = PE32Header::PE32;
    PE->ImageBase = 0x400000;
    PE->AddressOfEntryPoint = Old.AddressOfEntryPoint;
    PE->BaseOfCode = Old.BaseOfCode;
    PE->SectionAlignment = Old.SectionAlignment;
    PE->FileAlignment = Old.FileAlignment;
    PE->SizeOfImage = Old.SizeOfImage;
    PE->SizeOfHeaders = Old.SizeOfHeaders;
    PE->NumberOfRvaAndSize = 16;
  }
  return Bytes;
}

} // namespace neverd::test

#endif
