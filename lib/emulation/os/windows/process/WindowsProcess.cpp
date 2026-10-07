//===- WindowsProcess.cpp - Windows PE64 process continuations -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../../runtime/RuntimeValues.h"
#include "WindowsProcessExceptions.h"
#include "WindowsProcessLoader.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <string>
#include <vector>

namespace neverd::emulation::windows_process {
using namespace value;
namespace {
/// Reads one stopped process for an observer. It owns nothing and exposes no
/// operation that changes the CPU, guest memory or the loader.
class StoppedProcess final : public ProcessView {
public:
  StoppedProcess(ExecutionBackend &CPU, AddressSpace &Space,
                 const Program &Modules, const Environment &Env,
                 const std::optional<Lifetime::Call> &Active,
                 const std::vector<uint64_t> &Initializers,
                 const ProcessResult &Result, ProcessStackView Stack)
      : CPU(CPU), Space(Space), Modules(Modules), Env(Env), Active(Active),
        Initializers(Initializers), Result(Result), Stack(Stack) {}
  GuestArchitecture architecture() const override { return CPU.architecture(); }
  llvm::Expected<RegisterValue> readRegister(CPURegister Register) override {
    return CPU.readRegister(Register);
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override {
    return Space.snapshotBacking(Address, Bytes);
  }
  llvm::Expected<uint32_t> instructionSize(uint64_t Address) override {
    return CPU.instructionSize(Address);
  }
  llvm::Expected<std::vector<AddressMapping>> mappings() override {
    return Space.mappings();
  }
  std::vector<ProcessModuleView> modules() override {
    std::vector<ProcessModuleView> Result;
    auto Add = [&](size_t Index) {
      const auto &M = Modules.Modules[Index];
      if (resident(M))
        Result.push_back({Modules.Identities[Index].Name, M.Loaded.Base,
                          M.Loaded.Size, M.Loaded.Entry, Index == 0,
                          M.System || M.Opaque});
    };
    Add(0);
    for (size_t Index : Modules.LoaderInitializationOrder)
      if (Index)
        Add(Index);
    return Result;
  }
  std::vector<ProcessExportView> exports() override {
    std::vector<ProcessExportView> Result;
    for (const auto &Gate : Modules.Gates)
      Result.push_back({Gate.Gate, Gate.Module, Gate.Name, Gate.Ordinal});
    for (size_t Index = 0; Index < Modules.Modules.size(); ++Index) {
      const auto &M = Modules.Modules[Index];
      // System providers publish exactly their gates. A forwarder or hole has
      // no address of its own in the exporting image.
      if (!resident(M) || M.System)
        continue;
      for (const auto &Export : M.Loaded.Exports.Entries) {
        if (Export.Kind != PEExportKind::Address)
          continue;
        const uint64_t Address = M.Loaded.Base + Export.RVA;
        const auto &Module = Modules.Identities[Index].Name;
        if (Export.Names.empty())
          Result.push_back({Address, Module, {}, uint16_t(Export.Ordinal)});
        for (const auto &Name : Export.Names)
          Result.push_back({Address, Module, Name, uint16_t(Export.Ordinal)});
      }
    }
    return Result;
  }

  bool programInvocation() const override {
    return Active && Active->Kind == Lifetime::CallKind::Entry;
  }
  std::vector<uint64_t> completedInitializers() const override {
    return Initializers;
  }
  std::optional<ProcessStackView> stack() const override { return Stack; }
  std::optional<uint64_t> nativeCallCount() const override {
    return Result.NativeCalls.size();
  }
  llvm::Expected<std::optional<std::vector<uint8_t>>>
  threadLocalMemory() override {
    std::vector<uint8_t> Bytes(Modules.Modules.front().Loaded.TLSSize);
    if (!Bytes.empty()) {
      const auto Block = Env.TLS.find(0);
      if (Block == Env.TLS.end() || Bytes.size() > Block->second.Size)
        return failure(text::TLS);
      if (auto E = Space.snapshotBacking(Block->second.Address, Bytes))
        return std::move(E);
    }
    return std::move(Bytes);
  }

private:
  ExecutionBackend &CPU;
  AddressSpace &Space;
  const Program &Modules;
  const Environment &Env;
  const std::optional<Lifetime::Call> &Active;
  const std::vector<uint64_t> &Initializers;
  const ProcessResult &Result;
  ProcessStackView Stack;
};
} // namespace

llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options,
                                         ProcessObserver *Observer) {
  if (Options.Android || !Options.Limits.Instructions ||
      !Options.Limits.Events || !Options.Limits.TimeoutMicroseconds ||
      !Options.MemoryLimit || !Options.StackSize || !Options.OutputLimit ||
      !Options.InstructionQuantum || Options.StackSize % PageSize ||
      Options.StackSize > MaxStackSize ||
      Options.StackSize >= Options.MemoryLimit ||
      Options.OutputLimit > Options.MemoryLimit)
    return failure(text::Limits);
  auto Budget = ExecutionBudget::create(Options.Limits);
  if (!Budget)
    return Budget.takeError();
  std::shared_ptr<ExecutionBudget> Resources(std::move(*Budget));
  const uint64_t StackBase = StackTop - Options.StackSize;
  auto Physical = PhysicalMemory::create(Options.MemoryLimit);
  if (!Physical)
    return Physical.takeError();
  auto Space = AddressSpace::create(*Physical, Options.MemoryLimit);
  if (!Space)
    return Space.takeError();
  VirtualMemory Virtual(**Space, Options);
  auto Program = loadProgram(Path, Options, *Resources, Virtual);
  if (!Program)
    return Program.takeError();
  auto *Loaded = &Program->Modules.front().Loaded;
  for (const auto &Module : Program->Modules)
    for (const auto &Region : Module.Loaded.Regions) {
      if (auto E = (*Space)->map(Region.Address, Region.Bytes.size(),
                                 Read | Write | UserAccessible))
        return std::move(E);
      if (auto E = (*Space)->write(Region.Address, Region.Bytes))
        return std::move(E);
    }
  if (auto E = (*Space)->map(StackBase, Options.StackSize,
                             Read | Write | UserAccessible))
    return std::move(E);
  if (auto E = (*Space)->map(GateBase, GateSize, Read | Write | UserAccessible))
    return std::move(E);
  std::vector<uint8_t> Trap;
  const bool X64 = Loaded->Architecture == GuestArchitecture::X64;
  if (X64)
    Trap.assign(std::begin(X64Service), std::end(X64Service));
  else {
    Trap.resize(DWordSize);
    llvm::support::endian::write32le(Trap.data(), ArmServiceInstruction);
  }
  for (uint64_t Gate : {ReturnGate, AttachReturnGate, DetachReturnGate,
                        ExceptionReturnGate, ExceptionDispatchGate})
    if (auto E = (*Space)->write(Gate, Trap))
      return std::move(E);
  if (Program->DeferUnmodeled) {
    // Every opaque entry is the same service trap; its address is its
    // identity. Filling the region once lets later lookups bind without
    // touching guest memory.
    std::vector<uint8_t> Entries(OpaqueGateSize);
    for (uint64_t Offset = 0; Offset < OpaqueGateSize; Offset += GateStride)
      std::copy(Trap.begin(), Trap.end(), Entries.begin() + Offset);
    if (auto E = (*Space)->write(OpaqueGateBase, Entries))
      return std::move(E);
  }
  for (const auto &Module : Program->Modules)
    for (const auto &Import : Module.Loaded.Imports)
      if (auto E =
              (*Space)->writeInteger(Import.Slot, Import.Gate, PointerSize))
        return std::move(E);
  auto Env = prepareEnvironment(**Space, *Program, Options);
  if (!Env)
    return Env.takeError();
  if (auto E = (*Space)->protect(GateBase, GateSize,
                                 Read | Execute | UserAccessible))
    return std::move(E);
  for (const auto &Module : Program->Modules)
    for (const auto &Region : Module.Loaded.Regions)
      if (auto E = (*Space)->protect(Region.Address, Region.Bytes.size(),
                                     Region.Permissions))
        return std::move(E);
  for (auto &M : Program->Modules)
    M.State = ModuleState::Ready;
  auto ABI = IntegerABI::get(X64 ? IntegerCallingConvention::Win64
                                 : IntegerCallingConvention::AAPCS64);
  if (!ABI)
    return ABI.takeError();
  if (!Resources->remainingMicroseconds())
    return failure(text::ModuleTimeout);
  const ExecutionContract Checked = X64 ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedUserAArch64;
  const ExecutionContract Contract = Options.Contract.value_or(Checked);
  if (Contract != Checked &&
      !(X64 && Contract == ExecutionContract::DirectUserX64))
    return failure(text::Contract);
  auto Backend = createExecutionBackend(Options.Backend, Contract, *Space,
                                        Loaded->Architecture);
  if (!Backend)
    return Backend.takeError();
  if (auto E = Backend->CPU->writeRegister(
          X64 ? CPURegister::X64GSBase : CPURegister::AArch64X18, {TEB, 0}))
    return std::move(E);
  ExceptionDispatcher Exceptions(*Backend->CPU, *ABI, StackBase, &*Program,
                                 Resources.get());
  auto Session = ExecutionSession::create(
      std::move(Backend->CPU), Resources,
      [&](const BackendFault &Fault) { return Exceptions.accepts(Fault); });
  if (!Session)
    return Session.takeError();
  auto &CPU = (*Session)->cpu();
  const CPURegister PCRegister =
      X64 ? CPURegister::X64PC : CPURegister::AArch64PC;
  ProcessResult Result{ProcessProfile::WindowsPE64, Loaded->Architecture,
                       Backend->Kind, Backend->Reason};
  Result.Entry = Loaded->Entry;
  Result.InitializersEnabled = true;
  Services OS(CPU, **Space, *Loaded, *Env, Options, Result, Virtual, *Program,
              *Resources, Exceptions);
  Lifetime Life(*Program);
  Loader Modules(*Program, Virtual, **Space, *Env, CPU, *Resources);
  std::optional<Lifetime::Call> Active;
  std::vector<uint64_t> Initializers;
  uint64_t ExpectedSP = 0, ExpectedGate = 0;
  uint64_t RootStackPointer = StackTop;
  struct Continuation {
    Loader::Operation Operation;
    std::unique_ptr<BackendContext> Context;
    uint64_t StackPointer, ExpectedSP, ExpectedGate;
    size_t Event;
    std::optional<Lifetime::Call> Active;
  };
  std::vector<Continuation> Pending;
  auto Validate = [&](uint64_t Address, uint64_t Size,
                      unsigned Rights) -> llvm::Error {
    auto Access = CPU.canAccess(Address, Size, Rights | UserAccessible);
    if (!Access)
      return Access.takeError();
    return *Access ? llvm::Error::success() : failure(text::Return);
  };
  auto Return = [&](uint64_t StackPointer, size_t Event,
                    uint64_t Value) -> llvm::Error {
    Result.NativeCalls[Event].Result = Value;
    // Guest callbacks and API outputs may alter the live return slot. CPU
    // restoration preserves memory writes and precedes the ARM64 LR read.
    if (X64)
      if (auto E = Validate(StackPointer, PointerSize, Read))
        return E;
    auto ReturnPC = ABI->readReturnAddress(CPU, StackPointer);
    if (!ReturnPC)
      return ReturnPC.takeError();
    if (auto E = Validate(*ReturnPC, 1, Execute))
      return E;
    auto ReturnSP = ABI->returnStackPointer(StackPointer);
    if (!ReturnSP)
      return ReturnSP.takeError();
    if (auto E = CPU.writeRegister(ABI->info().Result, {Value, 0}))
      return E;
    if (auto E = CPU.writeRegister(ABI->info().StackPointer, {*ReturnSP, 0}))
      return E;
    Result.PC = *ReturnPC;
    return CPU.writeRegister(PCRegister, {Result.PC, 0});
  };
  auto CurrentLife = [&]() -> Lifetime & {
    return Pending.empty() ? Life : *Pending.back().Operation.Notifications;
  };
  StoppedProcess Stopped(CPU, **Space, *Program, *Env, Active, Initializers,
                         Result, {StackBase, Options.StackSize});
  bool Observing = false;
  auto Invoking = [&]() -> llvm::Error {
    if (!Observing || !Observer)
      return llvm::Error::success();
    auto Watches = Observer->invoking(Stopped);
    if (!Watches)
      return Watches.takeError();
    if (*Watches)
      if (auto E = (*Session)->watchExecution(std::move(**Watches)))
        return E;
    return (*Session)->watchMemoryWrites(Observer->writeWatches());
  };
  auto Prepare = [&]() -> llvm::Expected<bool> {
    while (true) {
      auto Next = CurrentLife().next(CPU);
      if (!Next)
        return Next.takeError();
      Active = std::move(*Next);
      if (Active) {
        const uint64_t Top =
            (Pending.empty() ? RootStackPointer : Pending.back().StackPointer) &
            ~(ABI->info().StackAlignment - 1);
        if (Top <= StackBase || Top > StackTop)
          return failure(text::Return);
        Result.PC = Active->PC;
        ExpectedGate = Active->ReturnGate;
        auto Frame = ABI->prepareCall(CPU, StackBase, Top - StackBase,
                                      ExpectedGate, Active->Arguments);
        if (!Frame)
          return Frame.takeError();
        ExpectedSP = Frame->ReturnStackPointer;
        if (auto E = CPU.writeRegister(PCRegister, {Result.PC, 0}))
          return std::move(E);
        if (auto E = Invoking())
          return std::move(E);
        return true;
      }
      if (Pending.empty())
        return false;
      auto &C = Pending.back();
      if (auto E = Modules.complete(C.Operation))
        return std::move(E);
      if (C.Operation.Notifications)
        continue;
      if (auto E = CPU.restoreContext(*C.Context))
        return std::move(E);
      if (C.Operation.Error)
        if (auto E = CPU.writeInteger(TEB + TebLastError, C.Operation.Error,
                                      DWordSize))
          return std::move(E);
      if (auto E = Return(C.StackPointer, C.Event, C.Operation.Value))
        return std::move(E);
      Active = std::move(C.Active);
      ExpectedSP = C.ExpectedSP;
      ExpectedGate = C.ExpectedGate;
      Pending.pop_back();
      if (auto E = Invoking())
        return std::move(E);
      return true;
    }
  };
  auto Prepared = Prepare();
  if (!Prepared)
    return Prepared.takeError();
  if (Observer) {
    auto Watches = Observer->started(Stopped);
    if (!Watches)
      return Watches.takeError();
    if (auto E = (*Session)->watchExecution(std::move(*Watches)))
      return std::move(E);
  }
  Observing = true;
  auto Failed = [&](llvm::Error E) {
    Result.Stop = ProcessStopReason::RuntimeFailure;
    Result.ExitStatus.reset();
    Result.Diagnostic = llvm::toString(std::move(E));
  };
  auto Complete = [&]() {
    Result.Stop = ProcessStopReason::Exited;
    Result.ExitStatus = Life.exitStatus();
  };
  while (true) {
    if (Observer) {
      auto Watches = Observer->resuming(Stopped);
      if (!Watches) {
        Failed(Watches.takeError());
        break;
      }
      if (*Watches)
        if (auto E = (*Session)->watchExecution(std::move(**Watches))) {
          Failed(std::move(E));
          break;
        }
      if (auto E = (*Session)->watchMemoryWrites(Observer->writeWatches())) {
        Failed(std::move(E));
        break;
      }
    }
    auto Exit = (*Session)->run(Result.PC, Options.InstructionQuantum);
    if (!Exit) {
      Failed(Exit.takeError());
      break;
    }
    Result.LastCPUExit = std::move(Exit->CPU);
    auto PC = CPU.readRegister(PCRegister);
    if (!PC) {
      Failed(PC.takeError());
      break;
    }
    Result.PC = (*PC)[0];
    if (Exit->Kind == SessionExitKind::Quantum ||
        Exit->Kind == SessionExitKind::MemoryWriteWatch)
      continue;
    if (Exit->Kind == SessionExitKind::ExecutionWatch) {
      // Only an observer installs watches. The watched instruction has not
      // been admitted, so the process is exactly at its boundary.
      auto Next = Observer->watched(Stopped, Result.PC);
      if (!Next) {
        Failed(Next.takeError());
        break;
      }
      if (!*Next) {
        Result.Stop = ProcessStopReason::Observer;
        Result.Diagnostic = runtime::ObserverStop;
        break;
      }
      if (auto E = (*Session)->watchExecution(std::move(**Next))) {
        Failed(std::move(E));
        break;
      }
      continue;
    }
    if (Exit->Kind == SessionExitKind::InstructionLimit) {
      Result.Stop = ProcessStopReason::InstructionLimit;
      Result.Diagnostic = runtime::InstructionLimit;
      break;
    }
    if (Exit->Kind == SessionExitKind::Timeout) {
      Result.Stop = ProcessStopReason::Timeout;
      Result.Diagnostic = runtime::Timeout;
      break;
    }
    const bool Fault =
        Result.LastCPUExit &&
        Result.LastCPUExit->Kind == ExecutionExitKind::RecoverableFault;
    if (!Result.LastCPUExit ||
        (Result.LastCPUExit->Kind != ExecutionExitKind::ServiceRequest &&
         !Fault)) {
      Result.Stop = ProcessStopReason::CPUFailure;
      Result.Diagnostic =
          Result.LastCPUExit ? Result.LastCPUExit->Diagnostic : text::CPUExit;
      break;
    }
    if (!Resources->consumeEvents()) {
      Result.Stop = ProcessStopReason::EventLimit;
      Result.Diagnostic = runtime::EventLimit;
      break;
    }
    if (Fault) {
      auto Raised = (*Session)->takeRecoverableFault();
      if (!Raised) {
        Failed(Raised.takeError());
        break;
      }
      auto SP = CPU.readRegister(ABI->info().StackPointer);
      if (!SP) {
        Failed(SP.takeError());
        break;
      }
      auto Transfer = Exceptions.beginFault(*Raised, (*SP)[0], Pending.size());
      if (!Transfer) {
        Failed(Transfer.takeError());
        break;
      }
      Result.PC = Transfer->PC;
      continue;
    }
    auto Request = (*Session)->takeServiceRequest();
    if (!Request) {
      Failed(Request.takeError());
      break;
    }
    auto SP = CPU.readRegister(ABI->info().StackPointer);
    if (!SP) {
      Failed(SP.takeError());
      break;
    }
    if (Request->Kind != (X64 ? ServiceRequestKind::X64Syscall
                              : ServiceRequestKind::AArch64SVC) ||
        Request->Immediate) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = text::Service;
      break;
    }
    if (Exceptions.returning(Request->PC, (*SP)[0], Pending.size())) {
      auto V = CPU.readRegister(ABI->info().Result);
      if (!V) {
        Failed(V.takeError());
        break;
      }
      auto Transfer = Exceptions.returned(uint32_t((*V)[0]));
      if (!Transfer) {
        Failed(Transfer.takeError());
        break;
      }
      Result.PC = Transfer->PC;
      if (Transfer->CompletedEvent)
        Result.NativeCalls[*Transfer->CompletedEvent].Result = 0;
      continue;
    }
    if (!Exceptions.activeAt(Pending.size()) && Request->PC == ExpectedGate &&
        (*SP)[0] == ExpectedSP) {
      auto V = CPU.readRegister(ABI->info().Result);
      if (!V) {
        Failed(V.takeError());
        break;
      }
      if (Active->Kind == Lifetime::CallKind::Entry)
        Result.ReturnValue = (*V)[0];
      if (auto E = CurrentLife().returned((*V)[0])) {
        Failed(std::move(E));
        break;
      }
      if (Active->Kind == Lifetime::CallKind::TLS &&
          Active->Arguments[0] == Loaded->Base &&
          Active->Arguments[1] == DLLProcessAttach)
        Initializers.push_back(Active->PC);
      auto More = Prepare();
      if (!More) {
        Failed(More.takeError());
        break;
      }
      if (!*More) {
        Complete();
        break;
      }
      continue;
    }
    const Import *Import = nullptr;
    auto NativeSyscall = [](llvm::StringRef Name) {
      return (Name.starts_with("Nt") || Name.starts_with("Zw"));
    };
    for (const auto &I : Program->Gates)
      if (I.Gate == Request->PC ||
          (Request->PC == I.Gate + NativeSyscallOffset &&
           NativeSyscall(I.Name))) {
        Import = &I;
        break;
      }
    if (!Import) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = text::Service;
      break;
    }
    if (Observing && Observer) {
      // This is call-boundary metadata, not an API result or signature.
      // Read only committed RAM/registers; a missing return cannot authorize
      // an import repair and does not change an opaque export's outcome.
      std::optional<uint64_t> ReturnAddress;
      if (ABI->info().Link != CPURegister::Invalid) {
        auto Link = Stopped.readRegister(ABI->info().Link);
        if (!Link)
          llvm::consumeError(Link.takeError());
        else
          ReturnAddress = (*Link)[0];
      } else {
        std::array<uint8_t, PointerSize> Bytes{};
        if (auto E = Stopped.read((*SP)[0], Bytes))
          llvm::consumeError(std::move(E));
        else
          ReturnAddress = llvm::support::endian::read64le(Bytes.data());
      }
      if (auto E = Observer->exporting(
              Stopped,
              {Import->Gate, Import->Module, Import->Name, Import->Ordinal},
              ReturnAddress)) {
        Failed(std::move(E));
        break;
      }
    }
    if (!Import->Target) {
      // The guest was free to resolve and store this address; only calling
      // it requires behavior the model does not have.
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic =
          (text::OpaqueEntry + Import->Module + text::ImportSeparator +
           (Import->Ordinal
                ? text::OpaqueOrdinal + llvm::Twine(*Import->Ordinal)
                : llvm::Twine(Import->Name)))
              .str();
      break;
    }
    const uint64_t StackPointer = (*SP)[0];
    if (auto E = ABI->validateStackPointer(StackPointer)) {
      Failed(std::move(E));
      break;
    }
    // User accessibility is an OS call-boundary requirement in addition to
    // the ABI's generic trusted-memory checks. Preflight before API effects.
    if (Import->Target->Returns && X64)
      if (auto E = Validate(StackPointer, PointerSize, Read)) {
        Failed(std::move(E));
        break;
      }
    NativeCallEvent Event{Request->PC, Import->Target->Name};
    Event.Module = Import->Module;
    Event.ArgumentCount = Import->Target->Arguments;
    if (X64) {
      auto Ret = (*Space)->readInteger(StackPointer, PointerSize);
      if (!Ret)
        llvm::consumeError(Ret.takeError());
      else
        Event.ReturnAddress = *Ret;
    }
    bool Invalid = false;
    for (unsigned I = 0; I < Event.ArgumentCount; ++I) {
      auto Location = ABI->argumentLocation(StackPointer, I);
      if (!Location) {
        Failed(Location.takeError());
        Invalid = true;
        break;
      }
      if (Location->Register == CPURegister::Invalid)
        if (auto E = Validate(Location->Address, PointerSize, Read)) {
          Failed(std::move(E));
          Invalid = true;
          break;
        }
      auto V = ABI->readArgument(CPU, StackPointer, I);
      if (!V) {
        Failed(V.takeError());
        Invalid = true;
        break;
      }
      Event.Arguments[I] = *V;
    }
    if (Invalid)
      break;
    const size_t EventIndex = Result.NativeCalls.size();
    Result.NativeCalls.push_back(Event);
    auto V = OS.invoke(*Import->Target, Event);
    if (!V) {
      Failed(V.takeError());
      break;
    }
    if (V->Raised) {
      // RaiseException resumes through its modeled executable RET. Its CONTEXT
      // therefore points inside the provider, preserving the original live
      // return slot and ARM64 LR when a handler elects to continue.
      if (auto E = CPU.writeRegister(PCRegister, {Request->NextPC, 0})) {
        Failed(std::move(E));
        break;
      }
      V->Raised->Address = Request->NextPC;
      auto Transfer = Exceptions.begin(std::move(*V->Raised), StackPointer,
                                       Pending.size(), EventIndex);
      if (!Transfer) {
        Failed(Transfer.takeError());
        break;
      }
      Result.PC = Transfer->PC;
      continue;
    }
    if (V->Request) {
      if (Pending.size() >= MaxLoaderDepth) {
        Failed(failure(text::ModuleBudget));
        break;
      }
      auto Operation = Modules.begin(*V->Request);
      if (!Operation) {
        Failed(Operation.takeError());
        break;
      }
      if (Operation->Notifications) {
        auto Context = CPU.saveContext();
        if (!Context) {
          Failed(Context.takeError());
          break;
        }
        Pending.push_back({std::move(*Operation), std::move(*Context),
                           StackPointer, ExpectedSP, ExpectedGate, EventIndex,
                           std::move(Active)});
        auto More = Prepare();
        if (!More) {
          Failed(More.takeError());
          break;
        }
        continue;
      }
      if (Operation->Error)
        if (auto E = CPU.writeInteger(TEB + TebLastError, Operation->Error,
                                      DWordSize)) {
          Failed(std::move(E));
          break;
        }
      V->Value = Operation->Value;
    }
    if (!V->Value) {
      if (Result.Stop != ProcessStopReason::Exited)
        break;
      const uint32_t Status = *Result.ExitStatus;
      Result.ExitStatus.reset();
      Pending.clear();
      Exceptions.abandon();
      // Process-detach callbacks may still observe the exiting caller's
      // frame. Abandon its continuation without overwriting that storage.
      RootStackPointer = StackPointer;
      if (auto E = Life.exit(Status)) {
        Failed(std::move(E));
        break;
      }
      auto More = Prepare();
      if (!More) {
        Failed(More.takeError());
        break;
      }
      if (*More)
        continue;
      Complete();
      break;
    }
    if (auto E = Return(StackPointer, EventIndex, *V->Value)) {
      Failed(std::move(E));
      break;
    }
  }
  Result.Instructions = Resources->instructions();
  Result.Events = Resources->events();
  return Result;
}
} // namespace neverd::emulation::windows_process
