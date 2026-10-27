//===- LibCResult.h - What a C library routine returns --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCRESULT_H
#define NEVERD_LIBC_LIBCRESULT_H

#include "neverd/Common.h"
#include "neverd/libc/LibCVariant.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace neverd::libc {

/// What a C library routine's declaration says it returns: nothing, a
/// pointer (to an object or to a function), or another value.
enum class LibCResult : uint8_t { Void, Value, Pointer };

/// What the routine C name \p Name links to in a program of C library
/// \p Variant returns, as its C declaration says: the runtime tables'
/// prototype (libcPrototype) or, for a routine a C library header declares,
/// the declared result (LibCReturnKinds.inc, LibCVariants.def).  Unknown for
/// a routine neither declares.
std::optional<LibCResult> libcResult(std::string_view Name,
                                     LibCVariant Variant);

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCRESULT_H
