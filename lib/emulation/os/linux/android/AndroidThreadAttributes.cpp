//===- AndroidThreadAttributes.cpp - Guest-owned pthread attributes ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::emulation::android_model {
using namespace thread_attribute_abi;

/// A view of one call's guest-owned attribute, with no independent state.
class Bionic::ThreadAttributes {
public:
  ThreadAttributes(Bionic &Model, const NativeCallEvent &Call)
      : Model(Model), Call(Call), A(Call.Arguments), Integer(A[1]) {}

  BionicResult invoke();

private:
  Bionic &Model;
  const NativeCallEvent &Call;
  llvm::ArrayRef<uint64_t> A;
  uint32_t Integer;

  llvm::Error wordAccess(uint64_t Address, unsigned Size, unsigned Rights);
  llvm::Error fieldAccess(unsigned Offset, unsigned Size, unsigned Rights);
  llvm::Expected<uint64_t> readWord(uint64_t Address, unsigned Size);
  llvm::Error putWord(uint64_t Address, unsigned Size, uint64_t Value);
  BionicResult storeField(unsigned Offset, unsigned Size, uint64_t Value);
  BionicResult copyField(unsigned Offset, unsigned Size, bool Setter);
  BionicResult flags(bool Detach, bool Setter);
  BionicResult stack(bool Both);

  BionicResult init();
  BionicResult destroy();
  BionicResult getScope();
  BionicResult setScope();
  BionicResult getDetachState() { return flags(true, false); }
  BionicResult setDetachState() { return flags(true, true); }
  BionicResult getInheritSched() { return flags(false, false); }
  BionicResult setInheritSched() { return flags(false, true); }
  BionicResult getSchedPolicy() { return copyField(PolicyOffset, 4, false); }
  BionicResult setSchedPolicy() { return storeField(PolicyOffset, 4, Integer); }
  BionicResult getSchedParam() { return copyField(PriorityOffset, 4, false); }
  BionicResult setSchedParam() { return copyField(PriorityOffset, 4, true); }
  BionicResult getGuardSize() { return copyField(GuardSizeOffset, 8, false); }
  BionicResult setGuardSize() { return storeField(GuardSizeOffset, 8, A[1]); }
  BionicResult getStack() { return stack(true); }
  BionicResult getStackSize() { return stack(false); }
  BionicResult setStack();
  BionicResult setStackSize();
};

llvm::Error Bionic::ThreadAttributes::wordAccess(uint64_t Address,
                                                 unsigned Size,
                                                 unsigned Rights) {
  if (Address % Size)
    return failure(diagnostic::PthreadAlignment);
  return Model.access(Address, Size, Rights);
}

llvm::Error Bionic::ThreadAttributes::fieldAccess(unsigned Offset,
                                                  unsigned Size,
                                                  unsigned Rights) {
  if (A[0] % ObjectAlignment)
    return failure(diagnostic::PthreadAlignment);
  if (A[0] > UINT64_MAX - Offset)
    return failure(diagnostic::GuestPointer);
  return Model.access(A[0] + Offset, Size, Rights);
}

llvm::Expected<uint64_t> Bionic::ThreadAttributes::readWord(uint64_t Address,
                                                            unsigned Size) {
  std::array<uint8_t, 8> Bytes{};
  if (auto E = Model.CPU.read(Address,
                              llvm::MutableArrayRef(Bytes).take_front(Size)))
    return std::move(E);
  return llvm::support::endian::read64le(Bytes.data());
}

llvm::Error Bionic::ThreadAttributes::putWord(uint64_t Address, unsigned Size,
                                              uint64_t Value) {
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, Value);
  return Model.CPU.write(Address, llvm::ArrayRef(Bytes).take_front(Size));
}

BionicResult Bionic::ThreadAttributes::storeField(unsigned Offset,
                                                  unsigned Size,
                                                  uint64_t Value) {
  if (auto E = fieldAccess(Offset, Size, Write))
    return std::move(E);
  if (auto E = putWord(A[0] + Offset, Size, Value))
    return std::move(E);
  return value(0);
}

BionicResult Bionic::ThreadAttributes::copyField(unsigned Offset, unsigned Size,
                                                 bool Setter) {
  if (auto E = fieldAccess(Offset, Size, Setter ? Write : Read))
    return std::move(E);
  if (auto E = wordAccess(A[1], Size, Setter ? Read : Write))
    return std::move(E);
  auto V = readWord(Setter ? A[1] : A[0] + Offset, Size);
  if (!V)
    return V.takeError();
  if (auto E = putWord(Setter ? A[0] + Offset : A[1], Size, *V))
    return std::move(E);
  return value(0);
}

BionicResult Bionic::ThreadAttributes::setScope() {
  return value(Integer == ScopeSystem    ? 0
               : Integer == ScopeProcess ? linux_model::OperationNotSupported
                                         : linux_model::InvalidArgument);
}

BionicResult Bionic::ThreadAttributes::getScope() {
  // Bionic never dereferences the attribute for either scope operation.
  if (auto E = wordAccess(A[1], 4, Write))
    return std::move(E);
  if (auto E = putWord(A[1], 4, ScopeSystem))
    return std::move(E);
  return value(0);
}

BionicResult Bionic::ThreadAttributes::init() {
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
    if (auto E = fieldAccess(F.Offset, F.Size, Write))
      return std::move(E);
  for (const auto &F : Fields)
    if (auto E = putWord(A[0] + F.Offset, F.Size, F.Value))
      return std::move(E);
  return value(0);
}

BionicResult Bionic::ThreadAttributes::destroy() {
  if (auto E = fieldAccess(0, ObjectBytes, Write))
    return std::move(E);
  std::array<uint8_t, ObjectBytes> Bytes;
  Bytes.fill(DestroyedByte);
  if (auto E = Model.CPU.write(A[0], Bytes))
    return std::move(E);
  return value(0);
}

BionicResult Bionic::ThreadAttributes::flags(bool Detach, bool Setter) {
  // Reject invalid enums before touching an attribute or output pointer.
  if (Setter && (Detach ? Integer != Detached && Integer != Joinable
                        : Integer != Explicit && Integer != Inherit))
    return value(linux_model::InvalidArgument);
  if (auto E = fieldAccess(FlagsOffset, 4, Setter ? Read | Write : Read))
    return std::move(E);
  auto Flags = readWord(A[0] + FlagsOffset, 4);
  if (!Flags)
    return Flags.takeError();
  if (Setter) {
    const uint32_t Clear = Detach ? DetachedFlag : InheritFlag | ExplicitFlag;
    const uint32_t Set =
        Detach ? (Integer == Detached ? DetachedFlag : 0)
               : (Integer == Inherit ? InheritFlag : ExplicitFlag);
    if (auto E = putWord(A[0] + FlagsOffset, 4, (*Flags & ~Clear) | Set))
      return std::move(E);
    return value(0);
  }
  uint32_t Output;
  if (Detach)
    Output = *Flags & DetachedFlag ? Detached : Joinable;
  else if (*Flags & (InheritFlag | ExplicitFlag))
    Output = *Flags & InheritFlag ? Inherit : Explicit;
  else {
    if (auto E = fieldAccess(PolicyOffset, 4, Read))
      return std::move(E);
    auto Policy = readWord(A[0] + PolicyOffset, 4);
    if (!Policy)
      return Policy.takeError();
    Output = *Policy == NormalPolicy ? Inherit : Explicit;
  }
  if (auto E = wordAccess(A[1], 4, Write))
    return std::move(E);
  if (auto E = putWord(A[1], 4, Output))
    return std::move(E);
  return value(0);
}

BionicResult Bionic::ThreadAttributes::setStackSize() {
  if (A[1] < MinimumStackSize)
    return value(linux_model::InvalidArgument);
  return storeField(StackSizeOffset, 8, A[1]);
}

BionicResult Bionic::ThreadAttributes::setStack() {
  if (A[2] < MinimumStackSize || A[2] % PageSize || A[1] % PageSize)
    return value(linux_model::InvalidArgument);
  if (auto E = fieldAccess(StackBaseOffset, 16, Write))
    return std::move(E);
  if (auto E = putWord(A[0] + StackBaseOffset, 8, A[1]))
    return std::move(E);
  if (auto E = putWord(A[0] + StackSizeOffset, 8, A[2]))
    return std::move(E);
  return value(0);
}

BionicResult Bionic::ThreadAttributes::stack(bool Both) {
  // Admit both outputs first, but retain ordered loads when they alias attr.
  if (auto E = fieldAccess(StackBaseOffset, 16, Read))
    return std::move(E);
  if (auto E = wordAccess(A[1], 8, Write))
    return std::move(E);
  if (Both)
    if (auto E = wordAccess(A[2], 8, Write))
      return std::move(E);
  auto Base = readWord(A[0] + StackBaseOffset, 8);
  if (!Base)
    return Base.takeError();
  if (Both)
    if (auto E = putWord(A[1], 8, *Base))
      return std::move(E);
  auto Size = readWord(A[0] + StackSizeOffset, 8);
  if (!Size)
    return Size.takeError();
  if (auto E = putWord(Both ? A[2] : A[1], 8, *Size))
    return std::move(E);
  return value(0);
}

BionicResult Bionic::ThreadAttributes::invoke() {
  using Handler = BionicResult (ThreadAttributes::*)();
  static const llvm::StringMap<Handler> Handlers{
#define NEVERD_ANDROID_THREAD_ATTRIBUTE_CALL(Name, Text, Method)               \
  {Text, &ThreadAttributes::Method},
#include "AndroidThreadAttributes.def"
#undef NEVERD_ANDROID_THREAD_ATTRIBUTE_CALL
  };
  auto I = Handlers.find(Call.Name);
  if (I != Handlers.end())
    return (this->*I->second)();
  Model.Result.Stop = ProcessStopReason::UnsupportedService;
  Model.Result.Diagnostic =
      Call.Name + ": " + diagnostic::ThreadAttributeOperation;
  return std::optional<BionicValue>();
}

BionicResult Bionic::threadAttributes(const NativeCallEvent &Call) {
  return ThreadAttributes(*this, Call).invoke();
}
} // namespace neverd::emulation::android_model
