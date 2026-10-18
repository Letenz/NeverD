//===- ELFOperatingSystem.h - An ELF image's system -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_ELFOPERATINGSYSTEM_H
#define NEVERD_LOADER_ELF_ELFOPERATINGSYSTEM_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"

#include <optional>

namespace neverd {

/// Whether \p Image runs under FreeBSD: an ELF image whose OS ABI byte is
/// FreeBSD's, as FreeBSD's toolchains brand it, or that carries FreeBSD's
/// ABI tag note.
inline bool runsUnderFreeBSD(const BinaryImage &Image) {
  return Image.isELF() && Image.ELFMetadata &&
         (Image.ELFMetadata->OSABI == llvm::ELF::ELFOSABI_FREEBSD ||
          Image.ELFMetadata->FreeBSDVersion.has_value());
}

/// The systems other than Linux whose ELF images NeverD tells apart.
enum class ELFSystem : uint8_t { FreeBSD };

/// The system other than Linux \p Image runs under; none for Linux's and
/// for one NeverD does not tell apart.
inline std::optional<ELFSystem> elfSystem(const BinaryImage &Image) {
  if (runsUnderFreeBSD(Image))
    return ELFSystem::FreeBSD;
  return std::nullopt;
}

/// Whether \p Image runs under Linux: an ELF image whose OS ABI byte is the
/// System V default that Linux toolchains write, or GNU/Linux, and that is
/// not FreeBSD's.
inline bool runsUnderLinux(const BinaryImage &Image) {
  return Image.isELF() && Image.ELFMetadata &&
         (Image.ELFMetadata->OSABI == llvm::ELF::ELFOSABI_NONE ||
          Image.ELFMetadata->OSABI == llvm::ELF::ELFOSABI_GNU) &&
         !runsUnderFreeBSD(Image);
}

/// The C libraries a program under Linux runs on, whose headers lay its
/// system's records out differently: musl's time_t is 64 bits on 32-bit
/// targets, and bionic's pthread_mutex_t is one word there.
enum class LinuxLibC : uint8_t { GNU, Bionic, Musl };

/// The C library \p Image runs on, as its own records name it: Android's
/// dynamic linker or the NDK's identification note for bionic, musl's
/// dynamic linker for musl, and glibc's otherwise, a static program
/// included.  None for an image that does not run under Linux.
inline std::optional<LinuxLibC> linuxLibC(const BinaryImage &Image) {
  if (!runsUnderLinux(Image))
    return std::nullopt;
  const llvm::StringRef Interpreter = Image.ELFMetadata->Interpreter;
  if (Image.ELFMetadata->AndroidApiLevel ||
      Interpreter.starts_with("/system/bin/linker"))
    return LinuxLibC::Bionic;
  if (Interpreter.contains("ld-musl-"))
    return LinuxLibC::Musl;
  return LinuxLibC::GNU;
}

} // namespace neverd

#endif // NEVERD_LOADER_ELF_ELFOPERATINGSYSTEM_H
