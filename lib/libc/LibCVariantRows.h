//===- LibCVariantRows.h - Rows of LibCVariants.def ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What LibCVariants.def gives a routine a C library declares otherwise than
/// the GNU C Library, for the lookups of LibCNames.cpp and
/// LibCCallTraits.cpp.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIBC_LIBCVARIANTROWS_H
#define NEVERD_LIB_LIBC_LIBCVARIANTROWS_H

#include "neverd/libc/LibCNames.h"
#include "neverd/libc/LibCResult.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace neverd::libc::variant_rows {

/// The arguments \p Name takes in \p Variant, where a row gives them.
std::optional<LibCArity> arity(std::string_view Name, LibCVariant Variant);

/// Whether a row makes \p Name variadic in \p Variant, and the categories of
/// its fixed parameters there.
std::optional<llvm::ArrayRef<VarArgFixedParamKind>>
variadic(std::string_view Name, LibCVariant Variant);

/// What \p Name returns in \p Variant, where a row gives it.
std::optional<LibCResult> result(std::string_view Name, LibCVariant Variant);

/// The pointer parameters of a routine in a C library.
struct Pointers {
  /// A bit for each parameter that points to an object.
  uint64_t ObjectPointers = 0;
  /// The parameter that points to a function, or -1, and its C type.
  int FunctionPointer = -1;
  std::string_view Type;
};

/// The pointer parameters of \p Name in \p Variant, where a row gives them;
/// they replace the GNU C Library's.
std::optional<Pointers> pointers(std::string_view Name, LibCVariant Variant);

/// The C types of the fixed parameters of the variadic routine \p Name in
/// \p Variant, where a row gives them.
std::optional<llvm::ArrayRef<std::string_view>>
forwardFixed(std::string_view Name, LibCVariant Variant);

} // namespace neverd::libc::variant_rows

#endif // NEVERD_LIB_LIBC_LIBCVARIANTROWS_H
