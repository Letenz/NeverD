//===- MedCallConventionItanium.h - Itanium C++ ABI routines ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The parameters the Itanium C++ ABI gives the routines of its runtime and
/// of the unwinder it runs on, for the calling conventions of the platforms
/// that follow it.  An image may link its own copies of them, whose bodies
/// read more argument registers than their declarations take: libgcc's
/// _Unwind_Resume saves the exception-data registers (x0-x3, RAX and RDX)
/// as it enters, for the __builtin_eh_return that ends it.
///
/// This header is an implementation detail of the med/ library and should NOT
/// be included by code outside lib/ir/med/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLCONVENTIONITANIUM_H
#define NEVERD_IR_MED_MEDCALLCONVENTIONITANIUM_H

#include "neverd/Common.h"
#include "neverd/libc/LibCExceptionRuntime.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <optional>

namespace neverd {

/// The parameter count of the declaration of \p Name among the runtime and
/// unwinder routines of the Itanium C++ ABI (libc::kItaniumRuntimePrototypes),
/// or nullopt.  The names are reserved to the implementation, so an image's
/// routine of that name is the one declared.
inline std::optional<size_t> itaniumRuntimeArgCount(llvm::StringRef Name) {
  const llvm::StringRef Bare = stripLeadingUnderscores(Name);
  for (const libc::LibCPrototype &Prototype : libc::kItaniumRuntimePrototypes)
    if (stripLeadingUnderscores(Prototype.Name) == Bare)
      return Prototype.ParamCount;
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLCONVENTIONITANIUM_H
