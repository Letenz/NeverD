//===- LibCVariant.h - The C library a program's calls follow --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The C library whose declarations the call model follows for a program.
/// Its tables follow the GNU C Library's; a routine another C library
/// declares otherwise, such as Darwin's sendfile, takes what
/// LibCVariants.def gives for that library.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCVARIANT_H
#define NEVERD_LIBC_LIBCVARIANT_H

#include "neverd/Common.h"

#include <cstdint>
#include <string_view>

namespace neverd {
struct BinaryImage;
} // namespace neverd

namespace neverd::libc {

/// A C library whose declarations of a routine may differ from another's.
enum class LibCVariant : uint8_t {
  GNU,    ///< The GNU C Library, and any program whose library is unknown.
  Musl,   ///< musl.
  Bionic, ///< Android's bionic.
  Darwin, ///< Darwin's libSystem: macOS and iOS.
  Windows ///< A Windows C runtime.
};

/// The C library a program of object format \p Format links: Darwin's for a
/// Mach-O program, a Windows C runtime for a PE one, and the GNU C
/// Library's otherwise.
LibCVariant libcVariantOf(BinaryFormat Format);

/// The C library \p Image links: as libcVariantOf(BinaryFormat) for its ABI
/// format, an ELF program's by the C library it runs on (linuxLibC).
LibCVariant libcVariantOf(const BinaryImage &Image);

/// The routine the symbol name \p Name links to in a program of C library
/// \p Variant: Darwin's variants of a routine, which take its arguments,
/// name it with suffixes (`syslog$DARWIN_EXTSN`, `stat$INODE64`,
/// `fcntl$NOCANCEL`), which this drops; an Objective-C selector stub's
/// (`objc_msgSend$length`) is no such suffix.  Otherwise \p Name itself.
std::string_view routineOfSymbol(std::string_view Name, LibCVariant Variant);

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCVARIANT_H
