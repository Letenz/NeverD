//===- RuntimeRoutineIntrinsics.h - Modelled runtime routines --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The compiler runtime routines whose whole contract an intrinsic models
/// (RuntimeRoutineIntrinsics.def), by name.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_RUNTIMEROUTINEINTRINSICS_H
#define NEVERD_IR_INTRINSICS_RUNTIMEROUTINEINTRINSICS_H

#include "neverd/Common.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringRef.h"

#include <optional>

namespace neverd {

/// The intrinsic a call to runtime routine \p Name on \p TheArch is; none for
/// any other routine.
inline std::optional<Intrinsic> runtimeRoutineIntrinsic(Arch TheArch,
                                                        llvm::StringRef Name) {
#define NEVERD_RUNTIME_ROUTINE_INTRINSIC(RoutineArch, RoutineName, Id)         \
  if (TheArch == Arch::RoutineArch && Name == RoutineName)                     \
    return Intrinsic::Id;
#include "neverd/ir/intrinsics/RuntimeRoutineIntrinsics.def"
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_RUNTIMEROUTINEINTRINSICS_H
