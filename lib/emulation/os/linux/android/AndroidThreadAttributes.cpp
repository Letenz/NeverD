//===- AndroidThreadAttributes.cpp - Guest-owned pthread attributes ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::emulation::android_model {
namespace thread_attribute_abi {
#define NEVERD_ANDROID_THREAD_ATTRIBUTE_VALUE(Name, Value)                     \
  constexpr unsigned Name = Value;
#include "AndroidThreadAttributes.def"
#undef NEVERD_ANDROID_THREAD_ATTRIBUTE_VALUE
} // namespace thread_attribute_abi

BionicResult Bionic::threadAttributes(const NativeCallEvent &Call) {
  using namespace thread_attribute_abi;
  const auto &A = Call.Arguments;
  const llvm::StringRef Name(Call.Name);
  const uint32_t Integer = A[1];
  auto Value = [](uint64_t V) { return std::optional<BionicValue>(V); };
  auto WordAccess = [&](uint64_t Address, unsigned Size, unsigned Rights) {
    if (Address % Size)
      return failure(diagnostic::PthreadAlignment);
    return access(Address, Size, Rights);
  };
  auto FieldAccess = [&](unsigned Offset, unsigned Size, unsigned Rights) {
    if (A[0] % ObjectAlignment)
      return failure(diagnostic::PthreadAlignment);
    if (A[0] > UINT64_MAX - Offset)
      return failure(diagnostic::GuestPointer);
    return access(A[0] + Offset, Size, Rights);
  };
  // Callers admit all affected spans before publishing any write. Individual
  // reads remain ordered with writes, including aliased getstack outputs.
  auto ReadWord = [&](uint64_t Address,
                      unsigned Size) -> llvm::Expected<uint64_t> {
    std::array<uint8_t, 8> Bytes{};
    if (auto E =
            CPU.read(Address, llvm::MutableArrayRef(Bytes).take_front(Size)))
      return std::move(E);
    return llvm::support::endian::read64le(Bytes.data());
  };
  auto PutWord = [&](uint64_t Address, unsigned Size, uint64_t V) {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, V);
    return CPU.write(Address, llvm::ArrayRef(Bytes).take_front(Size));
  };
  auto StoreField = [&](unsigned Offset, unsigned Size,
                        uint64_t V) -> BionicResult {
    if (auto E = FieldAccess(Offset, Size, Write))
      return std::move(E);
    if (auto E = PutWord(A[0] + Offset, Size, V))
      return std::move(E);
    return Value(0);
  };
  auto CopyField = [&](unsigned Offset, unsigned Size,
                       bool Setter) -> BionicResult {
    if (auto E = FieldAccess(Offset, Size, Setter ? Write : Read))
      return std::move(E);
    if (auto E = WordAccess(A[1], Size, Setter ? Read : Write))
      return std::move(E);
    auto V = ReadWord(Setter ? A[1] : A[0] + Offset, Size);
    if (!V)
      return V.takeError();
    if (auto E = PutWord(Setter ? A[0] + Offset : A[1], Size, *V))
      return std::move(E);
    return Value(0);
  };
  if (Name == symbol::ThreadAttrSetScope)
    return Value(Integer == ScopeSystem    ? 0
                 : Integer == ScopeProcess ? linux_model::OperationNotSupported
                                           : linux_model::InvalidArgument);
  if (Name == symbol::ThreadAttrGetScope) {
    // Bionic never dereferences the attribute for either scope operation.
    if (auto E = WordAccess(A[1], 4, Write))
      return std::move(E);
    if (auto E = PutWord(A[1], 4, ScopeSystem))
      return std::move(E);
    return Value(0);
  }
  if (Name == symbol::ThreadAttrInit) {
    struct Field {
      unsigned Offset, Size;
      uint64_t Value;
    };
    const Field Fields[] = {{FlagsOffset, 4, 0},
                            {StackBaseOffset, 8, 0},
                            {StackSizeOffset, 8, DefaultStackSize},
                            {GuardSizeOffset, 8, PageSize},
                            {PolicyOffset, 4, NormalPolicy},
                            {PriorityOffset, 4, 0}};
    for (const auto &F : Fields)
      if (auto E = FieldAccess(F.Offset, F.Size, Write))
        return std::move(E);
    for (const auto &F : Fields)
      if (auto E = PutWord(A[0] + F.Offset, F.Size, F.Value))
        return std::move(E);
    return Value(0);
  }
  if (Name == symbol::ThreadAttrDestroy) {
    if (auto E = FieldAccess(0, ObjectBytes, Write))
      return std::move(E);
    std::array<uint8_t, ObjectBytes> Bytes;
    Bytes.fill(DestroyedByte);
    if (auto E = CPU.write(A[0], Bytes))
      return std::move(E);
    return Value(0);
  }
  const bool SetDetach = Name == symbol::ThreadAttrSetDetachState;
  const bool SetInherit = Name == symbol::ThreadAttrSetInheritSched;
  if (SetDetach || SetInherit || Name == symbol::ThreadAttrGetDetachState ||
      Name == symbol::ThreadAttrGetInheritSched) {
    const bool Setter = SetDetach || SetInherit;
    // Reject invalid enums before touching an attribute or output pointer.
    if ((SetDetach && Integer != Detached && Integer != Joinable) ||
        (SetInherit && Integer != Explicit && Integer != Inherit))
      return Value(linux_model::InvalidArgument);
    if (auto E = FieldAccess(FlagsOffset, 4, Setter ? Read | Write : Read))
      return std::move(E);
    auto Flags = ReadWord(A[0] + FlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    if (Setter) {
      const uint32_t Clear =
          SetDetach ? DetachedFlag : InheritFlag | ExplicitFlag;
      const uint32_t Set =
          SetDetach ? (Integer == Detached ? DetachedFlag : 0)
                    : (Integer == Inherit ? InheritFlag : ExplicitFlag);
      if (auto E = PutWord(A[0] + FlagsOffset, 4, (*Flags & ~Clear) | Set))
        return std::move(E);
      return Value(0);
    }
    uint32_t Output;
    if (Name == symbol::ThreadAttrGetDetachState)
      Output = *Flags & DetachedFlag ? Detached : Joinable;
    else if (*Flags & (InheritFlag | ExplicitFlag))
      Output = *Flags & InheritFlag ? Inherit : Explicit;
    else {
      if (auto E = FieldAccess(PolicyOffset, 4, Read))
        return std::move(E);
      auto Policy = ReadWord(A[0] + PolicyOffset, 4);
      if (!Policy)
        return Policy.takeError();
      Output = *Policy == NormalPolicy ? Inherit : Explicit;
    }
    if (auto E = WordAccess(A[1], 4, Write))
      return std::move(E);
    if (auto E = PutWord(A[1], 4, Output))
      return std::move(E);
    return Value(0);
  }
  if (Name == symbol::ThreadAttrSetSchedPolicy)
    return StoreField(PolicyOffset, 4, Integer);
  if (Name == symbol::ThreadAttrGetSchedPolicy)
    return CopyField(PolicyOffset, 4, false);
  if (Name == symbol::ThreadAttrSetSchedParam ||
      Name == symbol::ThreadAttrGetSchedParam)
    return CopyField(PriorityOffset, 4,
                     Name == symbol::ThreadAttrSetSchedParam);
  if (Name == symbol::ThreadAttrSetGuardSize)
    return StoreField(GuardSizeOffset, 8, A[1]);
  if (Name == symbol::ThreadAttrGetGuardSize)
    return CopyField(GuardSizeOffset, 8, false);
  if (Name == symbol::ThreadAttrSetStackSize) {
    if (A[1] < MinimumStackSize)
      return Value(linux_model::InvalidArgument);
    return StoreField(StackSizeOffset, 8, A[1]);
  }
  if (Name == symbol::ThreadAttrSetStack) {
    if (A[2] < MinimumStackSize || A[2] % PageSize || A[1] % PageSize)
      return Value(linux_model::InvalidArgument);
    if (auto E = FieldAccess(StackBaseOffset, 16, Write))
      return std::move(E);
    if (auto E = PutWord(A[0] + StackBaseOffset, 8, A[1]))
      return std::move(E);
    if (auto E = PutWord(A[0] + StackSizeOffset, 8, A[2]))
      return std::move(E);
    return Value(0);
  }
  if (Name == symbol::ThreadAttrGetStack ||
      Name == symbol::ThreadAttrGetStackSize) {
    const bool Both = Name == symbol::ThreadAttrGetStack;
    if (auto E = FieldAccess(StackBaseOffset, 16, Read))
      return std::move(E);
    if (auto E = WordAccess(A[1], 8, Write))
      return std::move(E);
    if (Both)
      if (auto E = WordAccess(A[2], 8, Write))
        return std::move(E);
    auto Base = ReadWord(A[0] + StackBaseOffset, 8);
    if (!Base)
      return Base.takeError();
    if (Both)
      if (auto E = PutWord(A[1], 8, *Base))
        return std::move(E);
    auto Size = ReadWord(A[0] + StackSizeOffset, 8);
    if (!Size)
      return Size.takeError();
    if (auto E = PutWord(Both ? A[2] : A[1], 8, *Size))
      return std::move(E);
    return Value(0);
  }
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Call.Name + ": " + diagnostic::ThreadAttributeOperation;
  return std::optional<BionicValue>();
}
} // namespace neverd::emulation::android_model
