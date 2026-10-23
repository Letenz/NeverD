//===- FloatingCallArguments.cpp - Scalar FP call carriers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate complete scalar floating-point register arguments and results.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/FloatingCallArguments.h"

#include "neverd/ir/TargetRegInfo.h"

namespace neverd {

std::optional<FloatingCallArguments>
floatingCallArguments(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                      llvm::ArrayRef<TypeRef> Parameters) {
  if (A != Arch::X64 && A != Arch::AArch64)
    return std::nullopt;
  const auto Scalar = [](const TypeRef &Type) {
    return Type && Type->Kind == NdTypeKind::Float &&
           (Type->Size == sizeof(float) || Type->Size == sizeof(double));
  };
  if (!Scalar(ReturnType))
    return std::nullopt;
  const auto Registers = getTargetRegInfo(A).floatingParamRegs(F);
  if (Parameters.size() > Registers.size())
    return std::nullopt;
  for (const auto &Type : Parameters)
    if (!Scalar(Type))
      return std::nullopt;
  const auto Placement = placeSourceParameters(A, F, ReturnType, Parameters);
  if (!Placement || Placement->ResultPointer ||
      Placement->Parameters.size() != Parameters.size())
    return std::nullopt;
  FloatingCallArguments Result;
  Result.ReturnBytes = ReturnType->Size;
  for (size_t I = 0; I < Parameters.size(); ++I) {
    const auto &Pieces = Placement->Parameters[I];
    if (Pieces.size() != 1 || Pieces[0].Indirect || Pieces[0].Offset != 0 ||
        Pieces[0].Location.Kind != SourceABICarrierKind::FloatingRegister ||
        Pieces[0].Location.RegisterOffset != Registers[I] ||
        Pieces[0].Location.ValueBytes != Parameters[I]->Size)
      return std::nullopt;
    Result.ParameterBytes.push_back(Parameters[I]->Size);
  }
  return Result;
}

} // namespace neverd
