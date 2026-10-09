//===- SourceParameterPlacementI386.cpp - i386 cdecl/stdcall --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The i386 cdecl and stdcall conventions' parameter passing: every value on
/// the stack, in 4-byte slots, in signature order.  A record result comes back
/// through a hidden pointer passed first, except that Windows and Apple
/// return a record of 1, 2, 4 or 8 bytes in EAX:EDX, unless the Microsoft C++
/// ABI's record is a class with a constructor.  That ABI passes every record
/// argument as its bytes, whatever its class traits.  A complex result comes
/// back in registers, EAX:EDX or x87 ones, on Linux.
/// fastcall and thiscall pass leading values in ECX and EDX; their callers
/// do not ask these rules.
///
//===----------------------------------------------------------------------===//

#include "SourceParameterPlacementDetail.h"

#include "llvm/Support/MathExtras.h"

using namespace neverd;
using namespace neverd::source_placement;

namespace {
constexpr uint16_t kSlotBytes = 4;
/// An integer wider than this, or a floating value of 16 bytes (a Darwin
/// long double or a __float128), has no certain slot alignment here.
constexpr uint16_t kMaxIntegerBytes = 8;
constexpr uint16_t kMaxFloatingBytes = 12;

bool registerSizedRecord(const TypeRef &Type) {
  return Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
         Type->Size == 8;
}
} // namespace

std::optional<SourceParameterPlacement>
source_placement::placeI386(const TargetRegInfo &TRI, BinaryFormat F,
                            const TypeRef &ReturnType,
                            llvm::ArrayRef<TypeRef> ParamTypes) {
  const auto Layout = TRI.integerArgumentLayout(F);
  SourceParameterPlacement Placement;
  int64_t NextStack = Layout.EntryStackBase;
  if (isRecord(ReturnType)) {
    bool InRegisters = false;
    switch (ReturnType->Passing) {
    case NdRecordPassing::Complex:
      if (F != BinaryFormat::ELF)
        return Placement;
      InRegisters = true;
      break;
    case NdRecordPassing::ByValue:
      InRegisters = F != BinaryFormat::ELF && registerSizedRecord(ReturnType);
      break;
    case NdRecordPassing::ByReference:
      break;
    case NdRecordPassing::Microsoft:
      // A small record comes back in EAX:EDX unless it is a class with a
      // constructor, which PDB does not say.
      if (registerSizedRecord(ReturnType))
        return Placement;
      break;
    case NdRecordPassing::Unknown:
      // A result whose passing is unknown leaves every slot in doubt.
      return Placement;
    }
    if (!InRegisters) {
      SourceABIValueLocation Result;
      Result.Kind = SourceABICarrierKind::Stack;
      Result.EntryStackOffset = NextStack;
      Result.ValueBytes = kSlotBytes;
      Placement.ResultPointer = Result;
      NextStack += kSlotBytes;
    }
  }
  for (const TypeRef &Type : ParamTypes) {
    if (!Type || !Type->Size)
      break;
    if (isIntegerScalar(Type)    ? Type->Size > kMaxIntegerBytes
        : isFloatingScalar(Type) ? Type->Size > kMaxFloatingBytes
                                 : !isRecord(Type))
      break;
    if (isRecord(Type) && Type->Passing == NdRecordPassing::Unknown)
      break;
    // A record passed by reference is the address of a copy.
    const bool Indirect =
        isRecord(Type) && Type->Passing == NdRecordPassing::ByReference;
    const uint16_t Bytes = Indirect ? kSlotBytes : Type->Size;
    Placement.Parameters.push_back({stackPiece(NextStack, 0, Bytes, Indirect)});
    NextStack += static_cast<int64_t>(llvm::alignTo(Bytes, kSlotBytes));
  }
  return Placement;
}
