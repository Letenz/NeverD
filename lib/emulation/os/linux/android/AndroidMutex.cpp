//===- AndroidMutex.cpp - Guest-owned API 28 mutex state -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

#include <array>

namespace neverd::emulation::android_model {
namespace mutex_abi {
#define NEVERD_ANDROID_MUTEX_VALUE(Name, Value) constexpr unsigned Name = Value;
#include "AndroidMutex.def"
#undef NEVERD_ANDROID_MUTEX_VALUE
} // namespace mutex_abi

llvm::Expected<std::optional<uint64_t>>
Bionic::mutex(const NativeCallEvent &Call) {
  using namespace mutex_abi;
  const auto &A = Call.Arguments;
  llvm::StringRef Name(Call.Name);
  auto Value = [](uint64_t V) { return std::optional<uint64_t>(V); };
  auto Unsupported = [&](llvm::StringRef Reason) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Call.Name + ": " + Reason.str();
    return std::optional<uint64_t>();
  };
  auto ReadWord = [&](uint64_t Address, unsigned Size,
                      unsigned Alignment) -> llvm::Expected<uint64_t> {
    if (Address % Alignment)
      return failure(diagnostic::PthreadAlignment);
    if (auto E = access(Address, Size, Read))
      return std::move(E);
    std::array<uint8_t, AttributeBytes> Bytes{};
    if (auto E =
            CPU.read(Address, llvm::MutableArrayRef(Bytes).take_front(Size)))
      return std::move(E);
    return llvm::support::endian::read64le(Bytes.data());
  };
  auto Writable = [&](uint64_t Address, unsigned Size, unsigned Alignment) {
    if (Address % Alignment)
      return failure(diagnostic::PthreadAlignment);
    return access(Address, Size, Write);
  };
  // Every write span is checked before any part of an operation is published.
  auto PutWord = [&](uint64_t Address, unsigned Size, uint64_t V) {
    uint8_t Bytes[AttributeBytes];
    llvm::support::endian::write64le(Bytes, V);
    return CPU.write(Address, llvm::ArrayRef(Bytes).take_front(Size));
  };
  if (Name.starts_with(symbol::PthreadMutexAttrPrefix)) {
    if (Name == symbol::MutexAttrInit || Name == symbol::MutexAttrDestroy) {
      if (auto E = Writable(A[0], AttributeBytes, AttributeBytes))
        return std::move(E);
      if (auto E = PutWord(A[0], AttributeBytes,
                           Name == symbol::MutexAttrInit ? 0 : UINT64_MAX))
        return std::move(E);
      return Value(0);
    }
    const bool Setter = (Name == symbol::MutexAttrSetType ||
                         Name == symbol::MutexAttrSetPShared ||
                         Name == symbol::MutexAttrSetProtocol);
    unsigned Mask = 0, Shift = 0, Maximum = 0;
    if (Name == symbol::MutexAttrGetType || Name == symbol::MutexAttrSetType) {
      Mask = AttributeType;
      Maximum = ErrorChecking;
    } else if (Name == symbol::MutexAttrGetPShared ||
               Name == symbol::MutexAttrSetPShared) {
      Mask = AttributeShared;
      Shift = 4;
      Maximum = 1;
    } else if (Name == symbol::MutexAttrGetProtocol ||
               Name == symbol::MutexAttrSetProtocol) {
      Mask = AttributeProtocol;
      Shift = 5;
      Maximum = 1;
    } else {
      return Unsupported(diagnostic::MutexAttributeOperation);
    }
    const uint32_t Argument = static_cast<uint32_t>(A[1]);
    // Bionic validates an int setter argument before dereferencing attr.
    if (Setter && Argument > Maximum)
      return Value(linux_model::InvalidArgument);
    auto Attribute = ReadWord(A[0], AttributeBytes, AttributeBytes);
    if (!Attribute)
      return Attribute.takeError();
    const uint64_t Field = (*Attribute & Mask) >> Shift;
    if (!Setter && Field > Maximum)
      return Value(linux_model::InvalidArgument);
    if (Setter) {
      if (auto E = Writable(A[0], AttributeBytes, AttributeBytes))
        return std::move(E);
      if (auto E = PutWord(A[0], AttributeBytes,
                           (*Attribute & ~uint64_t(Mask)) |
                               (uint64_t(Argument) << Shift)))
        return std::move(E);
    } else {
      if (auto E = Writable(A[1], sizeof(uint32_t), alignof(uint32_t)))
        return std::move(E);
      if (auto E = PutWord(A[1], sizeof(uint32_t), Field))
        return std::move(E);
    }
    return Value(0);
  }
  if (!Name.starts_with(symbol::PthreadMutexObjectPrefix))
    return Unsupported(diagnostic::MutexOperation);
  if (Name == symbol::MutexInit) {
    if (auto E = Writable(A[0], MutexBytes, MutexAlignment))
      return std::move(E);
    uint64_t Attribute = 0;
    if (A[1]) {
      auto Loaded = ReadWord(A[1], AttributeBytes, AttributeBytes);
      if (!Loaded)
        return Loaded.takeError();
      Attribute = *Loaded;
      // The ABI implementation clears the object before reading its attr.
      // Preserve that ordering even if supplied guest storage overlaps.
      for (unsigned I = 0; I < AttributeBytes; ++I)
        if (A[1] + I >= A[0] && A[1] + I - A[0] < MutexBytes)
          Attribute &= ~(uint64_t(0xff) << (I * 8));
    }
    const uint64_t Type = Attribute & AttributeType;
    if (Type <= ErrorChecking && (Attribute & AttributeProtocol))
      return Unsupported(diagnostic::MutexPriorityInheritance);
    std::array<uint8_t, MutexBytes> Bytes{};
    if (Type <= ErrorChecking)
      llvm::support::endian::write16le(
          Bytes.data(),
          (Type << TypeShift) | (Attribute & AttributeShared ? Shared : 0));
    if (auto E = CPU.write(A[0], Bytes))
      return std::move(E);
    // An invalid type still clears the complete object in this ABI.
    return Value(Type <= ErrorChecking ? 0 : linux_model::InvalidArgument);
  }
  if (Name != symbol::MutexLock && Name != symbol::MutexTryLock &&
      Name != symbol::MutexUnlock && Name != symbol::MutexDestroy)
    return Unsupported(diagnostic::MutexOperation);
  auto Loaded = ReadWord(A[0], StateBytes, MutexAlignment);
  if (!Loaded)
    return Loaded.takeError();
  const uint16_t State = *Loaded;
  // API 28 fortifies destroyed-mutex use. Match the existing explicit abort
  // boundary instead of inventing an errno return or successful reuse.
  if (State == Destroyed)
    return failure(llvm::formatv(diagnostic::DestroyedMutex, Call.Name).str());
  const unsigned Type = State >> TypeShift, Status = State & LockState;
  const uint16_t Count = State & Counter;
  const uint16_t Base = State & ~uint16_t(Counter | LockState);
  if (Type > ErrorChecking)
    return Unsupported(diagnostic::MutexPriorityInheritance);
  if (Status > Contended ||
      (Count && (Type != Recursive || Status == Unlocked)))
    return failure(diagnostic::MutexState);
  auto SetState =
      [&](uint16_t Next) -> llvm::Expected<std::optional<uint64_t>> {
    if (auto E = Writable(A[0], StateBytes, MutexAlignment))
      return std::move(E);
    if (auto E = PutWord(A[0], StateBytes, Next))
      return std::move(E);
    return Value(0);
  };
  if (Name == symbol::MutexDestroy)
    return Status == Unlocked ? SetState(Destroyed)
                              : Value(linux_model::ResourceBusy);
  const bool Try = Name == symbol::MutexTryLock,
             Unlock = Name == symbol::MutexUnlock;
  if (Type == Normal) {
    if (!Unlock && Status != Unlocked)
      return Try ? Value(linux_model::ResourceBusy)
                 : Unsupported(diagnostic::MutexBlocking);
    if (Unlock && Status == Contended)
      return Unsupported(diagnostic::MutexWake);
    // Normal mutexes do not inspect or change the owner field.
    return SetState(Base | (Unlock ? Unlocked : Locked));
  }
  const uint64_t OwnerAddress = A[0] + OwnerOffset;
  auto Owner = ReadWord(OwnerAddress, OwnerBytes, alignof(uint32_t));
  if (!Owner)
    return Owner.takeError();
  if ((Status == Unlocked) != (*Owner == 0))
    return failure(diagnostic::MutexOwner);
  auto SetOwnedState =
      [&](uint16_t Next, uint32_t NextOwner,
          bool Release) -> llvm::Expected<std::optional<uint64_t>> {
    if (auto E = Writable(A[0], StateBytes, MutexAlignment))
      return std::move(E);
    if (auto E = Writable(OwnerAddress, OwnerBytes, alignof(uint32_t)))
      return std::move(E);
    if (Release) {
      if (auto E = PutWord(OwnerAddress, OwnerBytes, NextOwner))
        return std::move(E);
    }
    if (auto E = PutWord(A[0], StateBytes, Next))
      return std::move(E);
    if (!Release)
      if (auto E = PutWord(OwnerAddress, OwnerBytes, NextOwner))
        return std::move(E);
    return Value(0);
  };
  if (*Owner == threadID()) {
    if (Unlock) {
      if (Count)
        return SetState(State - CounterStep);
      if (Status == Contended)
        return Unsupported(diagnostic::MutexWake);
      return SetOwnedState(Base, 0, true);
    }
    if (Type == ErrorChecking)
      return Value(Try ? linux_model::ResourceBusy : linux_model::Deadlock);
    if (Count == Counter)
      return Value(linux_model::TryAgain);
    return SetState(State + CounterStep);
  }
  if (Unlock)
    return Value(linux_model::PermissionDenied);
  if (Status != Unlocked)
    return Try ? Value(linux_model::ResourceBusy)
               : Unsupported(diagnostic::MutexBlocking);
  return SetOwnedState(Base | Locked, threadID(), false);
}
} // namespace neverd::emulation::android_model
