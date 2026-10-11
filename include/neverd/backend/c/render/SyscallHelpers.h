//===- SyscallHelpers.h - C helpers that make supervisor calls --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The C definition of the helper that makes a modeled supervisor call on the
/// system it was lifted from.  HighC and LLVM-C share it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_SYSCALLHELPERS_H
#define NEVERD_BACKEND_C_RENDER_SYSCALLHELPERS_H

#include "neverd/ir/med/MedSyscallConvention.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {

/// The definition of \p Name, which makes Linux supervisor call
/// \p Convention: it takes the number and then each argument register's
/// value, and returns the result register.  Built anywhere else, the C stops
/// with #error.
std::string linuxSyscallHelperDefinition(const SyscallConvention &Convention,
                                         llvm::StringRef Name);

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_SYSCALLHELPERS_H
