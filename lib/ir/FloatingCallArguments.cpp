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

#include "neverd/Limits.h"
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
  if (Parameters.size() > static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
    return std::nullopt;
  bool AllFloating = true;
  for (const auto &Type : Parameters)
    if (!Scalar(Type)) {
      AllFloating = false;
      if (!Type || !Type->Size ||
          Type->Size > getTargetRegInfo(A).PointerSize ||
          (Type->Kind != NdTypeKind::Int && Type->Kind != NdTypeKind::Ptr &&
           !(Type->Kind == NdTypeKind::Struct && Type->IsEnum)))
        return std::nullopt;
    }
  if (AllFloating && Parameters.size() > Registers.size())
    return std::nullopt;
  const auto Placement = placeSourceParameters(A, F, ReturnType, Parameters);
  if (!Placement || Placement->ResultPointer ||
      Placement->Parameters.size() != Parameters.size())
    return std::nullopt;
  FloatingCallArguments Result;
  Result.ReturnBytes = ReturnType->Size;
  if (!AllFloating) {
    for (size_t I = 0; I < Parameters.size(); ++I) {
      const auto &Pieces = Placement->Parameters[I];
      if (Pieces.size() != 1 || Pieces[0].Indirect || Pieces[0].Offset != 0 ||
          Pieces[0].Location.ValueBytes != Parameters[I]->Size)
        return std::nullopt;
      const auto &Location = Pieces[0].Location;
      if (Location.Kind != SourceABICarrierKind::IntegerRegister &&
          Location.Kind != SourceABICarrierKind::FloatingRegister &&
          Location.Kind != SourceABICarrierKind::Stack)
        return std::nullopt;
      Result.Scalars.push_back({Location.Kind, Location.RegisterOffset,
                                Location.EntryStackOffset, Location.ValueBytes,
                                Scalar(Parameters[I]),
                                Parameters[I]->Kind == NdTypeKind::Ptr});
    }
    return Result;
  }
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
