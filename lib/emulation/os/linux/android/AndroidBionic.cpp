//===- AndroidBionic.cpp - Explicit bounded Bionic call models -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../kernel/LinuxTime.h"
#include "AndroidInternal.h"
#include "AndroidThreads.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::android_model {
uint64_t Bionic::tlsAddress() const {
  return Threads ? Threads->tls() : TLSAddress;
}
uint64_t Bionic::threadID() const {
  return Threads ? Threads->id() : linux_model::ThreadID;
}
llvm::Error Bionic::access(uint64_t Address, uint64_t Size,
                           unsigned Permissions) {
  if (!Budget.remainingMicroseconds()) {
    Expired = true;
    return failure(diagnostic::CallTimeout);
  }
  if (!Size)
    return llvm::Error::success();
  if (Size > Options.MemoryLimit)
    return failure(diagnostic::MemoryLimit);
  auto Allowed = CPU.canAccess(Address, Size, Permissions | UserAccessible);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return failure(diagnostic::GuestPointer);
  return llvm::Error::success();
}
llvm::Expected<uint8_t> Bionic::byte(uint64_t Address) {
  if (auto E = access(Address, 1, Read))
    return std::move(E);
  uint8_t Value;
  if (auto E = CPU.read(Address, llvm::MutableArrayRef<uint8_t>(&Value, 1)))
    return std::move(E);
  return Value;
}
llvm::Expected<std::string> Bionic::string(uint64_t Address) {
  std::string Text;
  for (uint64_t I = 0; I < Options.MemoryLimit && Address <= UINT64_MAX - I;
       ++I) {
    auto V = byte(Address + I);
    if (!V)
      return V.takeError();
    if (!*V)
      return Text;
    Text.push_back(*V);
  }
  return failure(diagnostic::UnterminatedString);
}
llvm::Error Bionic::setErrno(uint32_t Value) {
  uint8_t Bytes[4];
  llvm::support::endian::write32le(Bytes, Value);
  return CPU.write(tlsAddress() + ErrnoAddress - TLSAddress, Bytes);
}
llvm::Expected<uint64_t> Bionic::allocate(uint64_t Size) {
  uint64_t Effective = std::max(uint64_t(1), Size);
  if (Effective > Options.MemoryLimit ||
      Effective > UINT64_MAX - PageSize + 1) {
    if (auto E = setErrno(linux_model::NoMemory))
      return std::move(E);
    return uint64_t(0);
  }
  uint64_t Mapped = (Effective + PageSize - 1) & ~(PageSize - 1);
  ProcessServiceEvent Event{
      0,
      0,
      {0, Mapped, linux_model::ProtRead | linux_model::ProtWrite,
       linux_model::MapPrivate | linux_model::MapAnonymous, UINT64_MAX, 0},
      std::nullopt};
  auto Address = Kernel.handle(linux_model::ServiceKind::Mmap, Event);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return failure(diagnostic::AllocatorReturn);
  if (**Address >= uint64_t(0) - 4095) {
    if (auto E = setErrno(uint64_t(0) - **Address))
      return std::move(E);
    return uint64_t(0);
  }
  Allocations.emplace(**Address, Allocation{Size, Mapped});
  return **Address;
}
llvm::Error Bionic::release(uint64_t Address) {
  if (!Address)
    return llvm::Error::success();
  auto I = Allocations.find(Address);
  if (I == Allocations.end())
    return failure(diagnostic::AllocationOwnership);
  ProcessServiceEvent Event{
      0, 0, {Address, I->second.MappedSize, 0, 0, 0, 0}, std::nullopt};
  auto Returned = Kernel.handle(linux_model::ServiceKind::Munmap, Event);
  if (!Returned)
    return Returned.takeError();
  if (!*Returned || **Returned)
    return failure(diagnostic::AllocationRelease);
  Allocations.erase(I);
  return llvm::Error::success();
}
llvm::Expected<uint64_t> Bionic::linkerError(llvm::StringRef Message,
                                             uint64_t ReturnValue) {
  const uint64_t ErrorAddress = tlsAddress() + LinkerErrorAddress - TLSAddress;
  const uint64_t ErrorSlot = tlsAddress() + LinkerErrorSlot - TLSAddress;
  // A fixed guest buffer, separate from errno, TLS ABI slots and constructor
  // argv/envp. A later error may replace its contents; dlerror consumes it
  // once.
  if (Message.size() >= 1024)
    return failure(diagnostic::LinkerErrorLimit);
  std::vector<uint8_t> Bytes(Message.bytes_begin(), Message.bytes_end());
  Bytes.push_back(0);
  if (auto E = access(ErrorAddress, Bytes.size(), Write))
    return std::move(E);
  if (auto E = CPU.write(ErrorAddress, Bytes))
    return std::move(E);
  uint8_t Pointer[8];
  llvm::support::endian::write64le(Pointer, ErrorAddress);
  if (auto E = access(ErrorSlot, sizeof(Pointer), Write))
    return std::move(E);
  if (auto E = CPU.write(ErrorSlot, Pointer))
    return std::move(E);
  return ReturnValue;
}
bool Bionic::isResident(const std::string &Library) const {
  const auto &Scope = Options.Android->DefaultScope;
  return Scope &&
         std::find(Scope->begin(), Scope->end(), Library) != Scope->end();
}
BionicResult Bionic::dlfcn(NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Value = [](uint64_t V) { return std::optional<BionicValue>(V); };
  auto Error = [&](llvm::StringRef Message, uint64_t V = 0) -> BionicResult {
    auto R = linkerError(Message, V);
    if (!R)
      return R.takeError();
    return Value(*R);
  };
  auto Unsupported = [&](llvm::StringRef Detail) -> BionicResult {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = diagnostic::DynamicLinkingPrefix + Detail.str();
    return std::optional<BionicValue>();
  };
  if (Call.Name == symbol::DLError) {
    const uint64_t ErrorSlot = tlsAddress() + LinkerErrorSlot - TLSAddress;
    uint8_t Pointer[8], Empty[8]{};
    if (auto E = access(ErrorSlot, sizeof(Pointer), Read | Write))
      return std::move(E);
    if (auto E = CPU.read(ErrorSlot, Pointer))
      return std::move(E);
    uint64_t Address = llvm::support::endian::read64le(Pointer);
    if (auto E = CPU.write(ErrorSlot, Empty))
      return std::move(E);
    return Value(Address);
  }
  if (Call.Name == symbol::DLOpen) {
    if (!A[0])
      return Unsupported(diagnostic::NullOpenScope);
    auto Library = string(A[0]);
    if (!Library)
      return Library.takeError();
    if (Library->size() > 1024)
      return failure(diagnostic::LibraryNameLimit);
    Call.Library = *Library;
    // API 28 LP64 accepts any combination of these bits, including zero
    // and NOLOAD alone. Scope promotion and NODELETE need a fuller loader.
    uint32_t Flags = static_cast<uint32_t>(A[1]);
    if (Flags & ~0x1107u)
      return Error(diagnostic::OpenFlags);
    if (Flags & 0x1100u)
      return Unsupported(diagnostic::OpenScopeFlags);
    if (!Linked.Libraries.count(*Library))
      return Error(diagnostic::LibraryAbsent);
    auto &State = OpenLibraries[*Library];
    if (!State.References && !isResident(*Library) && (Flags & 4u))
      return Error(diagnostic::LibraryNotOpen);
    if (!State.Handle) {
      if (NextHandle > UINT64_MAX - 2)
        return failure(diagnostic::LibraryHandleLimit);
      State.Handle = NextHandle;
      NextHandle += 2;
      Handles.emplace(State.Handle, *Library);
    }
    if (State.References == UINT64_MAX)
      return failure(diagnostic::LibraryReferenceOverflow);
    ++State.References;
    return Value(State.Handle);
  }
  if (Call.Name == symbol::DLSym) {
    if (!A[1])
      return Error(diagnostic::NullSymbol);
    auto Symbol = string(A[1]);
    if (!Symbol)
      return Symbol.takeError();
    if (Symbol->size() > 1024)
      return failure(diagnostic::SymbolNameLimit);
    Call.Symbol = *Symbol;
    if (!A[0]) {
      const auto &Scope = Options.Android->DefaultScope;
      if (!Scope)
        return Unsupported(diagnostic::DefaultScope);
      for (const auto &Library : *Scope) {
        const auto &Symbols = Linked.Libraries.at(Library);
        auto I = Symbols.find(*Symbol);
        if (I != Symbols.end()) {
          Call.Library = Library;
          return Value(I->second);
        }
      }
      return Error(diagnostic::DefaultSymbolAbsent);
    }
    if (A[0] == UINT64_MAX)
      return Unsupported(diagnostic::NextScope);
    auto Handle = Handles.find(A[0]);
    if (Handle == Handles.end())
      return Error(diagnostic::LibraryHandle);
    Call.Library = Handle->second;
    const auto &Symbols = Linked.Libraries.at(Handle->second);
    auto I = Symbols.find(*Symbol);
    if (I == Symbols.end())
      return Error(diagnostic::LibrarySymbolAbsent);
    return Value(I->second);
  }
  if (Call.Name == symbol::DLClose) {
    auto Handle = Handles.find(A[0]);
    if (Handle == Handles.end())
      return Error(diagnostic::LibraryHandle, UINT64_MAX);
    Call.Library = Handle->second;
    auto &State = OpenLibraries.at(Handle->second);
    if (!State.References)
      return Error(diagnostic::LibraryReferences, UINT64_MAX);
    if (!--State.References && !isResident(Handle->second)) {
      Handles.erase(Handle);
      State.Handle = 0;
    }
    return Value(0);
  }
  return failure(diagnostic::DlfcnDispatch);
}
BionicResult Bionic::once(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (A[0] % once_abi::ControlBytes)
    return failure(diagnostic::OnceControlAlignment);
  if (auto E = access(A[0], once_abi::ControlBytes, Read))
    return std::move(E);
  uint8_t Bytes[once_abi::ControlBytes];
  if (auto E = CPU.read(A[0], Bytes))
    return std::move(E);
  uint32_t State = llvm::support::endian::read32le(Bytes);
  if (State == once_abi::Complete)
    return std::optional<BionicValue>(uint64_t(0));
  if (State == once_abi::Running && Threads && Threads->enabled())
    return Threads->waitOnce(A[0]);
  if (State != once_abi::NotStarted ||
      (Threads && Threads->enabled() && Threads->onceActive(A[0]))) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = State == once_abi::Running
                            ? diagnostic::OnceInProgress
                            : diagnostic::OnceControlState;
    return std::optional<BionicValue>();
  }
  if (auto E = access(A[0], once_abi::ControlBytes, Write))
    return std::move(E);
  if (A[1] % 4)
    return failure(diagnostic::OnceInitializerAlignment);
  if (auto E = access(A[1], 4, Execute))
    return std::move(E);
  llvm::support::endian::write32le(Bytes, once_abi::Running);
  if (auto E = CPU.write(A[0], Bytes))
    return std::move(E);
  return std::optional<BionicValue>(
      GuestCallback{A[1], std::nullopt, OnceCallback{A[0]}});
}
llvm::Error Bionic::finishOnce(const OnceCallback &Callback) {
  if (auto E = access(Callback.Control, once_abi::ControlBytes, Write))
    return E;
  uint8_t Bytes[once_abi::ControlBytes];
  llvm::support::endian::write32le(Bytes, once_abi::Complete);
  if (auto E = CPU.write(Callback.Control, Bytes))
    return E;
  if (Threads && Threads->enabled())
    Threads->completeOnce(Callback.Control);
  return llvm::Error::success();
}
BionicResult Bionic::invoke(NativeCallEvent &Call) {
  if (Threads && Threads->enabled())
    if (auto E = Threads->validateTLS())
      return std::move(E);
  llvm::StringRef Name(Call.Name);
  const auto &A = Call.Arguments;
  auto Value = [](uint64_t V) { return std::optional<BionicValue>(V); };
  if (!Call.Library.empty()) {
    auto I = OpenLibraries.find(Call.Library);
    if (!isResident(Call.Library) &&
        (I == OpenLibraries.end() || !I->second.References)) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::InactiveProviderPrefix + Call.Library;
      return std::optional<BionicValue>();
    }
  }
  if (Name == symbol::DLOpen || Name == symbol::DLSym ||
      Name == symbol::DLClose || Name == symbol::DLError)
    return dlfcn(Call);
  if (Name == symbol::PthreadOnce)
    return once(Call);
  if (Name.starts_with(symbol::PthreadAttrPrefix))
    return threadAttributes(Call);
  if (Threads && Threads->enabled() &&
      (Name == symbol::ThreadCreate || Name == symbol::ThreadJoin ||
       Name == symbol::ThreadDetach || Name == symbol::ThreadSelf ||
       Name == symbol::ThreadEqual || Name == symbol::ThreadExit ||
       Name == symbol::ThreadGetAttr || Name == symbol::ThreadGetTID))
    return Threads->invoke(Call);
  if (Name == symbol::CxaAtExit)
    return registerExit(Call);
  if (Name == symbol::CxaFinalize)
    return finalize(A[0]);
  if (Name == symbol::StrtokR) {
    auto R = tokenize(Call);
    if (!R)
      return R.takeError();
    return Value(*R);
  }
  if (Name == symbol::Strchr || Name == symbol::Strrchr ||
      Name == symbol::StrchrChk || Name == symbol::StrrchrChk) {
    auto R = findCharacter(Call);
    if (!R)
      return R.takeError();
    return Value(*R);
  }
  if (Name.starts_with(symbol::PthreadMutexPrefix))
    return mutex(Call);
  if (Name == symbol::Snprintf || Name == symbol::Vsnprintf ||
      Name == symbol::Sprintf || Name == symbol::Vsprintf)
    return format(Call);
  if (Name == symbol::Sscanf || Name == symbol::Vsscanf)
    return scan(Call);
  if (Name == symbol::Errno)
    return Value(tlsAddress() + ErrnoAddress - TLSAddress);
  if (Name == symbol::GetPageSize)
    return Value(PageSize);
  if (Name == symbol::Time) {
    auto Now =
        linux_model::clockValue(linux_model::ClockRealtime, Options, Result);
    if (!Now)
      return std::optional<BionicValue>();
    // API 28's fallback obtains a timeval, then stores through the caller's
    // time_t*. This is a user-space store, not the x64 time syscall's EFAULT.
    if (A[0]) {
      if (auto E = access(A[0], 8, Write))
        return std::move(E);
      uint8_t Bytes[8];
      llvm::support::endian::write64le(Bytes,
                                       static_cast<uint64_t>(Now->Seconds));
      if (auto E = CPU.write(A[0], Bytes))
        return std::move(E);
    }
    // A valid negative timestamp is not a Linux errno return.
    return Value(static_cast<uint64_t>(Now->Seconds));
  }
  if (Name == symbol::DeviceAPILevel)
    return Value(28);
  if (Name == symbol::StackCheckFail || Name == symbol::Abort)
    return failure(diagnostic::GuestCalledPrefix + Name);
  if (Name == symbol::Memcpy || Name == symbol::Memmove ||
      Name == symbol::Memset || Name == symbol::Memcmp) {
    uint64_t Size = A[2];
    if (auto E = access(A[0], Size, Name == symbol::Memcmp ? Read : Write))
      return std::move(E);
    if (Name != symbol::Memset)
      if (auto E = access(A[1], Size, Read))
        return std::move(E);
    if (Name == symbol::Memcpy && Size &&
        (A[0] <= A[1] ? A[1] - A[0] < Size : A[0] - A[1] < Size))
      return failure(diagnostic::MemcpyOverlap);
    std::vector<uint8_t> Bytes(Size, static_cast<uint8_t>(A[1]));
    if (Name != symbol::Memset && Size)
      if (auto E = CPU.read(A[1], Bytes))
        return std::move(E);
    if (Name == symbol::Memcmp) {
      std::vector<uint8_t> Left(Size);
      if (Size)
        if (auto E = CPU.read(A[0], Left))
          return std::move(E);
      for (uint64_t I = 0; I < Size; ++I)
        if (Left[I] != Bytes[I])
          return Value(static_cast<uint32_t>(int(Left[I]) - int(Bytes[I])));
      return Value(0);
    }
    if (Size)
      if (auto E = CPU.write(A[0], Bytes))
        return std::move(E);
    return Value(A[0]);
  }
  if (Name == symbol::Strlen || Name == symbol::Strnlen) {
    uint64_t Bound = Name == symbol::Strnlen
                         ? std::min(A[1], Options.MemoryLimit)
                         : Options.MemoryLimit;
    for (uint64_t I = 0; I < Bound; ++I) {
      if (A[0] > UINT64_MAX - I)
        return failure(diagnostic::StringAddressOverflow);
      auto V = byte(A[0] + I);
      if (!V)
        return V.takeError();
      if (!*V)
        return Value(I);
    }
    if (Name == symbol::Strnlen && Bound == A[1])
      return Value(Bound);
    return failure(diagnostic::StringScanLimit);
  }
  if (Name == symbol::Strcmp || Name == symbol::Strncmp) {
    uint64_t Bound = Name == symbol::Strncmp
                         ? std::min(A[2], Options.MemoryLimit)
                         : Options.MemoryLimit;
    for (uint64_t I = 0; I < Bound; ++I) {
      if (A[0] > UINT64_MAX - I || A[1] > UINT64_MAX - I)
        return failure(diagnostic::StringAddressOverflow);
      auto Left = byte(A[0] + I);
      if (!Left)
        return Left.takeError();
      auto Right = byte(A[1] + I);
      if (!Right)
        return Right.takeError();
      if (*Left != *Right)
        return Value(static_cast<uint32_t>(int(*Left) - int(*Right)));
      if (!*Left)
        return Value(0);
    }
    if (Name == symbol::Strncmp && Bound == A[2])
      return Value(0);
    return failure(diagnostic::StringCompareLimit);
  }
  if (Name == symbol::SystemPropertyGet) {
    auto Key = string(A[0]);
    if (!Key)
      return Key.takeError();
    auto I = Options.Android->Properties.find(*Key);
    std::string Text = I == Options.Android->Properties.end() ? "" : I->second;
    if (auto E = access(A[1], Text.size() + 1, Write))
      return std::move(E);
    std::vector<uint8_t> Bytes(Text.begin(), Text.end());
    Bytes.push_back(0);
    if (auto E = CPU.write(A[1], Bytes))
      return std::move(E);
    return Value(Text.size());
  }
  if (Name == symbol::Malloc || Name == symbol::Calloc ||
      Name == symbol::Realloc) {
    uint64_t Size = Name == symbol::Realloc ? A[1] : A[0];
    if (Name == symbol::Calloc) {
      if (A[0] && A[1] > UINT64_MAX / A[0]) {
        if (auto E = setErrno(linux_model::NoMemory))
          return std::move(E);
        return Value(0);
      }
      Size = A[0] * A[1];
    }
    std::vector<uint8_t> Saved;
    if (Name == symbol::Realloc && A[0]) {
      auto I = Allocations.find(A[0]);
      if (I == Allocations.end())
        return failure(diagnostic::ReallocationOwnership);
      uint64_t Copy = std::min(Size, I->second.Size);
      if (auto E = access(A[0], Copy, Read))
        return std::move(E);
      Saved.resize(Copy);
      if (Copy)
        if (auto E = CPU.read(A[0], Saved))
          return std::move(E);
    }
    auto Address = allocate(Size);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      return Value(0);
    // Anonymous allocations start zeroed, including calloc and realloc growth.
    if (!Saved.empty())
      if (auto E = CPU.write(*Address, Saved))
        return std::move(E);
    if (Name == symbol::Realloc)
      if (auto E = release(A[0]))
        return std::move(E);
    return Value(*Address);
  }
  if (Name == symbol::Free) {
    if (auto E = release(A[0]))
      return std::move(E);
    return Value(0); // The ABI leaves x0 unspecified for void calls.
  }
  std::optional<linux_model::ServiceKind> Kind;
#define NEVERD_ANDROID_KERNEL_SERVICE(Symbol, Service)                         \
  if (Name == Symbol)                                                          \
    Kind = linux_model::ServiceKind::Service;
#include "AndroidKernelServices.def"
#undef NEVERD_ANDROID_KERNEL_SERVICE
  const bool IsSyscall = Name == symbol::Syscall;
  if (Kind || IsSyscall) {
    // API 28 Bionic rejects flags before calling the three-argument syscall.
    if (Kind == linux_model::ServiceKind::FaccessAt && uint32_t(A[3])) {
      if (auto E = setErrno(linux_model::InvalidArgument))
        return std::move(E);
      return Value(UINT64_MAX);
    }
    ProcessServiceEvent Event{Call.PC, IsSyscall ? A[0] : 0, {}, std::nullopt};
    // AArch64 Bionic syscall(number, ...) shifts x1..x6 into the six
    // kernel argument registers. The shared Linux table owns the number.
    std::copy_n(A.begin() + (IsSyscall ? 1 : 0), Event.Arguments.size(),
                Event.Arguments.begin());
    // Raw Linux service semantics are shared. Bionic alone owns errno/-1.
    auto *Thread = Threads ? Threads->kernel() : nullptr;
    auto Returned = Kind ? Kernel.handle(*Kind, Event, Thread)
                         : Kernel.handle(Event, Thread);
    if (!Returned)
      return Returned.takeError();
    if (!*Returned)
      return std::optional<BionicValue>();
    if (**Returned >= uint64_t(0) - 4095) {
      if (auto E = setErrno(uint64_t(0) - **Returned))
        return std::move(E);
      return Value(UINT64_MAX);
    }
    return Value(**Returned);
  }
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = diagnostic::ImportPrefix + Name.str();
  return std::optional<BionicValue>();
}
} // namespace neverd::emulation::android_model
