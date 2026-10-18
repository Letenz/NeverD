//===- LibCVariant.cpp - The C library a program's calls follow -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/libc/LibCVariant.h"

#include "LibCVariantRows.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ELF/ELFOperatingSystem.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::libc {

LibCVariant libcVariantOf(BinaryFormat Format) {
  switch (Format) {
  case BinaryFormat::MachO:
    return LibCVariant::Darwin;
  case BinaryFormat::COFF:
    return LibCVariant::Windows;
  default:
    return LibCVariant::GNU;
  }
}

LibCVariant libcVariantOf(const BinaryImage &Image) {
  const BinaryFormat Format = Image.abiFormat();
  if (Format != BinaryFormat::ELF)
    return libcVariantOf(Format);
  if (runsUnderFreeBSD(Image))
    return LibCVariant::FreeBSD;
  if (const std::optional<LinuxLibC> LibC = linuxLibC(Image))
    switch (*LibC) {
    case LinuxLibC::GNU:
      return LibCVariant::GNU;
    case LinuxLibC::Musl:
      return LibCVariant::Musl;
    case LinuxLibC::Bionic:
      return LibCVariant::Bionic;
    }
  return LibCVariant::GNU;
}

std::string_view routineOfSymbol(std::string_view Name, LibCVariant Variant) {
  if (Variant != LibCVariant::Darwin)
    return Name;
  // Each suffix is a `$` and a word of capitals, digits and underscores.
  for (size_t Dollar = Name.rfind('$');
       Dollar != std::string_view::npos && Dollar > 0;
       Dollar = Name.rfind('$')) {
    const std::string_view Suffix = Name.substr(Dollar + 1);
    if (Suffix.empty() || !llvm::all_of(Suffix, [](char C) {
          return (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_';
        }))
      break;
    Name = Name.substr(0, Dollar);
  }
  return Name;
}

namespace variant_rows {
namespace {
/// Whether the C names \p Name and \p Routine name one routine, the tables
/// looking names up without their leading underscores.
bool sameRoutine(std::string_view Name, std::string_view Routine) {
  return stripLeadingUnderscores(Name) == stripLeadingUnderscores(Routine);
}
} // namespace

std::optional<LibCArity> arity(std::string_view Name, LibCVariant Variant) {
#define NEVERD_LIBC_VARIANT_ARITY(Library, Routine, Integers, Floats)          \
  if (Variant == LibCVariant::Library && sameRoutine(Name, Routine)) {         \
    LibCArity Arity;                                                           \
    Arity.IntArgs = Integers;                                                  \
    Arity.FpArgs = Floats;                                                     \
    return Arity;                                                              \
  }
#include "neverd/libc/LibCVariants.def"
  return std::nullopt;
}

std::optional<llvm::ArrayRef<VarArgFixedParamKind>>
variadic(std::string_view Name, LibCVariant Variant) {
  using enum VarArgFixedParamKind;
#define NEVERD_LIBC_VARIANT_VARIADIC(Library, Routine, ...)                    \
  if (Variant == LibCVariant::Library && sameRoutine(Name, Routine)) {         \
    static constexpr VarArgFixedParamKind Kinds[] = {__VA_ARGS__};             \
    return llvm::ArrayRef<VarArgFixedParamKind>(Kinds);                        \
  }
#include "neverd/libc/LibCVariants.def"
  return std::nullopt;
}

std::optional<LibCResult> result(std::string_view Name, LibCVariant Variant) {
#define NEVERD_LIBC_VARIANT_RESULT(Library, Routine, Kind)                     \
  if (Variant == LibCVariant::Library && sameRoutine(Name, Routine))           \
    return LibCResult::Kind;
#include "neverd/libc/LibCVariants.def"
  return std::nullopt;
}

std::optional<Pointers> pointers(std::string_view Name, LibCVariant Variant) {
#define NEVERD_LIBC_VARIANT_POINTERS(Library, Routine, Objects, Function,      \
                                     FunctionType)                             \
  if (Variant == LibCVariant::Library && sameRoutine(Name, Routine))           \
    return Pointers{Objects, Function, FunctionType};
#include "neverd/libc/LibCVariants.def"
  return std::nullopt;
}

std::optional<llvm::ArrayRef<std::string_view>>
forwardFixed(std::string_view Name, LibCVariant Variant) {
#define NEVERD_LIBC_VARIANT_FORWARD_FIXED(Library, Routine, ...)               \
  if (Variant == LibCVariant::Library && sameRoutine(Name, Routine)) {         \
    static constexpr std::string_view Types[] = {__VA_ARGS__};                 \
    return llvm::ArrayRef<std::string_view>(Types);                            \
  }
#include "neverd/libc/LibCVariants.def"
  return std::nullopt;
}

} // namespace variant_rows
} // namespace neverd::libc
