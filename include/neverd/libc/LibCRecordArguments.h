//===- LibCRecordArguments.h - C library record parameters -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The machine operands of a header-declared C library call with records.
/// The arity tables count source parameters, never a record's machine words.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCRECORDARGUMENTS_H
#define NEVERD_LIBC_LIBCRECORDARGUMENTS_H

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/libc/LibCNames.h"

namespace neverd::libc {

inline bool isRecordCallParameter(const FunctionSym &Function, size_t Index,
                                  LibCVariant Variant) {
  if (Index >= Function.Params.size())
    return false;
  const auto &Type = Function.Params[Index].second;
  return Type && Type->Kind == NdTypeKind::Struct && !Type->IsEnum &&
         !isObjectPointerParameter(Function.Name, static_cast<unsigned>(Index),
                                   Variant);
}

inline bool hasRecordCallParameters(const FunctionSym &Function,
                                    LibCVariant Variant) {
  for (size_t I = 0; I < Function.Params.size(); ++I)
    if (isRecordCallParameter(Function, I, Variant))
      return true;
  return false;
}

/// A scalar parameter's source index at a flat machine operand. Record pieces
/// have no scalar source parameter type, including an indirect copy carrier.
inline std::optional<unsigned>
recordCallScalarParameter(const FunctionSym &Function,
                          const IntegerCallArguments &Layout, unsigned Argument,
                          LibCVariant Variant) {
  for (unsigned I = 0; I < Layout.Parameters.size(); ++I)
    if (!isRecordCallParameter(Function, I, Variant) &&
        Layout.Parameters[I].Pieces.size() == 1 &&
        Layout.Parameters[I].Pieces[0].Argument == Argument)
      return I;
  return std::nullopt;
}

/// A user's declaration keeps its own call model. Header declarations retain
/// their original name and header while their record pieces take the places
/// SourceParameterPlacement assigns. Transparent pointer unions stay pointers.
inline std::optional<IntegerCallArguments>
recordCallArguments(const FunctionSym &Function, Arch Architecture,
                    BinaryFormat Format, LibCVariant Variant) {
  if ((!Function.PrototypeFromHeader && !Function.PrototypeFromCLibrary) ||
      !Function.PlatformConvention ||
      Function.CallConv == DebugCallConv::Fastcall ||
      Function.CallConv == DebugCallConv::Thiscall)
    return std::nullopt;
  std::vector<TypeRef> Types;
  bool HasRecord = false;
  for (size_t I = 0; I < Function.Params.size(); ++I) {
    auto Type = Function.Params[I].second;
    if (!Type)
      return std::nullopt;
    if (isRecordCallParameter(Function, I, Variant)) {
      HasRecord = true;
    } else if (Type->Kind == NdTypeKind::Struct && !Type->IsEnum) {
      Type = NdType::makePtr();
      Type->Size = getTargetRegInfo(Architecture).PointerSize;
    }
    Types.push_back(std::move(Type));
  }
  return HasRecord ? integerCallArguments(Architecture, Format,
                                          Function.ReturnType, Types)
                   : std::nullopt;
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCRECORDARGUMENTS_H
