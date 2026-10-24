//===- HeaderPrototype.h - Qualified source declarations ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared admission of portable source spellings after call-layout checks.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_HEADERPROTOTYPE_H
#define NEVERD_BACKEND_C_HEADERPROTOTYPE_H

#include "LibraryPrototype.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/debug/UserPrototype.h"

#include "llvm/ADT/StringRef.h"

namespace neverd::c_library {
/// Source qualifiers belong to the selected header or local declaration,
/// not to the machine carrier type. Reuse that spelling after checking the
/// call layout, and never replace an aggregate's lowered ABI with its source
/// signature. The outer calling convention is supplied separately.
inline llvm::StringRef portableScalarDeclaration(const FunctionSym &Function,
                                                 llvm::StringRef Name) {
  if ((!Function.PrototypeFromHeader && !Function.PrototypeFromCLibrary &&
       Function.PrototypeLibrary != UserPrototypeLibrary) ||
      !Function.PlatformConvention || !Function.ParamEntryLocations.empty() ||
      (Function.CallConv != DebugCallConv::Unknown &&
       Function.CallConv != DebugCallConv::Cdecl &&
       Function.CallConv != DebugCallConv::Stdcall) ||
      Function.Name != Name)
    return {};
  const auto Scalar = [](const TypeRef &Type) {
    return Type &&
           (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Float ||
            Type->Kind == NdTypeKind::Ptr ||
            (Type->Kind == NdTypeKind::Struct && Type->IsEnum));
  };
  if (!Function.ReturnType || (Function.ReturnType->Kind != NdTypeKind::Void &&
                               !Scalar(Function.ReturnType)))
    return {};
  std::set<std::string, std::less<>> Enumerations;
  const auto EnumName = [&](const TypeRef &Type) {
    if (Type && Type->Kind == NdTypeKind::Struct && Type->IsEnum) {
      if (!Type->SourceName.empty())
        Enumerations.insert(Type->SourceName);
      if (!Type->TypedefName.empty())
        Enumerations.insert(Type->TypedefName);
    }
  };
  EnumName(Function.ReturnType);
  for (const auto &[Parameter, Type] : Function.Params) {
    if (!Scalar(Type))
      return {};
    EnumName(Type);
  }
  return selfContainedDeclaration(Function.PortablePrototypeDeclaration, Name,
                                  Enumerations)
             ? llvm::StringRef(Function.PortablePrototypeDeclaration)
             : llvm::StringRef();
}

} // namespace neverd::c_library

#endif // NEVERD_BACKEND_C_HEADERPROTOTYPE_H
