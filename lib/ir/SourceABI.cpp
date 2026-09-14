#include "neverd/ir/SourceABI.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64Regs.h"

#include <algorithm>
#include <set>
#include <utility>

namespace neverd {
namespace {
bool equalTypes(const TypeRef &Left, const TypeRef &Right, unsigned Depth,
                unsigned &Remaining) {
  if (!Remaining || !Left || !Right || Depth > 16 ||
      Left->Kind != Right->Kind || Left->Size != Right->Size ||
      Left->IsSigned != Right->IsSigned)
    return false;
  --Remaining;
  switch (Left->Kind) {
  case NdTypeKind::Void:
    return Left->Size == 0;
  case NdTypeKind::Int:
    return Left->Size == 1 || Left->Size == 2 || Left->Size == 4 ||
           Left->Size == 8 || Left->Size == 16;
  case NdTypeKind::Float:
    return Left->Size == 4 || Left->Size == 8;
  case NdTypeKind::Ptr:
    return Left->Size == 8 &&
           equalTypes(Left->Pointee, Right->Pointee, Depth + 1, Remaining);
  case NdTypeKind::Func:
    if (Left->Size != 0 || !Left->RetType || !Right->RetType ||
        Left->RetType->Kind == NdTypeKind::Func ||
        Left->ParamTypes.size() > 64 ||
        Left->ParamTypes.size() != Right->ParamTypes.size() ||
        !equalTypes(Left->RetType, Right->RetType, Depth + 1, Remaining))
      return false;
    for (size_t I = 0; I < Left->ParamTypes.size(); ++I)
      if (!Left->ParamTypes[I] ||
          Left->ParamTypes[I]->Kind == NdTypeKind::Void ||
          Left->ParamTypes[I]->Kind == NdTypeKind::Func ||
          !equalTypes(Left->ParamTypes[I], Right->ParamTypes[I], Depth + 1,
                      Remaining))
        return false;
    return true;
  default:
    return false;
  }
}

bool scalarType(const TypeRef &Type) {
  if (!Type)
    return false;
  if (Type->Kind == NdTypeKind::Ptr)
    return Type->Size == 8 && equalSourceTypes(Type, Type);
  if (Type->Kind == NdTypeKind::Float)
    return Type->Size == 4 || Type->Size == 8;
  return Type->Kind == NdTypeKind::Int && (Type->Size == 1 || Type->Size == 2 ||
                                           Type->Size == 4 || Type->Size == 8);
}

bool fail(std::string &Diagnostic, const char *Message) {
  Diagnostic = Message;
  return false;
}
} // namespace

bool equalSourceTypes(const TypeRef &Left, const TypeRef &Right) {
  unsigned Remaining = 4096;
  return equalTypes(Left, Right, 0, Remaining);
}

bool validateSourceABI(const SourceFunctionTypeHint &Hint,
                       std::string &Diagnostic) {
  Diagnostic.clear();
  if (!Hint.HasExplicitABI ||
      (Hint.Architecture != Arch::AArch64 && Hint.Architecture != Arch::X64) ||
      Hint.Parameters.size() > 64 || !Hint.ReturnType)
    return fail(Diagnostic,
                "Source ABI requires an explicit arm64/x86_64 layout");
  const auto &TRI = getTargetRegInfo(Hint.Architecture);
  auto IntegerRegister = [&](uint64_t Offset) {
    if (TRI.isFrameOrLinkReg(Offset))
      return false;
    // AArch64's target table describes its contiguous scalar bank through
    // subregister records; GeneralRegs is currently populated only on x86.
    if (Hint.Architecture == Arch::AArch64)
      return Offset >= a64reg::X0 && Offset <= a64reg::X28 &&
             (Offset - a64reg::X0) % 8 == 0;
    return TRI.isGeneralReg(Offset);
  };
  auto ValidLocation = [&](const TypeRef &Type,
                           const SourceABIValueLocation &Location,
                           bool IsReturn) {
    if (!scalarType(Type) || Location.ValueBytes != Type->Size)
      return false;
    if (Location.ExtendTo32Bits &&
        (!IsReturn || Hint.Architecture != Arch::AArch64 ||
         Location.Kind != SourceABICarrierKind::IntegerRegister ||
         Type->Kind != NdTypeKind::Int || Type->Size >= 4))
      return false;
    if (Location.Kind == SourceABICarrierKind::Stack)
      return !IsReturn && Location.RegisterOffset == 0 &&
             Location.EntryStackOffset >=
                 (Hint.Architecture == Arch::X64 ? 8 : 0) &&
             Location.EntryStackOffset <= 4096 - Type->Size &&
             Location.EntryStackOffset % Type->Size == 0;
    if (Location.EntryStackOffset != 0)
      return false;
    if (Location.Kind == SourceABICarrierKind::FloatingRegister)
      return Type->Kind == NdTypeKind::Float &&
             TRI.isVectorReg(Location.RegisterOffset) &&
             (!IsReturn || Location.RegisterOffset == TRI.FPReturnReg);
    if (Location.Kind == SourceABICarrierKind::IntegerRegister)
      return Type->Kind != NdTypeKind::Float &&
             IntegerRegister(Location.RegisterOffset) &&
             (!IsReturn || Location.RegisterOffset == TRI.IntReturnReg);
    return false;
  };
  const auto EmptyLocation = [](const SourceABIValueLocation &Location) {
    return Location.Kind == SourceABICarrierKind::None &&
           Location.RegisterOffset == 0 && Location.EntryStackOffset == 0 &&
           Location.ValueBytes == 0 && !Location.ExtendTo32Bits;
  };
  if (!Hint.ReturnComponents.empty()) {
    if (Hint.ReturnType->Kind != NdTypeKind::Int ||
        Hint.ReturnType->Size != 16 || Hint.ReturnComponents.size() != 2 ||
        !EmptyLocation(Hint.ReturnLocation) || TRI.IntReturnRegs.size() < 2)
      return fail(Diagnostic, "Unsupported source return components");
    for (size_t I = 0; I != 2; ++I) {
      const auto &Component = Hint.ReturnComponents[I];
      if (Component.Kind != SourceABICarrierKind::IntegerRegister ||
          Component.RegisterOffset != TRI.IntReturnRegs[I] ||
          Component.EntryStackOffset != 0 || Component.ValueBytes != 8 ||
          Component.ExtendTo32Bits)
        return fail(Diagnostic, "Invalid source integer-pair return carrier");
    }
  } else if (Hint.ReturnType->Kind == NdTypeKind::Void) {
    if (!EmptyLocation(Hint.ReturnLocation))
      return fail(Diagnostic,
                  "Void source result has a physical value carrier");
  } else if (!ValidLocation(Hint.ReturnType, Hint.ReturnLocation, true)) {
    return fail(Diagnostic, "Unsupported source return carrier");
  }
  std::set<std::pair<SourceABICarrierKind, uint64_t>> Registers;
  std::vector<std::pair<int64_t, int64_t>> StackRanges;
  for (const auto &Parameter : Hint.Parameters) {
    if (!ValidLocation(Parameter.Type, Parameter.Location, false))
      return fail(Diagnostic, "Unsupported source parameter carrier");
    if (Parameter.Location.Kind == SourceABICarrierKind::Stack) {
      const int64_t Begin = Parameter.Location.EntryStackOffset;
      const int64_t End = Begin + Parameter.Location.ValueBytes;
      for (const auto &[OtherBegin, OtherEnd] : StackRanges)
        if (Begin < OtherEnd && OtherBegin < End)
          return fail(Diagnostic,
                      "Overlapping source stack parameter locations");
      StackRanges.emplace_back(Begin, End);
    } else if (!Registers
                    .emplace(Parameter.Location.Kind,
                             Parameter.Location.RegisterOffset)
                    .second) {
      return fail(Diagnostic, "Source parameters share one physical register");
    }
  }
  return true;
}

bool assignDarwinScalarSourceABI(SourceFunctionTypeHint &Hint,
                                 Arch Architecture, std::string &Diagnostic) {
  Diagnostic.clear();
  if ((Architecture != Arch::AArch64 && Architecture != Arch::X64) ||
      !Hint.ReturnType || Hint.Parameters.size() > 64)
    return fail(Diagnostic, "Unsupported Darwin scalar source ABI");
  const auto &TRI = getTargetRegInfo(Architecture);
  size_t IntegerIndex = 0;
  size_t FloatIndex = 0;
  int64_t StackOffset = Architecture == Arch::X64 ? 8 : 0;
  for (auto &Parameter : Hint.Parameters) {
    if (!scalarType(Parameter.Type))
      return fail(Diagnostic,
                  "Darwin source ABI currently supports scalar values");
    auto &Location = Parameter.Location;
    Location = {};
    Location.ValueBytes = Parameter.Type->Size;
    const bool Floating = Parameter.Type->Kind == NdTypeKind::Float;
    auto &Index = Floating ? FloatIndex : IntegerIndex;
    const auto Bank = Floating ? TRI.FPParamRegs : TRI.IntParamRegs;
    if (Index < Bank.size()) {
      Location.Kind = Floating ? SourceABICarrierKind::FloatingRegister
                               : SourceABICarrierKind::IntegerRegister;
      Location.RegisterOffset = Bank[Index++];
    } else {
      // Apple arm64 packs fixed stack scalars at natural alignment. x86_64
      // Darwin uses eight-byte argument slots, above the pushed return address.
      const int64_t Alignment =
          Architecture == Arch::AArch64 ? Parameter.Type->Size : 8;
      StackOffset = (StackOffset + Alignment - 1) & -Alignment;
      Location.Kind = SourceABICarrierKind::Stack;
      Location.EntryStackOffset = StackOffset;
      StackOffset += Architecture == Arch::AArch64 ? Parameter.Type->Size : 8;
    }
  }
  Hint.ReturnLocation = {};
  Hint.ReturnComponents.clear();
  if (Hint.ReturnType->Kind == NdTypeKind::Int && Hint.ReturnType->Size == 16 &&
      TRI.IntReturnRegs.size() >= 2) {
    for (size_t I = 0; I != 2; ++I)
      Hint.ReturnComponents.push_back(
          {SourceABICarrierKind::IntegerRegister, TRI.IntReturnRegs[I], 0, 8});
  } else if (Hint.ReturnType->Kind != NdTypeKind::Void) {
    if (!scalarType(Hint.ReturnType))
      return fail(Diagnostic, "Unsupported Darwin scalar return value");
    const bool Floating = Hint.ReturnType->Kind == NdTypeKind::Float;
    Hint.ReturnLocation.Kind = Floating ? SourceABICarrierKind::FloatingRegister
                                        : SourceABICarrierKind::IntegerRegister;
    Hint.ReturnLocation.RegisterOffset =
        Floating ? TRI.FPReturnReg : TRI.IntReturnReg;
    Hint.ReturnLocation.ValueBytes = Hint.ReturnType->Size;
    // Clang's DarwinPCS classifies these results with ABIArgInfo::getExtend;
    // AArch64's return convention promotes them to W0. Keep the source value
    // width separate so a byte result is still emitted with its byte type.
    Hint.ReturnLocation.ExtendTo32Bits =
        Architecture == Arch::AArch64 &&
        Hint.ReturnType->Kind == NdTypeKind::Int && Hint.ReturnType->Size < 4;
  }
  Hint.Architecture = Architecture;
  Hint.HasExplicitABI = true;
  return validateSourceABI(Hint, Diagnostic);
}

bool assignDarwinObjCSourceABI(SourceFunctionTypeHint &Hint, Arch Architecture,
                               std::string &Diagnostic) {
  if ((Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCRuntime &&
       Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCSDK) ||
      Hint.Parameters.size() < 2)
    return fail(Diagnostic, "Unsupported Objective-C source ABI");
  return assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic);
}

bool assignDarwinVariadicSourceABI(SourceFunctionTypeHint &Hint,
                                   unsigned FixedCount, Arch Architecture,
                                   std::string &Diagnostic) {
  if (!FixedCount || FixedCount > Hint.Parameters.size())
    return fail(Diagnostic, "Invalid variadic source prefix");
  for (size_t I = FixedCount; I < Hint.Parameters.size(); ++I) {
    const auto &T = Hint.Parameters[I].Type;
    if (!scalarType(T) || (T->Kind == NdTypeKind::Int && T->Size < 4) ||
        (T->Kind == NdTypeKind::Float && T->Size != 8))
      return fail(Diagnostic, "Variadic source arguments must be promoted");
  }
  if (!assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic))
    return false;
  if (Architecture == Arch::AArch64) {
    int64_t StackOffset = 0;
    for (size_t I = 0; I < FixedCount; ++I) {
      const auto &P = Hint.Parameters[I];
      if (P.Location.Kind == SourceABICarrierKind::Stack)
        StackOffset =
            std::max(StackOffset, P.Location.EntryStackOffset + P.Type->Size);
    }
    StackOffset = (StackOffset + 7) & ~int64_t(7);
    for (size_t I = FixedCount; I < Hint.Parameters.size(); ++I) {
      auto &P = Hint.Parameters[I];
      P.Location = {SourceABICarrierKind::Stack, 0, StackOffset, P.Type->Size};
      StackOffset += 8;
    }
  }
  return validateSourceABI(Hint, Diagnostic);
}

} // namespace neverd
