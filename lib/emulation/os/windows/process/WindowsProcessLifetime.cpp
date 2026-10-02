//===- WindowsProcessLifetime.cpp - Module initialization and exit --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessLifetime.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation::windows_process {
using namespace value;
Lifetime::Lifetime(const windows_process::Program &Program)
    : Program(Program), States(Program.Modules.size()) {
  for (size_t I : Program.InitializationOrder) {
    Pending.push_back({I, CallKind::TLS, DLLProcessAttach});
    Pending.push_back({I, CallKind::DLL, DLLProcessAttach});
  }
  Pending.push_back({0, CallKind::TLS, DLLProcessAttach});
  Pending.push_back({0, CallKind::Entry, 0});
}
void Lifetime::advance() {
  ++Position;
  Callback = 0;
  CallbackArray.reset();
}
llvm::Expected<std::optional<Lifetime::Call>>
Lifetime::next(ExecutionBackend &CPU) {
  if (Running)
    return failure(text::Lifetime);
  while (Position < Pending.size()) {
    const auto &N = Pending[Position];
    const auto &M = Program.Modules[N.Module].Loaded;
    uint64_t Target = M.Entry;
    if (N.Kind == CallKind::TLS) {
      if (!CallbackArray) {
        uint64_t Array = 0;
        if (M.TLSCallbackPointer) {
          auto V = CPU.readInteger(M.TLSCallbackPointer, PointerSize);
          if (!V)
            return V.takeError();
          Array = *V;
        }
        CallbackArray = Array;
      }
      Target = 0;
      if (*CallbackArray) {
        if (Callback > MaxCallbacks || *CallbackArray >= UserLimit ||
            Callback * PointerSize + PointerSize > UserLimit - *CallbackArray)
          return failure(text::TLS);
        const uint64_t Slot = *CallbackArray + Callback * PointerSize;
        auto Access = CPU.canAccess(Slot, PointerSize, Read | UserAccessible);
        if (!Access)
          return Access.takeError();
        if (!*Access)
          return failure(text::TLS);
        auto V = CPU.readInteger(Slot, PointerSize);
        if (!V)
          return V.takeError();
        Target = *V;
        if (Target) {
          if (Callback == MaxCallbacks ||
              (M.Architecture == GuestArchitecture::AArch64 &&
               Target % DWordSize))
            return failure(text::TLS);
          auto Executable = CPU.canAccess(Target, 1, Execute | UserAccessible);
          if (!Executable)
            return Executable.takeError();
          if (!*Executable)
            return failure(text::TLS);
          ++Callback;
          if (!Detaching)
            States[N.Module] = ModuleState::TLS;
        }
      }
    }
    if (!Target) {
      // A no-entry DLL receives startup TLS, but native process teardown
      // does not notify it. Only successful entry return completes attach.
      advance();
      continue;
    }
    Running = true;
    if (N.Kind == CallKind::Entry)
      return std::optional<Call>({N.Kind, M.Entry, ReturnGate, {PEB}});
    if (N.Kind == CallKind::DLL && !Detaching)
      States[N.Module] = ModuleState::Entry;
    return std::optional<Call>(
        {N.Kind,
         Target,
         Detaching ? DetachReturnGate : AttachReturnGate,
         {M.Base, N.Reason, N.Kind == CallKind::TLS ? 0 : StartupReserved}});
  }
  return std::optional<Call>();
}
llvm::Error Lifetime::returned(uint64_t Value) {
  if (!Running || Position >= Pending.size())
    return failure(text::Lifetime);
  Running = false;
  const auto N = Pending[Position];
  if (N.Kind == CallKind::TLS)
    return llvm::Error::success();
  advance();
  if (N.Kind == CallKind::Entry) {
    if (Program.Modules.size() != 1)
      return failure(text::EntryThreadExit);
    return beginExit(uint32_t(Value), false);
  }
  if (!Detaching) {
    if (!uint32_t(Value))
      return beginExit(StatusDLLInitFailed, true);
    States[N.Module] = ModuleState::Attached;
  }
  return llvm::Error::success();
}
llvm::Error Lifetime::exit(uint32_t Status) { return beginExit(Status, false); }
llvm::Error Lifetime::beginExit(uint32_t Status, bool InitializationFailed) {
  if (Detaching)
    return failure(text::ReentrantExit);
  ExitStatus = Status;
  Detaching = true;
  Running = false;
  Position = 0;
  Callback = 0;
  CallbackArray.reset();
  Pending.clear();
  // Startup failure terminates without DLL or executable detach. Explicit
  // ExitProcess only detaches DLLs whose attach call has returned success.
  if (InitializationFailed)
    return llvm::Error::success();
  for (auto I = Program.InitializationOrder.rbegin();
       I != Program.InitializationOrder.rend(); ++I) {
    if (States[*I] != ModuleState::Attached)
      continue;
    Pending.push_back({*I, CallKind::TLS, DLLProcessDetach});
    Pending.push_back({*I, CallKind::DLL, DLLProcessDetach});
  }
  Pending.push_back({0, CallKind::TLS, DLLProcessDetach});
  return llvm::Error::success();
}
} // namespace neverd::emulation::windows_process
