//===- LibCFloatingArguments.h - Header FP calls -----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Selected fixed C header signatures supported by the scalar FP adapter.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCFLOATINGARGUMENTS_H
#define NEVERD_LIBC_LIBCFLOATINGARGUMENTS_H

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/FloatingCallArguments.h"

#include <optional>
#include <vector>

namespace neverd::libc {

inline std::optional<FloatingCallArguments>
floatingCallArguments(const FunctionSym &Function, Arch A, BinaryFormat F) {
  if ((!Function.PrototypeFromHeader && !Function.PrototypeFromCLibrary) ||
      !Function.PlatformConvention || Function.IsVariadic ||
      Function.CallConv == DebugCallConv::Fastcall ||
      Function.CallConv == DebugCallConv::Thiscall)
    return std::nullopt;
  std::vector<TypeRef> Parameters;
  for (const auto &[Name, Type] : Function.Params)
    Parameters.push_back(Type);
  return neverd::floatingCallArguments(A, F, Function.ReturnType, Parameters);
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFLOATINGARGUMENTS_H
