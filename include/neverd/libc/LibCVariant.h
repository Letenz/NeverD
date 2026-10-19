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
  GNU,     ///< The GNU C Library, and any program whose library is unknown.
  Musl,    ///< musl.
  Bionic,  ///< Android's bionic.
  Darwin,  ///< Darwin's libSystem: macOS and iOS.
  FreeBSD, ///< FreeBSD's libc.
  Windows  ///< A Windows C runtime.
};

/// Whether NeverD reads the headers of C library \p Variant with
/// `_GNU_SOURCE` defined, as its type libraries were generated
/// (lib/types/headers), and as GNU programs build: a unit that includes
/// them defines it too, or the GNU declarations its types and routines name
/// (re_search, struct re_registers, strverscmp) are missing.
inline bool headersReadWithGnuSource(LibCVariant Variant) {
  return Variant == LibCVariant::GNU || Variant == LibCVariant::Musl ||
         Variant == LibCVariant::Bionic;
}

/// The C library a program of object format \p Format links: Darwin's for a
/// Mach-O program, a Windows C runtime for a PE one, and the GNU C
/// Library's otherwise.
LibCVariant libcVariantOf(BinaryFormat Format);

/// The C library \p Image links: as libcVariantOf(BinaryFormat) for its ABI
/// format, an ELF program's by the system and the C library it runs on
/// (runsUnderFreeBSD, linuxLibC).
LibCVariant libcVariantOf(const BinaryImage &Image);

/// The routine the symbol name \p Name links to in a program of C library
/// \p Variant: Darwin's variants of a routine, which take its arguments,
/// name it with suffixes (`syslog$DARWIN_EXTSN`, `stat$INODE64`,
/// `fcntl$NOCANCEL`), which this drops; an Objective-C selector stub's
/// (`objc_msgSend$length`) is no such suffix.  Otherwise \p Name itself.
std::string_view routineOfSymbol(std::string_view Name, LibCVariant Variant);

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCVARIANT_H
