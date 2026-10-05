//===- AndroidThreads.cpp - Deterministic guest pthread scheduling ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidThreads.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
namespace {
using namespace thread_attribute_abi;
std::array<uint8_t, ObjectBytes> defaultAttributes() {
  std::array<uint8_t, ObjectBytes> Bytes{};
  llvm::support::endian::write64le(Bytes.data() + StackSizeOffset,
                                   DefaultStackSize);
  llvm::support::endian::write64le(Bytes.data() + GuardSizeOffset, PageSize);
  return Bytes;
}
void stackAttributes(std::array<uint8_t, ObjectBytes> &Bytes,
                     const NativeThreadSnapshot &Thread) {
  llvm::support::endian::write64le(Bytes.data() + StackBaseOffset,
                                   Thread.StackBase);
  llvm::support::endian::write64le(Bytes.data() + StackSizeOffset,
                                   Thread.StackSize);
  llvm::support::endian::write64le(Bytes.data() + GuardSizeOffset,
                                   Thread.GuardSize);
  auto Flags = llvm::support::endian::read32le(Bytes.data() + FlagsOffset);
  Flags = (Flags & ~DetachedFlag) | (Thread.Detached ? DetachedFlag : 0);
  llvm::support::endian::write32le(Bytes.data() + FlagsOffset, Flags);
}
std::optional<BionicValue> value(uint64_t V) { return BionicValue(V); }
} // namespace

llvm::Error GuestThreads::access(uint64_t Address, uint64_t Size,
                                 unsigned Permissions, unsigned Alignment) {
  if (Address % Alignment)
    return failure(diagnostic::PthreadAlignment);
  auto Allowed = CPU.canAccess(Address, Size, Permissions | UserAccessible);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return failure(diagnostic::GuestPointer);
  return llvm::Error::success();
}
llvm::Error GuestThreads::put64(uint64_t Address, uint64_t Value) {
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, Value);
  return CPU.write(Address, Bytes);
}
llvm::Error GuestThreads::initialize(uint64_t StackBase) {
  Thread Main;
  Main.Report = {linux_model::ThreadID,
                 thread_model::ArenaBase - PageSize,
                 TLSAddress,
                 StackBase,
                 Options.StackSize,
                 0};
  Main.Kernel.ID = Main.Report.ID;
  Main.Attributes = defaultAttributes();
  stackAttributes(Main.Attributes, Main.Report);
  Threads.push_back(std::move(Main));
  if (enabled())
    return put64(TLSAddress + thread_model::IdentityOffset,
                 current().Report.Handle);
  return llvm::Error::success();
}
BionicResult GuestThreads::unsupported(llvm::StringRef Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason.str();
  return std::optional<BionicValue>();
}
llvm::Error GuestThreads::validateTLS() {
  auto Register = CPU.readRegister(CPURegister::AArch64TPIDR_EL0);
  if (!Register)
    return Register.takeError();
  if ((*Register)[0] != tls())
    return failure(diagnostic::ThreadTLS);
  if (auto E = access(tls(), 16, Read))
    return E;
  uint8_t Bytes[16];
  if (auto E = CPU.read(tls(), Bytes))
    return E;
  if (llvm::support::endian::read64le(Bytes) != tls() ||
      llvm::support::endian::read64le(Bytes + thread_model::IdentityOffset) !=
          current().Report.Handle)
    return failure(diagnostic::ThreadTLS);
  return llvm::Error::success();
}
BionicResult GuestThreads::create(const NativeCallEvent &Call) {
  using namespace thread_attribute_abi;
  const auto &A = Call.Arguments;
  Thread Child;
  Child.Attributes = defaultAttributes();
  if (A[1]) {
    if (auto E = access(A[1], ObjectBytes, Read))
      return std::move(E);
    if (auto E = CPU.read(A[1], Child.Attributes))
      return std::move(E);
  }
  const auto *Bytes = Child.Attributes.data();
  const uint32_t Flags = llvm::support::endian::read32le(Bytes + FlagsOffset);
  const uint64_t Base =
      llvm::support::endian::read64le(Bytes + StackBaseOffset);
  uint64_t Size = llvm::support::endian::read64le(Bytes + StackSizeOffset);
  uint64_t Guard = llvm::support::endian::read64le(Bytes + GuardSizeOffset);
  const uint32_t Policy = llvm::support::endian::read32le(Bytes + PolicyOffset);
  const uint32_t Priority =
      llvm::support::endian::read32le(Bytes + PriorityOffset);
  // Explicit model subset. Inherited policy is always the parent's normal
  // policy. Legacy API 28 flags also ignore priority for SCHED_NORMAL.
  const bool SetPolicy = !(Flags & InheritFlag) &&
                         ((Flags & ExplicitFlag) || Policy != NormalPolicy);
  if ((Flags & ~(DetachedFlag | InheritFlag | ExplicitFlag)) || Base ||
      (SetPolicy && (Policy != NormalPolicy || Priority)))
    return unsupported(diagnostic::ThreadAttributes);
  if (Threads.size() >= Options.Android->ThreadLimit ||
      Size < MinimumStackSize ||
      Size > thread_model::ArenaStride - 3 * PageSize ||
      Guard > thread_model::ArenaStride - Size - 3 * PageSize)
    return value(linux_model::TryAgain);
  Size = (Size + PageSize - 1) & ~(PageSize - 1);
  Guard = (Guard + PageSize - 1) & ~(PageSize - 1);
  auto &Space = *CPU.addressSpace();
  const uint64_t MappingSize = Guard + Size + PageSize;
  if (MappingSize > Options.MemoryLimit - Space.mappedBytes())
    return value(linux_model::TryAgain);
  if (auto E = access(A[0], 8, Write))
    return std::move(E);
  if (A[2] % 4)
    return failure(diagnostic::ThreadEntry);
  if (auto E = access(A[2], 4, Execute, 4))
    return std::move(E);

  // The first page in each slot is an unmapped opaque handle, not a guessed
  // pthread_internal_t. Stack guards have mapped PROT_NONE storage; TLS follows
  // the stack in this documented model placement.
  const uint64_t Handle = thread_model::ArenaBase +
                          (Threads.size() - 1) * thread_model::ArenaStride;
  const uint64_t MappingBase = Handle + PageSize;
  if (auto E =
          Space.map(MappingBase, MappingSize, Read | Write | UserAccessible)) {
    if (!E.isA<GuestMemoryLimitError>())
      return std::move(E);
    llvm::consumeError(std::move(E));
    return value(linux_model::TryAgain);
  }
  auto Rollback = llvm::scope_exit(
      [&] { llvm::consumeError(Space.unmap(MappingBase, MappingSize)); });
  if (Guard)
    if (auto E = Space.protect(MappingBase, Guard, 0))
      return std::move(E);
  Child.MappingBase = MappingBase;
  Child.MappingSize = MappingSize;
  Child.Report = {linux_model::ThreadID + Threads.size(),
                  Handle,
                  MappingBase + Guard + Size,
                  MappingBase,
                  Guard + Size,
                  Guard};
  Child.Report.Detached = Flags & DetachedFlag;
  Child.Kernel.ID = Child.Report.ID;
  stackAttributes(Child.Attributes, Child.Report);
  if (auto E = put64(Child.Report.TLS, Child.Report.TLS))
    return std::move(E);
  if (auto E = put64(Child.Report.TLS + thread_model::IdentityOffset, Handle))
    return std::move(E);
  if (auto E = put64(Child.Report.TLS + GuardAddress - TLSAddress, StackGuard))
    return std::move(E);

  // Creating the entry state executes no child instructions. Save and restore
  // the parent in full, including its live stack, flags, vectors and FP mode.
  auto Parent = CPU.saveContext();
  if (!Parent)
    return Parent.takeError();
  auto Prepare = [&]() -> llvm::Error {
    auto Frame =
        Calls.prepareCall(CPU, MappingBase + Guard, Size, ReturnPC, {A[3]});
    if (!Frame)
      return Frame.takeError();
    Child.ReturnSP = Frame->ReturnStackPointer;
    if (auto E = CPU.writeRegister(CPURegister::AArch64TPIDR_EL0,
                                   {Child.Report.TLS, 0}))
      return E;
    if (auto E = CPU.writeRegister(CPURegister::AArch64PC, {A[2], 0}))
      return E;
    auto Context = CPU.saveContext();
    if (!Context)
      return Context.takeError();
    Child.Context = std::move(*Context);
    return llvm::Error::success();
  };
  auto Prepared = Prepare();
  if (auto E =
          llvm::joinErrors(std::move(Prepared), CPU.restoreContext(**Parent)))
    return std::move(E);
  // Publish only after all input, memory and CPU preparation succeeds.
  if (auto E = put64(A[0], Handle))
    return std::move(E);
  Threads.push_back(std::move(Child));
  Rollback.release();
  return value(0);
}

llvm::Error GuestThreads::retire(size_t Index) {
  auto &T = Threads[Index];
  if (T.MappingSize) {
    if (auto E = CPU.addressSpace()->unmap(T.MappingBase, T.MappingSize))
      return E;
    T.MappingSize = 0;
  }
  T.Context.reset();
  T.Report.Retired = true;
  return llvm::Error::success();
}
std::optional<size_t> GuestThreads::onceOwner(uint64_t Control) const {
  for (size_t I = 0; I < Threads.size(); ++I)
    for (const auto &Pending : Threads[I].Callbacks)
      if (const auto *Once =
              std::get_if<OnceCallback>(&Pending.Call.Continuation))
        if (Once->Control == Control)
          return I;
  return std::nullopt;
}
bool GuestThreads::onceActive(uint64_t Control) const {
  return onceOwner(Control).has_value();
}
BionicResult GuestThreads::waitOnce(uint64_t Control) {
  const auto Owner = onceOwner(Control);
  // A raw running value is not evidence of an initializer that can finish.
  // Same-thread recursion and externally forged controls remain unsupported.
  if (!Owner || *Owner == Current)
    return unsupported(diagnostic::OnceInProgress);
  assert(!current().Waiting && "running thread already has a pending wait");
  current().Waiting = Wait{Once{*Owner, Control}, std::nullopt};
  return std::optional<BionicValue>(GuestThreadWait{});
}
void GuestThreads::completeOnce(uint64_t Control) {
  // Bionic calls this only after the actual initializer returns and the
  // completion word has been written successfully to shared guest memory.
  for (auto &T : Threads)
    if (T.Waiting)
      if (auto *Once = std::get_if<GuestThreads::Once>(&T.Waiting->Operation))
        if (Once->Owner == Current && Once->Control == Control)
          Once->Ready = true;
}
void GuestThreads::waitMutex(uint64_t Address, uint16_t Attributes) {
  auto &Waiting = current().Waiting;
  if (Waiting) {
    auto &Mutex = std::get<GuestThreads::Mutex>(Waiting->Operation);
    assert(Mutex.Address == Address && Mutex.Attributes == Attributes);
    // A wake does not grant ownership. Preserve the suspended request and
    // its event when another thread acquires the lock before this one runs.
    Mutex.Ready = false;
    return;
  }
  Waiting = Wait{Mutex{Address, Attributes}, std::nullopt};
}

void GuestThreads::wakeMutex(uint64_t Address) {
  // FUTEX_WAKE(1) has no guest ordering guarantee. Choose one sleeper in the
  // same deterministic round-robin order used to schedule runnable threads.
  for (size_t Offset = 1; Offset <= Threads.size(); ++Offset) {
    auto &Waiting = Threads[(Current + Offset) % Threads.size()].Waiting;
    if (!Waiting)
      continue;
    auto *Mutex = std::get_if<GuestThreads::Mutex>(&Waiting->Operation);
    if (Mutex && Mutex->Address == Address && !Mutex->Ready) {
      Mutex->Ready = true;
      return;
    }
  }
}

bool GuestThreads::ready(const Wait &Pending) const {
  if (const auto *J = std::get_if<Join>(&Pending.Operation))
    return Threads[J->Target].Report.Finished;
  if (const auto *O = std::get_if<Once>(&Pending.Operation))
    return O->Ready;
  return std::get<Mutex>(Pending.Operation).Ready;
}

llvm::Error GuestThreads::prepareJoin(const Join &Pending) {
  if (!Pending.Output)
    return llvm::Error::success();
  if (auto E = access(Pending.Output, 8, Write))
    return E;
  return put64(Pending.Output, *Threads[Pending.Target].Report.ReturnValue);
}

BionicResult GuestThreads::resumeWait(Bionic &LibC) {
  const auto &Pending = *current().Waiting;
  assert(Pending.Request && "scheduled wait has no suspended service");
  // Resumption can fail before another guest instruction executes. Attribute
  // that stop to this service, not the previous thread's last instruction.
  Result.PC = Pending.Request->PC;
  const auto &Operation = Pending.Operation;
  if (const auto *M = std::get_if<Mutex>(&Operation))
    return LibC.resumeMutex(M->Address, M->Attributes);
  if (const auto *J = std::get_if<Join>(&Operation)) {
    if (auto E = prepareJoin(*J))
      return std::move(E);
    return value(0);
  }
  const auto &Once = std::get<GuestThreads::Once>(Operation);
  // Recheck memory after suspension: a different thread may have unmapped
  // or modified it. Failure must leave the original import incomplete.
  if (auto E = access(Once.Control, once_abi::ControlBytes, Read,
                      once_abi::ControlBytes))
    return std::move(E);
  uint8_t Bytes[once_abi::ControlBytes];
  if (auto E = CPU.read(Once.Control, Bytes))
    return std::move(E);
  if (llvm::support::endian::read32le(Bytes) != once_abi::Complete)
    return failure(diagnostic::OnceWaitControl);
  return value(0);
}

llvm::Error GuestThreads::completeWait(uint64_t Value) {
  const auto &Pending = *current().Waiting;
  if (Pending.Request) {
    if (auto E = linux_model::returnService(CPU, *Pending.Request, Value))
      return E;
    Result.NativeCalls[Pending.Event].Result = Value;
  }
  if (const auto *J = std::get_if<Join>(&Pending.Operation))
    if (auto E = retire(J->Target))
      return E;
  current().Waiting.reset();
  return llvm::Error::success();
}
BionicResult GuestThreads::invoke(const NativeCallEvent &Call) {
  if (!enabled())
    return unsupported(diagnostic::ThreadDisabled);
  const auto &A = Call.Arguments;
  const llvm::StringRef Name(Call.Name);
  if (Name == symbol::ThreadCreate)
    return create(Call);
  if (Name == symbol::ThreadSelf)
    return value(current().Report.Handle);
  if (Name == symbol::ThreadEqual)
    return value(A[0] == A[1]);
  if (Name == symbol::ThreadExit) {
    if (!callbacks().empty())
      return unsupported(diagnostic::ThreadCallbackExit);
    return std::optional<BionicValue>(GuestThreadExit{A[0]});
  }
  if (Name == symbol::ThreadJoin && A[0] == current().Report.Handle)
    return value(linux_model::Deadlock);
  if (!A[0])
    return value(Name == symbol::ThreadGetTID ? uint64_t(UINT32_MAX)
                                              : linux_model::NoSuchProcess);
  size_t Index = 0;
  while (Index < Threads.size() && (Threads[Index].Report.Handle != A[0] ||
                                    Threads[Index].Report.Retired))
    ++Index;
  // API 28's target>=O invalid non-null pthread_t boundary is fatal.
  if (Index == Threads.size())
    return failure(diagnostic::ThreadHandle);
  auto &Target = Threads[Index];
  if (Name == symbol::ThreadGetTID)
    return value(Target.Report.Finished ? 0 : Target.Kernel.ID);
  if (Name == symbol::ThreadGetAttr) {
    if (auto E = access(A[1], Target.Attributes.size(), Write))
      return std::move(E);
    auto Bytes = Target.Attributes;
    stackAttributes(Bytes, Target.Report);
    if (auto E = CPU.write(A[1], Bytes))
      return std::move(E);
    return value(0);
  }
  if (Target.Report.Detached || Target.Joiner)
    return value(linux_model::InvalidArgument);
  if (Name == symbol::ThreadDetach) {
    Target.Report.Detached = true;
    if (Target.Report.Finished)
      if (auto E = retire(Index))
        return std::move(E);
    return value(0);
  }
  assert(Name == symbol::ThreadJoin && "unexpected thread operation");
  if (A[1])
    if (auto E = access(A[1], 8, Write))
      return std::move(E);
  Target.Joiner = Current;
  current().Waiting = Wait{Join{Index, A[1]}, std::nullopt};
  if (Target.Report.Finished) {
    if (auto E = prepareJoin(std::get<Join>(current().Waiting->Operation)))
      return std::move(E);
    if (auto E = completeWait(0))
      return std::move(E);
    return value(0);
  }
  return std::optional<BionicValue>(GuestThreadWait{});
}
void GuestThreads::suspend(const ServiceRequest &Request, size_t Event) {
  assert(current().Waiting && !current().Waiting->Request);
  current().Waiting->Request = Request;
  current().Waiting->Event = Event;
}
llvm::Error GuestThreads::finish(uint64_t Value, uint32_t ExitStatus) {
  LastExitStatus = ExitStatus;
  current().Report.Finished = true;
  current().Report.ReturnValue = Value;
  current().Kernel.Exit.reset();
  if (current().Report.Detached)
    return retire(Current);
  return llvm::Error::success();
}
std::optional<size_t> GuestThreads::nextRunnable() const {
  for (size_t Offset = 1; Offset <= Threads.size(); ++Offset) {
    size_t Candidate = (Current + Offset) % Threads.size();
    const auto &T = Threads[Candidate];
    if (!T.Report.Finished && (!T.Waiting || ready(*T.Waiting)))
      return Candidate;
  }
  return std::nullopt;
}

llvm::Error GuestThreads::switchTo(size_t Next) {
  if (Next == Current)
    return llvm::Error::success();
  if (!current().Report.Finished) {
    if (current().Context) {
      if (auto E = CPU.saveContext(*current().Context))
        return E;
    } else {
      auto Saved = CPU.saveContext();
      if (!Saved)
        return Saved.takeError();
      current().Context = std::move(*Saved);
    }
  }
  if (auto E = CPU.restoreContext(*Threads[Next].Context))
    return E;
  Current = Next;
  return llvm::Error::success();
}

llvm::Expected<bool> GuestThreads::schedule(Bionic &LibC) {
  while (auto Next = nextRunnable()) {
    if (auto E = switchTo(*Next))
      return std::move(E);
    if (!current().Waiting)
      return true;
    auto Resumed = resumeWait(LibC);
    if (!Resumed)
      return Resumed.takeError();
    if (!*Resumed)
      return false;
    if (std::holds_alternative<GuestThreadWait>(**Resumed))
      continue;
    if (auto E = completeWait(std::get<uint64_t>(**Resumed)))
      return std::move(E);
    return true;
  }
  for (const auto &T : Threads) {
    if (!T.Report.Finished) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::ThreadDeadlock;
      return false;
    }
  }
  if (Result.ReturnValue)
    Result.Stop = ProcessStopReason::Returned;
  else {
    Result.Stop = ProcessStopReason::Exited;
    Result.ExitStatus = LastExitStatus;
  }
  return false;
}
void GuestThreads::report() {
  if (!enabled())
    return;
  for (const auto &T : Threads) {
    auto Snapshot = T.Report;
    Snapshot.Waiting = T.Waiting.has_value();
    Result.NativeThreads.push_back(Snapshot);
  }
}
} // namespace neverd::emulation::android_model
