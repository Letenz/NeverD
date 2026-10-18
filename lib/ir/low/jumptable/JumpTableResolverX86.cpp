//===- JumpTableResolverX86.cpp - x86-family jump-table facts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-family image facts consumed by the architecture-neutral relative-table
/// recognizers:
///
///   - linkedI386GlobalOffsetTable — the GOT address i386 ELF PIC code adds
///     its switch-table entries to in a linked image.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/loader/BinaryImage.h"

#include <optional>

namespace neverd {

std::optional<va_t> linkedI386GlobalOffsetTable(const BinaryImage &Img) {
  if (Img.Arch != Arch::X86 || Img.getPointerSize() != 4 || !Img.isELF() ||
      Img.IsRelocatable)
    return std::nullopt;
  std::optional<va_t> GOT;
  if (Img.DynInfo.PltGotAddr != 0)
    GOT = Img.DynInfo.PltGotAddr;
  // A static or unstripped image may name the table itself.  Two records
  // that disagree leave the GOT address unknown.
  if (const Symbol *Sym = Img.findSymbol("_GLOBAL_OFFSET_TABLE_")) {
    if (GOT && *GOT != Sym->Addr)
      return std::nullopt;
    GOT = Sym->Addr;
  }
  if (!GOT || !Img.getSegmentFor(*GOT))
    return std::nullopt;
  return GOT;
}

} // namespace neverd
