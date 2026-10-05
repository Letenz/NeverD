//===- AndroidMutex.cpp - Guest-owned API 28 mutex state -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"
#include "AndroidThreads.h"

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

namespace {
using namespace mutex_abi;
enum class LockMode { Blocking, Try, Resumed };
std::optional<BionicValue> value(uint64_t Value) { return BionicValue(Value); }
} // namespace

llvm::Error Bionic::mutexAccess(uint64_t Address, unsigned Size,
                                unsigned Permissions, unsigned Alignment) {
  if (Address % Alignment)
    return failure(diagnostic::PthreadAlignment);
  return access(Address, Size, Permissions);
}

llvm::Expected<uint64_t> Bionic::readMutexWord(uint64_t Address, unsigned Size,
                                               unsigned Alignment) {
  if (auto E = mutexAccess(Address, Size, Read, Alignment))
    return std::move(E);
  std::array<uint8_t, AttributeBytes> Bytes{};
  if (auto E = CPU.read(Address, llvm::MutableArrayRef(Bytes).take_front(Size)))
    return std::move(E);
  return llvm::support::endian::read64le(Bytes.data());
}

llvm::Error Bionic::writeMutexWord(uint64_t Address, unsigned Size,
                                   uint64_t Value) {
  uint8_t Bytes[AttributeBytes];
  llvm::support::endian::write64le(Bytes, Value);
  return CPU.write(Address, llvm::ArrayRef(Bytes).take_front(Size));
}

std::optional<BionicValue> Bionic::unsupportedMutex(llvm::StringRef Name,
                                                    llvm::StringRef Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = (Name + ": " + Reason).str();
  return std::optional<BionicValue>();
}

BionicResult Bionic::mutexAttributes(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const llvm::StringRef Name(Call.Name);
  if (Name == symbol::MutexAttrInit || Name == symbol::MutexAttrDestroy) {
    if (auto E = mutexAccess(A[0], AttributeBytes, Write, AttributeBytes))
      return std::move(E);
    if (auto E = writeMutexWord(A[0], AttributeBytes,
                                Name == symbol::MutexAttrInit ? 0 : UINT64_MAX))
      return std::move(E);
    return value(0);
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
    return unsupportedMutex(Name, diagnostic::MutexAttributeOperation);
  }
  const uint32_t Argument = static_cast<uint32_t>(A[1]);
  // Bionic validates an int setter argument before dereferencing attr.
  if (Setter && Argument > Maximum)
    return value(linux_model::InvalidArgument);
  auto Attribute = readMutexWord(A[0], AttributeBytes, AttributeBytes);
  if (!Attribute)
    return Attribute.takeError();
  const uint64_t Field = (*Attribute & Mask) >> Shift;
  if (!Setter && Field > Maximum)
    return value(linux_model::InvalidArgument);
  if (Setter) {
    if (auto E = mutexAccess(A[0], AttributeBytes, Write, AttributeBytes))
      return std::move(E);
    if (auto E = writeMutexWord(A[0], AttributeBytes,
                                (*Attribute & ~uint64_t(Mask)) |
                                    (uint64_t(Argument) << Shift)))
      return std::move(E);
  } else {
    if (auto E = mutexAccess(A[1], sizeof(uint32_t), Write, alignof(uint32_t)))
      return std::move(E);
    if (auto E = writeMutexWord(A[1], sizeof(uint32_t), Field))
      return std::move(E);
  }
  return value(0);
}

BionicResult Bionic::initializeMutex(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (auto E = mutexAccess(A[0], MutexBytes, Write, MutexAlignment))
    return std::move(E);
  uint64_t Attribute = 0;
  if (A[1]) {
    auto Loaded = readMutexWord(A[1], AttributeBytes, AttributeBytes);
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
    return unsupportedMutex(Call.Name, diagnostic::MutexPriorityInheritance);
  std::array<uint8_t, MutexBytes> Bytes{};
  if (Type <= ErrorChecking)
    llvm::support::endian::write16le(
        Bytes.data(),
        (Type << TypeShift) | (Attribute & AttributeShared ? Shared : 0));
  if (auto E = CPU.write(A[0], Bytes))
    return std::move(E);
  // An invalid type still clears the complete object in this ABI.
  return value(Type <= ErrorChecking ? 0 : linux_model::InvalidArgument);
}

/// One admitted operation on guest storage. State never survives suspension:
/// resumed locks reload and validate the object before changing any bytes.
class Bionic::Mutex {
  Bionic &Model;
  const uint64_t Address;
  uint16_t State = 0;
  uint32_t Owner = 0;

  unsigned type() const { return State >> TypeShift; }
  unsigned status() const { return State & LockState; }
  uint16_t count() const { return State & Counter; }
  llvm::Error readOwner();
  BionicResult setState(uint16_t Next);
  BionicResult setOwnedState(uint16_t Next, uint32_t NextOwner);
  BionicResult wait();

public:
  Mutex(Bionic &Model, uint64_t Address) : Model(Model), Address(Address) {}
  uint16_t attributes() const { return State & ~(Counter | LockState); }
  llvm::Expected<bool> load(llvm::StringRef Operation);
  BionicResult lock(LockMode Mode);
  BionicResult unlock();
  BionicResult destroy();
};

llvm::Expected<bool> Bionic::Mutex::load(llvm::StringRef Operation) {
  auto Loaded = Model.readMutexWord(Address, StateBytes, MutexAlignment);
  if (!Loaded)
    return Loaded.takeError();
  State = *Loaded;
  if (State == Destroyed)
    return failure(llvm::formatv(diagnostic::DestroyedMutex, Operation).str());
  if (type() > ErrorChecking) {
    Model.unsupportedMutex(Operation, diagnostic::MutexPriorityInheritance);
    return false;
  }
  if (status() > Contended ||
      (count() && (type() != Recursive || status() == Unlocked)))
    return failure(diagnostic::MutexState);
  return true;
}

llvm::Error Bionic::Mutex::readOwner() {
  if (type() == Normal)
    return llvm::Error::success();
  auto Loaded =
      Model.readMutexWord(Address + OwnerOffset, OwnerBytes, alignof(uint32_t));
  if (!Loaded)
    return Loaded.takeError();
  Owner = *Loaded;
  if ((status() == Unlocked) != (Owner == 0))
    return failure(diagnostic::MutexOwner);
  return llvm::Error::success();
}

BionicResult Bionic::Mutex::setState(uint16_t Next) {
  if (auto E = Model.mutexAccess(Address, StateBytes, Write, MutexAlignment))
    return std::move(E);
  if (auto E = Model.writeMutexWord(Address, StateBytes, Next))
    return std::move(E);
  return value(0);
}

BionicResult Bionic::Mutex::setOwnedState(uint16_t Next, uint32_t NextOwner) {
  if (type() == Normal)
    return setState(Next);
  // Validate both destinations before publishing either part. Owner is
  // cleared before release and written after acquisition, as in the ABI.
  if (auto E = Model.mutexAccess(Address, StateBytes, Write, MutexAlignment))
    return std::move(E);
  const uint64_t OwnerAddress = Address + OwnerOffset;
  if (auto E =
          Model.mutexAccess(OwnerAddress, OwnerBytes, Write, alignof(uint32_t)))
    return std::move(E);
  if (!NextOwner)
    if (auto E = Model.writeMutexWord(OwnerAddress, OwnerBytes, 0))
      return std::move(E);
  if (auto E = Model.writeMutexWord(Address, StateBytes, Next))
    return std::move(E);
  if (NextOwner)
    if (auto E = Model.writeMutexWord(OwnerAddress, OwnerBytes, NextOwner))
      return std::move(E);
  return value(0);
}

BionicResult Bionic::Mutex::wait() {
  if (!Model.Threads || !Model.Threads->enabled())
    return Model.unsupportedMutex(symbol::MutexLock, diagnostic::MutexBlocking);
  // The LP64 futex comparison spans state and padding. Nonzero padding
  // would spin on EAGAIN rather than sleep; that retry loop is not modeled.
  auto Word = Model.readMutexWord(Address, FutexBytes, MutexAlignment);
  if (!Word)
    return Word.takeError();
  if (*Word >> (StateBytes * 8))
    return Model.unsupportedMutex(symbol::MutexLock,
                                  diagnostic::MutexWaitPadding);
  auto Updated = setState((State & ~LockState) | Contended);
  if (!Updated)
    return Updated.takeError();
  Model.Threads->waitMutex(Address, attributes());
  return std::optional<BionicValue>(GuestThreadWait{});
}

BionicResult Bionic::Mutex::lock(LockMode Mode) {
  if (auto E = readOwner())
    return std::move(E);
  if (type() != Normal && Owner == Model.threadID()) {
    if (Mode == LockMode::Resumed)
      return failure(diagnostic::MutexOwner);
    if (type() == ErrorChecking)
      return value(Mode == LockMode::Try ? linux_model::ResourceBusy
                                         : linux_model::Deadlock);
    if (count() == Counter)
      return value(linux_model::TryAgain);
    return setState(State + CounterStep);
  }
  if (status() != Unlocked)
    return Mode == LockMode::Try
               ? BionicResult(value(linux_model::ResourceBusy))
               : wait();
  // A woken waiter remains contended even if it is the final sleeper. Its
  // eventual unlock must wake another waiter that may still be queued.
  return setOwnedState(attributes() |
                           (Mode == LockMode::Resumed ? Contended : Locked),
                       Model.threadID());
}

BionicResult Bionic::Mutex::unlock() {
  if (auto E = readOwner())
    return std::move(E);
  if (type() != Normal) {
    if (Owner != Model.threadID())
      return value(linux_model::PermissionDenied);
    if (count())
      return setState(State - CounterStep);
  }
  if (status() == Contended && (!Model.Threads || !Model.Threads->enabled()))
    return Model.unsupportedMutex(symbol::MutexUnlock, diagnostic::MutexWake);
  auto Released = setOwnedState(attributes(), 0);
  if (!Released)
    return Released.takeError();
  if (status() == Contended)
    Model.Threads->wakeMutex(Address);
  return Released;
}

BionicResult Bionic::Mutex::destroy() {
  if (status() != Unlocked)
    return value(linux_model::ResourceBusy);
  return setState(Destroyed);
}

BionicResult Bionic::mutex(const NativeCallEvent &Call) {
  const llvm::StringRef Name(Call.Name);
  if (Name.starts_with(symbol::PthreadMutexAttrPrefix))
    return mutexAttributes(Call);
  if (Name == symbol::MutexInit)
    return initializeMutex(Call);
  if (Name != symbol::MutexLock && Name != symbol::MutexTryLock &&
      Name != symbol::MutexUnlock && Name != symbol::MutexDestroy)
    return unsupportedMutex(Name, diagnostic::MutexOperation);
  Mutex Object(*this, Call.Arguments[0]);
  auto Loaded = Object.load(Name);
  if (!Loaded)
    return Loaded.takeError();
  if (!*Loaded)
    return std::optional<BionicValue>();
  if (Name == symbol::MutexDestroy)
    return Object.destroy();
  if (Name == symbol::MutexUnlock)
    return Object.unlock();
  return Object.lock(Name == symbol::MutexTryLock ? LockMode::Try
                                                  : LockMode::Blocking);
}

BionicResult Bionic::resumeMutex(uint64_t Address, uint16_t Attributes) {
  assert(Threads && Threads->enabled());
  if (auto E = Threads->validateTLS())
    return std::move(E);
  Mutex Object(*this, Address);
  auto Loaded = Object.load(symbol::MutexLock);
  if (!Loaded)
    return Loaded.takeError();
  if (!*Loaded)
    return std::optional<BionicValue>();
  if (Object.attributes() != Attributes)
    return failure(diagnostic::MutexWaitState);
  return Object.lock(LockMode::Resumed);
}
} // namespace neverd::emulation::android_model
