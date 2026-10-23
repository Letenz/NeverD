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
  if (A != Arch::X86 && A != Arch::X64 && A != Arch::AArch64)
    return std::nullopt;
  const auto Scalar = [](const TypeRef &Type) {
    return Type && Type->Kind == NdTypeKind::Float &&
           (Type->Size == sizeof(float) || Type->Size == sizeof(double));
  };
  if (Parameters.size() > static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
    return std::nullopt;
  // Flatten only complete, tightly packed AAPCS64 HFAs whose members all
  // occupy registers. SourceParameterPlacement proves the real aggregate
  // placement; comparing the flat signature rejects a partly spilled HFA.
  const auto HFA =
      [&](const TypeRef &Type) -> std::optional<std::vector<NdScalarLeaf>> {
    if (A != Arch::AArch64 || !Type ||
        Type->Passing != NdRecordPassing::ByValue)
      return std::nullopt;
    const auto Leaves = aapcs64FloatingAggregate(Type);
    if (!Leaves || Leaves->empty() ||
        (Leaves->front().Size != 4 && Leaves->front().Size != 8))
      return std::nullopt;
    unsigned End = 0;
    for (const auto &Leaf : *Leaves) {
      if (Leaf.Offset != End)
        return std::nullopt;
      End += Leaf.Size;
    }
    return End == Type->Size ? Leaves : std::nullopt;
  };
  bool HasRecord = ReturnType && ReturnType->Kind == NdTypeKind::Struct &&
                   !ReturnType->IsEnum;
  for (const auto &Type : Parameters)
    HasRecord |= Type && Type->Kind == NdTypeKind::Struct && !Type->IsEnum;
  if (HasRecord) {
    const auto ResultLeaves = HFA(ReturnType);
    if (!Scalar(ReturnType) && !ResultLeaves)
      return std::nullopt;
    const auto Placement = placeSourceParameters(A, F, ReturnType, Parameters);
    if (!Placement || Placement->ResultPointer ||
        Placement->Parameters.size() != Parameters.size())
      return std::nullopt;
    std::vector<TypeRef> Flat;
    std::vector<uint16_t> Elements;
    std::vector<SourceABIValueLocation> Locations;
    for (size_t I = 0; I < Parameters.size(); ++I) {
      const auto &Type = Parameters[I];
      const auto Leaves = HFA(Type);
      const auto &Pieces = Placement->Parameters[I];
      if (Type && Type->Kind == NdTypeKind::Struct && !Type->IsEnum) {
        if (!Leaves || Pieces.size() != Leaves->size())
          return std::nullopt;
        Elements.push_back(Leaves->size());
        for (size_t J = 0; J < Leaves->size(); ++J) {
          if (Pieces[J].Indirect || Pieces[J].Offset != (*Leaves)[J].Offset ||
              Pieces[J].Location.Kind !=
                  SourceABICarrierKind::FloatingRegister ||
              Pieces[J].Location.ValueBytes != (*Leaves)[J].Size)
            return std::nullopt;
          Flat.push_back(NdType::makeFloat((*Leaves)[J].Size));
          Locations.push_back(Pieces[J].Location);
        }
      } else {
        if (Pieces.size() != 1 || Pieces[0].Indirect || Pieces[0].Offset)
          return std::nullopt;
        Elements.push_back(0);
        Flat.push_back(Type);
        Locations.push_back(Pieces[0].Location);
      }
    }
    auto Result = floatingCallArguments(
        A, F,
        ResultLeaves ? NdType::makeFloat(ResultLeaves->front().Size)
                     : ReturnType,
        Flat);
    if (!Result)
      return std::nullopt;
    if (Result->Scalars.empty()) {
      const auto Registers = getTargetRegInfo(A).floatingParamRegs(F);
      for (size_t I = 0; I < Result->ParameterBytes.size(); ++I)
        Result->Scalars.push_back({SourceABICarrierKind::FloatingRegister,
                                   Registers[I], 0, Result->ParameterBytes[I],
                                   true, false});
      Result->ParameterBytes.clear();
    }
    for (size_t I = 0; I < Locations.size(); ++I)
      if (Result->Scalars[I].Kind != Locations[I].Kind ||
          Result->Scalars[I].Register != Locations[I].RegisterOffset ||
          Result->Scalars[I].StackOffset != Locations[I].EntryStackOffset ||
          Result->Scalars[I].Bytes != Locations[I].ValueBytes)
        return std::nullopt;
    Result->ParameterElements = std::move(Elements);
    Result->ReturnElements = ResultLeaves ? ResultLeaves->size() : 0;
    Result->ReturnBytes = ReturnType->Size;
    return Result;
  }
  if (!Scalar(ReturnType))
    return std::nullopt;
  const auto Registers = getTargetRegInfo(A).floatingParamRegs(F);
  bool AllFloating = true;
  for (const auto &Type : Parameters)
    if (!Scalar(Type)) {
      AllFloating = false;
      if (!Type ||
          (Type->Size != 1 && Type->Size != 2 && Type->Size != 4 &&
           Type->Size != 8) ||
          (Type->Kind == NdTypeKind::Ptr &&
           Type->Size != getTargetRegInfo(A).PointerSize) ||
          (Type->Kind != NdTypeKind::Int && Type->Kind != NdTypeKind::Ptr &&
           !(Type->Kind == NdTypeKind::Struct && Type->IsEnum)))
        return std::nullopt;
    }
  if (A != Arch::X86 && AllFloating && Parameters.size() > Registers.size())
    return std::nullopt;
  const auto Placement = placeSourceParameters(A, F, ReturnType, Parameters);
  if (!Placement || Placement->ResultPointer ||
      Placement->Parameters.size() != Parameters.size())
    return std::nullopt;
  FloatingCallArguments Result;
  Result.ReturnBytes = ReturnType->Size;
  if (A == Arch::X86 || !AllFloating) {
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
