//===- ELFOperatingSystem.h - An ELF image's system -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_ELFOPERATINGSYSTEM_H
#define NEVERD_LOADER_ELF_ELFOPERATINGSYSTEM_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/BinaryFormat/ELF.h"

namespace neverd {

/// Whether \p Image runs under Linux: an ELF image whose OS ABI byte is the
/// System V default that Linux toolchains write, or GNU/Linux.
inline bool runsUnderLinux(const BinaryImage &Image) {
  return Image.isELF() && Image.ELFMetadata &&
         (Image.ELFMetadata->OSABI == llvm::ELF::ELFOSABI_NONE ||
          Image.ELFMetadata->OSABI == llvm::ELF::ELFOSABI_GNU);
}

} // namespace neverd

#endif // NEVERD_LOADER_ELF_ELFOPERATINGSYSTEM_H
